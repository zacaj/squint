# Cross-Server Mirroring Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable Squint to mirror across two independent X11 display servers, with XShmGetImage/XShmPutImage fallback for Xephyr testing (no GPU work needed in Phase 1).

**Architecture:** Add `--src-display` and `--dst-display` CLI flags to specify separate X11 servers. Extend initialization to open both displays independently. Create a cross-server rendering path that uses XShmGetImage (read from source pixmap) + XShmPutImage (write to dest window) when not using DRI3. Maintain 100% backward compatibility: when flags are omitted, use the existing single-server path unchanged.

**Tech Stack:** X11, XShm (MIT-SHM), XDamage, GTK3, Xephyr (nested X server for testing)

---

## File Structure

### Modified Files
- **squint.h** — Add src_display_str, dst_display_str config fields
- **squint.c** — Add CLI flags, initialize both displays, add config validation
- **x11.c** — Add cross-server rendering path (XShm), routing logic in x11_refresh_image

### New Files
- **tools/xephyr-harness.sh** — Spawn nested Xephyr servers and test content
- **docs/superpowers/plans/YYYY-MM-DD-cross-server-phase1.md** — This plan

---

## Implementation Tasks

### Task 1: Add config fields for cross-server display names

**Files:**
- Modify: `squint.h:5-14` (config struct)
- Modify: `squint.c:18-20` (global config init)

- [ ] **Step 1: Update squint.h config struct**

Add two new string fields to store the source and destination display names:

```c
// Config
extern struct config {
	const char* src_monitor_name;
	const char* dst_monitor_name;
	const char* src_display_name;  // new: X11 display name for source (e.g., ":10")
	const char* dst_display_name;  // new: X11 display name for dest (e.g., ":11")

	gboolean opt_version, opt_window, opt_disable, opt_passive, opt_scale, opt_list_monitors, opt_warp_cursor;
	gint opt_limit, opt_rate, opt_switch_threshold;
} config;
```

- [ ] **Step 2: Initialize new fields in squint.c**

The `config` global is already zero-initialized via `memset` in main(), so no additional init code needed. Verify in squint.c main() that memset still applies.

- [ ] **Step 3: Commit**

```bash
git add squint.h squint.c
git commit -m "config: add src_display_name and dst_display_name fields"
```

---

### Task 2: Add CLI flags for --src-display and --dst-display

**Files:**
- Modify: `squint.c:809-823` (option_entries array)

- [ ] **Step 1: Add two new GOptionEntry rows to option_entries**

Insert before the final NULL entry:

```c
GOptionEntry option_entries[] = {
  { "disable",	'd',	0,	G_OPTION_ARG_NONE,	&config.opt_disable,	"Do not enable screen duplication at startup", NULL},
  { "limit",	'l',	0,	G_OPTION_ARG_INT,	&config.opt_limit,	"Limit refresh rate to N frames per second", "N"},
  { "list-monitors",	'L',	0,	G_OPTION_ARG_NONE,	&config.opt_list_monitors,	"Print a numbered list of available monitors and exit", NULL},
  { "no-warp-cursor",	0,	0,	G_OPTION_ARG_NONE,	&config.opt_warp_cursor,	"Do not warp cursor on focus change to different monitor", NULL},
  { "passive",	'p',	0,	G_OPTION_ARG_NONE,	&config.opt_passive,	"Do not raise the window on user activity (has no effects in fullscreen mode)", NULL},
  { "rate",	'r',	0,	G_OPTION_ARG_INT,	&config.opt_rate,	"Use fixed refresh rate of N frames per second", "N"},
  { "src-display",	0,	0,	G_OPTION_ARG_STRING,	&config.src_display_name,	"X11 display for source (default: $DISPLAY)", "DISPLAY"},
  { "dst-display",	0,	0,	G_OPTION_ARG_STRING,	&config.dst_display_name,	"X11 display for mirror window (default: same as source)", "DISPLAY"},
  { "version",	'v',	0,	G_OPTION_ARG_NONE,	&config.opt_version,	"Display version information and exit", NULL},
  { "scale",	's',	0,	G_OPTION_ARG_NONE,	&config.opt_scale,	"Scale the source screen to fit the display area (requires XRender)", NULL},
  { "window",	'w',	0,	G_OPTION_ARG_NONE,	&config.opt_window,	"Run inside a window instead of going fullscreen", NULL},
  { "src",	'S',	0,	G_OPTION_ARG_STRING,	&config.src_monitor_name,	"Source monitor name(s) or number(s), comma-separated (see --list-monitors)", "MONITOR[,MONITOR...]"},
  { "dst",	'D',	0,	G_OPTION_ARG_STRING,	&config.dst_monitor_name,	"Destination monitor name or number (see --list-monitors)", "MONITOR"},
  { "switch-threshold",	'T',	0,	G_OPTION_ARG_INT,	&config.opt_switch_threshold,	"Pixels cursor must travel into a new monitor before switching (default 100, 0 to disable)", "N"},
  { NULL }
};
```

- [ ] **Step 2: Verify --help includes new flags**

Build and run:
```bash
ninja -C builddir
./builddir/squint --help | grep -A1 "src-display\|dst-display"
```

Expected output should show both new options.

- [ ] **Step 3: Commit**

```bash
git add squint.c
git commit -m "cli: add --src-display and --dst-display flags for cross-server mode"
```

---

### Task 3: Add cross-server display management to x11.c

**Files:**
- Modify: `x11.c:1-50` (global state)
- Modify: `x11.c:1020-1080` (x11_init function)

- [ ] **Step 1: Add global Display/XCB connection for source**

At the top of x11.c, add:

```c
// Cross-server support (Phase 1)
static Display* src_display = NULL;  // source X11 display (if cross-server mode)
static xcb_connection_t* src_xcb = NULL;  // XCB connection for source (for future DRI3)
static int src_display_fd = -1;  // file descriptor of source Display connection
static gboolean use_cross_server = FALSE;  // TRUE if src != dst display
```

- [ ] **Step 2: Modify x11_init() to open separate displays**

In x11_init(), after the existing `display = XOpenDisplay(NULL);` line (around line 1040), add cross-server initialization:

```c
gboolean
x11_init()
{
	// ... existing code ...
	
	// Open source display (existing code)
	display = XOpenDisplay(NULL);
	if (display == NULL) {
		squint_error("Cannot open display");
		return FALSE;
	}
	
	// NEW: Cross-server mode setup
	if (config.src_display_name || config.dst_display_name) {
		const char* src_name = config.src_display_name;
		const char* dst_name = config.dst_display_name;
		
		// If only one is specified, use it for both
		if (!src_name) src_name = dst_name;
		if (!dst_name) dst_name = src_name;
		
		// Open source display
		if (src_name && strcmp(src_name, DefaultDisplayString(display)) != 0) {
			src_display = XOpenDisplay(src_name);
			if (!src_display) {
				squint_error("Cannot open source display");
				return FALSE;
			}
			use_cross_server = TRUE;
			src_display_fd = ConnectionNumber(src_display);
			src_xcb = XGetXCBConnection(src_display);
		}
		
		// Open destination display (via GDK, already done via gdisplay)
		if (dst_name && strcmp(dst_name, DefaultDisplayString(display)) != 0) {
			// GDK display will be opened later in main GTK init
			use_cross_server = TRUE;
		}
	}
	
	// ... rest of existing x11_init code ...
}
```

- [ ] **Step 3: Add cleanup for src_display**

Find the x11_disable() function (around line 900), and add cleanup:

```c
void
x11_disable()
{
	// ... existing cleanup code ...
	
	// NEW: Close cross-server displays
	if (src_display) {
		XCloseDisplay(src_display);
		src_display = NULL;
	}
}
```

- [ ] **Step 4: Build and verify no compilation errors**

```bash
ninja -C builddir
```

Expected: Clean build with no errors.

- [ ] **Step 5: Commit**

```bash
git add x11.c
git commit -m "x11: add cross-server display initialization (Phase 1)"
```

---

### Task 4: Add XShmGetImage/XShmPutImage rendering path

**Files:**
- Modify: `x11.c:1-50` (add helper functions)
- Modify: `x11.c:257-292` (x11_refresh_image function)

- [ ] **Step 1: Add XShm helper function for cross-server rendering**

Before x11_refresh_image(), add:

```c
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
```

- [ ] **Step 2: Update x11_refresh_image() to use cross-server path when enabled**

Replace the body of x11_refresh_image():

```c
x11_refresh_image(const GdkRectangle* damaged_rect)
{
	if (!pixmap || !window) return FALSE;

#ifdef HAVE_XI
	if (!can_track_cursor)
#endif
	{
		x11_refresh_cursor_location(FALSE);
	}

	// location of the damaged area relative to the source rect
	int x = damaged_rect->x - src_rect.x;
	int y = damaged_rect->y - src_rect.y;

	x11_clear_cursor();

	// NEW: Cross-server or same-server rendering
	if (use_cross_server && src_display) {
		// For Phase 1 Xephyr testing: use XShmGetImage fallback
#ifdef HAVE_MIT_SHM
		if (!x11_refresh_image_shm_fallback(src_display, damaged_rect)) {
			return FALSE;
		}
#else
		squint_error("cross-server mode requires MIT-SHM support");
		return FALSE;
#endif
	} else {
		// Original same-server path
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
	}

	XFlush (display);

	return TRUE;
}
```

- [ ] **Step 3: Build and verify compilation**

```bash
ninja -C builddir
```

Expected: Clean build. If MIT-SHM is not available, it should still compile (conditional code).

- [ ] **Step 4: Commit**

```bash
git add x11.c
git commit -m "x11: add XShmGetImage/XShmPutImage fallback for cross-server rendering"
```

---

### Task 5: Handle GDK display initialization for cross-server

**Files:**
- Modify: `squint.c:870-915` (main function)

- [ ] **Step 1: Open destination GDK display before x11_init()**

In main(), after `gtk_init_with_args()`, add:

```c
int
main (int argc, char *argv[])
{
	GError *err = NULL;
	GOptionContext *context;

	memset(&config, 0, sizeof(config));
	config.opt_limit = -1;
	config.opt_warp_cursor = TRUE;
	config.opt_switch_threshold = 75;

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

	// NEW: Open destination GDK display if cross-server
	if (config.dst_display_name) {
		gdisplay = gdk_display_open(config.dst_display_name);
		if (!gdisplay) {
			char msg[256];
			snprintf(msg, sizeof(msg), "Cannot open destination display %s", config.dst_display_name);
			squint_error(msg);
			return 1;
		}
	} else {
		gdisplay = gdk_display_get_default();
	}

	// ... rest of main unchanged ...
}
```

- [ ] **Step 2: Build and test**

```bash
ninja -C builddir
./builddir/squint --help | grep -E "src-display|dst-display"
```

Expected: Flags appear in help output.

- [ ] **Step 3: Commit**

```bash
git add squint.c
git commit -m "main: open destination GDK display when cross-server mode is enabled"
```

---

### Task 6: Create xephyr-harness.sh test harness

**Files:**
- Create: `tools/xephyr-harness.sh`

- [ ] **Step 1: Write xephyr-harness.sh script**

```bash
#!/bin/bash
set -e

# Phase 1 Xephyr harness: spawn two nested X servers and test Squint cross-server mirroring
# Usage: ./tools/xephyr-harness.sh [start|stop|run]

XEPHYR_SRC=${XEPHYR_SRC:-:10}
XEPHYR_DST=${XEPHYR_DST:-:11}
XEPHYR_GEOM=${XEPHYR_GEOM:-1920x1080}

# Find builddir (assume called from repo root)
BUILDDIR="${BUILDDIR:-builddir}"
SQUINT="${BUILDDIR}/squint"

if [ ! -x "$SQUINT" ]; then
    echo "Error: $SQUINT not found. Run 'ninja -C builddir' first."
    exit 1
fi

start() {
    echo "Starting Xephyr servers..."
    
    # Start source Xephyr (will display test content)
    Xephyr -screen "${XEPHYR_GEOM}" "$XEPHYR_SRC" &
    SRC_PID=$!
    
    # Start destination Xephyr
    Xephyr -screen "${XEPHYR_GEOM}" "$XEPHYR_DST" &
    DST_PID=$!
    
    sleep 2  # Wait for servers to initialize
    
    echo "Source server: $XEPHYR_SRC (PID $SRC_PID)"
    echo "Dest server:   $XEPHYR_DST (PID $DST_PID)"
    echo "Save these PIDs to kill manually: kill $SRC_PID $DST_PID"
}

stop() {
    echo "Stopping Xephyr servers..."
    pkill -f "Xephyr.*:10\|Xephyr.*:11" || true
    sleep 1
}

run() {
    echo "Running Squint in cross-server mode..."
    export DISPLAY=:11  # Mirror output goes to dest
    
    # Run with test content on source
    DISPLAY=$XEPHYR_SRC xwd -root -out /tmp/test.xwd &
    TEST_PID=$!
    sleep 1
    
    # Launch Squint
    "$SQUINT" --src-display "$XEPHYR_SRC" --dst-display "$XEPHYR_DST" --window
    
    kill $TEST_PID 2>/dev/null || true
}

case "${1:-start}" in
    start) start ;;
    stop) stop ;;
    run) run ;;
    *)
        echo "Usage: $0 {start|stop|run}"
        echo "  start: spawn Xephyr servers"
        echo "  stop:  kill Xephyr servers"
        echo "  run:   launch Squint in cross-server mode"
        exit 1
        ;;
esac
```

- [ ] **Step 2: Make script executable**

```bash
chmod +x tools/xephyr-harness.sh
```

- [ ] **Step 3: Test script syntax**

```bash
bash -n tools/xephyr-harness.sh
```

Expected: No syntax errors.

- [ ] **Step 4: Commit**

```bash
git add tools/xephyr-harness.sh
git commit -m "tools: add xephyr-harness.sh for Phase 1 testing"
```

---

### Task 7: Manual testing in Xephyr

**Files:**
- None (testing only)

- [ ] **Step 1: Build Squint**

```bash
ninja -C builddir
```

Expected: Build succeeds.

- [ ] **Step 2: Start Xephyr servers**

In one terminal:
```bash
./tools/xephyr-harness.sh start
```

This spawns `:10` (source) and `:11` (destination). Keep this running.

- [ ] **Step 3: Run Squint in cross-server mode**

In another terminal:
```bash
# Set source display to :10 and run a test pattern
DISPLAY=:10 xterm &
sleep 1

# Mirror it to :11 (destination)
DISPLAY=:0 ./builddir/squint --src-display :10 --dst-display :11 --window
```

Expected: Squint window opens on `:11` showing the xterm from `:10`. Window should be interactive and respond to focus changes.

- [ ] **Step 4: Test single-server fallback (verify backward compatibility)**

```bash
# Kill Squint from Step 3 (Ctrl+C)
# Run in default single-server mode (no cross-server flags)
DISPLAY=:0 ./builddir/squint --window
```

Expected: Works as before (no regression).

- [ ] **Step 5: Cleanup**

```bash
./tools/xephyr-harness.sh stop
```

- [ ] **Step 6: Verify no code regressions**

Run any existing tests:
```bash
# Check if meson/ninja has test targets
ninja -C builddir test 2>/dev/null || echo "No test target defined"
```

- [ ] **Step 7: Commit test results**

Document what was tested in CROSS-SERVER-PLAN.md (update):

```bash
# Edit CROSS-SERVER-PLAN.md to mark Phase 1 as tested
# Append to the Phase 1 section:
# ✅ Manual testing: cross-server mirroring works in Xephyr
# ✅ Backward compatibility: single-server mode unchanged
# ✅ xephyr-harness.sh spawns servers and Squint mirrors content

git add CROSS-SERVER-PLAN.md
git commit -m "docs: mark Phase 1 testing complete"
```

---

## Spec Coverage Check

- ✅ **--src-display flag** — Task 2 & 3
- ✅ **--dst-display flag** — Task 2 & 3
- ✅ **Cross-display initialization** — Task 3, 5
- ✅ **XShmGetImage/XShmPutImage fallback** — Task 4
- ✅ **Backward compatibility** — Task 4 (original path preserved)
- ✅ **xephyr-harness.sh** — Task 6
- ✅ **Manual testing** — Task 7
