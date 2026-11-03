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
	const char* kms_device;

	gboolean opt_version, opt_window, opt_disable, opt_passive, opt_scale, opt_list_monitors, opt_warp_cursor, opt_perf_log, opt_wayland_source, opt_kms_source;
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
void x11_watch_screen_changes(void);

// Non-X11 source capture backends. Sources are plain rectangles (only their
// relative layout and size matter); the backend renders the active one into
// gdkwin with EGL and drives cursor-follow via squint_switch_source/show/hide.
typedef struct {
	const char* name;
	gboolean (*init)(void);
	int (*get_sources)(GdkRectangle** rects);  // g_new'd array, returns count
	void (*enable)(void);
	void (*disable)(void);
	void (*switch_source)(int idx);
	gboolean self_enable;  // backend calls squint_enable() itself when ready
} capture_backend_t;

extern const capture_backend_t* capture_backend;  // NULL = X11 source

#ifdef HAVE_PIPEWIRE
extern const capture_backend_t wl_backend;  // wl_capture.c: portal + PipeWire
#endif
#ifdef HAVE_KMS
extern const capture_backend_t kms_backend; // kms_capture.c: scanout readback
#endif
