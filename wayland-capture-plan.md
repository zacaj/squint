# Plan: Wayland capture backend for squint (source = KWin Wayland, sink = X11 `:1`)

## Goal
Run `:0` as Plasma **Wayland** (KWin on AMD) and keep the existing sink unchanged:
squint renders via EGL into a fullscreen window on the iGPU Xorg `:1`, importing a
**linear DMA-BUF** produced on the AMD side. Only the *source* half is replaced.

## Constraints found
- KWin 6.7.5 exposes **no** `ext-image-copy-capture` / `wlr-screencopy`. The only
  compositor capture paths are `zkde_screencast_unstable_v1` (privileged) and the
  xdg-desktop-portal ScreenCast (which wraps it). Both deliver frames via **PipeWire**.
- The Intel iGPU can only import AMD buffers that are **LINEAR** (same as today's
  DRI3 linear-pixmap trick).
- Wayland gives clients no global pointer position and no `_NET_ACTIVE_WINDOW`, so
  cursor-follow and active-window-follow need new mechanisms.

## Capture options (latency ranked)

| Option | Latency | Pros | Cons |
|---|---|---|---|
| A. KMS/DRM plane grab (`drmModeGetFB2` + PRIME, like Sunshine-KMS/ReFrame) | lowest (~0–½ frame, reads scanout directly) | compositor-agnostic, no prompts | needs CAP_SYS_ADMIN/root; scanout is AMD-tiled/DCC → needs our own AMD EGL detile blit to linear; can tear (reads a buffer KWin may reuse); cursor is a separate HW plane; **same mechanism as ReFrame, the current lead suspect in the freeze investigation** |
| B. PipeWire via portal ScreenCast, DMA-BUF w/ LINEAR modifier only | ~1 frame (KWin blits output into our buffer right after compositing) | supported path; KWin does the AMD-side copy *into a linear buffer we negotiate*, so no AMD EGL code needed; cursor metadata gives position per monitor | one-time portal dialog (restore token makes it persistent); PipeWire dependency |
| C. PipeWire via `zkde_screencast` directly | same as B | no portal dialog | must be whitelisted as privileged (`X-KDE-Wayland-Interfaces` .desktop), private/unstable protocol |

**Recommendation: B**, with C as a later swap-in if the portal is annoying (both end
at the same PipeWire stream, so the consumer code is shared). A is only worth it if
B measures meaningfully laggy — and not until the freeze investigation clears KMS grabbing.

## Design (option B)
1. **New file `wl_capture.c/.h`** (PipeWire consumer), behind meson option
   `wayland_capture` (deps: `libpipewire-0.3`, `gio-2.0` for portal D-Bus, `libdrm` headers).
   - Portal: `CreateSession` → `SelectSources(types=MONITOR, multiple=true,
     cursor_mode=METADATA, persist_mode=2, restore_token=<saved>)` → `Start` →
     `OpenPipeWireRemote`. Save the restore token under `~/.local/state/squint/`.
   - One `pw_stream` per selected monitor; offer `SPA_VIDEO_FORMAT_BGRx/BGRA` with
     `SPA_FORMAT_VIDEO_modifier = DRM_FORMAT_MOD_LINEAR` only, `SPA_DATA_DmaBuf`.
   - Integrate PipeWire's loop fd into the GLib main loop (`pw_loop_get_fd` +
     `g_unix_fd_add`), so no extra thread; frame arrival drives redraw (replaces XDamage).
   - On `process`: dequeue newest buffer (drop stale ones), import fd via existing
     `egl_create_image_from_fd` (cache EGLImage/texture per buffer fd — PipeWire
     cycles a small buffer pool), `egl_draw_quad` + swap, requeue.
2. **Cursor-follow**: with `cursor_mode=METADATA`, each monitor stream reports cursor
   position only while the pointer is on it → "active source = stream whose cursor
   metadata is on-screen". Cursor bitmap from metadata, composited in the shader
   (second textured quad) instead of XFixes.
3. **Active-window follow (later phase)**: tiny KWin script (`workspace.windowActivated`
   → `callDBus` to squint) giving the active window's output. Optional; skip in v1.
4. **squint.c wiring**: `--src-wayland` (or auto when `--src-display` absent and
   `WAYLAND_DISPLAY` set). Source monitors come from the portal selection / stream
   order instead of XRandR on `:0`. GDK stays X11 for `:1` (`GDK_BACKEND=x11`).
5. **Perf**: extend `--perf-log` with *PipeWire frame PTS → swap* latency, so B can be
   compared numerically against the current X11 path.

## Environment changes (need your OK, not code)
- Plasma Wayland must not claim the iGPU: `KWIN_DRM_DEVICES=/dev/dri/card0` (AMD) in the
  session env (the existing `99-igpu-seat1.rules` may already be enough — to verify).
- `:0` is currently `startx` → plasmax11; the systemd units/`igpu-mirrors-up.sh` assume
  X auth for `:0`. A Wayland variant of the mirrors script is needed.
- Devcontainer Dockerfile: add `pipewire-devel` (and `libdrm-devel` if missing).

## Verification
- Phase 1 needs no session switch: run a nested/virtual KWin (`kwin_wayland --virtual
  --xwayland` or `--drm` on nothing) as source, capture via portal is awkward there, so
  instead test the PipeWire consumer against KWin by running squint from a Wayland
  login on a spare VT, sink to `:1`; prove with the existing pixel-diff method
  (`magick compare` source screenshot vs `:1` capture) and `--perf-log` latency.
- Leak checks as before: `/proc/<pid>/fd` dmabuf count flat over 150 s+.
- I can't switch your login session myself — the first real run needs you to log into
  Plasma Wayland (or a second VT session).

## Open questions
1. OK with option B (portal + PipeWire) as the first implementation?
2. Is the switch of `:0` to Wayland already decided, or should this stay a side mode
   while X11 `:0` remains the default?
3. OK to add `pipewire-devel` to the devcontainer and a `libpipewire-0.3` meson dep?

## Status
- Implemented: portal handshake + restore token, PipeWire consumer on the GLib loop,
  linear DMA-BUF import with SHM fallback, metadata-cursor overlay + cursor-follow,
  `--perf-log` pts->swap latency, dst RandR hotplug handling, meson option
  `-Dwayland_capture=auto|enabled|disabled` (`HAVE_PIPEWIRE`).
- Verified: builds; fails cleanly without a portal. **Not yet run against a real
  KWin/portal** — next step is `tools/wayland-nested-test.sh`, then a real Wayland login.
- Not done: active-window follow (KWin script), tray source-menu still lists dst monitors.
