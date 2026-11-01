/* enum-modifiers.c — list DRM format modifiers supported by
 *   (a) Intel EGL import on the destination display (:1)
 *   (b) AMD DRI3 for the root drawable on the source display (:0)
 * for DRM_FORMAT_XRGB8888. The intersection is what a cross-GPU import
 * could actually use.
 *
 * build: gcc -O2 -o enum-modifiers enum-modifiers.c \
 *        -lX11 -lX11-xcb -lxcb -lxcb-dri3 -lEGL
 * run:   XAUTHORITY=/tmp/xauth-squint-combined ./enum-modifiers :0 :1
 */
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

#define DRM_FORMAT_XRGB8888 0x34325258

int main(int argc, char** argv) {
    const char* src_name = argc > 1 ? argv[1] : ":0";
    const char* dst_name = argc > 2 ? argv[2] : ":1";

    /* ---- Intel side: EGL modifiers on :1 ---- */
    Display* dst = XOpenDisplay(dst_name);
    if (!dst) { fprintf(stderr, "cannot open %s\n", dst_name); return 1; }
    EGLDisplay ed = eglGetDisplay((EGLNativeDisplayType)dst);
    EGLint maj, min;
    eglInitialize(ed, &maj, &min);

    PFNEGLQUERYDMABUFMODIFIERSEXTPROC qmod =
        (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    printf("=== Intel (%s) EGL import modifiers for XRGB8888 ===\n", dst_name);
    if (!qmod) {
        printf("  eglQueryDmaBufModifiersEXT not available\n");
    } else {
        EGLint n = 0;
        qmod(ed, DRM_FORMAT_XRGB8888, 0, NULL, NULL, &n);
        printf("  count=%d\n", n);
        if (n > 0) {
            EGLuint64KHR* mods = calloc(n, sizeof(EGLuint64KHR));
            EGLBoolean* ext = calloc(n, sizeof(EGLBoolean));
            qmod(ed, DRM_FORMAT_XRGB8888, n, mods, ext, &n);
            for (int i = 0; i < n; i++)
                printf("  0x%016" PRIx64 " (external_only=%d)\n",
                       (uint64_t)mods[i], ext[i]);
            free(mods); free(ext);
        }
    }

    /* ---- AMD side: DRI3 supported modifiers on :0 root ---- */
    Display* src = XOpenDisplay(src_name);
    if (!src) { fprintf(stderr, "cannot open %s\n", src_name); return 1; }
    xcb_connection_t* xcb = XGetXCBConnection(src);
    Window root = DefaultRootWindow(src);

    printf("\n=== AMD (%s) DRI3 get_supported_modifiers, root, depth24 bpp32 ===\n", src_name);
    xcb_dri3_get_supported_modifiers_cookie_t c =
        xcb_dri3_get_supported_modifiers(xcb, root, 24, 32);
    xcb_dri3_get_supported_modifiers_reply_t* r =
        xcb_dri3_get_supported_modifiers_reply(xcb, c, NULL);
    if (!r) {
        printf("  request failed (DRI3 <1.2 or unsupported)\n");
    } else {
        uint64_t* win = xcb_dri3_get_supported_modifiers_window_modifiers(r);
        int nwin = xcb_dri3_get_supported_modifiers_window_modifiers_length(r);
        uint64_t* scr = xcb_dri3_get_supported_modifiers_screen_modifiers(r);
        int nscr = xcb_dri3_get_supported_modifiers_screen_modifiers_length(r);
        printf("  window_modifiers (%d):\n", nwin);
        for (int i = 0; i < nwin; i++) printf("    0x%016" PRIx64 "\n", win[i]);
        printf("  screen_modifiers (%d):\n", nscr);
        for (int i = 0; i < nscr; i++) printf("    0x%016" PRIx64 "\n", scr[i]);
        free(r);
    }
    return 0;
}
