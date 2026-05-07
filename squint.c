#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

#ifdef HAVE_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#endif

#include "squint.h"

// Config
struct config config;

// State
gboolean enabled = FALSE;
gboolean fullscreen = FALSE;
gboolean raised = FALSE;

GtkWidget* gtkwin = NULL;
GdkWindow* gdkwin = NULL;
GdkDisplay* gdisplay = NULL;

GdkRectangle src_rect, dst_rect, active_window_rect;

GdkRectangle* src_rects_list = NULL;
int n_src_monitors = 0;
static GdkMonitor** src_monitors_list = NULL;

static GdkMonitor* src_monitor = NULL;
static GdkMonitor* dst_monitor = NULL;

static GApplication* gtkapp = NULL;
static GdkPixbuf* icon = NULL;
static GIcon* gicon = NULL;

static GdkCursor* cursor_icon = NULL;


#ifdef HAVE_APPINDICATOR
static AppIndicator* app_indicator = NULL;
static struct
{
	GtkMenuShell* shell;
	int update_index;
} menu
= {NULL, 0};

void refresh_app_indicator();
#endif

gboolean squint_enable();

void
show_about_dialog()
{
	gtk_show_about_dialog(
		NULL,
		"copyright",	"© 2013-2021 Anthony Baire",
		"license-type",	GTK_LICENSE_GPL_3_0,
		"logo",		icon,
		"program-name",	APPNAME " " VERSION,
		"website",	"https://github.com/a-ba/squint",
		"website-label","https://github.com/a-ba/squint",
		NULL
	);
}

void
squint_error(const char* msg)
{
	if (g_application_get_is_registered(gtkapp)) {
		GNotification* notif = g_notification_new(APPNAME " error");
		g_notification_set_body(notif, msg);
		g_notification_set_icon(notif, gicon);
		g_application_send_notification(gtkapp, NULL, notif);
	} else {
		fprintf(stderr, "error: %s\n", msg);
	}
}

void
squint_show()
{
	if (!raised)
	{
		raised = TRUE;
		if (fullscreen) {
			gtk_widget_show(gtkwin);
		} else if (!config.opt_passive) {
			gdk_window_raise(gdkwin);
		}
	}
}

void
do_hide()
{
	raised = FALSE;
	if (fullscreen) {
		gtk_widget_hide(gtkwin);
	} else if (!config.opt_passive) {
		gdk_window_lower(gdkwin);
	}
}

void
squint_hide()
{
	if(raised)
	{
		do_hide();
	}
}


gboolean
on_window_button_press_event(GtkWidget* widget, GdkEvent* event, gpointer data)
{
	GdkEventButton* ev = (GdkEventButton*) event;

	if (enabled
		&& (ev->type == GDK_2BUTTON_PRESS)
		&& (ev->button = 1)
	) {
		gdk_window_unmaximize(gdkwin);
		gdk_window_resize(gdkwin, src_rect.width, src_rect.height);
	}
	return FALSE;
}

gboolean
on_window_delete_event(GtkWidget* widget, GdkEvent* event, gpointer data)
{
	squint_disable();
	return TRUE;
}

#define ITEM_MASK		0xffffff00
#define ITEM_ENABLE		(1<<8)
#define ITEM_FULLSCREEN		(1<<9)
#define ITEM_QUIT		(1<<10)
#define ITEM_SRC_MONITOR	(1<<11)
#define ITEM_DST_MONITOR	(1<<12)
#define ITEM_ABOUT		(1<<13)
#define ITEM_PASSIVE		(1<<14)
#define ITEM_AUTO		0xff

void
update_monitor_config(const char** monitor_name, int id)
{
	if (*monitor_name) {
		g_free((gpointer)*monitor_name);
	}
	*monitor_name = NULL;

	if (id != ITEM_AUTO)
	{
		GdkMonitor* monitor = gdk_display_get_monitor(gdisplay, id);
		*monitor_name = g_strdup(gdk_monitor_get_model(monitor));
	}
}

#ifdef HAVE_APPINDICATOR
void
on_menu_item_activate(gpointer pointer, gpointer user_data)
{
	if (menu.update_index >= 0) {
		return;
	}

	intptr_t code = (intptr_t)user_data;
	switch (code & ITEM_MASK)
	{
	case ITEM_ENABLE:
		if (enabled) {
			squint_disable();
		} else {
			squint_enable();
		}
		break;

	case ITEM_PASSIVE:
		config.opt_passive = !config.opt_passive;
		goto reset;

	case ITEM_FULLSCREEN:
		config.opt_window = !config.opt_window;
		goto reset;

	case ITEM_QUIT:
		g_application_release(gtkapp);
		break;
	
	case ITEM_SRC_MONITOR:
		update_monitor_config(&config.src_monitor_name, code & ITEM_AUTO);
		goto reset;
		
	case ITEM_DST_MONITOR:
		update_monitor_config(&config.dst_monitor_name, code & ITEM_AUTO);
		goto reset;
	
	case ITEM_ABOUT:
		show_about_dialog();
		break;
	}
	return;
reset:
	if (enabled) {
		squint_disable();
		squint_enable();
	} else {
		refresh_app_indicator();
	}
}

void
connect_menu_item(GtkWidget* item, intptr_t user_data) {
	g_signal_connect(item, "activate", G_CALLBACK (on_menu_item_activate), (gpointer) user_data);
}

void
populate_menu_with_monitors(int index, const char* config_name, GdkMonitor* active_monitor, intptr_t userdata)
{
	void append(int* index, GtkWidget* item) {
		if (*index < 0) {
			gtk_menu_shell_append(menu.shell, item);
		} else {
			gtk_menu_shell_insert(menu.shell, item, (*index)++);
		}
	}

	// Auto button
	GtkWidget* auto_item = gtk_check_menu_item_new_with_label("Auto");
	{
		if (config_name == NULL) {
			gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(auto_item), TRUE);
		}
		connect_menu_item(auto_item, userdata | ITEM_AUTO);
		append(&index, auto_item);
	}

	int i, n = gdk_display_get_n_monitors(gdisplay);
	gboolean found = FALSE;
	char buff[64];
	GtkWidget* item;
	for (i=0 ; i<n ; i++)
	{
		GdkMonitor* monitor = gdk_display_get_monitor(gdisplay, i);
		const char* name = gdk_monitor_get_model(monitor);

		GdkRectangle r;
		gdk_monitor_get_geometry(monitor, &r);
		g_snprintf(buff, 64, "%s %d×%d", name , r.width, r.height);

		item = gtk_check_menu_item_new_with_label(buff);
		connect_menu_item(item, userdata | (i & 0xff));
		append(&index, item);

		if (config_name == NULL) {
			if (enabled && monitor==active_monitor) {
				g_snprintf(buff, 64, "Auto (%s)", name);
				gtk_menu_item_set_label(GTK_MENU_ITEM(auto_item), buff);
			}
		} else if (!strcmp(config_name, name)) {
			found = TRUE;
			gtk_check_menu_item_set_active(
					GTK_CHECK_MENU_ITEM(item), TRUE);
		}
	}
	if (!found && config_name) {
		// choosen monitor is not active
		item = gtk_check_menu_item_new_with_label(config_name);
		gtk_check_menu_item_set_active(
			GTK_CHECK_MENU_ITEM(item), TRUE);
		append(&index, item);
	}
	gtk_widget_show_all(GTK_WIDGET(menu.shell));
}

void
init_app_indicator()
{
	// initialise the menu
	GtkWidget* item;
	menu.shell = GTK_MENU_SHELL(gtk_menu_new());

	// enabled
	item = gtk_check_menu_item_new_with_label("Enabled");
	connect_menu_item(item, ITEM_ENABLE);
	gtk_menu_shell_append(menu.shell, item);
	GtkWidget* enabled_item = item;

	// fullscreen
	item = gtk_check_menu_item_new_with_label("Fullscreen");
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), !config.opt_window);
	connect_menu_item(item, ITEM_FULLSCREEN);
	gtk_menu_shell_append(menu.shell, item);

	// passive
	item = gtk_check_menu_item_new_with_label("Passive");
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), config.opt_passive);
	connect_menu_item(item, ITEM_PASSIVE);
	gtk_menu_shell_append(menu.shell, item);

	// about
	item = gtk_menu_item_new_with_label("About");
	connect_menu_item(item, ITEM_ABOUT);
	gtk_menu_shell_append(menu.shell, item);

	// quit
	item = gtk_menu_item_new_with_label("Quit");
	connect_menu_item(item, ITEM_QUIT);
	gtk_menu_shell_append(menu.shell, item);


	// monitors
	const char*  mon_labels[] = {"Source monitor", "Destination monitor"};
	const intptr_t mon_data[] = {ITEM_SRC_MONITOR, ITEM_DST_MONITOR};
	for (int i=0; i<2 ; i++) {
		gtk_menu_shell_append(menu.shell, gtk_separator_menu_item_new());

		item = gtk_menu_item_new_with_label(mon_labels[i]);
		gtk_widget_set_sensitive(item, FALSE);
		gtk_menu_shell_append(menu.shell, item);

		item = gtk_check_menu_item_new_with_label("Auto");
		connect_menu_item(item, mon_data[i] | ITEM_AUTO);
		gtk_menu_shell_append(menu.shell, item);
	}

	// initialise the app_indicator
	app_indicator = app_indicator_new_with_path("squint", "",
			APP_INDICATOR_CATEGORY_APPLICATION_STATUS,
			PREFIX "/share/squint");
	app_indicator_set_status(app_indicator, APP_INDICATOR_STATUS_ACTIVE);
	app_indicator_set_secondary_activate_target(app_indicator, enabled_item);

	g_object_ref(menu.shell);
	app_indicator_set_menu(app_indicator, GTK_MENU(menu.shell));
}

void
refresh_app_indicator()
{
	void each_menu_item(GtkWidget* item, gpointer cb_data)
	{
		switch (menu.update_index++) {
		case 0:
			// enabled button
			gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), enabled);
			break;
		case 1:
			// fullscreen button
			gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), !config.opt_window);
			break;
		case 2:
			// passive button
			gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), config.opt_passive);

			// disable the button if running in fullscreen mode (because it has no effects)
			gtk_widget_set_sensitive(item, config.opt_window);
			break;
		default:
			// delete all other GtkTypeCheckMenuItem objects
			if (G_OBJECT_TYPE(item) == GTK_TYPE_CHECK_MENU_ITEM) {
				gtk_container_remove(GTK_CONTAINER(menu.shell), item);
			}
		}
	}

	if (enabled) {
		app_indicator_set_icon_full(app_indicator, "squint",          "squint enabled");
	} else {
		app_indicator_set_icon_full(app_indicator, "squint-disabled", "squint disabled");
	}

	menu.update_index = 0;
	gtk_container_foreach(GTK_CONTAINER(menu.shell), each_menu_item, NULL);
	populate_menu_with_monitors(-1, config.dst_monitor_name, dst_monitor, ITEM_DST_MONITOR);
	populate_menu_with_monitors(6,  config.src_monitor_name, src_monitor, ITEM_SRC_MONITOR);
	menu.update_index = -1;

}
#endif


gboolean
init()
{
	gtkapp = G_APPLICATION(gtk_application_new("org.github.a-ba.squint",
				G_APPLICATION_NON_UNIQUE));
	g_application_hold(gtkapp);


	gdisplay = gdk_display_get_default();
	if (!gdisplay) {
		squint_error("No display available");
		return FALSE;
	}

	{
		GError* err = NULL;

		// Try to load the icon from the executable directory first, then fall back to PREFIX
		char* icon_path = NULL;
		gboolean icon_found = FALSE;

		// Try loading from the same directory as the executable
		{
			char exe_path[PATH_MAX];
			ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
			if (len > 0) {
				exe_path[len] = '\0';
				char* dir = g_path_get_dirname(exe_path);
				icon_path = g_build_filename(dir, "squint.png", NULL);
				if (g_file_test(icon_path, G_FILE_TEST_EXISTS)) {
					icon_found = TRUE;
				} else {
					g_free(icon_path);
				}
				g_free(dir);
			}
		}

		// Fall back to PREFIX path
		if (!icon_found) {
			icon_path = g_strdup(PREFIX "/share/squint/squint.png");
		}

		icon = gdk_pixbuf_new_from_file(icon_path, &err);
		if (!icon)
		{
			squint_error(err->message);
			g_clear_error(&err);
		}
		gicon = g_file_icon_new(g_file_new_for_path(icon_path));
		g_free(icon_path);
	}
	cursor_icon = gdk_cursor_new_for_display(gdisplay, GDK_X_CURSOR);

	gboolean result = x11_init();

#ifdef HAVE_APPINDICATOR
	if (result) {
		// create the status icon in the tray
		init_app_indicator();
		refresh_app_indicator();
	}
#endif
	return result;
}


gboolean select_monitor(GdkMonitor** mon, GdkRectangle* rect, GdkMonitor* candidate)
{
	g_assert_null(*mon);

	if (candidate == NULL) {
		return FALSE;
	} else {
		gdk_monitor_get_geometry(candidate, rect);
		*mon = candidate;
		g_object_ref(*mon);
		return TRUE;
	}
}

void unselect_monitor(GdkMonitor** mon, GdkRectangle* rect)
{
	if (*mon != NULL) {
		g_object_unref(*mon);
		*mon = NULL;
	}
	memset(rect, 0, sizeof(GdkRectangle));
}

gboolean
select_monitor_by_name(GdkDisplay* dsp, const char* name,
		GdkMonitor** mon, GdkRectangle* rect)
{
	int n = gdk_display_get_n_monitors(dsp);
	int i;
	for (i=0 ; i<n ; i++)
	{
		GdkMonitor* candidate_mon = gdk_display_get_monitor(dsp, i);

		if (strcmp(name, gdk_monitor_get_model(candidate_mon)) == 0) {
			return select_monitor(mon, rect, candidate_mon);
		}
	}

	char buff[128];
	g_snprintf(buff, 128, "Monitor %s is not active", name);
	squint_error(buff);
	return FALSE;
}

gboolean
select_rightmost_monitor_but(GdkDisplay* dsp, GdkMonitor** mon, GdkRectangle* rect,
		const GdkRectangle* other_rect)
{
	int n = gdk_display_get_n_monitors(dsp);
	int i;
	GdkMonitor* found_monitor = NULL;
	GdkRectangle found_rect;
	for (i=0 ; i<n ; i++)
	{
		GdkRectangle candidate_rect;
		GdkMonitor* candidate_mon = gdk_display_get_monitor(dsp, i);
		gdk_monitor_get_geometry(candidate_mon, &candidate_rect);
		if (!other_rect || memcmp(&candidate_rect, other_rect, sizeof(GdkRectangle)))
		{
			if ((found_monitor == NULL) ||
			    (candidate_rect.x+candidate_rect.width > found_rect.x+found_rect.width))
			{
				memcpy(&found_rect, &candidate_rect, sizeof(GdkRectangle));
				found_monitor = candidate_mon;
			}
		}
	}
	return select_monitor(mon, rect, found_monitor);
}

gboolean
select_any_monitor_but(GdkDisplay* dsp, GdkMonitor** mon, GdkRectangle* rect, const GdkRectangle* other_rect)
{
	int n = gdk_display_get_n_monitors(dsp);
	int i;
	for (i=0 ; i<n ; i++)
	{
		GdkMonitor*  candidate_mon = gdk_display_get_monitor(dsp, i);
		GdkRectangle candidate_rect;
		gdk_monitor_get_geometry(candidate_mon, &candidate_rect);
		if (!other_rect || memcmp(&candidate_rect, other_rect, sizeof(GdkRectangle)))
		{
			return select_monitor(mon, rect, candidate_mon);
		}
	}
	return FALSE;
}


// If *name is a positive integer string, replace it with the name of that monitor (1-based).
static void
resolve_monitor_number(const char** name)
{
	if (!*name) return;
	char* end;
	long idx = strtol(*name, &end, 10);
	if (*end != '\0' || idx < 1) return; // not a plain integer
	int n = gdk_display_get_n_monitors(gdisplay);
	if (idx > n) {
		char buff[64];
		g_snprintf(buff, sizeof(buff), "Monitor number %ld is out of range (1-%d)", idx, n);
		squint_error(buff);
		return;
	}
	GdkMonitor* mon = gdk_display_get_monitor(gdisplay, (int)(idx - 1));
	g_free((gpointer)*name);
	*name = g_strdup(gdk_monitor_get_model(mon));
}

static void
free_src_monitors_list()
{
	if (src_monitors_list) {
		for (int i = 0; i < n_src_monitors; i++) {
			if (src_monitors_list[i]) g_object_unref(src_monitors_list[i]);
		}
		g_free(src_monitors_list);
		src_monitors_list = NULL;
	}
	g_free(src_rects_list);
	src_rects_list = NULL;
	n_src_monitors = 0;
}

//
// select which monitor is going to be duplicated
//
// initialises:
// 	src_rect
// 	src_monitor
// 	src_rects_list
// 	n_src_monitors
// 	dst_rect
// 	dst_monitor
gboolean
select_monitors()
{
	int n;
	unselect_monitor(&src_monitor, &src_rect);
	unselect_monitor(&dst_monitor, &dst_rect);
	free_src_monitors_list();

	n = gdk_display_get_n_monitors (gdisplay);
	if ((n < 2) && !config.src_monitor_name) {
		squint_error("There is only one monitor. What am I supposed to do?");
		return FALSE;
	}

	// parse comma-separated source monitor names
	if (config.src_monitor_name) {
		gchar** names = g_strsplit(config.src_monitor_name, ",", -1);
		int count = g_strv_length(names);

		src_monitors_list = g_new0(GdkMonitor*, count);
		src_rects_list = g_new0(GdkRectangle, count);

		for (int i = 0; i < count; i++) {
			// own a fresh copy so resolve_monitor_number can free/replace it safely
			gchar* name = g_strstrip(g_strdup(names[i]));
			resolve_monitor_number((const char**)&name);
			GdkMonitor* mon = NULL;
			GdkRectangle rect;
			memset(&rect, 0, sizeof(rect));
			gboolean ok = select_monitor_by_name(gdisplay, name, &mon, &rect);
			g_free(name);
			if (!ok) {
				g_strfreev(names);
				free_src_monitors_list();
				return FALSE;
			}
			src_monitors_list[n_src_monitors] = mon;
			src_rects_list[n_src_monitors] = rect;
			n_src_monitors++;
		}
		g_strfreev(names);

		// set current source to the first in the list
		src_monitor = g_object_ref(src_monitors_list[0]);
		src_rect = src_rects_list[0];
	}

	if (config.dst_monitor_name
		&& !select_monitor_by_name(gdisplay, config.dst_monitor_name, &dst_monitor, &dst_rect)) {
			return FALSE;
	}
	if (src_monitor && dst_monitor && !memcmp(&src_rect, &dst_rect, sizeof(GdkRectangle)))
	{
		squint_error("Source and destination both map the same screen area");
		return FALSE;
	}

	// if the source monitor is not yet decided, then use the rightmost monitor
	if (src_monitor == NULL) {
		select_rightmost_monitor_but(gdisplay, &src_monitor, &src_rect,
				(dst_monitor ? &dst_rect : NULL));
		// single auto-detected source: build a 1-entry list
		if (src_monitor) {
			src_monitors_list = g_new(GdkMonitor*, 1);
			src_rects_list = g_new(GdkRectangle, 1);
			src_monitors_list[0] = g_object_ref(src_monitor);
			src_rects_list[0] = src_rect;
			n_src_monitors = 1;
		}
	}

	// if the destination_monitor is not yet decided, then use the first unused monitor
	if (dst_monitor == NULL) {
		select_any_monitor_but(gdisplay, &dst_monitor, &dst_rect,
				(src_monitor ? &src_rect : NULL));
	}

	if (src_monitor && dst_monitor) {
		return TRUE;
	} else {
		unselect_monitor(&src_monitor, &src_rect);
		unselect_monitor(&dst_monitor, &dst_rect);
		free_src_monitors_list();

		squint_error("Could not find any monitor to be cloned");
		return FALSE;
	}
}

void
squint_switch_source(int idx)
{
	g_assert(idx >= 0 && idx < n_src_monitors);
	if (src_monitor) g_object_unref(src_monitor);
	src_monitor = g_object_ref(src_monitors_list[idx]);
	src_rect = src_rects_list[idx];
	x11_switch_source();
	squint_show();
}

//
// Prepare the window to host the duplicated screen (create the pixmap, subwindow)
//
// initialises:
// 	gtkwin
// 	gdkwin
//	fullscreen
//
void
enable_window()
{
	fullscreen = !config.opt_window;

	if (fullscreen)
	{
		// create the window
		gtkwin = gtk_window_new (GTK_WINDOW_POPUP);

		// resize it to the dimensions of the dst monitor
		gtk_window_resize(GTK_WINDOW(gtkwin),
				dst_rect.width, dst_rect.height);

		// move the window into the cover the destination screen
		gtk_window_move(GTK_WINDOW(gtkwin), dst_rect.x, dst_rect.y);

		// create the gdkwindow
		gtk_widget_realize(gtkwin);
		gdkwin = gtk_widget_get_window(gtkwin);
	} else {
		// create the window
		gtkwin = gtk_window_new (GTK_WINDOW_TOPLEVEL);
		gtk_window_set_resizable(GTK_WINDOW(gtkwin), TRUE);
		if (icon) {
			gtk_window_set_icon (GTK_WINDOW(gtkwin), icon);
		}

		// resize the window
		int w = src_rect.width;
		int max_w = dst_rect.width - 100;
		int h = src_rect.height;
		int max_h = dst_rect.height - 100;
		gtk_window_resize(GTK_WINDOW(gtkwin),
			((w < max_w) ? w : max_w),
			((h < max_h) ? h : max_h));

		// move the window into the destination screen
		gtk_window_move(GTK_WINDOW(gtkwin), dst_rect.x+50, dst_rect.y+50);

		// map my window
		gtk_widget_show (gtkwin);
		gdkwin = gtk_widget_get_window(gtkwin);

		// register events
		// - disable on window closed
		g_signal_connect (gtkwin, "delete-event", G_CALLBACK (on_window_delete_event), NULL);

		// - resize the window to src monitor size on double click
		g_signal_connect (gtkwin, "button-press-event", G_CALLBACK (on_window_button_press_event), NULL);
		gdk_window_set_events (gdkwin, gdk_window_get_events(gdkwin) | GDK_BUTTON_PRESS_MASK);
	}

	// do not get the focus when the window is raised
	gdk_window_set_accept_focus(gdkwin, FALSE);

	// override the cursor icon
	gdk_window_set_cursor(gdkwin, cursor_icon);

	// hide the window for the moment
	do_hide();
}

void
disable_window()
{
	gtk_widget_destroy(gtkwin);
	gdkwin = NULL;
	gtkwin = NULL;
}

gboolean
squint_enable()
{
	if (!enabled)
	{
		if(select_monitors())
		{
			enable_window();

			x11_enable();

			enabled = TRUE;
		}
#ifdef HAVE_APPINDICATOR
		refresh_app_indicator();
#endif
	}
	return enabled;
}

void
squint_disable()
{
	enabled = FALSE;

	x11_disable();

	disable_window();

#ifdef HAVE_APPINDICATOR
	refresh_app_indicator();
#endif
}


GOptionEntry option_entries[] = {
  { "disable",	'd',	0,	G_OPTION_ARG_NONE,	&config.opt_disable,	"Do not enable screen duplication at startup", NULL},
  { "limit",	'l',	0,	G_OPTION_ARG_INT,	&config.opt_limit,	"Limit refresh rate to N frames per second", "N"},
  { "list-monitors",	'L',	0,	G_OPTION_ARG_NONE,	&config.opt_list_monitors,	"Print a numbered list of available monitors and exit", NULL},
  { "no-warp-cursor",	0,	0,	G_OPTION_ARG_NONE,	&config.opt_warp_cursor,	"Do not warp cursor on focus change to different monitor", NULL},
  { "passive",	'p',	0,	G_OPTION_ARG_NONE,	&config.opt_passive,	"Do not raise the window on user activity (has no effects in fullscreen mode)", NULL},
  { "rate",	'r',	0,	G_OPTION_ARG_INT,	&config.opt_rate,	"Use fixed refresh rate of N frames per second", "N"},
  { "version",	'v',	0,	G_OPTION_ARG_NONE,	&config.opt_version,	"Display version information and exit", NULL},
  { "scale",	's',	0,	G_OPTION_ARG_NONE,	&config.opt_scale,	"Scale the source screen to fit the display area (requires XRender)", NULL},
  { "window",	'w',	0,	G_OPTION_ARG_NONE,	&config.opt_window,	"Run inside a window instead of going fullscreen", NULL},
  { "src",	'S',	0,	G_OPTION_ARG_STRING,	&config.src_monitor_name,	"Source monitor name(s) or number(s), comma-separated (see --list-monitors)", "MONITOR[,MONITOR...]"},
  { "dst",	'D',	0,	G_OPTION_ARG_STRING,	&config.dst_monitor_name,	"Destination monitor name or number (see --list-monitors)", "MONITOR"},
  { NULL }
};

void
list_monitors()
{
	int n = gdk_display_get_n_monitors(gdisplay);
	if (n == 0) {
		printf("No monitors found\n");
		return;
	}

	printf("Available monitors:\n");
	for (int i = 0; i < n; i++) {
		GdkMonitor* mon = gdk_display_get_monitor(gdisplay, i);
		const char* name = gdk_monitor_get_model(mon);
		GdkRectangle rect;
		gdk_monitor_get_geometry(mon, &rect);
		printf("%d: %s %dx%d @(%d, %d)\n", i + 1, name, rect.width, rect.height, rect.x, rect.y);
	}
}

int
main (int argc, char *argv[])
{
	GError *err = NULL;
	GOptionContext *context;

	memset(&config, 0, sizeof(config));
	config.opt_limit = -1;
	config.opt_warp_cursor = TRUE;

	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, option_entries, NULL);
	g_option_context_add_group (context, gtk_get_option_group (TRUE));

	if (!gtk_init_with_args (&argc, &argv, "[SourceMonitor|N [DestinationMonitor|N]]", option_entries, NULL, &err))
	{
		squint_error(err->message);
		return 1;
	}

	g_option_context_free(context);

	if (config.opt_version) {
		puts(APPNAME " " VERSION);
		return 0;
	}

	// initialisation
	if (!init()) {
		return 1;
	}

	if (config.opt_list_monitors) {
		list_monitors();
		return 0;
	}

	// TODO: manage args w/ GApplication
	switch (argc)
	{
		case 3:
			if (strcmp("-", argv[2])) {
				config.dst_monitor_name = g_strdup(argv[2]);
			}
		case 2:
			if (strcmp("-", argv[1])) {
				config.src_monitor_name = g_strdup(argv[1]);
			}
		case 1:
			break;
		default:
			squint_error("invalid arguments");
			return 1;
	}

	// initialisation
	if (!init()) {
		return 1;
	}

	// Resolve numeric monitor specs now that gdisplay is available
	resolve_monitor_number(&config.src_monitor_name);
	resolve_monitor_number(&config.dst_monitor_name);

	// activation
	if (!config.opt_disable) {
		squint_enable();
	}

	return g_application_run(gtkapp, argc, argv);
}
