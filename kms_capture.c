// kms_capture.c — KMS source backend: read the compositor's scanout buffers
// straight off the source GPU's KMS planes (drmModeGetFB2 + PRIME), the way
// ReFrame / Sunshine-KMS do, and render them with EGL into the squint window
// on the (X11) destination display.
//
// Pipeline per frame (active output only):
//   1. vblank event on the active CRTC (DRM_VBLANK_EVENT on the card fd,
//      polled by the GLib loop) -- the new flip has just latched;
//   2. primary plane FB_ID changed (= the compositor flipped) or the HW cursor
//      plane moved/changed -> new frame;
//   3. source-GPU EGL (GBM platform, surfaceless): import the scanout FB with
//      its real modifier (tiled/DCC, multi-plane) and the cursor FB, draw both
//      into an intermediate buffer;
//   4. hand it to the destination GPU, by one of two transfers:
//      - dmabuf:   the intermediate is a LINEAR GBM buffer (double-buffered)
//                  that the destination imports once -- zero-copy, glFinish
//                  for sync. Needs the source driver to put shareable linear
//                  buffers in memory the other GPU can reach (amdgpu, i915,
//                  nouveau do; NVIDIA's proprietary driver generally doesn't).
//      - readback: the intermediate is a plain texture, glReadPixels into a
//                  PBO, CPU upload on the destination. Works between any two
//                  vendors, costs a frame-sized copy each way.
//      --kms-transfer=auto tries dmabuf and falls back to readback if any
//      step of it fails;
//   5. destination EGL: draw into the window and swap.
//
// Vendor-neutral by construction: no driver is special-cased, the source is
// any KMS device (auto: the one with lit outputs that isn't the destination's
// GPU) and the destination is whatever GPU runs the X server of the window.
//
// Needs CAP_SYS_ADMIN (or DRM master): without it GETFB2 returns no GEM
// handles. `sudo setcap cap_sys_admin+ep builddir/squint` is enough.
//
// Known limits: overlay planes (KWin direct scanout of video onto an overlay)
// are not composited, plane SRC/CRTC scaling and rotation are ignored, and a
// software cursor gives no cursor-follow (it is in the image though).
//
// Wayland gives no output layout to a KMS reader, so source rectangles are
// laid out left to right in selection order; only their sizes matter.

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <glib-unix.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <libdrm/drm_fourcc.h>

#include "squint.h"
#include "egl.h"

#define FB_CACHE_SIZE 8

// An imported KMS framebuffer on the source GPU. Keyed by the dma-buf inode:
// exporting the same GEM object always yields the same dma_buf, while FB ids
// can be recycled for different buffers after the compositor frees one.
typedef struct {
	ino_t ino;
	uint32_t w, h, format;
	uint64_t modifier;
	EGLImageKHR image;
	GLuint tex;
	guint64 last_used;
} fb_entry_t;

typedef struct {
	uint32_t crtc_id;
	int pipe;
	uint32_t primary_plane, cursor_plane;
	char name[32];          // connector driving it (empty = none)
	uint32_t mode_w, mode_h;
} kms_crtc_t;

typedef struct {
	int crtc;               // index into crtcs[]
	GdkRectangle rect;

	// intermediate buffers (source GPU renders, destination GPU samples)
	int out_w, out_h;
	int transfer;           // XFER_DMABUF / XFER_READBACK once allocated
	struct gbm_bo* bo[2];   // dmabuf: linear double buffer
	EGLImageKHR src_image[2];
	GLuint src_tex[2], src_fbo[2];  // readback uses only [0]
	GLuint pbo;             // readback: pack buffer
	GLuint dst_tex[2];      // readback uses only [0]
	int cur;

	uint32_t last_fb;
	gboolean drawn_cursor;
	int drawn_cx, drawn_cy;
	uint32_t drawn_cursor_fb;
} kms_out_t;

enum { XFER_AUTO, XFER_DMABUF, XFER_READBACK };
static const char* xfer_names[] = { "auto", "dmabuf", "readback" };
static int transfer_mode = XFER_AUTO;  // resolved to DMABUF/READBACK on first alloc

// device
static int card_fd = -1;
static int render_fd = -1;
static struct gbm_device* gbm = NULL;
static EGLDisplay src_dpy = EGL_NO_DISPLAY;
static EGLContext src_ctx = EGL_NO_CONTEXT;
static GLuint src_prog = 0;
static GLint src_pos_attr, src_tex_attr;
static uint32_t prop_crtc_x, prop_crtc_y, prop_crtc_w, prop_crtc_h;

static kms_crtc_t* crtcs = NULL;
static int n_crtcs = 0;
static kms_out_t* outs = NULL;
static int n_outs = 0;
static int active = 0;

static fb_entry_t fb_cache[FB_CACHE_SIZE];
static guint64 fb_clock = 0;

// destination
static egl_context_t* dst_ctx = NULL;
static gboolean bound = FALSE;
static gboolean force_render = FALSE;

// pacing
static gboolean vblank_armed = FALSE;
static guint fallback_timer = 0;
static guint fd_watch = 0;

// cursor follow
static int pending_switch = -1;
static int switch_entry_x, switch_entry_y;

// perf
static guint perf_renders = 0, perf_flips = 0, perf_ticks = 0;
static gdouble perf_lat_sum = 0, perf_lat_max = 0;
static gdouble perf_blit_sum = 0, perf_blit_max = 0;
static gdouble perf_dst_sum = 0, perf_dst_max = 0;
static gint64 perf_last_report = 0;

static void kms_fatal(const char* msg)
{
	// Exit non-zero so a supervising unit (Restart=on-failure) retries.
	fprintf(stderr, "kms: fatal: %s\n", msg);
	exit(1);
}

static gint64 now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (gint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

//
// Source-GPU EGL
//

static gboolean
src_make_current(void)
{
	return eglMakeCurrent(src_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, src_ctx);
}

static GLuint
compile(GLenum type, const char* src)
{
	GLuint sh = glCreateShader(type);
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	GLint ok;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		glGetShaderInfoLog(sh, sizeof(log), NULL, log);
		fprintf(stderr, "kms: shader: %s\n", log);
		return 0;
	}
	return sh;
}

static void
src_egl_cleanup(void)
{
	if (src_ctx != EGL_NO_CONTEXT) {
		eglMakeCurrent(src_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroyContext(src_dpy, src_ctx);
	}
	if (src_dpy != EGL_NO_DISPLAY) eglTerminate(src_dpy);
	if (gbm) gbm_device_destroy(gbm);
	src_ctx = EGL_NO_CONTEXT;
	src_dpy = EGL_NO_DISPLAY;
	gbm = NULL;
	src_prog = 0;
}

static gboolean
src_egl_init(void)
{
	gbm = gbm_create_device(render_fd);
	if (!gbm) { fprintf(stderr, "kms: gbm_create_device failed\n"); return FALSE; }

	src_dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	EGLint maj, min;
	if (src_dpy == EGL_NO_DISPLAY || !eglInitialize(src_dpy, &maj, &min)) {
		fprintf(stderr, "kms: EGL on the source GPU failed\n");
		return FALSE;
	}
	if (!epoxy_has_egl_extension(src_dpy, "EGL_EXT_image_dma_buf_import_modifiers") ||
	    !epoxy_has_egl_extension(src_dpy, "EGL_KHR_surfaceless_context") ||
	    !epoxy_has_egl_extension(src_dpy, "EGL_KHR_no_config_context")) {
		fprintf(stderr, "kms: source EGL lacks dma_buf_import_modifiers/surfaceless/no_config\n");
		return FALSE;
	}
	eglBindAPI(EGL_OPENGL_API);
	EGLint attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
	src_ctx = eglCreateContext(src_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
	if (src_ctx == EGL_NO_CONTEXT || !src_make_current()) {
		fprintf(stderr, "kms: source EGL context failed (0x%x)\n", eglGetError());
		return FALSE;
	}
	char* node = drmGetDeviceNameFromFd2(render_fd);
	fprintf(stderr, "kms: source EGL %d.%d on %s (%s)\n", maj, min,
			node ? node : "?", glGetString(GL_RENDERER));
	free(node);

	// Texture rows and framebuffer rows are both "memory row 0 first", so an
	// identity mapping copies the image without any flip.
	const char* vs =
		"#version 120\n"
		"attribute vec2 position;\n"
		"attribute vec2 texcoord;\n"
		"varying vec2 uv;\n"
		"void main() { gl_Position = vec4(position, 0.0, 1.0); uv = texcoord; }\n";
	const char* fs =
		"#version 120\n"
		"varying vec2 uv;\n"
		"uniform sampler2D tex;\n"
		"void main() { gl_FragColor = texture2D(tex, uv); }\n";
	GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
	if (!v || !f) return FALSE;
	src_prog = glCreateProgram();
	glAttachShader(src_prog, v);
	glAttachShader(src_prog, f);
	glLinkProgram(src_prog);
	glDeleteShader(v);
	glDeleteShader(f);
	GLint ok;
	glGetProgramiv(src_prog, GL_LINK_STATUS, &ok);
	if (!ok) return FALSE;
	src_pos_attr = glGetAttribLocation(src_prog, "position");
	src_tex_attr = glGetAttribLocation(src_prog, "texcoord");
	return TRUE;
}

static EGLImageKHR
src_import(int n_planes, const int* fds, const uint32_t* pitches, const uint32_t* offsets,
		uint32_t w, uint32_t h, uint32_t format, uint64_t modifier)
{
	static const EGLint plane_attr[4][5] = {
		{ EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
		  EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
		{ EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
		  EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
		{ EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
		  EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
		{ EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
		  EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT },
	};
	EGLint a[64];
	int n = 0;
	a[n++] = EGL_WIDTH;  a[n++] = w;
	a[n++] = EGL_HEIGHT; a[n++] = h;
	a[n++] = EGL_LINUX_DRM_FOURCC_EXT; a[n++] = format;
	for (int i = 0; i < n_planes; i++) {
		a[n++] = plane_attr[i][0]; a[n++] = fds[i];
		a[n++] = plane_attr[i][1]; a[n++] = offsets[i];
		a[n++] = plane_attr[i][2]; a[n++] = pitches[i];
		if (modifier != DRM_FORMAT_MOD_INVALID) {
			a[n++] = plane_attr[i][3]; a[n++] = (EGLint)(modifier & 0xffffffff);
			a[n++] = plane_attr[i][4]; a[n++] = (EGLint)(modifier >> 32);
		}
	}
	a[n++] = EGL_NONE;
	EGLImageKHR img = eglCreateImageKHR(src_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
	if (img == EGL_NO_IMAGE_KHR)
		fprintf(stderr, "kms: import %ux%u fourcc %.4s mod 0x%llx (%d planes) failed: 0x%x\n",
				w, h, (char*)&format, (unsigned long long)modifier, n_planes, eglGetError());
	return img;
}

static GLuint
tex_from_image(EGLImageKHR img)
{
	GLuint t;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	return t;
}

// Returns the cached source-GPU texture for a KMS framebuffer, importing it
// on first sight. Source context must be current.
static fb_entry_t*
fb_lookup(uint32_t fb_id)
{
	drmModeFB2* fb = drmModeGetFB2(card_fd, fb_id);
	if (!fb) return NULL;
	if (!fb->handles[0]) {
		drmModeFreeFB2(fb);
		kms_fatal("GETFB2 returned no buffer handles: squint needs CAP_SYS_ADMIN for "
				"--kms-source (sudo setcap cap_sys_admin+ep <path to squint>)");
	}

	// One dma-buf fd per distinct GEM handle (DCC planes share the BO).
	int fds[4] = { -1, -1, -1, -1 };
	int n_planes = 0;
	for (int i = 0; i < 4 && fb->handles[i]; i++) {
		n_planes = i + 1;
		for (int j = 0; j < i; j++)
			if (fb->handles[j] == fb->handles[i]) { fds[i] = dup(fds[j]); break; }
		if (fds[i] < 0 && drmPrimeHandleToFD(card_fd, fb->handles[i], DRM_CLOEXEC, &fds[i]) < 0)
			fds[i] = -1;
	}
	// GETFB2 created fresh GEM handles in our file; drop them (the dma-buf
	// fds / EGLImage keep the BO alive).
	for (int i = 0; i < n_planes; i++) {
		gboolean seen = FALSE;
		for (int j = 0; j < i; j++) seen |= fb->handles[j] == fb->handles[i];
		if (!seen) drmCloseBufferHandle(card_fd, fb->handles[i]);
	}

	fb_entry_t* e = NULL;
	struct stat st;
	if (fds[0] >= 0 && fstat(fds[0], &st) == 0) {
		for (int i = 0; i < FB_CACHE_SIZE; i++)
			if (fb_cache[i].image && fb_cache[i].ino == st.st_ino &&
			    fb_cache[i].w == fb->width && fb_cache[i].h == fb->height &&
			    fb_cache[i].format == fb->pixel_format) {
				e = &fb_cache[i];
				break;
			}
		if (!e) {
			uint64_t mod = (fb->flags & DRM_MODE_FB_MODIFIERS) ? fb->modifier : DRM_FORMAT_MOD_INVALID;
			EGLImageKHR img = src_import(n_planes, fds, fb->pitches, fb->offsets,
					fb->width, fb->height, fb->pixel_format, mod);
			if (img != EGL_NO_IMAGE_KHR) {
				// evict LRU
				fb_entry_t* victim = &fb_cache[0];
				for (int i = 0; i < FB_CACHE_SIZE; i++) {
					if (!fb_cache[i].image) { victim = &fb_cache[i]; break; }
					if (fb_cache[i].last_used < victim->last_used) victim = &fb_cache[i];
				}
				if (victim->image) {
					glDeleteTextures(1, &victim->tex);
					eglDestroyImageKHR(src_dpy, victim->image);
				}
				*victim = (fb_entry_t){ st.st_ino, fb->width, fb->height, fb->pixel_format,
				                        mod, img, tex_from_image(img), 0 };
				e = victim;
				fprintf(stderr, "kms: imported fb %u: %ux%u %.4s mod 0x%llx, %d plane(s)\n",
						fb_id, fb->width, fb->height, (char*)&fb->pixel_format,
						(unsigned long long)mod, n_planes);
			}
		}
	}
	for (int i = 0; i < 4; i++)
		if (fds[i] >= 0) close(fds[i]);
	drmModeFreeFB2(fb);
	if (e) e->last_used = ++fb_clock;
	return e;
}

static void
src_draw(GLuint tex, float x0, float y0, float x1, float y1)
{
	// x/y in clip space, row-0-first (see src_egl_init)
	const float v[] = {
		x0, y0, 0, 0,
		x1, y0, 1, 0,
		x1, y1, 1, 1,
		x0, y1, 0, 1,
	};
	glBindTexture(GL_TEXTURE_2D, tex);
	glVertexAttribPointer(src_pos_attr, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), v);
	glVertexAttribPointer(src_tex_attr, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), v + 2);
	glEnableVertexAttribArray(src_pos_attr);
	glEnableVertexAttribArray(src_tex_attr);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

//
// Linear output buffers
//

static void
out_free_buffers(kms_out_t* o)
{
	for (int i = 0; i < 2; i++) {
		if (o->dst_tex[i] && dst_ctx) egl_delete_texture(dst_ctx, o->dst_tex[i]);
		o->dst_tex[i] = 0;
	}
	src_make_current();
	for (int i = 0; i < 2; i++) {
		if (o->src_fbo[i]) glDeleteFramebuffers(1, &o->src_fbo[i]);
		if (o->src_tex[i]) glDeleteTextures(1, &o->src_tex[i]);
		if (o->src_image[i]) eglDestroyImageKHR(src_dpy, o->src_image[i]);
		if (o->bo[i]) gbm_bo_destroy(o->bo[i]);
		o->src_fbo[i] = o->src_tex[i] = 0;
		o->src_image[i] = NULL;
		o->bo[i] = NULL;
	}
	if (o->pbo) glDeleteBuffers(1, &o->pbo);
	o->pbo = 0;
	o->out_w = o->out_h = 0;
	o->transfer = 0;
	o->cur = 0;
}

static gboolean
fbo_for(GLuint tex, GLuint* fbo)
{
	glGenFramebuffers(1, fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	gboolean ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return ok;
}

// readback: plain RGBA texture + FBO on the source, PBO for glReadPixels, a
// CPU-upload texture on the destination. Source context current on entry.
static gboolean
alloc_readback(kms_out_t* o, int w, int h)
{
	glGenTextures(1, &o->src_tex[0]);
	glBindTexture(GL_TEXTURE_2D, o->src_tex[0]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	if (!fbo_for(o->src_tex[0], &o->src_fbo[0])) return FALSE;
	glGenBuffers(1, &o->pbo);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, o->pbo);
	glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 4, NULL, GL_STREAM_READ);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	egl_make_current(dst_ctx);
	o->dst_tex[0] = egl_create_texture(dst_ctx);
	return o->dst_tex[0] != 0;
}

// dmabuf: linear GBM double buffer imported on both GPUs. Source context
// current on entry. Any failure -> FALSE (caller cleans up / falls back).
static gboolean
alloc_dmabuf(kms_out_t* o, int w, int h)
{
	int fds[2] = { -1, -1 };
	uint32_t strides[2];
	gboolean ok = TRUE;
	for (int i = 0; i < 2 && ok; i++) {
		o->bo[i] = gbm_bo_create(gbm, w, h, GBM_FORMAT_XRGB8888,
				GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
		if (!o->bo[i]) { fprintf(stderr, "kms: linear gbm_bo_create failed\n"); ok = FALSE; break; }
		fds[i] = gbm_bo_get_fd(o->bo[i]);
		strides[i] = gbm_bo_get_stride(o->bo[i]);
		uint32_t off = 0;
		o->src_image[i] = src_import(1, &fds[i], &strides[i], &off, w, h,
				DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR);
		if (!o->src_image[i]) { ok = FALSE; break; }
		o->src_tex[i] = tex_from_image(o->src_image[i]);
		if (!fbo_for(o->src_tex[i], &o->src_fbo[i])) {
			fprintf(stderr, "kms: linear FBO incomplete\n");
			ok = FALSE;
		}
	}
	if (ok) {
		egl_make_current(dst_ctx);
		for (int i = 0; i < 2 && ok; i++) {
			o->dst_tex[i] = egl_create_image_from_fd(dst_ctx, fds[i], w, h, strides[i],
					DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR);
			if (!o->dst_tex[i]) {
				fprintf(stderr, "kms: the destination GPU could not import the linear buffer\n");
				ok = FALSE;
			}
		}
	}
	for (int i = 0; i < 2; i++)
		if (fds[i] >= 0) close(fds[i]);
	return ok;
}

// (Re)allocate the intermediates for a w x h source, picking the transfer.
// Source context current on entry; destination context current on return.
static gboolean
out_alloc_buffers(kms_out_t* o, int w, int h)
{
	out_free_buffers(o);
	src_make_current();
	if (transfer_mode != XFER_READBACK) {
		if (alloc_dmabuf(o, w, h)) {
			o->transfer = XFER_DMABUF;
		} else if (transfer_mode == XFER_DMABUF) {
			kms_fatal("--kms-transfer=dmabuf: zero-copy sharing between these GPUs failed");
		} else {
			fprintf(stderr, "kms: zero-copy sharing failed, falling back to readback\n");
			out_free_buffers(o);
			transfer_mode = XFER_READBACK;  // don't retry for other outputs
			src_make_current();
		}
	}
	if (!o->transfer) {
		if (!alloc_readback(o, w, h)) return FALSE;
		o->transfer = XFER_READBACK;
	}
	if (transfer_mode == XFER_AUTO) transfer_mode = o->transfer;
	o->out_w = w;
	o->out_h = h;
	fprintf(stderr, "kms: %s: %dx%d via %s\n", crtcs[o->crtc].name, w, h,
			o->transfer == XFER_DMABUF ? "linear dma-buf (zero-copy)" : "readback (CPU copy)");
	return TRUE;
}

//
// Plane state
//

typedef struct {
	uint32_t fb;
	gboolean on;      // cursor plane only: enabled on its CRTC
	int x, y, w, h;   // cursor plane only
} plane_state_t;

static void
read_plane(uint32_t plane_id, uint32_t crtc_id, gboolean want_geometry, plane_state_t* ps)
{
	memset(ps, 0, sizeof(*ps));
	if (!plane_id) return;
	drmModePlane* p = drmModeGetPlane(card_fd, plane_id);
	if (!p) return;
	ps->fb = p->fb_id;
	ps->on = p->fb_id && p->crtc_id == crtc_id;
	drmModeFreePlane(p);
	if (!ps->on || !want_geometry) return;

	drmModeObjectProperties* props = drmModeObjectGetProperties(card_fd, plane_id, DRM_MODE_OBJECT_PLANE);
	if (!props) return;
	for (uint32_t i = 0; i < props->count_props; i++) {
		int64_t v = (int64_t)props->prop_values[i];  // CRTC_X/Y are signed
		if (props->props[i] == prop_crtc_x) ps->x = (int)v;
		else if (props->props[i] == prop_crtc_y) ps->y = (int)v;
		else if (props->props[i] == prop_crtc_w) ps->w = (int)v;
		else if (props->props[i] == prop_crtc_h) ps->h = (int)v;
	}
	drmModeFreeObjectProperties(props);
}

//
// Render
//

static void
perf_report(void)
{
	gint64 now = g_get_monotonic_time();
	if (!perf_last_report) perf_last_report = now;
	if (now - perf_last_report < 2 * G_USEC_PER_SEC) return;
	gdouble secs = (now - perf_last_report) / (gdouble)G_USEC_PER_SEC;
	gdouble n = perf_renders ? perf_renders : 1;
	fprintf(stderr, "perf[kms]: %.1f vblanks/s  %.1f flips/s  %.1f renders/s  "
			"vblank->swap avg %.2fms max %.2fms  src blit+xfer avg %.2fms max %.2fms  "
			"dst draw+swap avg %.2fms max %.2fms\n",
			perf_ticks / secs, perf_flips / secs, perf_renders / secs,
			perf_lat_sum / n / 1000, perf_lat_max / 1000,
			perf_blit_sum / n / 1000, perf_blit_max / 1000,
			perf_dst_sum / n / 1000, perf_dst_max / 1000);
	perf_renders = perf_flips = perf_ticks = 0;
	perf_lat_sum = perf_lat_max = perf_blit_sum = perf_blit_max = perf_dst_sum = perf_dst_max = 0;
	perf_last_report = now;
}

static void
render(kms_out_t* o, uint32_t fb_id, const plane_state_t* cur, gint64 vblank_us)
{
	gint64 t0 = now_us();
	src_make_current();

	fb_entry_t* fb = fb_lookup(fb_id);
	if (!fb) return;
	if (o->out_w != (int)fb->w || o->out_h != (int)fb->h) {
		if (!out_alloc_buffers(o, fb->w, fb->h)) kms_fatal("could not set up the linear buffers");
		src_make_current();
	}
	fb_entry_t* cfb = cur->on ? fb_lookup(cur->fb) : NULL;

	if (o->transfer == XFER_DMABUF) o->cur ^= 1;  // readback has one buffer
	glBindFramebuffer(GL_FRAMEBUFFER, o->src_fbo[o->cur]);
	glViewport(0, 0, o->out_w, o->out_h);
	glUseProgram(src_prog);
	glUniform1i(glGetUniformLocation(src_prog, "tex"), 0);
	glDisable(GL_BLEND);
	src_draw(fb->tex, -1, -1, 1, 1);
	if (cfb) {
		int cw = cur->w ? cur->w : (int)cfb->w, ch = cur->h ? cur->h : (int)cfb->h;
		float W = o->out_w, H = o->out_h;
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // KMS cursors are premultiplied
		src_draw(cfb->tex, cur->x / W * 2 - 1, cur->y / H * 2 - 1,
				(cur->x + cw) / W * 2 - 1, (cur->y + ch) / H * 2 - 1);
		glDisable(GL_BLEND);
	}
	const void* pixels = NULL;
	if (o->transfer == XFER_DMABUF) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		// The destination GPU samples this buffer next; don't rely on
		// cross-driver implicit sync.
		glFinish();
	} else {
		// Synchronous: mapping waits for the read (lowest latency; a second
		// PBO would overlap the copy with the next frame at +1 frame lag).
		glBindBuffer(GL_PIXEL_PACK_BUFFER, o->pbo);
		glReadPixels(0, 0, o->out_w, o->out_h, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
		pixels = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)o->out_w * o->out_h * 4,
				GL_MAP_READ_BIT);
		if (!pixels) {
			fprintf(stderr, "kms: readback map failed\n");
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	gint64 t1 = now_us();

	egl_make_current(dst_ctx);
	if (pixels) {
		egl_upload_texture(dst_ctx, o->dst_tex[0], o->out_w, o->out_h, o->out_w * 4,
				GL_BGRA, pixels);
		src_make_current();
		glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		egl_make_current(dst_ctx);
	}
	egl_draw_quad(dst_ctx, o->dst_tex[o->cur]);
	egl_swap_buffers(dst_ctx);
	gint64 t2 = now_us();

	o->last_fb = fb_id;
	o->drawn_cursor = cfb != NULL;
	o->drawn_cx = cur->x;
	o->drawn_cy = cur->y;
	o->drawn_cursor_fb = cur->fb;

	if (config.opt_perf_log) {
		gdouble lat = vblank_us ? t2 - vblank_us : 0;
		perf_renders++;
		perf_lat_sum += lat;   if (lat > perf_lat_max) perf_lat_max = lat;
		perf_blit_sum += t1 - t0; if (t1 - t0 > perf_blit_max) perf_blit_max = t1 - t0;
		perf_dst_sum += t2 - t1;  if (t2 - t1 > perf_dst_max) perf_dst_max = t2 - t1;
	}
}

//
// Cursor follow + per-vblank tick
//

static int
out_for_crtc(int crtc)
{
	for (int i = 0; i < n_outs; i++)
		if (outs[i].crtc == crtc) return i;
	return -1;
}

static void
follow_cursor(int idx, int x, int y)
{
	if (idx == active) {
		pending_switch = -1;
		squint_show();
		return;
	}
	int threshold = config.opt_switch_threshold;
	if (threshold <= 0) {
		pending_switch = -1;
		squint_switch_source(idx);
	} else if (pending_switch != idx) {
		pending_switch = idx;
		switch_entry_x = x;
		switch_entry_y = y;
	} else if (abs(x - switch_entry_x) >= threshold || abs(y - switch_entry_y) >= threshold) {
		pending_switch = -1;
		squint_switch_source(idx);
	}
}

static void
tick(gint64 vblank_us)
{
	if (!enabled || !bound || n_outs == 0) return;
	perf_ticks++;

	// Which CRTC shows the HW cursor? KWin disables the cursor plane on outputs
	// the pointer isn't on. None at all = hidden pointer or software cursor:
	// keep the current state.
	// The active output wins when the cursor straddles an edge (both planes on).
	plane_state_t active_cursor = { 0 }, other_cursor = { 0 }, ps;
	int other_crtc = -1;
	for (int c = 0; c < n_crtcs; c++) {
		if (!crtcs[c].name[0]) continue;
		int oi = out_for_crtc(c);
		read_plane(crtcs[c].cursor_plane, crtcs[c].crtc_id, oi >= 0, &ps);
		if (!ps.on) continue;
		if (oi == active) active_cursor = ps;
		else if (other_crtc < 0) { other_crtc = c; other_cursor = ps; }
	}
	if (active_cursor.on) {
		follow_cursor(active, active_cursor.x, active_cursor.y);
	} else if (other_crtc >= 0) {
		int oi = out_for_crtc(other_crtc);
		if (oi >= 0) {
			follow_cursor(oi, other_cursor.x, other_cursor.y);
		} else {
			// pointer is on an output we don't mirror
			pending_switch = -1;
			if (!config.opt_passive) squint_hide();
		}
	}
	if (!enabled || !bound) return;  // follow_cursor may have switched/disabled

	kms_out_t* o = &outs[active];
	kms_crtc_t* kc = &crtcs[o->crtc];
	plane_state_t prim;
	read_plane(kc->primary_plane, kc->crtc_id, FALSE, &prim);
	if (!prim.fb) return;  // output off

	gboolean flipped = prim.fb != o->last_fb;
	if (flipped) perf_flips++;
	gboolean cursor_changed = active_cursor.on != o->drawn_cursor ||
		(active_cursor.on && (active_cursor.x != o->drawn_cx || active_cursor.y != o->drawn_cy ||
		                      active_cursor.fb != o->drawn_cursor_fb));
	if (flipped || cursor_changed || force_render) {
		force_render = FALSE;
		render(o, prim.fb, &active_cursor, vblank_us);
	}
	if (config.opt_perf_log) perf_report();
}

static gboolean arm_vblank(void);

static gboolean
fallback_cb(gpointer data)
{
	tick(0);
	if (arm_vblank()) {
		fallback_timer = 0;
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static gboolean
arm_vblank(void)
{
	if (vblank_armed) return TRUE;
	if (!enabled || n_outs == 0) return FALSE;
	int pipe = crtcs[outs[active].crtc].pipe;
	drmVBlank vbl;
	memset(&vbl, 0, sizeof(vbl));
	vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
	if (pipe == 1) vbl.request.type |= DRM_VBLANK_SECONDARY;
	else if (pipe > 1) vbl.request.type |= (pipe << DRM_VBLANK_HIGH_CRTC_SHIFT) & DRM_VBLANK_HIGH_CRTC_MASK;
	vbl.request.sequence = 1;
	if (drmWaitVBlank(card_fd, &vbl) == 0) {
		vblank_armed = TRUE;
		return TRUE;
	}
	// CRTC off / DPMS: poll at ~60 Hz until it comes back.
	if (!fallback_timer)
		fallback_timer = g_timeout_add(16, fallback_cb, NULL);
	return FALSE;
}

static void
on_vblank(int fd, unsigned int seq, unsigned int sec, unsigned int usec, void* data)
{
	vblank_armed = FALSE;
	tick((gint64)sec * 1000000 + usec);  // DRM vblank timestamps are CLOCK_MONOTONIC
	arm_vblank();
}

static gboolean
card_fd_cb(gint fd, GIOCondition cond, gpointer data)
{
	drmEventContext ev = { .version = 2, .vblank_handler = on_vblank };
	drmHandleEvent(card_fd, &ev);
	return G_SOURCE_CONTINUE;
}

//
// Enumeration
//

static uint32_t
find_prop(uint32_t obj, uint32_t type, const char* name, uint64_t* value)
{
	drmModeObjectProperties* props = drmModeObjectGetProperties(card_fd, obj, type);
	uint32_t id = 0;
	if (!props) return 0;
	for (uint32_t i = 0; i < props->count_props && !id; i++) {
		drmModePropertyRes* p = drmModeGetProperty(card_fd, props->props[i]);
		if (p && !strcmp(p->name, name)) {
			id = p->prop_id;
			if (value) *value = props->prop_values[i];
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return id;
}

static void
free_outputs(void)
{
	for (int i = 0; i < n_outs; i++) out_free_buffers(&outs[i]);
	g_free(outs);
	outs = NULL;
	n_outs = 0;
	g_free(crtcs);
	crtcs = NULL;
	n_crtcs = 0;
}

static gboolean
enumerate(void)
{
	drmModeRes* res = drmModeGetResources(card_fd);
	drmModePlaneRes* pres = drmModeGetPlaneResources(card_fd);
	if (!res || !pres) {
		fprintf(stderr, "kms: %s is not a KMS device\n", config.kms_device);
		return FALSE;
	}

	n_crtcs = res->count_crtcs;
	crtcs = g_new0(kms_crtc_t, n_crtcs);
	for (int c = 0; c < n_crtcs; c++) {
		crtcs[c].crtc_id = res->crtcs[c];
		crtcs[c].pipe = c;
		drmModeCrtc* crtc = drmModeGetCrtc(card_fd, res->crtcs[c]);
		if (crtc && crtc->mode_valid) {
			crtcs[c].mode_w = crtc->mode.hdisplay;
			crtcs[c].mode_h = crtc->mode.vdisplay;
		}
		drmModeFreeCrtc(crtc);
	}

	// Primary / cursor plane per CRTC (prefer planes dedicated to that CRTC).
	for (uint32_t i = 0; i < pres->count_planes; i++) {
		drmModePlane* p = drmModeGetPlane(card_fd, pres->planes[i]);
		if (!p) continue;
		uint64_t type = 0;
		find_prop(p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
		for (int c = 0; c < n_crtcs; c++) {
			if (!(p->possible_crtcs & (1u << c))) continue;
			gboolean dedicated = p->possible_crtcs == (1u << c);
			uint32_t* slot = type == DRM_PLANE_TYPE_PRIMARY ? &crtcs[c].primary_plane
			               : type == DRM_PLANE_TYPE_CURSOR  ? &crtcs[c].cursor_plane : NULL;
			if (slot && (!*slot || dedicated)) *slot = p->plane_id;
		}
		if (!prop_crtc_x) {
			prop_crtc_x = find_prop(p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", NULL);
			prop_crtc_y = find_prop(p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", NULL);
			prop_crtc_w = find_prop(p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", NULL);
			prop_crtc_h = find_prop(p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", NULL);
		}
		drmModeFreePlane(p);
	}

	// Lit connectors -> CRTC names
	int n_lit = 0;
	int lit[32];
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector* conn = drmModeGetConnector(card_fd, res->connectors[i]);
		if (!conn) continue;
		if (conn->connection == DRM_MODE_CONNECTED && conn->encoder_id) {
			drmModeEncoder* enc = drmModeGetEncoder(card_fd, conn->encoder_id);
			if (enc) {
				for (int c = 0; c < n_crtcs; c++) {
					if (crtcs[c].crtc_id == enc->crtc_id && crtcs[c].mode_w && !crtcs[c].name[0]) {
						const char* tn = drmModeGetConnectorTypeName(conn->connector_type);
						g_snprintf(crtcs[c].name, sizeof(crtcs[c].name), "%s-%u",
								tn ? tn : "Unknown", conn->connector_type_id);
						if (n_lit < (int)G_N_ELEMENTS(lit)) lit[n_lit++] = c;
					}
				}
				drmModeFreeEncoder(enc);
			}
		}
		drmModeFreeConnector(conn);
	}
	drmModeFreePlaneResources(pres);
	drmModeFreeResources(res);

	for (int i = 0; i < n_lit; i++) {
		kms_crtc_t* c = &crtcs[lit[i]];
		fprintf(stderr, "kms: output %d: %s %ux%u (crtc %u pipe %d, primary plane %u, cursor plane %u)\n",
				i + 1, c->name, c->mode_w, c->mode_h, c->crtc_id, c->pipe,
				c->primary_plane, c->cursor_plane);
	}

	// Select sources: names/numbers from the source-monitor argument, else all.
	outs = g_new0(kms_out_t, n_lit ? n_lit : 1);
	if (config.src_monitor_name) {
		gchar** names = g_strsplit(config.src_monitor_name, ",", -1);
		for (int k = 0; names[k]; k++) {
			const char* nm = g_strstrip(names[k]);
			char* end;
			long num = strtol(nm, &end, 10);
			int found = -1;
			for (int i = 0; i < n_lit; i++)
				if ((*end == 0 && num == i + 1) || !g_ascii_strcasecmp(nm, crtcs[lit[i]].name))
					found = lit[i];
			if (found < 0) {
				fprintf(stderr, "kms: no lit output named '%s'\n", nm);
				g_strfreev(names);
				return FALSE;
			}
			if (out_for_crtc(found) < 0) outs[n_outs++].crtc = found;
		}
		g_strfreev(names);
	} else {
		for (int i = 0; i < n_lit; i++) outs[n_outs++].crtc = lit[i];
	}

	int x = 0;
	for (int i = 0; i < n_outs; i++) {
		kms_crtc_t* c = &crtcs[outs[i].crtc];
		if (!c->primary_plane) {
			fprintf(stderr, "kms: %s has no primary plane\n", c->name);
			return FALSE;
		}
		outs[i].rect = (GdkRectangle){ x, 0, c->mode_w, c->mode_h };
		x += c->mode_w;
		fprintf(stderr, "kms: source %d: %s\n", i + 1, c->name);
	}
	return n_outs > 0;
}

//
// Backend API
//

static gboolean
on_window_draw(GtkWidget* widget, void* cr, gpointer data)
{
	force_render = TRUE;  // next vblank redraws (a static screen never flips)
	return TRUE;
}

// A card with at least one lit output (connector connected to an active CRTC).
static gboolean
card_has_lit_output(int fd)
{
	drmModeRes* res = drmModeGetResources(fd);
	if (!res) return FALSE;
	gboolean lit = FALSE;
	for (int i = 0; i < res->count_connectors && !lit; i++) {
		drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
		if (c && c->connection == DRM_MODE_CONNECTED && c->encoder_id) {
			drmModeEncoder* e = drmModeGetEncoder(fd, c->encoder_id);
			if (e && e->crtc_id) {
				drmModeCrtc* crtc = drmModeGetCrtc(fd, e->crtc_id);
				lit = crtc && crtc->mode_valid;
				drmModeFreeCrtc(crtc);
			}
			drmModeFreeEncoder(e);
		}
		drmModeFreeConnector(c);
	}
	drmModeFreeResources(res);
	return lit;
}

static gboolean
same_device(const char* a, const char* b)
{
	struct stat sa, sb;
	return a && b && stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_rdev == sb.st_rdev;
}

// Default source: a KMS card with lit outputs that isn't the destination's
// GPU (the destination GPU drives the mirror monitor, so it's lit too).
static char*
pick_device(const char* dst_dev)
{
	drmDevicePtr devs[16];
	int n = drmGetDevices2(0, devs, G_N_ELEMENTS(devs));
	char* pick = NULL;
	char* same = NULL;
	for (int i = 0; i < n; i++) {
		if (!(devs[i]->available_nodes & (1 << DRM_NODE_PRIMARY))) continue;
		const char* node = devs[i]->nodes[DRM_NODE_PRIMARY];
		int fd = open(node, O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		gboolean lit = card_has_lit_output(fd);
		close(fd);
		if (!lit) continue;
		if (same_device(node, dst_dev)) { if (!same) same = g_strdup(node); }
		else if (!pick) pick = g_strdup(node);
	}
	drmFreeDevices(devs, n > 0 ? n : 0);
	if (!pick && same) {
		fprintf(stderr, "kms: only the destination GPU has lit outputs; capturing it\n");
		pick = same;
		same = NULL;
	}
	g_free(same);
	return pick;
}

static gboolean
kms_init(void)
{
	if (config.kms_transfer) {
		for (int i = 0; i < (int)G_N_ELEMENTS(xfer_names); i++)
			if (!strcmp(config.kms_transfer, xfer_names[i])) transfer_mode = i;
		if (strcmp(config.kms_transfer, xfer_names[transfer_mode])) {
			squint_error("--kms-transfer must be auto, dmabuf or readback");
			return FALSE;
		}
	}

	dst_ctx = egl_init(gdisplay);
	if (!dst_ctx) return FALSE;
	char* dst_dev = egl_get_drm_device(dst_ctx);
	fprintf(stderr, "kms: destination GPU: %s\n", dst_dev ? dst_dev : "(unknown)");

	if (!config.kms_device) {
		config.kms_device = pick_device(dst_dev);
		if (!config.kms_device) {
			g_free(dst_dev);
			squint_error("No KMS device with lit outputs found (use --kms-device)");
			return FALSE;
		}
	}
	if (same_device(config.kms_device, dst_dev))
		fprintf(stderr, "kms: note: source and destination are the same GPU\n");
	g_free(dst_dev);

	card_fd = open(config.kms_device, O_RDWR | O_CLOEXEC);
	if (card_fd < 0) {
		fprintf(stderr, "kms: open %s: %s\n", config.kms_device, strerror(errno));
		squint_error("Cannot open the KMS device");
		return FALSE;
	}
	fprintf(stderr, "kms: source device %s\n", config.kms_device);
	drmSetClientCap(card_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

	// GBM/EGL on the render node; some drivers (NVIDIA's GBM backend, older
	// versions) only accept the primary node, so retry on the card fd.
	char* rnode = drmGetRenderDeviceNameFromFd(card_fd);
	render_fd = rnode ? open(rnode, O_RDWR | O_CLOEXEC) : -1;
	free(rnode);
	if (render_fd < 0 || !src_egl_init()) {
		fprintf(stderr, "kms: retrying source EGL on the primary node\n");
		src_egl_cleanup();
		if (render_fd >= 0) close(render_fd);
		render_fd = dup(card_fd);
		if (!src_egl_init()) {
			squint_error("EGL on the source GPU failed");
			return FALSE;
		}
	}

	fd_watch = g_unix_fd_add(card_fd, G_IO_IN, card_fd_cb, NULL);
	x11_watch_screen_changes();
	return TRUE;
}

static int
kms_get_sources(GdkRectangle** rects)
{
	// Re-enumerate on every enable: outputs may have changed while disabled.
	free_outputs();
	for (int i = 0; i < FB_CACHE_SIZE; i++) {
		if (!fb_cache[i].image) continue;
		src_make_current();
		glDeleteTextures(1, &fb_cache[i].tex);
		eglDestroyImageKHR(src_dpy, fb_cache[i].image);
		fb_cache[i].image = NULL;
	}
	if (!enumerate()) n_outs = 0;
	if (active >= n_outs) active = 0;

	*rects = g_new0(GdkRectangle, n_outs ? n_outs : 1);
	for (int i = 0; i < n_outs; i++) (*rects)[i] = outs[i].rect;
	return n_outs;
}

static gboolean
arm_idle_cb(gpointer data)
{
	arm_vblank();
	return G_SOURCE_REMOVE;
}

static void
kms_enable(void)
{
	if (!egl_bind_window(dst_ctx, gdkwin)) {
		squint_error("EGL: could not bind the mirror window");
		return;
	}
	bound = TRUE;
	force_render = TRUE;
	pending_switch = -1;
	gtk_widget_set_app_paintable(gtkwin, TRUE);
	g_signal_connect(gtkwin, "draw", G_CALLBACK(on_window_draw), NULL);
	// squint_enable() sets `enabled` after this returns; arm on the next turn.
	g_idle_add(arm_idle_cb, NULL);
}

static void
kms_disable(void)
{
	if (fallback_timer) {
		g_source_remove(fallback_timer);
		fallback_timer = 0;
	}
	// an armed vblank event just fires once more and is ignored by tick()
	bound = FALSE;
	egl_unbind_window(dst_ctx);
}

static void
kms_switch_source(int idx)
{
	active = idx;
	outs[idx].last_fb = 0;  // force a fresh frame
	force_render = TRUE;
	// The armed event is for the old CRTC; the next one is armed on the new one.
}

const capture_backend_t kms_backend = {
	.name = "kms",
	.init = kms_init,
	.get_sources = kms_get_sources,
	.enable = kms_enable,
	.disable = kms_disable,
	.switch_source = kms_switch_source,
	.self_enable = FALSE,
};
