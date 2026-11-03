// wl_capture.c — Wayland source backend: capture monitors of a Wayland
// compositor (KWin) through the xdg-desktop-portal ScreenCast API + PipeWire,
// and render them with EGL into the squint window on the (X11) destination
// display.
//
// Frames arrive as DMA-BUFs. We only offer the LINEAR modifier, so the
// compositor's own post-composite copy lands directly in a buffer the other
// GPU can import -- the same trick as the X11 path's linear DRI3 pixmap, but
// without any AMD-side work of our own. If the compositor can't allocate a
// linear DMA-BUF the stream falls back to shared memory (CPU upload).
//
// Cursor: requested as metadata (not embedded) so that each monitor stream
// tells us whether the pointer is on it. That replaces XQueryPointer for
// cursor-follow, and the cursor bitmap is drawn as a GL overlay.
//
// All PipeWire work runs on the GLib main loop (the pw_loop fd is polled by
// GLib), so no locking is needed.

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/debug/types.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/utils/result.h>
#include <libdrm/drm_fourcc.h>

#include "squint.h"
#include "egl.h"

#define PORTAL_BUS   "org.freedesktop.portal.Desktop"
#define PORTAL_PATH  "/org/freedesktop/portal/desktop"
#define PORTAL_SC    "org.freedesktop.portal.ScreenCast"
#define PORTAL_REQ   "org.freedesktop.portal.Request"

#define SOURCE_TYPE_MONITOR   1
#define CURSOR_MODE_EMBEDDED  2
#define CURSOR_MODE_METADATA  4
#define PERSIST_MODE_PERSISTENT 2

#define CURSOR_META_SIZE(w, h) (sizeof(struct spa_meta_cursor) + \
	sizeof(struct spa_meta_bitmap) + (w) * (h) * 4)
#define CURSOR_MAX 256

typedef struct {
	GLuint texture;  // EGL import of this buffer's DMA-BUF (lazy)
} wl_buffer_priv_t;

typedef struct {
	int idx;
	uint32_t node_id;
	GdkRectangle rect;          // logical position/size reported by the portal

	struct pw_stream* stream;
	struct spa_hook listener;
	struct spa_video_info_raw format;
	gboolean has_format;
	gboolean is_dmabuf;
	gboolean was_streaming;

	struct pw_buffer* held;     // buffer currently shown / to show next
	GLuint shm_texture;
	gboolean shm_dirty;

	// cursor (stream pixel coordinates)
	gboolean cursor_on;
	int cursor_x, cursor_y, hot_x, hot_y;
	guint8* cursor_pixels;      // RGBA/BGRA copy of the last bitmap
	int cursor_w, cursor_h;
	GLenum cursor_gl_format;
	gboolean cursor_dirty;
	GLuint cursor_texture;

	// perf
	guint frames;
	gint64 last_pts_age_us;
} wl_source_t;

static GDBusConnection* bus = NULL;
static char* session_handle = NULL;
static guint request_counter = 0;
static guint cursor_mode = CURSOR_MODE_METADATA;

static struct pw_loop* pw_loop = NULL;
static struct pw_context* pw_ctx = NULL;
static struct pw_core* pw_core = NULL;
static struct spa_hook core_listener;

static wl_source_t* sources = NULL;
static int n_sources = 0;
static int active = 0;
static int pending_switch = -1;
static int switch_entry_x, switch_entry_y;

static egl_context_t* egl_ctx = NULL;
static gboolean bound = FALSE;
static guint render_idle_id = 0;

// perf-log accumulators (reset every report)
static guint perf_renders = 0;
static gdouble perf_render_us_sum = 0, perf_render_us_max = 0;
static gdouble perf_age_us_sum = 0, perf_age_us_max = 0;
static guint perf_age_n = 0;
static gint64 perf_last_report = 0;

static void wl_fatal(const char* msg)
{
	// Fatal for the capture session: the portal session or the PipeWire
	// connection is gone and nothing here can recover it. Exit non-zero so a
	// supervising systemd unit (Restart=on-failure) starts a fresh session.
	fprintf(stderr, "wl: fatal: %s\n", msg);
	exit(1);
}

//
// Restore token persistence (so the portal dialog is shown only once)
//

static char*
token_path(void)
{
	const char* id = config.portal_id ? config.portal_id : "default";
	return g_strdup_printf("%s/squint/portal-%s.token", g_get_user_state_dir(), id);
}

static char*
load_restore_token(void)
{
	char* path = token_path();
	char* contents = NULL;
	if (g_file_get_contents(path, &contents, NULL, NULL))
		g_strstrip(contents);
	g_free(path);
	if (contents && !*contents) {
		g_free(contents);
		contents = NULL;
	}
	return contents;
}

static void
save_restore_token(const char* token)
{
	char* path = token_path();
	char* dir = g_path_get_dirname(path);
	GError* err = NULL;
	g_mkdir_with_parents(dir, 0700);
	if (!g_file_set_contents(path, token, -1, &err)) {
		fprintf(stderr, "wl: could not save restore token to %s: %s\n", path, err->message);
		g_clear_error(&err);
	}
	g_free(dir);
	g_free(path);
}

//
// Rendering
//

static void wl_schedule_render(void);

static GLenum
spa_to_gl_format(uint32_t spa_fmt)
{
	switch (spa_fmt) {
	case SPA_VIDEO_FORMAT_RGBA:
	case SPA_VIDEO_FORMAT_RGBx:
		return GL_RGBA;
	default:  // BGRA / BGRx
		return GL_BGRA;
	}
}

static uint32_t
spa_to_drm_format(uint32_t spa_fmt)
{
	switch (spa_fmt) {
	case SPA_VIDEO_FORMAT_BGRA: return DRM_FORMAT_ARGB8888;
	case SPA_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
	case SPA_VIDEO_FORMAT_RGBA: return DRM_FORMAT_ABGR8888;
	case SPA_VIDEO_FORMAT_RGBx: return DRM_FORMAT_XBGR8888;
	default: return 0;
	}
}

static GLuint
source_texture(wl_source_t* s)
{
	struct pw_buffer* b = s->held;
	if (!b) return 0;
	struct spa_data* d = &b->buffer->datas[0];

	if (s->is_dmabuf) {
		wl_buffer_priv_t* priv = b->user_data;
		if (!priv->texture) {
			if (d->chunk->offset != 0)
				fprintf(stderr, "wl: warning: dmabuf offset %u ignored\n", d->chunk->offset);
			// The fd stays owned by PipeWire; the texture keeps its own
			// reference to the buffer through the (destroyed) EGLImage.
			priv->texture = egl_create_image_from_fd(egl_ctx, d->fd,
					s->format.size.width, s->format.size.height,
					d->chunk->stride, spa_to_drm_format(s->format.format),
					DRM_FORMAT_MOD_LINEAR);
			if (!priv->texture)
				wl_fatal("DMA-BUF import into the destination GPU failed");
		}
		return priv->texture;
	}

	if (!s->shm_texture)
		s->shm_texture = egl_create_texture(egl_ctx);
	if (s->shm_dirty && d->data) {
		egl_upload_texture(egl_ctx, s->shm_texture,
				s->format.size.width, s->format.size.height,
				d->chunk->stride, spa_to_gl_format(s->format.format),
				SPA_PTROFF(d->data, d->chunk->offset, void));
		s->shm_dirty = FALSE;
	}
	return s->shm_texture;
}

static void
perf_report(void)
{
	gint64 now = g_get_monotonic_time();
	if (!perf_last_report) perf_last_report = now;
	if (now - perf_last_report < 2 * G_USEC_PER_SEC) return;

	gdouble secs = (now - perf_last_report) / (gdouble)G_USEC_PER_SEC;
	fprintf(stderr, "perf[wl]: %.1f renders/s  render+swap avg %.2fms max %.2fms",
			perf_renders / secs,
			perf_renders ? perf_render_us_sum / perf_renders / 1000.0 : 0,
			perf_render_us_max / 1000.0);
	if (perf_age_n)
		fprintf(stderr, "  pts->swap avg %.2fms max %.2fms",
				perf_age_us_sum / perf_age_n / 1000.0, perf_age_us_max / 1000.0);
	fprintf(stderr, "  frames/s:");
	for (int i = 0; i < n_sources; i++) {
		fprintf(stderr, " %s%d=%.1f", i == active ? "*" : "", i + 1, sources[i].frames / secs);
		sources[i].frames = 0;
	}
	fprintf(stderr, "\n");

	perf_renders = 0;
	perf_render_us_sum = perf_render_us_max = 0;
	perf_age_us_sum = perf_age_us_max = 0;
	perf_age_n = 0;
	perf_last_report = now;
}

static gboolean
render_idle_cb(gpointer data)
{
	render_idle_id = 0;
	if (!bound || n_sources == 0) return G_SOURCE_REMOVE;

	wl_source_t* s = &sources[active];
	gint64 t0 = g_get_monotonic_time();

	GLuint tex = source_texture(s);
	if (!tex) return G_SOURCE_REMOVE;  // no frame yet
	egl_draw_quad(egl_ctx, tex);

	if (s->cursor_on && s->cursor_pixels && s->cursor_w > 0) {
		if (!s->cursor_texture)
			s->cursor_texture = egl_create_texture(egl_ctx);
		if (s->cursor_dirty) {
			egl_upload_texture(egl_ctx, s->cursor_texture, s->cursor_w, s->cursor_h,
					s->cursor_w * 4, s->cursor_gl_format, s->cursor_pixels);
			s->cursor_dirty = FALSE;
		}
		float sw = s->format.size.width, sh = s->format.size.height;
		egl_draw_overlay(egl_ctx, s->cursor_texture,
				(s->cursor_x - s->hot_x) / sw, (s->cursor_y - s->hot_y) / sh,
				s->cursor_w / sw, s->cursor_h / sh);
	}

	egl_swap_buffers(egl_ctx);

	if (config.opt_perf_log) {
		gint64 t1 = g_get_monotonic_time();
		gdouble us = t1 - t0;
		perf_renders++;
		perf_render_us_sum += us;
		if (us > perf_render_us_max) perf_render_us_max = us;
		if (s->last_pts_age_us > 0) {
			// age of the frame at capture + time spent here until swap returned
			gdouble age = s->last_pts_age_us + us;
			perf_age_us_sum += age;
			if (age > perf_age_us_max) perf_age_us_max = age;
			perf_age_n++;
			s->last_pts_age_us = 0;
		}
		perf_report();
	}
	return G_SOURCE_REMOVE;
}

static void
wl_schedule_render(void)
{
	// Coalesce: several streams / a frame plus cursor updates arriving in one
	// pw_loop iteration produce a single draw+swap.
	if (bound && !render_idle_id)
		render_idle_id = g_idle_add_full(G_PRIORITY_HIGH_IDLE, render_idle_cb, NULL, NULL);
}

//
// Cursor follow
//

static void
wl_any_cursor_check(void)
{
	if (config.opt_passive) return;
	for (int i = 0; i < n_sources; i++)
		if (sources[i].cursor_on) return;
	squint_hide();
}

static void
on_cursor(wl_source_t* s)
{
	if (!enabled) return;

	if (!s->cursor_on) {
		if (s->idx == pending_switch) pending_switch = -1;
		if (s->idx == active) {
			wl_schedule_render();  // erase the cursor
			wl_any_cursor_check();
		}
		return;
	}

	if (s->idx == active) {
		pending_switch = -1;
		squint_show();
		wl_schedule_render();
		return;
	}

	int threshold = config.opt_switch_threshold;
	if (threshold <= 0) {
		pending_switch = -1;
		squint_switch_source(s->idx);
	} else if (pending_switch != s->idx) {
		pending_switch = s->idx;
		switch_entry_x = s->cursor_x;
		switch_entry_y = s->cursor_y;
	} else if (abs(s->cursor_x - switch_entry_x) >= threshold ||
	           abs(s->cursor_y - switch_entry_y) >= threshold) {
		pending_switch = -1;
		squint_switch_source(s->idx);
	}
}

static gboolean
read_cursor_meta(wl_source_t* s, struct spa_buffer* buf)
{
	struct spa_meta_cursor* mc = spa_buffer_find_meta_data(buf, SPA_META_Cursor, sizeof(*mc));
	if (!mc) return FALSE;

	gboolean was_on = s->cursor_on;
	int ox = s->cursor_x, oy = s->cursor_y;

	if (!spa_meta_cursor_is_valid(mc)) {
		s->cursor_on = FALSE;
	} else {
		s->cursor_x = mc->position.x;
		s->cursor_y = mc->position.y;
		s->hot_x = mc->hotspot.x;
		s->hot_y = mc->hotspot.y;
		s->cursor_on = s->has_format
			&& s->cursor_x >= 0 && s->cursor_y >= 0
			&& s->cursor_x < (int)s->format.size.width
			&& s->cursor_y < (int)s->format.size.height;

		if (mc->bitmap_offset >= sizeof(*mc)) {
			struct spa_meta_bitmap* bmp = SPA_PTROFF(mc, mc->bitmap_offset, struct spa_meta_bitmap);
			int w = bmp->size.width, h = bmp->size.height;
			if (w > 0 && h > 0 && w <= CURSOR_MAX && h <= CURSOR_MAX && bmp->stride >= w * 4) {
				const guint8* px = SPA_PTROFF(bmp, bmp->offset, guint8);
				g_free(s->cursor_pixels);
				s->cursor_pixels = g_malloc((gsize)w * h * 4);
				for (int y = 0; y < h; y++)
					memcpy(s->cursor_pixels + (gsize)y * w * 4, px + (gsize)y * bmp->stride, (gsize)w * 4);
				s->cursor_w = w;
				s->cursor_h = h;
				s->cursor_gl_format = spa_to_gl_format(bmp->format);
				s->cursor_dirty = TRUE;
			} else if (w == 0 || h == 0) {
				// empty bitmap = cursor hidden (e.g. a game grabbed it)
				s->cursor_w = s->cursor_h = 0;
			}
		}
	}
	return was_on != s->cursor_on || ox != s->cursor_x || oy != s->cursor_y || s->cursor_dirty;
}

//
// PipeWire stream callbacks
//

static void
on_stream_state_changed(void* data, enum pw_stream_state old,
		enum pw_stream_state state, const char* error)
{
	wl_source_t* s = data;
	fprintf(stderr, "wl: stream %d: %s -> %s%s%s\n", s->idx + 1,
			pw_stream_state_as_string(old), pw_stream_state_as_string(state),
			error ? ": " : "", error ? error : "");
	if (state == PW_STREAM_STATE_STREAMING)
		s->was_streaming = TRUE;
	if (state == PW_STREAM_STATE_ERROR ||
	    (state == PW_STREAM_STATE_UNCONNECTED && s->was_streaming))
		wl_fatal("capture stream ended");
}

static void
build_format_params(struct spa_pod_builder* b, const struct spa_pod** params,
		int* n, gboolean with_modifier, uint32_t fixed_format)
{
	uint32_t formats[] = { SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
	                       SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA };
	for (unsigned i = 0; i < G_N_ELEMENTS(formats); i++) {
		if (fixed_format && formats[i] != fixed_format) continue;
		struct spa_pod_frame f;
		spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
		spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format, SPA_POD_Id(formats[i]),
			0);
		if (with_modifier) {
			// Only LINEAR: the destination GPU (a different vendor) can't
			// de-tile the compositor GPU's native layouts.
			spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
			spa_pod_builder_long(b, DRM_FORMAT_MOD_LINEAR);
		}
		spa_pod_builder_add(b,
			SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
				&SPA_RECTANGLE(1920, 1080), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(16384, 16384)),
			SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
				&SPA_FRACTION(0, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)),
			0);
		params[(*n)++] = spa_pod_builder_pop(b, &f);
	}
}

static void
on_stream_param_changed(void* data, uint32_t id, const struct spa_pod* param)
{
	wl_source_t* s = data;
	if (!param || id != SPA_PARAM_Format) return;

	uint32_t media_type, media_subtype;
	if (spa_format_parse(param, &media_type, &media_subtype) < 0 ||
	    media_type != SPA_MEDIA_TYPE_video || media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	spa_format_video_raw_parse(param, &s->format);
	s->has_format = TRUE;
	const struct spa_pod_prop* mod = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier);
	s->is_dmabuf = mod != NULL;

	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod* params[8];
	int n = 0;

	// A plain (non-choice) LINEAR value is already fixed, whatever the flags
	// say; only renegotiate when the producer really left a choice open,
	// otherwise a producer that echoes DONT_FIXATE back would loop forever.
	int64_t mod_value = 0;
	gboolean mod_fixed = mod && spa_pod_get_long(&mod->value, &mod_value) == 0
		&& (uint64_t)mod_value == DRM_FORMAT_MOD_LINEAR;
	if (mod && !mod_fixed && (mod->flags & SPA_POD_PROP_FLAG_DONT_FIXATE)) {
		// Producer left the modifier open for us to pick; we only ever accept
		// LINEAR, so answer with that single fixed choice and wait for the
		// renegotiated format.
		build_format_params(&b, params, &n, TRUE, s->format.format);
		pw_stream_update_params(s->stream, params, n);
		return;
	}

	fprintf(stderr, "wl: stream %d: %ux%u %s via %s\n", s->idx + 1,
			s->format.size.width, s->format.size.height,
			spa_debug_type_find_short_name(spa_type_video_format, s->format.format),
			s->is_dmabuf ? "linear DMA-BUF (zero-copy)" : "SHM (CPU copy, slower)");

	int data_types = s->is_dmabuf ? (1 << SPA_DATA_DmaBuf)
	                              : ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr));
	params[n++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(data_types));
	params[n++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	params[n++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
		SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
			CURSOR_META_SIZE(64, 64), CURSOR_META_SIZE(1, 1),
			CURSOR_META_SIZE(CURSOR_MAX, CURSOR_MAX)));
	pw_stream_update_params(s->stream, params, n);
}

static void
on_stream_add_buffer(void* data, struct pw_buffer* b)
{
	b->user_data = g_new0(wl_buffer_priv_t, 1);
}

static void
on_stream_remove_buffer(void* data, struct pw_buffer* b)
{
	wl_source_t* s = data;
	wl_buffer_priv_t* priv = b->user_data;
	if (priv && priv->texture && egl_ctx)
		egl_delete_texture(egl_ctx, priv->texture);
	g_free(priv);
	b->user_data = NULL;
	if (s->held == b) s->held = NULL;
}

static void
on_stream_process(void* data)
{
	wl_source_t* s = data;

	// Drain to the newest buffer; anything older is already stale.
	struct pw_buffer *b, *latest = NULL;
	while ((b = pw_stream_dequeue_buffer(s->stream))) {
		if (latest) {
			gboolean changed = read_cursor_meta(s, latest->buffer);
			(void)changed;
			pw_stream_queue_buffer(s->stream, latest);
		}
		latest = b;
	}
	if (!latest) return;

	struct spa_buffer* buf = latest->buffer;
	gboolean cursor_changed = read_cursor_meta(s, buf);

	struct spa_meta_header* h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h));
	struct spa_data* d = &buf->datas[0];
	// Cursor-only updates (metadata cursor moved, picture unchanged) come as
	// buffers with an empty chunk -- for DMA-BUFs too, whose fd is always
	// valid, so the size is the only reliable "new picture" signal.
	gboolean has_frame = d->chunk != NULL
		&& d->chunk->size > 0
		&& !(d->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)
		&& !(h && (h->flags & SPA_META_HEADER_FLAG_CORRUPTED))
		&& (s->is_dmabuf ? d->fd >= 0 : d->data != NULL);

	if (has_frame) {
		// Keep this buffer (the compositor won't write into it until we give
		// it back) and return the previously shown one.
		if (s->held) pw_stream_queue_buffer(s->stream, s->held);
		s->held = latest;
		s->shm_dirty = TRUE;
		s->frames++;
		if (config.opt_perf_log && h && h->pts > 0) {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			gint64 now_ns = (gint64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
			gint64 age = (now_ns - h->pts) / 1000;
			// Only trust it if it looks like CLOCK_MONOTONIC (< 1s old).
			s->last_pts_age_us = (age >= 0 && age < G_USEC_PER_SEC) ? age : 0;
		}
		if (s->idx == active && enabled) wl_schedule_render();
	} else {
		// Cursor-only update: the buffer's pixels are an old frame.
		pw_stream_queue_buffer(s->stream, latest);
	}

	if (cursor_changed)
		on_cursor(s);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.param_changed = on_stream_param_changed,
	.add_buffer = on_stream_add_buffer,
	.remove_buffer = on_stream_remove_buffer,
	.process = on_stream_process,
};

static void
on_core_error(void* data, uint32_t id, int seq, int res, const char* message)
{
	fprintf(stderr, "wl: pipewire error id:%u seq:%d res:%d (%s): %s\n",
			id, seq, res, spa_strerror(res), message);
	if (id == PW_ID_CORE && res == -EPIPE)
		wl_fatal("lost connection to PipeWire");
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static gboolean
pw_loop_cb(gint fd, GIOCondition cond, gpointer data)
{
	int res = pw_loop_iterate(pw_loop, 0);
	if (res < 0 && res != -EINTR)
		fprintf(stderr, "wl: pw_loop_iterate: %s\n", spa_strerror(res));
	return G_SOURCE_CONTINUE;
}

static void
connect_streams(int pw_fd)
{
	pw_init(NULL, NULL);
	pw_loop = pw_loop_new(NULL);
	pw_ctx = pw_context_new(pw_loop, NULL, 0);
	pw_core = pw_context_connect_fd(pw_ctx, pw_fd, NULL, 0);
	if (!pw_core) wl_fatal("pw_context_connect_fd failed");
	pw_core_add_listener(pw_core, &core_listener, &core_events, NULL);

	pw_loop_enter(pw_loop);
	g_unix_fd_add(pw_loop_get_fd(pw_loop), G_IO_IN, pw_loop_cb, NULL);

	for (int i = 0; i < n_sources; i++) {
		wl_source_t* s = &sources[i];
		s->stream = pw_stream_new(pw_core, "squint",
			pw_properties_new(
				PW_KEY_MEDIA_TYPE, "Video",
				PW_KEY_MEDIA_CATEGORY, "Capture",
				PW_KEY_MEDIA_ROLE, "Screen",
				NULL));
		pw_stream_add_listener(s->stream, &s->listener, &stream_events, s);

		uint8_t buffer[4096];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		const struct spa_pod* params[8];
		int n = 0;
		build_format_params(&b, params, &n, TRUE, 0);   // preferred: linear DMA-BUF
		build_format_params(&b, params, &n, FALSE, 0);  // fallback: SHM
		pw_stream_connect(s->stream, PW_DIRECTION_INPUT, s->node_id,
				PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
				params, n);
	}
}

//
// Portal handshake: CreateSession -> SelectSources -> Start -> OpenPipeWireRemote
//

typedef void (*response_cb_t)(guint32 response, GVariant* results);

typedef struct {
	guint signal_id;
	response_cb_t cb;
} request_t;

static void
on_request_response(GDBusConnection* conn, const char* sender, const char* path,
		const char* iface, const char* signal, GVariant* params, gpointer data)
{
	request_t* req = data;
	guint32 response;
	GVariant* results;
	g_variant_get(params, "(u@a{sv})", &response, &results);
	g_dbus_connection_signal_unsubscribe(conn, req->signal_id);
	response_cb_t cb = req->cb;
	g_free(req);
	cb(response, results);
	g_variant_unref(results);
}

// Builds a handle_token and subscribes to its Request::Response before the
// call is made (subscribing after could miss a fast response).
static char*
new_request(response_cb_t cb)
{
	char* token = g_strdup_printf("squint%u", ++request_counter);
	char* sender = g_strdup(g_dbus_connection_get_unique_name(bus) + 1);
	for (char* p = sender; *p; p++)
		if (*p == '.') *p = '_';
	char* path = g_strdup_printf(PORTAL_PATH "/request/%s/%s", sender, token);

	request_t* req = g_new0(request_t, 1);
	req->cb = cb;
	req->signal_id = g_dbus_connection_signal_subscribe(bus, PORTAL_BUS, PORTAL_REQ,
			"Response", path, NULL, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
			on_request_response, req, NULL);
	g_free(sender);
	g_free(path);
	return token;
}

static void
portal_call(const char* method, GVariant* args)
{
	GError* err = NULL;
	GVariant* ret = g_dbus_connection_call_sync(bus, PORTAL_BUS, PORTAL_PATH, PORTAL_SC,
			method, args, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	if (!ret) {
		char* msg = g_strdup_printf("portal %s failed: %s", method, err->message);
		wl_fatal(msg);
	}
	g_variant_unref(ret);
}

static void
check_response(const char* step, guint32 response)
{
	if (response == 0) return;
	char* msg = g_strdup_printf("portal %s %s", step,
			response == 1 ? "was cancelled (screen sharing dialog dismissed?)" : "failed");
	wl_fatal(msg);
}

static int
compare_sources(const void* a, const void* b)
{
	const wl_source_t* sa = a;
	const wl_source_t* sb = b;
	if (sa->rect.x != sb->rect.x) return sa->rect.x - sb->rect.x;
	return sa->rect.y - sb->rect.y;
}

static void
on_started(guint32 response, GVariant* results)
{
	check_response("Start", response);

	const char* token = NULL;
	if (g_variant_lookup(results, "restore_token", "&s", &token))
		save_restore_token(token);

	GVariant* streams = g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("a(ua{sv})"));
	if (!streams || g_variant_n_children(streams) == 0)
		wl_fatal("portal returned no streams");

	n_sources = g_variant_n_children(streams);
	sources = g_new0(wl_source_t, n_sources);
	for (int i = 0; i < n_sources; i++) {
		GVariant* props;
		g_variant_get_child(streams, i, "(u@a{sv})", &sources[i].node_id, &props);
		gint32 x = 0, y = 0, w = 0, h = 0;
		g_variant_lookup(props, "position", "(ii)", &x, &y);
		g_variant_lookup(props, "size", "(ii)", &w, &h);
		sources[i].rect = (GdkRectangle){ x, y, w, h };
		g_variant_unref(props);
	}
	g_variant_unref(streams);

	// Portal order follows the dialog's selection order; sort left-to-right so
	// source numbering is stable across restore-token restarts.
	qsort(sources, n_sources, sizeof(wl_source_t), compare_sources);
	for (int i = 0; i < n_sources; i++) {
		sources[i].idx = i;
		fprintf(stderr, "wl: source %d: node %u, %dx%d @(%d,%d)\n", i + 1,
				sources[i].node_id, sources[i].rect.width, sources[i].rect.height,
				sources[i].rect.x, sources[i].rect.y);
	}

	GError* err = NULL;
	GUnixFDList* fds = NULL;
	GVariantBuilder opts;
	g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
	GVariant* ret = g_dbus_connection_call_with_unix_fd_list_sync(bus, PORTAL_BUS,
			PORTAL_PATH, PORTAL_SC, "OpenPipeWireRemote",
			g_variant_new("(oa{sv})", session_handle, &opts),
			G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &fds, NULL, &err);
	if (!ret) {
		char* msg = g_strdup_printf("OpenPipeWireRemote failed: %s", err->message);
		wl_fatal(msg);
	}
	gint32 handle;
	g_variant_get(ret, "(h)", &handle);
	int pw_fd = g_unix_fd_list_get(fds, handle, &err);
	g_variant_unref(ret);
	g_object_unref(fds);
	if (pw_fd < 0) wl_fatal("could not get the PipeWire fd");

	connect_streams(pw_fd);

	if (!config.opt_disable)
		squint_enable();
}

static void
on_sources_selected(guint32 response, GVariant* results)
{
	check_response("SelectSources", response);

	char* token = new_request(on_started);
	GVariantBuilder opts;
	g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
	portal_call("Start", g_variant_new("(osa{sv})", session_handle, "", &opts));
	g_free(token);
}

static void
on_session_closed(GDBusConnection* conn, const char* sender, const char* path,
		const char* iface, const char* signal, GVariant* params, gpointer data)
{
	wl_fatal("portal session was closed (sharing stopped from the compositor?)");
}

static void
on_session_created(guint32 response, GVariant* results)
{
	check_response("CreateSession", response);
	if (!g_variant_lookup(results, "session_handle", "s", &session_handle))
		wl_fatal("CreateSession returned no session handle");

	g_dbus_connection_signal_subscribe(bus, PORTAL_BUS, "org.freedesktop.portal.Session",
			"Closed", session_handle, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
			on_session_closed, NULL, NULL);

	char* token = new_request(on_sources_selected);
	char* restore = load_restore_token();
	GVariantBuilder opts;
	g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
	g_variant_builder_add(&opts, "{sv}", "types", g_variant_new_uint32(SOURCE_TYPE_MONITOR));
	g_variant_builder_add(&opts, "{sv}", "multiple", g_variant_new_boolean(TRUE));
	g_variant_builder_add(&opts, "{sv}", "cursor_mode", g_variant_new_uint32(cursor_mode));
	g_variant_builder_add(&opts, "{sv}", "persist_mode", g_variant_new_uint32(PERSIST_MODE_PERSISTENT));
	if (restore)
		g_variant_builder_add(&opts, "{sv}", "restore_token", g_variant_new_string(restore));
	else
		fprintf(stderr, "wl: no saved portal permission for '%s' -- pick the source "
				"monitor(s) in the screen sharing dialog\n",
				config.portal_id ? config.portal_id : "default");
	portal_call("SelectSources", g_variant_new("(oa{sv})", session_handle, &opts));
	g_free(restore);
	g_free(token);
}

static gboolean
portal_property_u(const char* name, guint32* out)
{
	GError* err = NULL;
	GVariant* ret = g_dbus_connection_call_sync(bus, PORTAL_BUS, PORTAL_PATH,
			"org.freedesktop.DBus.Properties", "Get",
			g_variant_new("(ss)", PORTAL_SC, name), G_VARIANT_TYPE("(v)"),
			G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	if (!ret) {
		fprintf(stderr, "wl: screen cast portal unavailable: %s\n", err->message);
		g_clear_error(&err);
		return FALSE;
	}
	GVariant* v;
	g_variant_get(ret, "(v)", &v);
	guint32 val = g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(v) : 0;
	g_variant_unref(v);
	g_variant_unref(ret);
	*out = val;
	return TRUE;
}

//
// Public API
//

gboolean
wl_init(void)
{
	GError* err = NULL;
	bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (!bus) {
		fprintf(stderr, "wl: no session bus: %s\n", err->message);
		return FALSE;
	}

	guint32 modes = 0;
	if (!portal_property_u("AvailableCursorModes", &modes)) {
		squint_error("No xdg-desktop-portal ScreenCast service on the session bus");
		return FALSE;
	}
	if (!(modes & CURSOR_MODE_METADATA)) {
		fprintf(stderr, "wl: portal lacks metadata cursors (modes=0x%x); using embedded "
				"cursor, cursor-follow disabled\n", modes);
		cursor_mode = CURSOR_MODE_EMBEDDED;
	}

	egl_ctx = egl_init(gdisplay);
	if (!egl_ctx) return FALSE;

	x11_watch_screen_changes();

	char* token = new_request(on_session_created);
	char* stoken = g_strdup_printf("squint_session%d", getpid());
	GVariantBuilder opts;
	g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
	g_variant_builder_add(&opts, "{sv}", "session_handle_token", g_variant_new_string(stoken));
	portal_call("CreateSession", g_variant_new("(a{sv})", &opts));
	g_free(token);
	g_free(stoken);
	return TRUE;
}

int
wl_get_sources(GdkRectangle** rects)
{
	*rects = g_new0(GdkRectangle, n_sources ? n_sources : 1);
	for (int i = 0; i < n_sources; i++)
		(*rects)[i] = sources[i].rect;
	return n_sources;
}

static gboolean
on_window_draw(GtkWidget* widget, void* cr, gpointer data)
{
	// Expose/map (e.g. squint_show() remapping the fullscreen window): a
	// static screen sends no new frames, so redraw the held one ourselves.
	wl_schedule_render();
	return TRUE;  // never let GTK paint over the EGL surface
}

void
wl_enable(void)
{
	if (!egl_bind_window(egl_ctx, gdkwin)) {
		squint_error("EGL: could not bind the mirror window");
		return;
	}
	bound = TRUE;
	gtk_widget_set_app_paintable(gtkwin, TRUE);
	g_signal_connect(gtkwin, "draw", G_CALLBACK(on_window_draw), NULL);
	pending_switch = -1;
	if (active >= n_sources) active = 0;
	wl_schedule_render();
}

void
wl_disable(void)
{
	if (render_idle_id) {
		g_source_remove(render_idle_id);
		render_idle_id = 0;
	}
	bound = FALSE;
	egl_unbind_window(egl_ctx);
}

void
wl_switch_source(int idx)
{
	active = idx;
	wl_schedule_render();
}
