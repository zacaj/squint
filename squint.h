#include <gtk/gtk.h>
#include <gdk/gdk.h>

// Forward declaration of EGL context (defined in egl.c)
typedef struct egl_context egl_context_t;


// Config
extern struct config {
	const char* src_monitor_name;
	const char* dst_monitor_name;
	const char* src_display_name;
	const char* dst_display_name;
	const char* portal_id;

	gboolean opt_version, opt_window, opt_disable, opt_passive, opt_scale, opt_list_monitors, opt_warp_cursor, opt_perf_log, opt_wayland_source;
	gint opt_limit, opt_rate, opt_switch_threshold;
} config;


// State
extern gboolean enabled;
extern gboolean fullscreen;
extern gboolean raised;

extern GtkWidget* gtkwin;
extern GdkWindow* gdkwin;
extern GdkDisplay* gdisplay;

extern GdkRectangle src_rect, dst_rect, active_window_rect;

extern GdkRectangle* src_rects_list;
extern int n_src_monitors;

void squint_show();
void squint_hide();
gboolean squint_enable();
void squint_disable(const char* reason);
void squint_switch_source(int idx);

void squint_error(const char* msg);

gboolean x11_init();
void x11_enable();
void x11_disable();
void x11_switch_source();
void x11_reposition_window(GdkWindow* win, int x, int y);

// Wayland source backend (wl_capture.c): portal ScreenCast + PipeWire
#ifdef HAVE_PIPEWIRE
gboolean wl_init(void);
int wl_get_sources(GdkRectangle** rects);
void wl_enable(void);
void wl_disable(void);
void wl_switch_source(int idx);
#endif
