# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**squint** is an X11 utility that duplicates a monitor's output into a GTK3 window. This enables presentations or demos on multiple monitors without cloning the entire screen, keeping the primary monitor available for other work.

- **Language**: C
- **Build System**: Meson/Ninja (builddir already configured)
- **GUI Framework**: GTK3
- **Platform**: X11 (Linux)

## Architecture

The project has a clean separation between application logic and X11 integration:

- **squint.c** — Main application: GTK window management, UI event handling, configuration parsing, monitor switching logic
- **x11.c** — X11 layer: Low-level monitor detection, window tracking, cursor warping, input event interception via X11 extensions (XDamage, XFixes, XI, XRandr, etc.)
- **squint.h** — Shared header: Configuration struct, global state (enabled/fullscreen/raised flags, display/window handles, monitor rectangles)

Key architectural pattern: Configuration is read-only after startup; state mutations (enable/disable/switch_source) are coordinated through callbacks from GTK events and X11 event handlers.

## Build & Development Commands

```bash
# Build (builddir already exists)
ninja -C builddir

# Rebuild after changes (automatic dependency tracking)
ninja -C builddir

# Install to system
ninja -C builddir install

# Clean build
rm -rf builddir && meson setup builddir && ninja -C builddir

# Reconfigure with different prefix
meson configure builddir --prefix=~/.local

# Run from builddir (without install)
./builddir/squint --help
```

## Development Workflow

1. **Make code changes** in squint.c or x11.c
2. **Build**: `ninja -C builddir` (fast incremental)
3. **Test**: Run `./builddir/squint` with test flags or in nested X server if needed
4. **Optional dependencies** are gracefully handled — missing libs (XDamage, XFixes, etc.) degrade functionality, not crash

## Key Implementation Details

- **Optional X11 extensions**: Meson checks for availability at build time; the app adapts at runtime
- **Cursor warping**: Only available if both XFixes and XRender are present (COPY_CURSOR flag)
- **Monitor detection**: Uses XRandr when available; falls back to basic Xlib calls
- **Input interception**: XI (XInput) extension for low-latency event handling
- **Status icon**: Appindicator integration if libayatana-appindicator3 is present

## User Agent Rules

This project adheres to rules defined in:
- `@~/.claude/zacaj/agent-rules.md` — User interaction and workflow preferences
- `@~/.claude/zacaj/coding-general.md` — Code style and Git practices
- `@~/.claude/zacaj/testing.md` — Testing guidelines

Key points: Don't modify config files or add packages without asking. Run Meson/Ninja builds directly; don't invent custom build steps. Always verify changes work before marking done.

## Testing Approach

Since this is an X11 GUI app with no formal test suite:
- **Manual verification**: Run `./builddir/squint` and test against real monitors or nested X server (Xephyr)
- **Cross-server testing tools** in `/tools/` (dmabuf-test, xbench) for GPU/rendering validation
- **Before changes**: Understand what aspect you're modifying (GTK UI, X11 events, monitor detection, etc.) and plan how to verify it

## Devcontainer

The project uses a Fedora-based devcontainer with all dependencies pre-installed. Build and development should happen inside the container.
