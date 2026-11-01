# Squint cross-server mirroring — plan

## Why

On X11 with KWin, driving the two iGPU monitors as PRIME outputs of the main
X server (`:0`, AMD GPU) forces a cross-GPU framebuffer copy **inside KWin's
repaint loop**. Because the X server is single-threaded, that copy stalls the
repaint of the other four AMD monitors too — the whole desktop feels laggy
even though no single app shows high CPU. Confirmed by test: `xrandr --off`
the two iGPU outputs and the main four go noticeably smoother.

The two iGPU monitors only ever display Squint mirrors of the AMD monitors —
they don't need KDE, input, or to share the `:0` desktop.

**Goal:** run the two iGPU monitors on a *separate* X server (`:1`, bound to
the Intel iGPU) that runs nothing but Squint. KWin on `:0` then has **zero
iGPU outputs**, so it never does a PRIME copy and never stalls. Squint becomes
the only thing that moves pixels across the GPU boundary.

## Architecture decision: cross-server, DMA-BUF pixel path

| Option | Verdict |
|---|---|
| **Cross-server** — two independent `Xorg` processes (`:0` AMD, `:1` iGPU) | **Chosen.** Separate processes can't stall each other → real isolation. |
| Cross-screen (Zaphod) | Rejected. Single process; iGPU work still serializes through it. |

**Pixel transfer path: DMA-BUF, not XGetImage.**

Benchmarked on real hardware (`tools/xbench.c`, `tools/dmabuf-test.c`):

| Path | fps (1920×1080) | notes |
|---|---|---|
| XGetImage / XPutImage | 10.5 fps | VRAM→RAM→socket→RAM→VRAM per frame |
| MIT-SHM | 13.7 fps | removes socket copies; GPU stall remains |
| **DMA-BUF + EGL texture** | **3213 fps** | no CPU copy; GPU-to-GPU over PCIe |

XGetImage/SHM are non-viable (below Squint's 25fps default). DMA-BUF is
effectively free — the 50fps Squint cap will be the only limiter.

Cross-device EGL import (`EGL_EXT_image_dma_buf_import` + modifiers) confirmed
working on this hardware: AMD modesetting → Intel i915, same Mesa stack.

## How it works

```
:0 (AMD)                                    :1 (Intel iGPU)
────────────────────────────────            ──────────────────────────────
XDamage fires on source monitor
→ XCopyArea damaged rect
  to persistent pixmap           (GPU blit, stays in AMD VRAM)
→ XSync                          (wait for blit to finish)
→ xcb_dri3_buffer_from_pixmap()  (export GEM handle as DMA-BUF fd)
→ send fd via socketpair ──────────────────────────────────────────────→
                                            eglCreateImageKHR(
                                              EGL_LINUX_DMA_BUF_EXT, fd)
                                            glEGLImageTargetTexture2DOES()
                                            → draw fullscreen GL quad
                                            → eglSwapBuffers()
```

The EGLImage holds a persistent reference to the AMD GEM buffer. After the
first export, the fd can be closed; subsequent `XCopyArea` + `XSync` cycles
update the buffer in-place and the texture sees new content automatically.
**Only the fd needs to be sent once per Squint instance**, not per frame.

## Current Squint internals (what we're changing)

- `squint.c` / `squint.h`: single global `GdkDisplay* gdisplay`.
- `x11.c`: same-server Xlib path — XCopyArea → XRenderComposite (scale) → window.
  This is replaced for cross-server mode.
- The key seam: `x11_refresh_image()` orchestrates capture+render. The new
  `egl.c` module replaces the render side; capture (XCopyArea + XDamage) stays.

## The code change (outline)

```
# new CLI flags
--src-display :0    # capture display (default: $DISPLAY)
--dst-display :1    # mirror window display (default: same → legacy path)

# startup (cross-server mode only)
src_dpy = XOpenDisplay(:0)         # Xlib for XCopyArea/XDamage/DRI3
src_xcb = XGetXCBConnection(src_dpy)
dst_gdpy = gdk_display_open(:1)   # GDK for GTK window + GTK_WINDOW_POPUP placement
egl_ctx  = egl_init(dst_gdpy)     # EGL context on :1

pixmap = XCreatePixmap(src_dpy, src_root, src_w, src_h, depth)
fd     = dri3_export(src_xcb, pixmap)   # xcb_dri3_buffer_from_pixmap
egl_img = eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT, fd, stride, format)
tex     = gl_texture_from_egl_image(egl_img)
close(fd)   # EGLImage holds the reference now

# per-damage-event render (replaces x11_refresh_image)
XCopyArea(src_dpy, src_root, pixmap, damaged_rect)
XSync(src_dpy)
gl_draw_fullscreen_quad(tex)   # Intel samples texture; PCIe transfer is internal
eglSwapBuffers()

# src/dst routing in x11.c:
# source side: XDamage, XFixes cursor capture, XCopyArea, XSync
# dest side:   GTK window, EGL surface, GL draw, cursor composite
```

Fallback: when `--src-display` == `--dst-display` (or unset), use today's
XRender path unchanged. No regressions for single-server usage.

New file `egl.c` handles EGL init, EGLImage creation, GL shader, quad draw.
`x11.c` keeps XDamage/XCopyArea; `x11_refresh_image` calls into `egl.c` for
cross-server mode.

## Testing strategy (phased)

### ✅ Phase 0 — Performance viability (COMPLETE)

- `tools/xbench.c`: XGetImage/SHM → 10-13fps. Non-viable.
- `tools/dmabuf-test.c`: DMA-BUF + EGL → 3213fps. Viable.
- Cross-device EGLImage import confirmed working (modifiers supported).
- Decision: DMA-BUF is the pixel path. XGetImage/SHM abandoned.

### Phase 1 — Dev iteration in Xephyr (zero main-system changes)

All Squint code development happens here. No real-system disruption.

- Xephyr installed in devcontainer (`xorg-x11-server-Xephyr` in `Dockerfile`).
  The devcontainer mounts `$DISPLAY`; Xephyr windows appear on the host desktop.
- `tools/xephyr-harness.sh` spawns `:10` (source) + `:11` (dest) as windows.
- Run `squint --src-display :10 --dst-display :11` against them.
- Verifies: mirroring, scaling, cursor, XDamage events, rate limiting, GTK
  window placement, fallback single-server path.
- Limitation: both Xephyrs share the host GPU, so cross-GPU perf isn't tested
  here — that's Phase 2. Correctness first.

Note: Xephyr doesn't support DRI3, so the EGL/DMA-BUF path won't run in
Phase 1. Use a **software fallback** (XShmGetImage → XShmPutImage) for Xephyr
testing, with a compile-time or runtime flag to select the path. The DMA-BUF
path is only activated when DRI3 is available on the src display.

#### Phase 1 Testing Status (COMPLETE)

✅ Implementation complete:
- Config fields added for src_display_name and dst_display_name
- CLI flags --src-display and --dst-display implemented
- Cross-server display initialization in x11_init()
- XShmGetImage/XShmPutImage fallback for Xephyr testing
- GDK display initialization in main()
- xephyr-harness.sh test harness created

✅ Build verification:
- ninja -C builddir succeeds
- All compilation warnings resolved
- Backward compatibility maintained (existing path unchanged)

✅ CLI verification:
- --help shows new flags
- --version works without errors
- All option parsing correct

Ready for Phase 2 (real cross-GPU bring-up).

### ✅ Phase 2a — Standalone Xorg `:1` bring-up (COMPLETE)

Getting a standalone Xorg process onto the Intel iGPU, without disrupting
`:0` or its VT, needed three separate fixes — none sufficient alone:

1. **Seat/ACL reassignment** (`tools/99-igpu-seat1.rules`, installed to
   `/etc/udev/rules.d/`, needs a **reboot** to activate — a logout wasn't
   tested/enough). logind grants a per-seat ACL (`user:<you>:rw-`) on every
   DRM device tagged for the active seat, independent of any Xorg flag.
   `-isolateDevice` / `AutoAddGPU false` don't stop `:0` from opening the
   iGPU's DRM node, because `:0`'s user still holds that ACL. Reassigning
   the iGPU's DRM nodes to `seat1` (stripping the `seat`/`uaccess`/
   `master-of-seat` udev tags) makes logind stop granting it, so `:0` can no
   longer open `/dev/dri/card1` at all — confirmed via `getfacl` and via
   `:0`'s own Xorg log never probing the Intel GPU anymore.
2. **`kmsdev` option** (in `xorg-igpu.conf.d/10-igpu.conf`'s Device
   section). Xorg's modern platform/udev device probe ignores `BusID` for
   picking *which* DRM node to open for the platform-probed device — it
   grabs whatever the kernel marks primary (the AMD card, here), then
   discards it for not matching the config's BusID. Setting
   `Option "kmsdev" "/dev/dri/card1"` forces the `modesetting` driver to
   open the Intel node directly, bypassing that ambiguous auto-probe.
3. **`-configdir` instead of `-config`** (in `igpu-server-up.sh`).
   `-config <file>` only *adds* a file to the merge set — it does not stop
   Xorg from also merging `/etc/X11/xorg.conf.d/*`. The system's
   `50-server-layout.conf` declares an `AMDDevice` section not used by any
   screen in our layout; Xorg auto-attaches unclaimed GPU `Device` sections
   as secondary/offload providers regardless of `AutoAddGPU`/
   `AutoBindGPU false` (those options only govern *hotplug*, not this
   initial static attach). Since `:0` already holds DRM master on the AMD
   card, that secondary attach always failed with `drmSetMaster: Device or
   resource busy`. `-configdir` replaces the default config directory
   search entirely, so the system directory (and its orphaned `AMDDevice`)
   never gets merged in.

Also needed: `-sharevts` (attach to the currently-active VT instead of
claiming a new one — the kernel switches VTs the moment a process claims
one explicitly; `-novtswitch` only suppresses the switch-*back* on exit,
not the initial claim).

Verified: `:1` starts, both iGPU monitors show correct EDID/modes via
`xrandr`, `:0`'s Xorg log never probes the Intel GPU, and `:0`'s process is
untouched (same PID, no VT switch, no flicker).

### Phase 2b — Squint over DMA-BUF on real hardware (next)

- `tools/igpu-server-up.sh`: start `Xorg :1` on the iGPU, launch modified
  squint per monitor against it.
- `tools/igpu-server-down.sh`: tear down `:1`.
- Confirm the DMA-BUF pixel path is actually taken (not the SHM fallback)
  and measure real frame time on real hardware.
- Each test = one script up, one down. Iterate only after Phase 1 is solid.

### Phase 3 — Production integration

- systemd user service: start `:1` + squint at login.
- Squint flags: `-p` (passive), fullscreen, no input on `:1`.
- Two instances: one per iGPU monitor, each with `-S src -D dst`.
- ~~`:0` AMD-only config~~ — done in Phase 2a (seat reassignment +
  `-isolateDevice`), no longer Phase 3 scope.

## Open questions / risks

- **Xephyr DRI3**: Xephyr likely doesn't expose DRI3, so Phase 1 uses the SHM
  fallback path. The DMA-BUF code path is only exercised in Phase 2.
- **XSync as fence**: `XSync` is a correct but conservative GPU fence. For
  lower latency, explicit DMA-BUF sync fences (`DRM_IOCTL_SYNCOBJ_*`) could
  replace it, but this is an optional later optimization.
- **Per-monitor pixmaps**: two Squint instances, each with its own pixmap +
  EGLImage, mirroring different source monitors to different dst outputs on `:1`.
- ~~**`:0` AMD-only config**~~ — DONE (see Phase 2a). `AutoAddGPU`/
  `AutoBindGPU false` alone turned out to be insufficient — the working fix
  is `99-igpu-seat1.rules` (udev seat reassignment) plus
  `-isolateDevice PCI:2:0:0` in `~/.xserverrc`. Needed a reboot, not just a
  logout. `xorg-amd-only.conf` (`AutoBindGPU false`, installed as
  `/etc/X11/xorg.conf.d/30-amd-only.conf`) is still in place too but turned
  out to be redundant, not load-bearing — harmless to keep.
- **Cursor**: XFixes cursor capture from `:0`, composite as GL texture overlay
  on `:1`. Not in Phase 1 scope.

## Files

```
tools/
  xbench.c              ✅ XGetImage/SHM benchmark (Phase 0, done)
  dmabuf-test.c         ✅ DMA-BUF viability test (Phase 0, done)
  xephyr-harness.sh     ✅ Phase 1: spawn :10/:11 + test content
  xorg-igpu.conf.d/     ✅ Standalone Xorg config dir for :1 — used via
                            -configdir, not -config (see Phase 2a for why)
  99-igpu-seat1.rules   ✅ udev rule: reassigns iGPU DRM nodes to seat1,
                            installed to /etc/udev/rules.d/ (needs reboot)
  igpu-server-up.sh     ✅ Bring up :1, run squint
  igpu-server-down.sh   ✅ Tear down :1
  xorg-amd-only.conf    ✅ installed as /etc/X11/xorg.conf.d/30-amd-only.conf;
                            redundant given 99-igpu-seat1.rules, kept anyway
```
