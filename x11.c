#include "config.h"

#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>

#include <gdk/gdkx.h>

#include "squint.h"

#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/sync.h>
#include <libdrm/drm_fourcc.h>
#include "egl.h"
#ifdef HAVE_XI
#include <X11/extensions/XInput2.h>
#endif
#ifdef COPY_CURSOR
#include <X11/extensions/Xfixes.h>
#endif
#ifdef HAVE_XRENDER
#include <X11/extensions/Xrender.h>
#endif
#ifdef HAVE_XDAMAGE
#include <X11/extensions/Xdamage.h>
#endif
#ifdef HAVE_XRANDR
#include <X11/extensions/Xrandr.h>
#endif

static Window root_window = 0;
static GdkRectangle root_window_rect;
// Source-screen counterpart to root_window_rect, for the cross-server
// active-window poll (see x11_refresh_active_window_cross_server): comparing
// a :0 window's geometry against :1's screen size would be meaningless.
static GdkRectangle src_root_window_rect;
static Window window = 0;
static Pixmap pixmap = -1;
static int depth = -1, screen = -1;
static GC gc = NULL;
static GC gc_white = NULL;
static Display* display = NULL;
static gint refresh_timer = 0;
static Atom net_active_window_atom = 0;
// _NET_ACTIVE_WINDOW interned on src_display -- atom values are per-connection,
// so the destination-server atom above cannot be reused for source queries.
static Atom net_active_window_atom_src = 0;

static GdkPoint backup;
static Pixmap   backup_pixmap = 0;
#define CURSOR_CROSSHAIR_LEN 3
#define CURSOR_SIZE 9

static Window active_window = 0;

static GdkPoint offset;
static GdkPoint cursor;

#ifdef HAVE_XI
static gboolean can_track_cursor = FALSE;
static int xi_opcode = 0;
#endif

#ifdef HAVE_XDAMAGE
static gboolean can_use_xdamage = FALSE;
static int xdamage_event_base;
static Damage damage = 0;
static int min_refresh_period=0;
static Time next_refresh=0;
static gint refresh_timeout=0;

gboolean x11_compute_damaged_rect(GdkRectangle* rect);
#endif

#ifdef COPY_CURSOR
#undef  CURSOR_SIZE
#define CURSOR_SIZE 32
static int xfixes_event_base;
static int copy_cursor = 0;
static Pixmap  cursor_pixmap = 0;
static Picture cursor_picture = 0;
static XImage* cursor_image = NULL;
static GC      cursor_gc = NULL;
static Picture pixmap_picture = 0;

static int cursor_xhot=0;
static int cursor_yhot=0;
static uint32_t* cursor_pixels;
#define CURSOR_PIXELS_SIZE (sizeof(*cursor_pixels) * CURSOR_SIZE * CURSOR_SIZE)

// Cross-server cursor overlay: the EGL path renders straight from the source
// pixmap, so the destination-side COPY_CURSOR compositing never appears. Here
// we instead bake the SOURCE (:0) cursor into src_pixmap each frame (after the
// XCopyArea, before the DRI3 export) using XRender on the source display. The
// next frame's XCopyArea wipes it, so there are no trails. All resources live
// on src_display.
static gboolean src_cursor_avail = FALSE;     // XFixes+XRender present on src
static Pixmap   src_cursor_pixmap = 0;        // ARGB32 scratch for the cursor image
static Picture  src_cursor_picture = 0;
static GC       src_cursor_gc = 0;
static XImage*  src_cursor_ximage = NULL;
static uint32_t* src_cursor_pixels = NULL;
static Picture  src_pixmap_picture = 0;       // RGB24 Picture wrapping src_pixmap
#endif

#ifdef HAVE_XRANDR
static int xrandr_event_base = 0;
static char* x11_describe_randr_outputs(Display* dpy);
#endif

#ifdef HAVE_XRENDER
static double scale_factor = 1.0;
static int scaled_w, scaled_h, scaled_off_x, scaled_off_y;
static Picture scale_src_picture = 0;
static Picture scale_dst_picture = 0;
#endif

gboolean x11_draw_cursor();
gboolean x11_clear_cursor();
void x11_redraw_cursor(gboolean do_clear);

#ifdef COPY_CURSOR
// Cross-server source-cursor overlay (defined after the COPY_CURSOR block)
static void x11_init_source_cursor(void);
static void x11_composite_source_cursor(void);
#endif

// Forward declaration for DRI3 export function
static int dri3_export_pixmap(xcb_connection_t* conn, xcb_pixmap_t pixmap,
                              int width, int height, uint32_t* drm_format,
                              int* stride, uint64_t* modifier);

static gboolean inhibit_source_switch = FALSE;
static int pending_switch_idx = -1;
static GdkPoint switch_entry_point;
static gboolean alt_pressed = FALSE;

// Cross-server support (Phase 1)
static Display* src_display = NULL;  // source X11 display (if cross-server mode)
static xcb_connection_t* src_xcb = NULL;  // XCB connection for source (for future DRI3)
static gboolean use_cross_server = FALSE;  // TRUE if src != dst display
static Pixmap src_pixmap = 0;  // source pixmap for DRI3 export

// DRI3 and EGL state for cross-GPU rendering
static gboolean dri3_available = FALSE;
static egl_context_t* egl_ctx = NULL;
static GLuint egl_texture = 0;
static int last_export_fd = -1;

// Frame count since process start, incremented on every completed
// x11_refresh_image(). Read by the heartbeat log so a mirror that's gone
// quiet can be told apart from one that's just idling (no damage events).
static unsigned long frame_counter = 0;

#ifdef HAVE_XRENDER
static void
x11_update_scale(int dst_w, int dst_h)
{
	double sx = (double)dst_w / src_rect.width;
	double sy = (double)dst_h / src_rect.height;
	scale_factor = (sx < sy) ? sx : sy;
	scaled_w     = (int)(src_rect.width  * scale_factor);
	scaled_h     = (int)(src_rect.height * scale_factor);
	scaled_off_x = (dst_w - scaled_w) / 2;
	scaled_off_y = (dst_h - scaled_h) / 2;
}

static void
x11_repaint_scaled_rect(int x, int y, int w, int h)
{
	if (!scale_src_picture || !scale_dst_picture) return;
	int dx = (int)(x * scale_factor);
	int dy = (int)(y * scale_factor);
	int dw = (int)((x + w) * scale_factor) - dx + 1;
	int dh = (int)((y + h) * scale_factor) - dy + 1;
	if (dw <= 0 || dh <= 0) return;
	XRenderComposite(display, PictOpSrc,
		scale_src_picture, None, scale_dst_picture,
		dx, dy, 0, 0, dx, dy, dw, dh);
}
#endif


void
x11_adjust_offset_value(gint* offset, gint src, gint dst, gint cursor)
{
	if (dst >= src)
	{
		// dst window is enough big 
		// -> static offset
		*offset = (dst - src) / 2;
	} else {
		// dynamic offset
		if (cursor >= 0) {
			// on-screen
			int v = cursor + *offset;
			
			if (v < 0) {
				*offset -= v;
			} else if (v >= dst) {
				*offset -= v - dst;
			}
		} else {
			// off-screen
			// -> update only if there is unused space
			if (*offset > 0) {
				*offset = 0;
			} else {
				int min_offset = dst - src;
				if (*offset < min_offset) {
					*offset = min_offset;
				}
			}
		}
	}
}

// return true if offset was updated
// NOTE: must clear the window if it returns true
gboolean
x11_fix_offset()
{
#ifdef HAVE_XRENDER
	if (config.opt_scale)
		return FALSE;
#endif
	GdkPoint offset_bak = {offset.x, offset.y};

	// Adjust the offsets
	x11_adjust_offset_value(&offset.x, src_rect.width,  dst_rect.width,  cursor.x);
	x11_adjust_offset_value(&offset.y, src_rect.height, dst_rect.height, cursor.y);
	
	gboolean updated = memcmp(&offset, &offset_bak, sizeof(offset));
	if (updated) {
		// offset was updated
		// -> move the windows
		XMoveWindow(display, window, offset.x, offset.y);
	}
	return updated;
}


// Move a destination GdkWindow using a raw XMoveWindow on its X window,
// bypassing GTK/GDK window-geometry management. On a WM-less destination
// display GTK keeps resetting an override-redirect popup to 0,0 on map, and
// gtk_window_move/gdk_window_move don't reliably override that -- but a direct
// XMoveWindow does (and nothing moves it back without a WM).
void
x11_reposition_window(GdkWindow* win, int x, int y)
{
	if (!win) return;
	Display* d = gdk_x11_display_get_xdisplay(gdk_window_get_display(win));
	XMoveWindow(d, gdk_x11_window_get_xid(win), x, y);
	XFlush(d);
}

void
x11_refresh_cursor_location(gboolean force)
{
	Window root_return, w;
	int wx, wy;
	unsigned int mask;
	GdkPoint c;
	// In cross-server mode the source-monitor rectangles (src_rects_list) are
	// in the SOURCE display's coordinate space, so the pointer must be queried
	// from the source server. Querying the destination (`display`) pointer --
	// as the single-server path does -- would compare :1 coordinates against
	// :0 rectangles and follow the wrong cursor entirely.
	Display* q_display = (use_cross_server && src_display) ? src_display : display;
	Window   q_root    = (use_cross_server && src_display)
	                       ? XDefaultRootWindow(src_display) : root_window;
	XQueryPointer(q_display, q_root, &root_return, &w,
			&c.x, &c.y, &wx, &wy, &mask);

	// check which (if any) listed source monitor contains the cursor
	int found_idx = -1;
	for (int i = 0; i < n_src_monitors; i++) {
		int lx = c.x - src_rects_list[i].x;
		int ly = c.y - src_rects_list[i].y;
		if (lx >= 0 && ly >= 0 && lx < src_rects_list[i].width && ly < src_rects_list[i].height) {
			found_idx = i;
			break;
		}
	}

	if (found_idx < 0) {
		cursor.x = cursor.y = -1;
		if (!config.opt_passive) {
			squint_hide();
		}
		return;
	}

	// switch source if the cursor moved to a different listed monitor
	if (memcmp(&src_rects_list[found_idx], &src_rect, sizeof(GdkRectangle)) != 0) {
		if (!inhibit_source_switch) {
			int threshold = config.opt_switch_threshold;
			if (threshold <= 0) {
				pending_switch_idx = -1;
				squint_switch_source(found_idx);
			} else {
				if (pending_switch_idx != found_idx) {
					pending_switch_idx = found_idx;
					switch_entry_point = c;
				} else {
					int dx = abs(c.x - switch_entry_point.x);
					int dy = abs(c.y - switch_entry_point.y);
					if ((dx >= threshold) || (dy >= threshold)) {
						pending_switch_idx = -1;
						squint_switch_source(found_idx);
					}
				}
			}
		}
		return;
	}
	pending_switch_idx = -1;

	c.x -= src_rect.x;
	c.y -= src_rect.y;
	cursor = c;

	squint_show();

	// update the offsets and redraw the cursor
	gboolean updated = x11_fix_offset();
	x11_redraw_cursor(!updated);
	if (updated) {
		XClearWindow(display, window);
	}
}

#ifdef HAVE_MIT_SHM
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

static gboolean
x11_refresh_image_shm_fallback(Display* src_dpy, const GdkRectangle* damaged_rect)
{
	// Fallback for Xephyr and systems without DRI3
	// Read from source pixmap, write to destination window via XShm

	if (!pixmap || !window || !src_dpy) return FALSE;

	XImage* img = XGetImage(src_dpy, pixmap,
		damaged_rect->x - src_rect.x, damaged_rect->y - src_rect.y,
		damaged_rect->width, damaged_rect->height,
		AllPlanes, ZPixmap);

	if (!img) return FALSE;

	// Write to destination window
	GC dst_gc = gdk_x11_get_default_xlib_gc_for_colormap(
		gdk_window_get_colormap(gdkwin));

	XPutImage(display, window, dst_gc, img,
		0, 0,
		damaged_rect->x - src_rect.x, damaged_rect->y - src_rect.y,
		damaged_rect->width, damaged_rect->height);

	XDestroyImage(img);
	return TRUE;
}
#endif

// --- Optional per-stage frame timing (config.opt_perf_log) -----------------
// Times each stage of the capture/render pipeline and prints a min/avg/max
// summary every ~2s. Deliberately NOT per-frame -- a per-frame debug log was
// removed earlier for spamming the output; this aggregates instead.
typedef struct { gdouble sum_us, min_us, max_us; } perf_stat_t;

static perf_stat_t perf_cursor, perf_capture, perf_export, perf_draw, perf_swap, perf_total;
static guint perf_frames = 0;
static gint64 perf_window_start = 0;

static void
perf_stat_reset(perf_stat_t* s)
{
	s->sum_us = 0;
	s->min_us = -1;
	s->max_us = 0;
}

static void
perf_stat_add(perf_stat_t* s, gdouble us)
{
	s->sum_us += us;
	if (s->min_us < 0 || us < s->min_us) s->min_us = us;
	if (us > s->max_us) s->max_us = us;
}

static void
perf_stat_report(const perf_stat_t* s, const char* name, guint frames)
{
	fprintf(stderr, "  %-7s avg=%6.2fms min=%6.2fms max=%6.2fms\n",
			name, s->sum_us / frames / 1000.0, s->min_us / 1000.0, s->max_us / 1000.0);
}

// Call once at the very start of a frame, and once at the end with the
// stage durations (in microseconds; pass 0 for stages that weren't run).
static void
x11_perf_frame(gdouble cursor_us, gdouble capture_us, gdouble export_us,
				gdouble draw_us, gdouble swap_us, gdouble total_us)
{
	gint64 now = g_get_monotonic_time();

	if (perf_frames == 0) {
		perf_stat_reset(&perf_cursor);
		perf_stat_reset(&perf_capture);
		perf_stat_reset(&perf_export);
		perf_stat_reset(&perf_draw);
		perf_stat_reset(&perf_swap);
		perf_stat_reset(&perf_total);
		perf_window_start = now;
	}

	perf_stat_add(&perf_cursor, cursor_us);
	perf_stat_add(&perf_capture, capture_us);
	perf_stat_add(&perf_export, export_us);
	perf_stat_add(&perf_draw, draw_us);
	perf_stat_add(&perf_swap, swap_us);
	perf_stat_add(&perf_total, total_us);
	perf_frames++;

	if (now - perf_window_start >= 2 * G_USEC_PER_SEC) {
		gdouble elapsed_s = (now - perf_window_start) / 1e6;
		fprintf(stderr, "perf: %u frames in %.2fs (%.1f fps)\n",
				perf_frames, elapsed_s, perf_frames / elapsed_s);
		perf_stat_report(&perf_cursor,  "cursor",  perf_frames);
		perf_stat_report(&perf_capture, "capture", perf_frames);
		perf_stat_report(&perf_export,  "export",  perf_frames);
		perf_stat_report(&perf_draw,    "draw",    perf_frames);
		perf_stat_report(&perf_swap,    "swap",    perf_frames);
		perf_stat_report(&perf_total,   "total",   perf_frames);
		perf_frames = 0;  // next frame re-opens a window via the branch above
	}
}

void x11_refresh_active_window_cross_server(void);

gboolean
x11_refresh_image(const GdkRectangle* damaged_rect)
{
	if (!pixmap || !window) return FALSE;

	frame_counter++;

	gint64 perf_t0 = 0, perf_t1 = 0, perf_t2 = 0, perf_t3 = 0, perf_t4 = 0, perf_t5 = 0;
	if (config.opt_perf_log) perf_t0 = g_get_monotonic_time();

#ifdef HAVE_XI
	// In cross-server mode the XI motion events arrive on the DESTINATION
	// server (:1), which is useless for following the SOURCE (:0) cursor, so
	// XI-driven tracking is bypassed (see x11_on_x11_event) and we poll the
	// source pointer here on the periodic refresh instead.
	if (!can_track_cursor || use_cross_server)
#endif
	{
		x11_refresh_cursor_location(FALSE);
	}
	if (use_cross_server && src_display) {
		x11_refresh_active_window_cross_server();
	}
	if (config.opt_perf_log) perf_t1 = g_get_monotonic_time();

	x11_clear_cursor();

	// Check if we're in cross-server mode
	if (use_cross_server && src_display) {
		// XCopyArea from source root to persistent pixmap.
		// IncludeInferiors is essential: with the default ClipByChildren the
		// copy sees only the root window's own drawing (the desktop
		// background), which is black under a compositing WM like KWin --
		// every application window is a child of root and gets clipped out.
		// IncludeInferiors pulls the composited child-window content too, so
		// the pixmap (and thus the DRI3-exported DMA-BUF) holds the real
		// desktop image.
		XGCValues gc_vals = {0};
		gc_vals.subwindow_mode = IncludeInferiors;
		// graphics_exposures defaults to True, making every XCopyArea emit a
		// NoExpose event. Nothing drains src_display's queue, so those queued
		// forever inside Xlib (~224 bytes/frame -> GBs over days).
		gc_vals.graphics_exposures = False;
		GC gc = XCreateGC(src_display, src_pixmap, GCSubwindowMode | GCGraphicsExposures, &gc_vals);

		XCopyArea(src_display, XDefaultRootWindow(src_display), src_pixmap, gc,
				 damaged_rect->x, damaged_rect->y,
				 damaged_rect->width, damaged_rect->height,
				 0, 0);

		XFreeGC(src_display, gc);

#ifdef COPY_CURSOR
		// Bake the source cursor into the freshly-copied desktop before export.
		// (The next frame's XCopyArea wipes it, so no trails.)
		x11_composite_source_cursor();
#endif

		XSync(src_display, False);  // Wait for blit (and cursor composite)
		if (config.opt_perf_log) perf_t2 = g_get_monotonic_time();

		// Try DRI3/EGL path first
		if (dri3_available && egl_ctx) {
			uint32_t drm_format = 0;
			int stride = 0;
			uint64_t modifier = DRM_FORMAT_MOD_INVALID;
			int fd = dri3_export_pixmap(src_xcb, src_pixmap,
									   src_rect.width, src_rect.height,
									   &drm_format, &stride, &modifier);
			if (config.opt_perf_log) perf_t3 = g_get_monotonic_time();
			if (fd >= 0) {
				// If this is the first frame, create texture from fd
				if (egl_texture == 0) {
					egl_texture = egl_create_image_from_fd(egl_ctx, fd,
														  src_rect.width, src_rect.height,
														  stride, drm_format, modifier);
					close(fd);  // fd no longer needed; EGLImage owns the buffer
					if (egl_texture == 0) {
						fprintf(stderr, "x11: failed to create EGL texture\n");
						dri3_available = FALSE;  // Fallback to XShm
					}
				} else {
					close(fd);  // XCopyArea updated pixmap in-place
				}

				if (egl_texture) {
					egl_draw_quad(egl_ctx, egl_texture);
					if (config.opt_perf_log) perf_t4 = g_get_monotonic_time();
					egl_swap_buffers(egl_ctx);
					if (config.opt_perf_log) {
						perf_t5 = g_get_monotonic_time();
						x11_perf_frame(perf_t1 - perf_t0, perf_t2 - perf_t1,
									   perf_t3 - perf_t2, perf_t4 - perf_t3,
									   perf_t5 - perf_t4, perf_t5 - perf_t0);
					}
					return TRUE;
				}
			}
		}

		// Fallback to XShm if DRI3 unavailable
#ifdef HAVE_MIT_SHM
		gboolean result = x11_refresh_image_shm_fallback(src_display, damaged_rect);
		XFlush(display);
		if (config.opt_perf_log)
			x11_perf_frame(perf_t1 - perf_t0, perf_t2 - perf_t1, 0, 0, 0,
						   g_get_monotonic_time() - perf_t0);
		return result;
#else
		// Cross-server mode requires MIT-SHM support
		g_warning("cross-server mode requires DRI3 or MIT-SHM support");
		XFlush(display);
		return FALSE;
#endif
	}

	// Original same-server rendering path
	// location of the damaged area relative to the source rect
	int x = damaged_rect->x - src_rect.x;
	int y = damaged_rect->y - src_rect.y;

	XCopyArea (display, root_window, pixmap, gc,
			damaged_rect->x,     damaged_rect->y,
			damaged_rect->width , damaged_rect->height,
			x, y);

	x11_draw_cursor();

	// redraw the damaged area
#ifdef HAVE_XRENDER
	if (config.opt_scale && scale_src_picture)
		x11_repaint_scaled_rect(x, y, damaged_rect->width, damaged_rect->height);
	else
#endif
	XClearArea(display, window, x, y, damaged_rect->width, damaged_rect->height, FALSE);

	XFlush (display);

	if (config.opt_perf_log)
		x11_perf_frame(perf_t1 - perf_t0, 0, 0, 0, 0, g_get_monotonic_time() - perf_t0);

	return TRUE;
}

#ifdef HAVE_XDAMAGE
void x11_try_refresh_image (Time timestamp, const GdkRectangle* damaged_rect);

gboolean 
x11_try_refresh_image_timeout (gpointer data)
{
	refresh_timeout=0;

	if ((Time)data == next_refresh)
	{
		x11_try_refresh_image(next_refresh, NULL);
	}
	return FALSE;
}

void
x11_try_refresh_image (Time timestamp, const GdkRectangle* damaged_rect)
{
	static GdkRectangle acc = { 0, 0, 0, 0 };
	if (damaged_rect != NULL) {
		if (acc.width == 0) {
			acc = *damaged_rect;
		} else {
			gdk_rectangle_union(damaged_rect, &acc, &acc);
		}
	}

	if ((timestamp >= next_refresh) || (timestamp < next_refresh - 1000)) {
		if (refresh_timeout) {
			g_source_remove(refresh_timeout);
			refresh_timeout=0;
		}
		next_refresh = timestamp + min_refresh_period;
		if (acc.width) {
			x11_refresh_image(&acc);
			acc.width = 0;
		}

	} else if (!refresh_timeout) {
		refresh_timeout = g_timeout_add (next_refresh - timestamp, x11_try_refresh_image_timeout, (gpointer)next_refresh);
	}
}
#endif



#ifdef COPY_CURSOR
void
x11_refresh_cursor_image()
{
	XFixesCursorImage* img = XFixesGetCursorImage (display);
	if (!img)
		return;

	int width  = (img->width  < CURSOR_SIZE) ? img->width  : CURSOR_SIZE;
	int height = (img->height < CURSOR_SIZE) ? img->height : CURSOR_SIZE;

	int x, y;
	// copy the cursor image
	for(y=0 ; y<height ; y++)
	{
		for(x=0 ; x<width ; x++)
		{
			cursor_pixels[y*CURSOR_SIZE + x] = img->pixels[y*img->width + x];
		}
	}

	// clear the cursor_pixmap and upload the new image
	XFillRectangle(display, cursor_pixmap, cursor_gc, 0, 0, CURSOR_SIZE, CURSOR_SIZE);
	XPutImage(display, cursor_pixmap, cursor_gc, cursor_image,
			0, 0, 0, 0, width, height);
	cursor_xhot = img->xhot;
	cursor_yhot = img->yhot;

	XFree(img);

	x11_redraw_cursor(TRUE);
}

void
x11_init_copy_cursor()
{
	// ensure we are in true color
	if (XDefaultDepth(display, screen) != 24) {
		return;
	}

	// ensure the screen supports 32 bit depth
	{
		int depth_count = 0;
		int* depths = XListDepths(display, screen, &depth_count);
		if (depths == NULL) {
			return;
		}
		int i;
		for (i=depth_count-1 ; i>=0 ; i--) {
			if (depths[i] == 32) {
				break;
			}
		}
		XFree(depths);
		if (i < 0) {
			return;
		}
	}

	// check if xfixes and xrender are available on this display
	int major=1, minor=0, event_base, error_base;
	if ((	   !XFixesQueryExtension(display, &xfixes_event_base, &error_base)
		|| !XFixesQueryVersion(display, &major, &minor)
		|| (major<1)
		|| !XRenderQueryExtension(display, &event_base, &error_base)
	)) {
		return;
	}

	// create an image for storing the cursor
	cursor_pixels = (uint32_t*) malloc(CURSOR_PIXELS_SIZE);
	cursor_image = XCreateImage(display, NULL, 32, ZPixmap, 0, (char*)cursor_pixels,
				CURSOR_SIZE, CURSOR_SIZE, 32, 4*CURSOR_SIZE);
	if (!cursor_image) {
		squint_error("XCreateImage() failed");
		return;
	}

	// create a pixmap for storing the cursor
	cursor_pixmap = XCreatePixmap(display, root_window,
				CURSOR_SIZE, CURSOR_SIZE, 32);

	// create a context for manipulating the cursor pixmap (must have 32-bit depth)
	cursor_gc = XCreateGC(display, cursor_pixmap, 0, NULL);
	if(!cursor_gc) {
		squint_error("XCreateGC() failed");
		return;
	}

	// create a picture for the cursor_pixmap
	cursor_picture = XRenderCreatePicture(display, cursor_pixmap,
			XRenderFindStandardFormat(display, PictStandardARGB32),
			0, NULL);
}

void
x11_enable_copy_cursor()
{
	if (copy_cursor || (cursor_picture == 0)) {
		return;
	}

	// create a picture for the main pixmap
	pixmap_picture = XRenderCreatePicture(display, pixmap,
			XRenderFindStandardFormat(display, PictStandardRGB24),
			0, NULL);

	copy_cursor = TRUE;

	// refresh the cursor
	x11_refresh_cursor_image();

	x11_refresh_cursor_location(TRUE);

	// request cursor change notifications
	XFixesSelectCursorInput(display, gdk_x11_window_get_xid(gdkwin), XFixesCursorNotify);
}

void
x11_disable_copy_cursor()
{
	if (!copy_cursor) {
		return;
	}
	copy_cursor = FALSE;

	if (pixmap_picture) {
		XRenderFreePicture(display, pixmap_picture);
		pixmap_picture = 0;
	}
}

// Set up the source-cursor overlay resources on the source display. Safe to
// call once at init; a no-op unless cross-server and XFixes+XRender exist on
// the source. Leaves src_cursor_avail FALSE if anything is missing (the mirror
// then simply shows no cursor, as before).
static void
x11_init_source_cursor(void)
{
	if (!use_cross_server || !src_display) {
		return;
	}

	int ev, er, major = 1, minor = 0;
	if (   !XFixesQueryExtension(src_display, &ev, &er)
	    || !XFixesQueryVersion(src_display, &major, &minor)
	    || (major < 1)
	    || !XRenderQueryExtension(src_display, &ev, &er)) {
		fprintf(stderr, "x11: source display lacks XFixes/XRender; no cursor overlay\n");
		return;
	}

	XRenderPictFormat* argb = XRenderFindStandardFormat(src_display, PictStandardARGB32);
	if (!argb) {
		return;
	}

	src_cursor_pixels = (uint32_t*) malloc(CURSOR_PIXELS_SIZE);
	src_cursor_ximage = XCreateImage(src_display, NULL, 32, ZPixmap, 0,
			(char*)src_cursor_pixels, CURSOR_SIZE, CURSOR_SIZE, 32, 4*CURSOR_SIZE);
	if (!src_cursor_ximage) {
		free(src_cursor_pixels);
		src_cursor_pixels = NULL;
		return;
	}

	src_cursor_pixmap = XCreatePixmap(src_display, XDefaultRootWindow(src_display),
			CURSOR_SIZE, CURSOR_SIZE, 32);
	src_cursor_gc = XCreateGC(src_display, src_cursor_pixmap, 0, NULL);
	src_cursor_picture = XRenderCreatePicture(src_display, src_cursor_pixmap, argb, 0, NULL);

	src_cursor_avail = TRUE;
}

// Composite the current source-display cursor into src_pixmap at its live
// position. Called each frame in the cross-server EGL path, after XCopyArea has
// refreshed src_pixmap and before the DRI3 export.
static void
x11_composite_source_cursor(void)
{
	if (!src_cursor_avail || !src_pixmap) {
		return;
	}

	XFixesCursorImage* img = XFixesGetCursorImage(src_display);
	if (!img) {
		return;
	}

	int w = (img->width  < CURSOR_SIZE) ? img->width  : CURSOR_SIZE;
	int h = (img->height < CURSOR_SIZE) ? img->height : CURSOR_SIZE;

	// Top-left of the cursor image in source-pixmap (src_rect-local) coords.
	int px = (img->x - img->xhot) - src_rect.x;
	int py = (img->y - img->yhot) - src_rect.y;

	// Cull if the cursor is entirely outside the current source rect.
	if (px + w <= 0 || py + h <= 0 || px >= src_rect.width || py >= src_rect.height) {
		XFree(img);
		return;
	}

	// Copy the ARGB pixels (img->pixels is unsigned long per pixel; take the
	// low 32 bits) into our scratch image and upload it to the ARGB pixmap.
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			src_cursor_pixels[y*CURSOR_SIZE + x] = (uint32_t) img->pixels[y*img->width + x];
		}
	}
	XFillRectangle(src_display, src_cursor_pixmap, src_cursor_gc, 0, 0, CURSOR_SIZE, CURSOR_SIZE);
	XPutImage(src_display, src_cursor_pixmap, src_cursor_gc, src_cursor_ximage,
			0, 0, 0, 0, w, h);
	XFree(img);

	// (Re)create the RGB24 Picture wrapping src_pixmap. It's dropped whenever
	// src_pixmap is reallocated on a source switch (see x11_free_source_buffer).
	if (!src_pixmap_picture) {
		src_pixmap_picture = XRenderCreatePicture(src_display, src_pixmap,
				XRenderFindStandardFormat(src_display, PictStandardRGB24), 0, NULL);
	}

	// PictOpOver blends the cursor's alpha over the desktop. XRender clips to
	// the destination pixmap, so partially-offscreen cursors are handled.
	XRenderComposite(src_display, PictOpOver,
			src_cursor_picture, None, src_pixmap_picture,
			0, 0, 0, 0, px, py, w, h);
}
#endif


void
x11_show_active_window()
{
	if (!active_window)
		return;

	// find which listed source monitor the active window overlaps most
	int best_src_idx = -1;
	int best_src_area = 0;
	for (int i = 0; i < n_src_monitors; i++) {
		GdkRectangle inter;
		gdk_rectangle_intersect(&active_window_rect, &src_rects_list[i], &inter);
		int area = inter.width * inter.height;
		if (area > best_src_area) {
			best_src_area = area;
			best_src_idx = i;
		}
	}

	GdkRectangle inter_dst;
	gdk_rectangle_intersect(&active_window_rect, &dst_rect, &inter_dst);
	int dst_area = inter_dst.width * inter_dst.height;

	if (best_src_area > dst_area) {
		// active window overlaps more with a source monitor
		if (best_src_idx >= 0 &&
		    memcmp(&src_rects_list[best_src_idx], &src_rect, sizeof(GdkRectangle)) != 0) {
			squint_switch_source(best_src_idx);
		} else {
			squint_show();
		}
	} else {
		if (!config.opt_passive) {
			squint_hide();
		}
	}
}

gboolean
x11_get_window_geometry(Display* dpy, Window w, GdkRectangle* r)
{
	gboolean result;
	Window root;
	int x, y;
	unsigned int width, height, border_width, depth;

	gdk_x11_display_error_trap_push(gdisplay);

	if (XGetGeometry(dpy, w, &root, &x, &y, &width, &height,
			&border_width, &depth))
	{
		r->x = x - border_width;
		r->y = y - border_width;
		r->width  = width  + 2*border_width;
		r->height = height + 2*border_width;

		result = TRUE;
	} else {
		result = FALSE;
	}
	gdk_x11_display_error_trap_pop_ignored(gdisplay);

	return result;
}

void
x11_refresh_active_window_geometry()
{
	if(!active_window)
		return;

	// In cross-server mode active_window is a SOURCE (:0) XID -- walk its
	// tree and query its geometry on src_display, same q_display/q_root
	// pattern as x11_refresh_cursor_location. Single-server mode is
	// unaffected: q_display == display, q_root == root_window.
	Display* q_display = (use_cross_server && src_display) ? src_display : display;
	Window   q_root    = (use_cross_server && src_display)
	                       ? XDefaultRootWindow(src_display) : root_window;

	// ignore X11 errors (this function can produce BadWindow errors since
	// it makes queries on windows controlled by other applications)
	gdk_x11_display_error_trap_push(gdisplay);

	{
		Window w = active_window;
		Window root, parent, *children;
		unsigned int nchildren;

		// identify the top-level window
		parent = w;
		while(parent != q_root)
		{
			w = parent;
			if(!XQueryTree(q_display, w, &root, &parent, &children, &nchildren))
				goto err;
			XFree(children);
		}

		// get its coordinates
		x11_get_window_geometry(q_display, w, &active_window_rect);
	}
err:
	gdk_x11_display_error_trap_pop_ignored(gdisplay);
}

Window
x11_get_active_window_on(Display* dpy, Window root, Atom atom)
{
	Window result = 0;
	Window* w;
	Atom actual_type_return;
	int  actual_format_return;
	unsigned long nitems_return, bytes_after_return;

	if (XGetWindowProperty(dpy, root, atom, 0, 1,
			FALSE, AnyPropertyType,	&actual_type_return,
			&actual_format_return, &nitems_return,
			&bytes_after_return, (unsigned char**)&w)
		== Success)
	{
		// No window manager (e.g. a bare Squint-only display) never sets
		// _NET_ACTIVE_WINDOW, in which case nitems_return is 0 and w is NULL.
		if (w) {
			if (nitems_return > 0)
				result = *w;
			XFree(w);
		}
	}
	return result;
}

Window
x11_get_active_window()
{
	return x11_get_active_window_on(display, root_window, net_active_window_atom);
}

void
x11_active_window_stop_monitoring()
{
	// Cross-server: the poll below never subscribes StructureNotify on the
	// tracked window (there's no async event channel from src_display to
	// deliver it), so there's nothing to unwind here -- and active_window is
	// a foreign :0 XID that XChangeWindowAttributes(display, ...) below would
	// otherwise send to the wrong (:1) connection.
	if (use_cross_server) {
		active_window = 0;
		return;
	}

	if (!gdk_x11_window_lookup_for_display(gdisplay, active_window))
	{
		// ignore X11 errors (this function can produce BadWindow errors since
		// it makes queries on windows controlled by other applications)
		gdk_x11_display_error_trap_push(gdisplay);

		XSetWindowAttributes attr;
		attr.event_mask = 0;
		XChangeWindowAttributes(display, active_window, CWEventMask, &attr);

		gdk_x11_display_error_trap_pop_ignored(gdisplay);
	} 
	active_window = 0;
}

void
x11_active_window_start_monitoring()
{
	if (active_window)
		x11_active_window_stop_monitoring();

	active_window = x11_get_active_window();
	if (!active_window)
		return;

	x11_refresh_active_window_geometry();
	if (!memcmp(&active_window_rect, &root_window_rect, sizeof(GdkRectangle))) {
		// same geometry as the root window
		// -> ignore it
		// TODO: make the match more loose
		active_window = 0;
		return;
	}

	// in passive mode, only track focus changes, not window geometry changes
	if (config.opt_passive)
		return;

	if (!gdk_x11_window_lookup_for_display(gdisplay, active_window))
	{
		// this is a foreign window
		// -> we need to monitor it explicitely


		// ignore X11 errors (this function can produce BadWindow errors since
		// it makes queries on windows controlled by other applications)
		gdk_x11_display_error_trap_push(gdisplay);

		XSetWindowAttributes attr;
		attr.event_mask = StructureNotifyMask;
		XChangeWindowAttributes(display, active_window, CWEventMask, &attr);

		gdk_x11_display_error_trap_pop_ignored(gdisplay);
	}
}

// Cross-server counterpart to x11_active_window_start_monitoring's
// PropertyNotify trigger. There is no WM on the destination (:1) to ever set
// _NET_ACTIVE_WINDOW there, and no async event channel from the source
// (:0) connection -- src_display is synchronous-query-only, never added to
// the GLib main loop (see the XI_RawMotion comment in x11_on_x11_event). So,
// like cursor-follow, poll _NET_ACTIVE_WINDOW on the SOURCE root every
// damage-driven refresh, and only redo the geometry walk when the active
// window handle actually changed.
void
x11_refresh_active_window_cross_server()
{
	if (!net_active_window_atom_src)
		return;
	if (n_src_monitors <= 1)
		return;  // nothing to switch between (e.g. mirror 1)

	Window w = x11_get_active_window_on(src_display, XDefaultRootWindow(src_display),
	                                     net_active_window_atom_src);
	if (w == active_window)
		return;  // no focus change since the last poll

	active_window = w;
	if (!active_window)
		return;

	x11_refresh_active_window_geometry();
	if (!memcmp(&active_window_rect, &src_root_window_rect, sizeof(GdkRectangle))) {
		// same geometry as the source root window -> ignore it, same
		// heuristic as x11_active_window_start_monitoring's single-server path
		active_window = 0;
		return;
	}

	x11_show_active_window();
}

#ifdef HAVE_XRANDR
// A hotplug fires several RRScreenChangeNotify events in quick succession
// while the output renegotiates EDID/mode, so re-enabling on the first one
// would just get disabled again by the next. Debounce: each new event pushes
// the retry back out, so squint_enable() only runs once the storm has settled.
#define RANDR_REENABLE_DEBOUNCE_MS 2000
static guint randr_reenable_source_id = 0;

static gboolean
x11_randr_reenable_cb(gpointer data)
{
	randr_reenable_source_id = 0;

	// squint_enable() re-resolves our destination monitor via
	// select_monitors() -- if it's still gone this is a no-op, if it's back
	// this is the only thing that ever turns the mirror back on.
	if (squint_enable()) {
		fprintf(stderr, "x11: re-enabled after RandR screen change settled\n");
	} else {
		fprintf(stderr, "x11: still disabled after RandR screen change -- "
			"destination monitor not found\n");
	}
	return G_SOURCE_REMOVE;
}
#endif

GdkFilterReturn
x11_on_x11_event (GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
	XEvent* ev = (XEvent*)xevent;

	if (ev->type == PropertyNotify)
	{
		XPropertyEvent* pn_ev = (XPropertyEvent*) ev;
		if ((pn_ev->window == root_window) && (pn_ev->atom == net_active_window_atom))
		{
			// property _NET_ACTIVE_WINDOW was changed
			GdkRectangle prev_src = src_rect;
			GdkPoint prev_cursor = cursor;
			x11_active_window_start_monitoring();
			x11_show_active_window();

			if (config.opt_warp_cursor && alt_pressed &&
			    memcmp(&prev_src, &src_rect, sizeof(GdkRectangle)) != 0 &&
			    prev_cursor.x >= 0 && prev_src.width > 0 && prev_src.height > 0)
			{
				// Focus switched to a window on a different source monitor during Alt+Tab.
				// Preserve the same fractional position within the squint window:
				// cursor.x/y is relative to the source rect, so normalize against
				// the old source dimensions and map to the new source.
				float rel_x = (float)prev_cursor.x / prev_src.width;
				float rel_y = (float)prev_cursor.y / prev_src.height;
				int tx = src_rect.x + (int)(rel_x * src_rect.width);
				int ty = src_rect.y + (int)(rel_y * src_rect.height);
				tx = CLAMP(tx, src_rect.x, src_rect.x + src_rect.width  - 1);
				ty = CLAMP(ty, src_rect.y, src_rect.y + src_rect.height - 1);
				XWarpPointer(display, None, root_window, 0, 0, 0, 0, tx, ty);
				XFlush(display);
			}

			return GDK_FILTER_REMOVE;
		}
		
	}

	if (ev->type == ConfigureNotify)
	{
		XConfigureEvent* c_ev = (XConfigureEvent*) ev;
		if (c_ev->window == active_window)
		{
			x11_refresh_active_window_geometry();
			return GDK_FILTER_CONTINUE;
		}
	}

#ifdef HAVE_XI
	if(can_track_cursor)
	{
		XGenericEventCookie *cookie = &ev->xcookie;

		if (	(cookie->type == GenericEvent)
		    &&	(cookie->extension == xi_opcode))
		{
			switch (cookie->evtype)
			{
			case XI_RawMotion:
				// cursor was moved. In cross-server mode these events are for
				// the destination (:1) pointer, not the source (:0) one we
				// follow, so ignore them -- the refresh timer polls the source
				// pointer instead (see x11_refresh_image).
				if (!use_cross_server)
					x11_refresh_cursor_location(FALSE);

				return GDK_FILTER_REMOVE;
			case XI_RawKeyPress:
				// a key was pressed
				// -> we ensure that the active window is on screen
				{
					XIRawEvent* xi_ev = (XIRawEvent*) cookie->data;
					GdkKeymap* km = gdk_keymap_get_for_display(gdisplay);
					guint keyval;

					if(gdk_keymap_translate_keyboard_state(km, xi_ev->detail,
							0, // FIXME: do we need the modifier?
							0, // FIXME: how to determine the group?
							&keyval, NULL, NULL, NULL))
					{
						switch(keyval)
						{
						case  GDK_KEY_Alt_L:
						case  GDK_KEY_Alt_R:
							alt_pressed = TRUE;
							return GDK_FILTER_REMOVE;
						case  GDK_KEY_Control_L:
						case  GDK_KEY_Control_R:
						case  GDK_KEY_Meta_L:
						case  GDK_KEY_Meta_R:
							// ignore modifier keys (except shift)
							// because they may be used by the
							// window manager
							return GDK_FILTER_REMOVE;
						}
					}

					x11_show_active_window();
					return GDK_FILTER_REMOVE;
				}
			case XI_RawKeyRelease:
				// a key was released
				{
					XIRawEvent* xi_ev = (XIRawEvent*) cookie->data;
					GdkKeymap* km = gdk_keymap_get_for_display(gdisplay);
					guint keyval;

					if(gdk_keymap_translate_keyboard_state(km, xi_ev->detail,
							0, // FIXME: do we need the modifier?
							0, // FIXME: how to determine the group?
							&keyval, NULL, NULL, NULL))
					{
						switch(keyval)
						{
						case  GDK_KEY_Alt_L:
						case  GDK_KEY_Alt_R:
							alt_pressed = FALSE;
							return GDK_FILTER_REMOVE;
						}
					}
					return GDK_FILTER_CONTINUE;
				}
			}
		}
	}
#endif

#ifdef COPY_CURSOR
	if(copy_cursor)
	{
		if (ev->type == xfixes_event_base + XFixesCursorNotify) {
			x11_refresh_cursor_image();

			return GDK_FILTER_REMOVE;
		}
	}
#endif

#ifdef HAVE_XDAMAGE
	if(damage)
	{
		if (ev->type == xdamage_event_base + XDamageNotify)
		{
			XDamageNotifyEvent* xd_ev = (XDamageNotifyEvent*) ev;
			static GdkRectangle accumulated_damage = { 0, 0, 0, 0 };

			// get the damaged area
			GdkRectangle rect = {
				xd_ev->area.x,     xd_ev->area.y,
				xd_ev->area.width, xd_ev->area.height
			};

			if (x11_compute_damaged_rect(&rect)) {
				// source screen damaged
				if (accumulated_damage.width == 0) {
					accumulated_damage = rect;
				} else {
					gdk_rectangle_union(&rect,
							&accumulated_damage,
							&accumulated_damage);
				}
			}

			if (!xd_ev->more && accumulated_damage.width)
			{
				x11_try_refresh_image(xd_ev->timestamp, &accumulated_damage);
				accumulated_damage.width = 0;
			}
		}
	}
#endif

#ifdef HAVE_XRANDR
	if (xrandr_event_base)
	{
		if (ev->type == xrandr_event_base + RRScreenChangeNotify) {
			// RRScreenChangeNotify is a per-root-window broadcast: it fires
			// for every client that selected it on this root, regardless of
			// which output actually changed. So a HDMI hotplug on :1 also
			// disables a mirror whose own output (e.g. DP-3) never moved --
			// log the full connector snapshot so that's distinguishable
			// after the fact from a change that genuinely affects us.
			char* outputs = x11_describe_randr_outputs(display);
			fprintf(stderr, "x11: RandR screen change on %s -- %s\n",
				DisplayString(display), outputs);
			g_free(outputs);
			squint_disable("RandR screen change");

			if (randr_reenable_source_id) {
				g_source_remove(randr_reenable_source_id);
			}
			randr_reenable_source_id = g_timeout_add(
				RANDR_REENABLE_DEBOUNCE_MS, x11_randr_reenable_cb, NULL);
		}
	}
#endif
	return GDK_FILTER_CONTINUE;
}

#ifdef HAVE_XI
void
x11_set_xi_eventmask(gboolean active)
{
	// inspired from: http://keithp.com/blogs/Cursor_tracking/

	XIEventMask evmasks[1];
	unsigned char mask1[(XI_LASTEVENT + 7)/8];
	memset(mask1, 0, sizeof(mask1));

	if (active) {
		// select for button and key events from all master devices
		XISetMask(mask1, XI_RawMotion);
		if (!config.opt_passive) {
			XISetMask(mask1, XI_RawKeyPress);
			XISetMask(mask1, XI_RawKeyRelease);
		}
	}

	evmasks[0].deviceid = XIAllMasterDevices;
	evmasks[0].mask_len = sizeof(mask1);
	evmasks[0].mask = mask1;

	XISelectEvents(display, root_window, evmasks, 1);
}

void x11_init_cursor_tracking()
{
	int event, error;
	if (!XQueryExtension(display, "XInputExtension", &xi_opcode, &event, &error))
		return;

	int major=2, minor=2;
	if (XIQueryVersion(display, &major, &minor) != Success)
		return;

	if ((major > 2) || ((major == 2) && (minor >= 2))) {
		can_track_cursor = TRUE;
	}
}

void
x11_enable_cursor_tracking()
{
	if (can_track_cursor) {
		x11_set_xi_eventmask(TRUE);
	}
}

void
x11_disable_cursor_tracking()
{
	if (can_track_cursor) {
		x11_set_xi_eventmask(FALSE);
	}
}
#endif

#ifdef HAVE_XDAMAGE
void
x11_init_xdamage()
{
	if (config.opt_rate > 0) {
		// user requested fixed refresh rate
		return;
	}

	if (use_cross_server) {
		// XDamageCreate below watches root_window on `display`, which is
		// the DESTINATION display. In cross-server mode that's a different
		// X server than the one showing the actual content (src_display),
		// so it never reports the real source changes -- leave xdamage
		// disabled and let the periodic timer in x11_enable_window() poll
		// the source display instead (see the `!(damage && can_track_cursor)`
		// branch there).
		return;
	}

	// detect the Xdamage extension
	int major=1, minor=0, error_base;
	if (	   !XDamageQueryExtension(display, &xdamage_event_base, &error_base)
		|| !XDamageQueryVersion(display, &major, &minor)
		|| (major<1)
	) {
		return;
	}

	if (config.opt_limit == 0) {
		// no limit
		min_refresh_period = 0;
	} else {
		// 50 fps by default
		min_refresh_period = 1000 / ((config.opt_limit<0) ? 50 : config.opt_limit); 
	}

	can_use_xdamage = TRUE;
}

void
x11_enable_xdamage()
{
	if (can_use_xdamage) {
		damage = XDamageCreate(display, root_window, XDamageReportRawRectangles);
	}
}

void
x11_disable_xdamage()
{
	if (damage) {
		XDamageDestroy(display, damage);
		damage = 0;
	}
}

gboolean
x11_compute_damaged_rect(GdkRectangle* rect)
{
	// intersect the rectangle with src_rect
	if (!gdk_rectangle_intersect(&src_rect, rect, rect)) {
		// src_rect not damaged
		return FALSE;
	}

	// does it intersect with the dst_rect?
	if (gdk_rectangle_intersect(rect, &dst_rect, NULL)) {
		// part of the damages are inside dst_rect
		// return TRUE only if we have other damages outside dst_rect
		GdkRectangle r;
		gdk_rectangle_union(rect,   &dst_rect, &r);
		return !gdk_rectangle_equal(&dst_rect, &r);
	} else {
		// all damages outside dst_rect
		return TRUE;
	}
}
#endif


gboolean
x11_on_window_configure_event(GtkWidget *widget, GdkEvent *event, gpointer   user_data)
{
	GdkEventConfigure* e = (GdkEventConfigure*) event;
	GdkRectangle rect;
	rect.x = e->x;
	rect.y = e->y;
	rect.width  = e->width;
	rect.height = e->height;

	if(!enabled) {
		return TRUE;
	}

	if(!fullscreen) {
		memcpy(&dst_rect, &rect, sizeof(rect));
#ifdef HAVE_XRENDER
		if (config.opt_scale && scale_src_picture) {
			x11_update_scale(rect.width, rect.height);

			// reposition and resize the sub-window
			XMoveResizeWindow(display, window,
				scaled_off_x, scaled_off_y, scaled_w, scaled_h);

			// update the transform for the new scale
			XTransform xform = {{
				{ XDoubleToFixed(1.0/scale_factor), 0, 0 },
				{ 0, XDoubleToFixed(1.0/scale_factor), 0 },
				{ 0, 0, XDoubleToFixed(1.0) }
			}};
			XRenderSetPictureTransform(display, scale_src_picture, &xform);

			// repaint the full sub-window
			XRenderComposite(display, PictOpSrc,
				scale_src_picture, None, scale_dst_picture,
				0, 0, 0, 0, 0, 0, scaled_w, scaled_h);
			XFlush(display);
		} else
#endif
		if (x11_fix_offset()) {
			XClearWindow(display, window);
		}
	}
	return TRUE;
}

#ifdef HAVE_XRANDR
// Heap-allocated "name=connected/disconnected/unknown ..." summary of every
// RandR output on dpy's default screen. Caller g_free()s the result.
static char*
x11_describe_randr_outputs(Display* dpy)
{
	XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, DefaultRootWindow(dpy));
	if (!res) return g_strdup("(could not query RandR outputs)");

	GString* s = g_string_new(NULL);
	for (int i = 0; i < res->noutput; i++) {
		XRROutputInfo* info = XRRGetOutputInfo(dpy, res, res->outputs[i]);
		if (!info) continue;
		g_string_append_printf(s, "%s%s=%s", i ? " " : "", info->name,
			info->connection == RR_Connected    ? "connected" :
			info->connection == RR_Disconnected ? "disconnected" : "unknown");
		XRRFreeOutputInfo(info);
	}
	XRRFreeScreenResources(res);
	return g_string_free(s, FALSE);
}

void
x11_init_xrandr()
{
	int error;
	if (!XRRQueryExtension(display, &xrandr_event_base, &error)) {
		return;
	}

	XRRSelectInput(display, root_window, RRScreenChangeNotifyMask);
}
#endif

// Periodic proof-of-life: silence in the log is ambiguous between "healthy,
// nothing changed" and "quietly wedged/disabled", so log frame throughput and
// enabled-state on a timer regardless of activity.
static gboolean
x11_heartbeat_cb(gpointer data)
{
	static unsigned long last_frame_counter = 0;
	fprintf(stderr, "x11: heartbeat enabled=%d frames_total=%lu frames_since_last=%lu\n",
		enabled, frame_counter, frame_counter - last_frame_counter);
	last_frame_counter = frame_counter;
	return G_SOURCE_CONTINUE;
}

// Map an X pixmap depth to a DRM fourcc format code.
// Returns 0 on unsupported depth.
static uint32_t depth_to_drm_format(int depth) {
    switch (depth) {
        case 24: return DRM_FORMAT_XRGB8888;
        case 32: return DRM_FORMAT_ARGB8888;
        case 16: return DRM_FORMAT_RGB565;
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// Linear source buffer allocation via GBM.
//
// AMD tiles its pixmaps with a vendor-specific DRM modifier that Intel cannot
// import, and AMD's DRI3 reports the modifier as INVALID. The only pixel
// layout both GPUs share is LINEAR. So instead of a plain XCreatePixmap (which
// the AMD driver backs with a tiled buffer), we allocate a LINEAR buffer on
// AMD's own device via GBM and wrap it as an X pixmap. XCopyArea then makes the
// AMD driver de-tile the framebuffer into our linear buffer on-GPU, and Intel
// can import that fd directly (as linear) without any CPU copy.
//
// libgbm ships no -devel package here, so we dlopen it and declare the few
// prototypes we need -- no new build dependency.
// ---------------------------------------------------------------------------
struct gbm_device; struct gbm_bo;
#define GBM_FORMAT_XRGB8888  0x34325258
#define GBM_BO_USE_RENDERING (1u << 2)
#define GBM_BO_USE_LINEAR    (1u << 4)

static struct {
    void* lib;
    struct gbm_device* (*create_device)(int fd);
    struct gbm_bo* (*bo_create)(struct gbm_device*, uint32_t, uint32_t, uint32_t, uint32_t);
    int (*bo_get_fd)(struct gbm_bo*);
    uint32_t (*bo_get_stride)(struct gbm_bo*);
    void (*bo_destroy)(struct gbm_bo*);
    void (*device_destroy)(struct gbm_device*);
    int amd_fd;                 // DRI3-opened fd to AMD's DRM device
    struct gbm_device* dev;
    struct gbm_bo* bo;          // the persistent linear source buffer
} gbm = { .amd_fd = -1 };

// Load libgbm and resolve the symbols we use. Returns TRUE once ready.
static gboolean gbm_load(void) {
    if (gbm.lib) return TRUE;
    gbm.lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!gbm.lib) {
        fprintf(stderr, "x11: dlopen libgbm.so.1 failed: %s\n", dlerror());
        return FALSE;
    }
    gbm.create_device  = dlsym(gbm.lib, "gbm_create_device");
    gbm.bo_create      = dlsym(gbm.lib, "gbm_bo_create");
    gbm.bo_get_fd      = dlsym(gbm.lib, "gbm_bo_get_fd");
    gbm.bo_get_stride  = dlsym(gbm.lib, "gbm_bo_get_stride");
    gbm.bo_destroy     = dlsym(gbm.lib, "gbm_bo_destroy");
    gbm.device_destroy = dlsym(gbm.lib, "gbm_device_destroy");
    if (!gbm.create_device || !gbm.bo_create || !gbm.bo_get_fd || !gbm.bo_get_stride) {
        fprintf(stderr, "x11: libgbm missing required symbols\n");
        dlclose(gbm.lib);
        gbm.lib = NULL;
        return FALSE;
    }
    return TRUE;
}

// Allocate a LINEAR source buffer on the source X server's GPU and wrap it as
// an X pixmap. On success returns the pixmap XID and fills *out_stride; the
// backing gbm_bo is retained in the gbm state (freed by gbm_free_linear).
// Returns 0 on failure so the caller can fall back to the CPU/XShm path.
static xcb_pixmap_t dri3_create_linear_pixmap(xcb_connection_t* conn, Window root,
                                              int w, int h, int depth, int* out_stride) {
    *out_stride = 0;
    if (!gbm_load()) return 0;

    // Get a usable fd for the source server's DRM device via DRI3.
    if (gbm.amd_fd < 0) {
        xcb_dri3_open_cookie_t oc = xcb_dri3_open(conn, root, 0);
        xcb_dri3_open_reply_t* orep = xcb_dri3_open_reply(conn, oc, NULL);
        if (!orep) { fprintf(stderr, "x11: xcb_dri3_open failed\n"); return 0; }
        int* ofds = xcb_dri3_open_reply_fds(conn, orep);
        gbm.amd_fd = (ofds && ofds[0] >= 0) ? ofds[0] : -1;
        free(orep);
        if (gbm.amd_fd < 0) { fprintf(stderr, "x11: DRI3 open gave no fd\n"); return 0; }
    }
    if (!gbm.dev) {
        gbm.dev = gbm.create_device(gbm.amd_fd);
        if (!gbm.dev) { fprintf(stderr, "x11: gbm_create_device failed\n"); return 0; }
    }

    uint32_t fourcc = depth_to_drm_format(depth);
    if (fourcc == 0) fourcc = GBM_FORMAT_XRGB8888;
    // Free any previous buffer before allocating a new one (a source switch
    // reallocates for the new monitor's dimensions). x11_free_source_buffer()
    // normally does this first, but guard here too so we never leak a bo.
    if (gbm.bo && gbm.bo_destroy) { gbm.bo_destroy(gbm.bo); gbm.bo = NULL; }
    gbm.bo = gbm.bo_create(gbm.dev, w, h, fourcc,
                           GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    if (!gbm.bo) { fprintf(stderr, "x11: gbm_bo_create(LINEAR) failed\n"); return 0; }

    uint32_t stride = gbm.bo_get_stride(gbm.bo);
    int fd = gbm.bo_get_fd(gbm.bo);      // fresh fd; consumed by pixmap_from_buffer
    if (fd < 0) { fprintf(stderr, "x11: gbm_bo_get_fd failed\n"); return 0; }

    xcb_pixmap_t px = xcb_generate_id(conn);
    xcb_void_cookie_t pc = xcb_dri3_pixmap_from_buffer(
        conn, px, root, stride * h, w, h, stride, depth, 32, fd);
    xcb_generic_error_t* err = xcb_request_check(conn, pc);
    if (err) {
        fprintf(stderr, "x11: pixmap_from_buffer failed (err %d)\n", err->error_code);
        free(err);
        return 0;
    }
    *out_stride = stride;
    fprintf(stderr, "x11: linear source pixmap 0x%x stride=%u (GBM)\n", px, stride);
    return px;
}

// Destroy only the linear source buffer, keeping the GBM device + DRI3 fd for
// reuse (they're re-openable but cheap to keep across a source switch).
static void gbm_free_bo(void) {
    if (gbm.bo && gbm.bo_destroy) { gbm.bo_destroy(gbm.bo); gbm.bo = NULL; }
}

static void gbm_free_linear(void) {
    gbm_free_bo();
    if (gbm.dev && gbm.device_destroy) { gbm.device_destroy(gbm.dev); gbm.dev = NULL; }
    if (gbm.amd_fd >= 0) { close(gbm.amd_fd); gbm.amd_fd = -1; }
}

// Release the per-source GPU buffer: the EGL texture, its X pixmap, and the
// GBM bo. The EGL texture is a live reference to a specific DMA-BUF; on a
// source switch the buffer is reallocated for the new monitor, so a cached
// texture would keep sampling the OLD buffer (which XCopyArea no longer
// updates) and the mirror freezes/goes black. Dropping it here forces
// x11_refresh_image to import a fresh texture from the new buffer. The GBM
// device + DRI3 fd are kept (only gbm_free_linear tears those down).
static void x11_free_source_buffer(void) {
    if (egl_ctx && egl_texture) {
        egl_delete_texture(egl_ctx, egl_texture);
        egl_texture = 0;
    }
#ifdef COPY_CURSOR
    // The cursor-overlay Picture wraps src_pixmap; drop it so the next frame
    // rebuilds it against the newly-allocated pixmap.
    if (src_pixmap_picture && src_display) {
        XRenderFreePicture(src_display, src_pixmap_picture);
        src_pixmap_picture = 0;
    }
#endif
    if (src_pixmap && src_display) {
        XFreePixmap(src_display, src_pixmap);
        src_pixmap = 0;
    }
    gbm_free_bo();
}

// Export a pixmap as a DMA-BUF fd via DRI3.
// Prefers DRI3 1.2 buffers_from_pixmap, which reports the DRM format
// *modifier* describing the GPU's tiled/compressed memory layout. Without
// the modifier the importing driver (Intel) assumes a linear layout and
// mis-reads AMD's tiled buffer -- the image comes out horizontally streaked.
// On success returns fd and fills *drm_format/*stride/*modifier. The modifier
// is DRM_FORMAT_MOD_INVALID if only the legacy (DRI3 1.0) path was available.
// Returns -1 on failure.
static int dri3_export_pixmap(xcb_connection_t* conn, xcb_pixmap_t pixmap,
                              int width, int height, uint32_t* drm_format,
                              int* stride, uint64_t* modifier) {
    // Initialize output parameters to safe defaults
    *stride = 0;
    *drm_format = 0;
    *modifier = DRM_FORMAT_MOD_INVALID;

    // Query DRI3 version to decide which export path is available.
    xcb_dri3_query_version_cookie_t qv_cookie = xcb_dri3_query_version(conn, 1, 2);
    xcb_dri3_query_version_reply_t* qv_reply = xcb_dri3_query_version_reply(conn, qv_cookie, NULL);
    if (!qv_reply || qv_reply->major_version < 1) {
        if (qv_reply) free(qv_reply);
        fprintf(stderr, "x11: DRI3 not available\n");
        return -1;
    }
    int have_1_2 = (qv_reply->major_version > 1) ||
                   (qv_reply->major_version == 1 && qv_reply->minor_version >= 2);
    free(qv_reply);

    if (have_1_2) {
        // DRI3 1.2 path: buffers_from_pixmap reports the modifier.
        xcb_dri3_buffers_from_pixmap_cookie_t c =
            xcb_dri3_buffers_from_pixmap(conn, pixmap);
        xcb_dri3_buffers_from_pixmap_reply_t* r =
            xcb_dri3_buffers_from_pixmap_reply(conn, c, NULL);
        if (r) {
            uint32_t fmt = depth_to_drm_format(r->depth);
            int* fds = xcb_dri3_buffers_from_pixmap_reply_fds(conn, r);
            uint32_t* strides = xcb_dri3_buffers_from_pixmap_strides(r);
            if (fmt && r->nfd >= 1 && fds && fds[0] >= 0) {
                if (r->nfd > 1) {
                    // Multi-plane (e.g. AMD DCC): we only import plane 0. Close
                    // the extra fds so we don't leak them. Plane 0 + modifier is
                    // still usable; DCC metadata in later planes is ignored.
                    for (int i = 1; i < r->nfd; i++)
                        if (fds[i] >= 0) close(fds[i]);
                    fprintf(stderr, "x11: DRI3 export returned %d planes; importing plane 0 only\n", r->nfd);
                }
                int fd = dup(fds[0]);
                // fds[0] is ours from _reply_fds() (same as the extra planes
                // closed above) and is never freed by free(r) -- leaving this
                // out leaked one fd per frame, exhausting the 1024 fd table
                // in a fixed ~42s and permanently breaking cross-server mode.
                close(fds[0]);
                *stride = strides ? strides[0] : 0;
                *drm_format = fmt;
                *modifier = r->modifier;
                free(r);
                return fd;
            }
            // Unsupported: close all fds and fall through to legacy path.
            if (fds) for (int i = 0; i < r->nfd; i++)
                if (fds[i] >= 0) close(fds[i]);
            free(r);
        }
        fprintf(stderr, "x11: DRI3 1.2 buffers_from_pixmap unusable, trying 1.0\n");
    }

    // Legacy DRI3 1.0 path (no modifier -- assumes linear layout).
    xcb_dri3_buffer_from_pixmap_cookie_t exp_cookie = xcb_dri3_buffer_from_pixmap(conn, pixmap);
    xcb_dri3_buffer_from_pixmap_reply_t* exp_reply = xcb_dri3_buffer_from_pixmap_reply(conn, exp_cookie, NULL);
    if (!exp_reply) {
        fprintf(stderr, "x11: dri3_buffer_from_pixmap failed\n");
        return -1;
    }

    int fd = -1;
    int* fds = xcb_dri3_buffer_from_pixmap_reply_fds(conn, exp_reply);
    if (fds && *fds >= 0) {
        // Duplicate fd so we own a stable copy. XCB does NOT close reply fds
        // on our behalf (free(exp_reply) below only frees the reply struct),
        // so the original must be closed explicitly or it leaks every frame --
        // this is the same fd-per-frame leak fixed in the DRI3 1.2 path above.
        fd = dup(fds[0]);
        close(fds[0]);
    }

    // Set output parameters (for EGLImage creation)
    *stride = exp_reply->stride;
    *drm_format = depth_to_drm_format(exp_reply->depth);
    if (*drm_format == 0) {
        fprintf(stderr, "x11: unsupported pixmap depth %d for DRI3 export\n", exp_reply->depth);
        free(exp_reply);
        if (fd >= 0) close(fd);
        return -1;
    }

    free(exp_reply);
    return fd;
}

gboolean
x11_init()
{
	display = gdk_x11_get_default_xdisplay();

	// Cross-server initialization (Phase 1)
	if (config.src_display_name || config.dst_display_name) {
		// Determine source and destination display names
		const char* src_name = config.src_display_name ? config.src_display_name : config.dst_display_name;
		const char* dst_name = config.dst_display_name ? config.dst_display_name : config.src_display_name;

		// If only one display was specified, use it for both source and destination
		if (!config.src_display_name && !config.dst_display_name) {
			// Both NULL, use default display (already open)
			use_cross_server = FALSE;
		} else if (g_strcmp0(src_name, dst_name) != 0) {
			// Different source and destination displays
			src_display = XOpenDisplay(src_name);
			if (!src_display) {
				squint_error("Failed to open source display");
				return FALSE;
			}
			use_cross_server = TRUE;

			// Get XCB connection from source display for DRI3 support
			src_xcb = XGetXCBConnection(src_display);
		} else {
			// Same display name for both (or only one specified)
			use_cross_server = FALSE;
		}

		// Check if DRI3 is available and initialize EGL if so
		if (use_cross_server && src_xcb) {
			xcb_dri3_query_version_cookie_t qv = xcb_dri3_query_version(src_xcb, 1, 0);
			xcb_dri3_query_version_reply_t* qv_reply = xcb_dri3_query_version_reply(src_xcb, qv, NULL);
			if (qv_reply && qv_reply->major_version >= 1) {
				dri3_available = TRUE;
				fprintf(stderr, "x11: DRI3 available, initializing EGL\n");
				egl_ctx = egl_init(gdisplay);
				if (!egl_ctx) {
					fprintf(stderr, "x11: EGL initialization failed, falling back to XShm\n");
					dri3_available = FALSE;
				}
			}
			if (qv_reply) free(qv_reply);
		}

#ifdef COPY_CURSOR
		// Set up the source-cursor overlay (no-op if not cross-server or if the
		// source display lacks XFixes/XRender).
		x11_init_source_cursor();
#endif
	}

	screen = DefaultScreen (display);

	depth = XDefaultDepth (display, screen);

	// get the root window
	root_window = XDefaultRootWindow(display);
	
	// create the graphic contextes
	{
		XGCValues values;
		values.subwindow_mode = IncludeInferiors;

		gc = XCreateGC (display, root_window, GCSubwindowMode, &values);
		if(!gc) {
			squint_error("XCreateGC() failed");
			return FALSE;
		}

		values.line_width = 3;
		values.foreground = 0xe0e0e0;
		gc_white = XCreateGC (display, root_window, GCLineWidth | GCForeground, &values);
		if(!gc_white) {
			squint_error("XCreateGC() failed");
			return FALSE;
		}
	}

	// NOTE: the persistent DRI3 source pixmap is NOT created here. src_rect
	// isn't populated yet at this point -- select_monitors() (which sets it)
	// runs later, after x11_init() returns. Creating it here would make a
	// zero-size pixmap that DRI3 can't export. It's created in
	// x11_enable_window() instead, which runs after select_monitors().

#ifdef HAVE_XRANDR
	x11_init_xrandr();
#endif
#ifdef COPY_CURSOR
	x11_init_copy_cursor();
#endif
#ifdef HAVE_XI
	x11_init_cursor_tracking();
#endif
#ifdef HAVE_XDAMAGE
	x11_init_xdamage();
#endif

	g_timeout_add_seconds(60, x11_heartbeat_cb, NULL);

	// atom name
	net_active_window_atom = XInternAtom(display, "_NET_ACTIVE_WINDOW", FALSE);
	if (use_cross_server && src_display)
		net_active_window_atom_src = XInternAtom(src_display, "_NET_ACTIVE_WINDOW", FALSE);

	return TRUE;
}

gboolean
x11_on_squint_window_draw(GtkWidget* widget, void* cairo_ctx, gpointer user_data)
{
	// do not propagate the event to the GtkWindow
	return TRUE;
}

// erase the previous cursor (if any)
gboolean
x11_clear_cursor()
{
	if (backup.x != -CURSOR_SIZE)
	{
		XCopyArea(display, backup_pixmap, pixmap, gc,
				0, 0,
				CURSOR_SIZE, CURSOR_SIZE,
				backup.x, backup.y);
		backup.x = -CURSOR_SIZE;
		return TRUE;
	} else {
		return FALSE;
	}
}

// draw the new cursor (if on screen)
gboolean
x11_draw_cursor()
{
	if (cursor.x >= 0)
	{
#ifdef COPY_CURSOR
		if (copy_cursor) {
			backup.x = cursor.x - cursor_xhot;
			backup.y = cursor.y - cursor_yhot;
			XCopyArea(display, pixmap, backup_pixmap, gc,
					backup.x, backup.y,
					CURSOR_SIZE, CURSOR_SIZE,
					0, 0);
			XRenderComposite(display, PictOpOver,
					cursor_picture, 0,
					pixmap_picture,
					0, 0, 0, 0,
					backup.x, backup.y, CURSOR_SIZE, CURSOR_SIZE);
		}
		else
#endif
		{
			const int len = CURSOR_CROSSHAIR_LEN;
			backup.x = cursor.x - (len+1);
			backup.y = cursor.y - (len+1);
			XCopyArea(display, pixmap, backup_pixmap, gc,
					backup.x, backup.y,
					CURSOR_SIZE, CURSOR_SIZE,
					0, 0);
			XDrawLine(display, pixmap, gc_white,
					cursor.x-(len+1), cursor.y,
					cursor.x+(len+2), cursor.y);
			XDrawLine(display, pixmap, gc_white,
					cursor.x, cursor.y-(len+1),
					cursor.x, cursor.y+(len+2));
			XDrawLine(display, pixmap, gc,
					cursor.x-len, cursor.y,
					cursor.x+len, cursor.y);
			XDrawLine(display, pixmap, gc,
					cursor.x, cursor.y-len,
					cursor.x, cursor.y+len);

		}
		return TRUE;
	} else {
		return FALSE;
	}
}

void
x11_redraw_cursor(gboolean clear_window)
{
	int cleared_x = backup.x;
	int cleared_y = backup.y;

	gboolean cleared = x11_clear_cursor();
	gboolean drawn   = x11_draw_cursor();

	if (clear_window) {
		GdkRectangle rect = {0, 0, CURSOR_SIZE, CURSOR_SIZE };
		if (drawn && cleared) {
			rect.x = MIN(backup.x, cleared_x);
			rect.y = MIN(backup.y, cleared_y);
			rect.width  += ABS(backup.x - cleared_x);
			rect.height += ABS(backup.y - cleared_y);
		}
		else if (drawn)
		{
			rect.x = backup.x;
			rect.y = backup.y;
		}
		else if (cleared)
		{
			rect.x = cleared_x;
			rect.y = cleared_y;
		}
		else
		{
			return;
		}
#ifdef HAVE_XRENDER
		if (config.opt_scale && scale_src_picture)
			x11_repaint_scaled_rect(rect.x, rect.y, rect.width, rect.height);
		else
#endif
		XClearArea(display, window, rect.x, rect.y,
				rect.width, rect.height, FALSE);
	}
}

//
// Prepare the window to host the duplicated screen (create the pixmap, subwindow)
//
// initialises:
// 	offset
// 	pixmap
// 	window
//	cursor
//

void
x11_enable_window()
{
	cursor.x = -1;
	cursor.y = -1;
	offset.x = 0;
	offset.y = 0;

	if (!fullscreen) {
		// register events
		// - window moved/resized
		g_signal_connect (gtkwin, "configure-event", G_CALLBACK (x11_on_window_configure_event), NULL);
	}

	// intercept the 'draw' event of the squint window to prevent any rendering by gtk
	g_signal_connect(gtkwin, "draw", G_CALLBACK(x11_on_squint_window_draw), NULL);

	// have the main window painted black by X11
	Window squint_window = gdk_x11_window_get_xid(gdkwin);
	XSetWindowBackground(display, squint_window, 0);

	// create the pixmap
	pixmap = XCreatePixmap (display, root_window, src_rect.width, src_rect.height, depth);

	// Create persistent pixmap for DRI3 export (cross-server mode). Done
	// here rather than x11_init() because src_rect is only valid now, after
	// select_monitors() has run.
	if (dri3_available) {
		int src_depth = DefaultDepth(src_display, DefaultScreen(src_display));
		// Allocate the source pixmap as a LINEAR GBM buffer so the AMD driver
		// de-tiles into it during XCopyArea and Intel can import it directly.
		// A plain XCreatePixmap would be tiled and import as a scrambled image.
		int lin_stride = 0;
		src_pixmap = dri3_create_linear_pixmap(src_xcb,
		                           XDefaultRootWindow(src_display),
		                           src_rect.width, src_rect.height,
		                           src_depth, &lin_stride);
		if (!src_pixmap) {
			// No linear GPU buffer -> the tiled DRI3 path would render a
			// scrambled image, so fall back to the CPU/XShm path instead.
			fprintf(stderr, "x11: linear pixmap alloc failed, falling back to XShm\n");
			dri3_available = FALSE;
		} else if (!egl_bind_window(egl_ctx, gdkwin)) {
			// Bind an EGL surface to the destination window now that gdkwin is
			// realized, and make it current -- required before any EGL/GL calls
			// (eglCreateImageKHR etc.) can be dispatched to a vendor driver.
			fprintf(stderr, "x11: egl_bind_window failed, falling back to XShm\n");
			dri3_available = FALSE;
		}
	}

#ifdef HAVE_XRENDER
	if (config.opt_scale) {
		x11_update_scale(dst_rect.width, dst_rect.height);

		// sub-window is sized to the scaled area, no background pixmap
		XSetWindowAttributes attr;
		attr.background_pixel = 0;
		window = XCreateWindow(display, squint_window,
					scaled_off_x, scaled_off_y,
					scaled_w, scaled_h,
					0, CopyFromParent,
					InputOutput, CopyFromParent,
					CWBackPixel, &attr);
		// See the DRI3/EGL comment in the non-scaled branch below: leave
		// unmapped when EGL is rendering directly onto gdkwin.
		if (!dri3_available) {
			XMapWindow(display, window);
		}

		XRenderPictFormat *fmt = XRenderFindStandardFormat(display, PictStandardRGB24);
		scale_src_picture = XRenderCreatePicture(display, pixmap, fmt, 0, NULL);
		scale_dst_picture = XRenderCreatePicture(display, window, fmt, 0, NULL);

		XTransform xform = {{
			{ XDoubleToFixed(1.0/scale_factor), 0, 0 },
			{ 0, XDoubleToFixed(1.0/scale_factor), 0 },
			{ 0, 0, XDoubleToFixed(1.0) }
		}};
		XRenderSetPictureTransform(display, scale_src_picture, &xform);
		XRenderSetPictureFilter(display, scale_src_picture, FilterBilinear, NULL, 0);
	} else
#endif
	{
		// create the sub-window
		XSetWindowAttributes attr;
		attr.background_pixmap = pixmap;
		window = XCreateWindow (display, squint_window,
					offset.x, offset.y,
					src_rect.width, src_rect.height,
					0, CopyFromParent,
					InputOutput, CopyFromParent,
					CWBackPixmap, &attr);
		// In DRI3/EGL mode, the frame is rendered by EGL directly onto
		// gdkwin (the parent). This sub-window's background is only ever
		// painted by the XShm/XCopyArea fallback path, which never runs
		// once DRI3 succeeds -- leave it unmapped so it doesn't sit as an
		// opaque black rectangle on top of the EGL-rendered content. It's
		// still created (not left NULL) since cursor-follow code guards
		// and moves/clears it regardless of render backend.
		if (!dri3_available) {
			XMapWindow(display, window);
		}
	}

	// create a backup pixmap for storing the background (below the cursor)
	backup.x = -CURSOR_SIZE;
	backup_pixmap = XCreatePixmap(display, root_window,
				CURSOR_SIZE, CURSOR_SIZE, 24);

	// force refreshing the cursor position without switching sources
	// (source switches from x11_enable_window would undo focus-based switches)
	inhibit_source_switch = TRUE;
	x11_refresh_cursor_location(TRUE);
	inhibit_source_switch = FALSE;
}

void
x11_disable_window()
{
	// Ensure all pending X11 requests are processed before freeing resources
	XSync(display, False);

	XFreePixmap(display, backup_pixmap);
	backup_pixmap = 0;
	backup.x = -CURSOR_SIZE;

#ifdef HAVE_XRENDER
	if (scale_src_picture) { XRenderFreePicture(display, scale_src_picture); scale_src_picture = 0; }
	if (scale_dst_picture) { XRenderFreePicture(display, scale_dst_picture); scale_dst_picture = 0; }
#endif

	XDestroyWindow(display, window);
	window = 0;

	XFreePixmap(display, pixmap);
	pixmap = 0;

	// Drop the source DMA-BUF/texture so a source switch (which calls
	// x11_disable_window then x11_enable_window) rebinds a fresh texture to the
	// newly-allocated source buffer instead of sampling the stale one.
	x11_free_source_buffer();
}


void
x11_enable_focus_tracking()
{
	memset(&active_window_rect, 0, sizeof(active_window_rect));

	XSetWindowAttributes attr;
	attr.event_mask = PropertyChangeMask;
	XChangeWindowAttributes(display, root_window, CWEventMask, &attr);
}

void
x11_disable_focus_tracking()
{
	XSetWindowAttributes attr;
	attr.event_mask = 0;
	XChangeWindowAttributes(display, root_window, CWEventMask, &attr);
}

void
x11_enable()
{
	x11_enable_window();

	x11_enable_focus_tracking();
	
#ifdef HAVE_XI
	x11_enable_cursor_tracking();
#endif

#ifdef COPY_CURSOR
	x11_enable_copy_cursor();
#endif

#ifdef HAVE_XDAMAGE
	x11_enable_xdamage();
#endif

	XFlush (display);

	x11_get_window_geometry(display, root_window, &root_window_rect);
	if (use_cross_server && src_display)
		x11_get_window_geometry(src_display, XDefaultRootWindow(src_display), &src_root_window_rect);
	x11_active_window_start_monitoring();

	// catch all X11 events
	gdk_window_add_filter(NULL, x11_on_x11_event, NULL);

#if HAVE_XDAMAGE && HAVE_XI
	if (!(damage && can_track_cursor))
#endif
	{
		int rate = 25; // default to 25 fps
		if(config.opt_rate > 0) {
			rate = config.opt_rate;
		} else if ((config.opt_limit > 0) && (config.opt_limit < rate)) {
			rate = config.opt_limit;
		}

		refresh_timer = g_timeout_add (1000/rate,
				G_SOURCE_FUNC(&x11_refresh_image), &src_rect);
	}

	// Redraw the window
	XClearWindow(display, gdk_x11_window_get_xid(gdkwin));
}

void
x11_disable()
{
	// Cleanup DRI3/EGL resources. src_display itself is a process-lifetime
	// connection opened once in x11_init() -- NOT closed here, since disable
	// is a pause (e.g. a RandR screen change), not a shutdown, and nothing
	// ever reopens it: closing it here left any later squint_enable() call
	// (RandR re-enable, or the tray-menu toggle in cross-server mode)
	// dereferencing a NULL src_display in x11_enable_window()'s DRI3 setup.
	//
	// The source-cursor overlay (src_cursor_picture/gc/pixmap/ximage, set up
	// once by x11_init_source_cursor() in x11_init()) is the same kind of
	// process-lifetime state and is deliberately NOT torn down here either --
	// it doesn't reference the destination window this function is about to
	// destroy, and nothing ever calls x11_init_source_cursor() a second time
	// to rebuild it. Tearing it down here silently disabled the mirrored
	// cursor after the first RandR-triggered disable/re-enable cycle. Only
	// src_pixmap_picture (the Picture wrapping src_pixmap, freed below via
	// x11_disable_window() -> x11_free_source_buffer()) needs to go, since
	// src_pixmap itself is reallocated fresh on every enable.
	if (src_pixmap) {
		XFreePixmap(src_display, src_pixmap);
		src_pixmap = 0;
	}
	gbm_free_linear();
	if (egl_ctx) {
		// egl_ctx is also process-lifetime (created once in x11_init(),
		// same as src_display above) -- egl_cleanup() would free its GL
		// context/shader/VAO/VBO, which nothing ever recreates, leaving
		// the next x11_enable_window()'s egl_bind_window() call to
		// dereference a stale ctx. Only the window-bound surface needs to
		// go, since the window itself is being destroyed; delete the
		// texture first since it lives in the context we're keeping.
		if (egl_texture) {
			egl_delete_texture(egl_ctx, egl_texture);
			egl_texture = 0;
		}
		egl_unbind_window(egl_ctx);
	}

#ifdef HAVE_XDAMAGE
	if (refresh_timeout) {
		g_source_remove(refresh_timeout);
		refresh_timeout = 0;
	}
#endif
	if (refresh_timer) {
		g_source_remove(refresh_timer);
		refresh_timer = 0;
	}

	gdk_window_remove_filter(NULL, x11_on_x11_event, NULL);

	x11_active_window_stop_monitoring();

#ifdef HAVE_XI
	x11_disable_cursor_tracking();
#endif

#ifdef COPY_CURSOR
	x11_disable_copy_cursor();
#endif

#ifdef HAVE_XDAMAGE
	x11_disable_xdamage();
#endif
	x11_disable_focus_tracking();

	x11_disable_window();
}

void
x11_switch_source()
{
#ifdef COPY_CURSOR
	x11_disable_copy_cursor();
#endif
	x11_disable_window();
	x11_enable_window();
#ifdef COPY_CURSOR
	x11_enable_copy_cursor();
#endif
	XClearWindow(display, gdk_x11_window_get_xid(gdkwin));
}
