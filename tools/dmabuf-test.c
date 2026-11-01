/*
 * dmabuf-test.c — DMA-BUF cross-device texture import benchmark
 *
 * Tests the fast path for cross-server Squint:
 *   XCopyArea to pixmap (GPU blit, stays in AMD VRAM)
 *   → DRI3 DMA-BUF export (fd, no pixel copy)
 *   → EGL image import on Intel
 *   → GL fullscreen quad render
 *
 * This eliminates all CPU-side pixel copies. The AMD→Intel transfer
 * happens inside the Intel driver over PCIe when it samples the texture,
 * asynchronously and without going through system RAM.
 *
 * If the DMA-BUF import fails with a format/modifier error, the AMD
 * pixmap is using a tiled layout that the Intel driver can't import
 * without modifier negotiation — see the NOTES section in the output.
 *
 * Build (inside devcontainer):
 *   gcc -O2 -o dmabuf-test dmabuf-test.c \
 *       -lX11 -lX11-xcb -lxcb -lxcb-dri3 -lEGL -lGL
 *
 * Usage:
 *   XAUTHORITY=/tmp/xauth-igpu ./dmabuf-test [src] [dst] [w] [h] [secs]
 *   XAUTHORITY=/tmp/xauth-igpu ./dmabuf-test :0 :1 1920 1080 10
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* Linear XRGB8888 — most common uncompressed X11 pixmap format */
#define DRM_FORMAT_XRGB8888 0x34325258u

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Extension function pointers — loaded at runtime */
static PFNEGLCREATEIMAGEKHRPROC      p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC     p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;

static int load_gl_extensions(void) {
    p_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    p_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!p_eglCreateImageKHR || !p_eglDestroyImageKHR || !p_glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "ERROR: required EGL/GL extension procs not found\n");
        return 0;
    }
    return 1;
}

static const char* vs_src =
    "#version 130\n"
    "out vec2 uv;\n"
    "void main() {\n"
    /* generate fullscreen quad from vertex ID without a VBO */
    "    float x = float((gl_VertexID & 1) * 2) - 1.0;\n"
    "    float y = float((gl_VertexID >> 1) * 2) - 1.0;\n"
    "    gl_Position = vec4(x, y, 0.0, 1.0);\n"
    /* flip Y: X11 origin is top-left, GL is bottom-left */
    "    uv = vec2(x * 0.5 + 0.5, 1.0 - (y * 0.5 + 0.5));\n"
    "}\n";

static const char* fs_src =
    "#version 130\n"
    "in vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "out vec4 color;\n"
    "void main() { color = texture(tex, uv); }\n";

static GLuint build_shader_program(void) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(vs);
    glCompileShader(fs);

    GLint ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(vs, sizeof(log), NULL, log);
        fprintf(stderr, "vertex shader error: %s\n", log); return 0;
    }
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        fprintf(stderr, "fragment shader error: %s\n", log); return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "link error: %s\n", log); return 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

int main(int argc, char** argv) {
    const char* src_name = argc > 1 ? argv[1] : ":0";
    const char* dst_name = argc > 2 ? argv[2] : ":1";
    int width    = argc > 3 ? atoi(argv[3]) : 1920;
    int height   = argc > 4 ? atoi(argv[4]) : 1080;
    int duration = argc > 5 ? atoi(argv[5]) : 10;

    printf("dmabuf-test: src=%s dst=%s  rect=%dx%d  duration=%ds\n\n",
           src_name, dst_name, width, height, duration);

    /* ── Source side: Xlib + XCB for DRI3 ─────────────────────── */
    Display* src_dpy = XOpenDisplay(src_name);
    if (!src_dpy) { fprintf(stderr, "cannot open src display %s\n", src_name); return 1; }

    xcb_connection_t* xcb = XGetXCBConnection(src_dpy);
    if (!xcb) { fprintf(stderr, "XGetXCBConnection failed\n"); return 1; }

    /* Verify DRI3 is available on src */
    {
        xcb_dri3_query_version_cookie_t c = xcb_dri3_query_version(xcb, 1, 0);
        xcb_generic_error_t* err = NULL;
        xcb_dri3_query_version_reply_t* r = xcb_dri3_query_version_reply(xcb, c, &err);
        if (err || !r) {
            fprintf(stderr, "ERROR: DRI3 not available on %s\n", src_name);
            fprintf(stderr, "       Check that the X server was started with DRI3 enabled\n");
            return 1;
        }
        printf("[src] DRI3 %d.%d available\n", r->major_version, r->minor_version);
        free(r);
    }

    Window src_root  = DefaultRootWindow(src_dpy);
    int    src_depth = DefaultDepth(src_dpy, DefaultScreen(src_dpy));
    Pixmap src_px    = XCreatePixmap(src_dpy, src_root, width, height, src_depth);
    GC     src_gc    = XCreateGC(src_dpy, src_px, 0, NULL);

    /* Fill pixmap with real content so warm-up is meaningful */
    XCopyArea(src_dpy, src_root, src_px, src_gc, 0, 0, width, height, 0, 0);
    XSync(src_dpy, False);

    /* Export pixmap to DMA-BUF fd via DRI3 */
    xcb_dri3_buffer_from_pixmap_cookie_t bfp_cookie =
        xcb_dri3_buffer_from_pixmap(xcb, src_px);
    xcb_generic_error_t* bfp_err = NULL;
    xcb_dri3_buffer_from_pixmap_reply_t* bfp =
        xcb_dri3_buffer_from_pixmap_reply(xcb, bfp_cookie, &bfp_err);

    if (bfp_err || !bfp) {
        fprintf(stderr, "ERROR: xcb_dri3_buffer_from_pixmap failed\n");
        fprintf(stderr, "       DRI3 export requires the modesetting or radeon/amdgpu DDX\n");
        return 1;
    }

    int* fds   = xcb_dri3_buffer_from_pixmap_reply_fds(xcb, bfp);
    int  dma_fd = fds[0];
    uint32_t stride = bfp->stride;
    printf("[src] DMA-BUF exported: fd=%d  stride=%u  depth=%u  bpp=%u\n\n",
           dma_fd, stride, bfp->depth, bfp->bpp);
    free(bfp);

    /* ── Destination side: EGL + OpenGL ───────────────────────── */
    Display* dst_dpy = XOpenDisplay(dst_name);
    if (!dst_dpy) { fprintf(stderr, "cannot open dst display %s\n", dst_name); return 1; }

    EGLDisplay egl_dpy = eglGetDisplay((EGLNativeDisplayType)dst_dpy);
    if (egl_dpy == EGL_NO_DISPLAY) { fprintf(stderr, "eglGetDisplay failed\n"); return 1; }

    EGLint egl_major, egl_minor;
    if (!eglInitialize(egl_dpy, &egl_major, &egl_minor)) {
        fprintf(stderr, "eglInitialize failed\n"); return 1;
    }
    printf("[dst] EGL %d.%d\n", egl_major, egl_minor);

    const char* egl_exts = eglQueryString(egl_dpy, EGL_EXTENSIONS);
    if (!strstr(egl_exts, "EGL_EXT_image_dma_buf_import")) {
        fprintf(stderr, "ERROR: EGL_EXT_image_dma_buf_import not supported on dst\n");
        fprintf(stderr, "       Intel Mesa driver too old, or EGL platform mismatch\n");
        return 1;
    }
    printf("[dst] EGL_EXT_image_dma_buf_import: supported\n");

    int has_modifiers = strstr(egl_exts, "EGL_EXT_image_dma_buf_import_modifiers") != NULL;
    printf("[dst] EGL_EXT_image_dma_buf_import_modifiers: %s\n\n",
           has_modifiers ? "supported" : "NOT supported (may affect tiled imports)");

    if (!load_gl_extensions()) return 1;

    /* EGL context + window surface on :1 */
    eglBindAPI(EGL_OPENGL_API);
    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_NONE
    };
    EGLConfig egl_cfg;
    EGLint    n_cfg = 0;
    eglChooseConfig(egl_dpy, cfg_attrs, &egl_cfg, 1, &n_cfg);
    if (n_cfg < 1) { fprintf(stderr, "no suitable EGL config on dst\n"); return 1; }

    /* Fullscreen override-redirect window — no WM needed on :1 */
    Window dst_root = DefaultRootWindow(dst_dpy);
    XSetWindowAttributes wa = { .override_redirect = True };
    int dst_vis_depth = DefaultDepth(dst_dpy, DefaultScreen(dst_dpy));
    XVisualInfo vt = { .depth = dst_vis_depth };
    int nv;
    XVisualInfo* vis = XGetVisualInfo(dst_dpy, VisualDepthMask, &vt, &nv);
    Window dst_win = XCreateWindow(dst_dpy, dst_root,
                                   0, 0, width, height, 0,
                                   vis->depth, InputOutput, vis->visual,
                                   CWOverrideRedirect, &wa);
    XFree(vis);
    XMapRaised(dst_dpy, dst_win);
    XSync(dst_dpy, False);

    EGLSurface egl_surf = eglCreateWindowSurface(egl_dpy, egl_cfg,
                                                  (EGLNativeWindowType)dst_win, NULL);
    EGLContext egl_ctx  = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, NULL);
    eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx);
    eglSwapInterval(egl_dpy, 0); /* disable vsync for unthrottled measurement */

    /* Build shader program */
    GLuint prog = build_shader_program();
    if (!prog) return 1;
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);

    /* VAO (required by GL core profile) */
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    /* Import DMA-BUF as EGLImage on the Intel side.
     * The EGLImage holds a reference to the AMD GEM buffer; subsequent
     * XCopyArea updates to the pixmap are visible here after XSync. */
    EGLint img_attrs[] = {
        EGL_WIDTH,                      width,
        EGL_HEIGHT,                     height,
        EGL_LINUX_DRM_FOURCC_EXT,       (EGLint)DRM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT,     dma_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,  (EGLint)stride,
        EGL_NONE
    };
    EGLImageKHR egl_img = p_eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT,
                                               EGL_LINUX_DMA_BUF_EXT,
                                               NULL, img_attrs);
    if (egl_img == EGL_NO_IMAGE_KHR) {
        EGLint err = eglGetError();
        fprintf(stderr, "ERROR: eglCreateImageKHR failed (0x%x)\n", err);
        fprintf(stderr, "\nNOTES:\n");
        fprintf(stderr, "  0x3009 (EGL_BAD_MATCH) usually means the AMD pixmap uses a\n");
        fprintf(stderr, "  tiled/compressed memory layout that Intel cannot import without\n");
        fprintf(stderr, "  DRM format modifiers. This is a driver-level constraint.\n");
        fprintf(stderr, "  Next step: use xcb_dri3_buffers_from_pixmap (DRI3 1.2) to get\n");
        fprintf(stderr, "  the modifier, then pass EGL_DMA_BUF_PLANE0_MODIFIER_LO/HI_EXT.\n");
        return 1;
    }
    printf("[dst] EGLImage created from DMA-BUF fd — cross-device import succeeded\n\n");

    /* Bind EGLImage to GL texture */
    GLuint tex;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, egl_img);

    printf("warming up...\n");
    XCopyArea(src_dpy, src_root, src_px, src_gc, 0, 0, width, height, 0, 0);
    XSync(src_dpy, False);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    eglSwapBuffers(egl_dpy, egl_surf);

    printf("running for %d seconds...\n", duration);
    long frames = 0;
    double t0 = now_sec();
    double deadline = t0 + duration;

    while (now_sec() < deadline) {
        /* Update source: GPU blit on AMD, content stays in VRAM */
        XCopyArea(src_dpy, src_root, src_px, src_gc, 0, 0, width, height, 0, 0);
        /* Fence: ensure AMD GPU finished before Intel reads the texture.
         * XSync is the conservative but correct approach here; proper
         * explicit sync would use DRI3 FenceFromFD / SyncFence. */
        XSync(src_dpy, False);

        /* Render: Intel samples the shared DMA-BUF texture.
         * The PCIe transfer (if any) happens inside the Intel driver here. */
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        eglSwapBuffers(egl_dpy, egl_surf);
        frames++;
    }

    double elapsed = now_sec() - t0;
    double fps = frames / elapsed;

    printf("\n--- results ---\n");
    printf("frames:  %ld\n", frames);
    printf("fps:     %.1f\n", fps);
    printf("\n");
    if (fps >= 50)
        printf("VERDICT: excellent — zero-copy path viable at 50fps cap\n");
    else if (fps >= 25)
        printf("VERDICT: viable — meets Squint's 25fps XDamage default\n");
    else if (fps >= 10)
        printf("VERDICT: marginal — may still beat XGetImage for damage-driven updates\n");
    else
        printf("VERDICT: too slow — bottleneck is likely XSync on the AMD side\n");

    /* Cleanup */
    p_eglDestroyImageKHR(egl_dpy, egl_img);
    close(dma_fd);
    glDeleteTextures(1, &tex);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
    eglDestroySurface(egl_dpy, egl_surf);
    eglDestroyContext(egl_dpy, egl_ctx);
    eglTerminate(egl_dpy);
    XDestroyWindow(dst_dpy, dst_win);
    XFreeGC(src_dpy, src_gc);
    XFreePixmap(src_dpy, src_px);
    XCloseDisplay(src_dpy);
    XCloseDisplay(dst_dpy);
    return 0;
}
