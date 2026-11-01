# Phase 2: DMA-BUF Cross-GPU Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a high-performance EGL + DRI3 pixel path for cross-GPU mirror transfers, achieving 3200+ fps on dual-GPU hardware while maintaining backward compatibility with the XShm fallback.

**Architecture:** The DMA-BUF path separates concerns into two modules: `egl.c` handles GPU-side operations (EGL context, EGLImage creation, GL rendering), while `x11.c` handles X11-side operations (XCopyArea, XDamage, DRI3 pixmap export). At startup, DRI3 capability is detected; if available, the source pixmap is exported as a DMA-BUF fd and wrapped in an EGLImage on the destination GPU. Per-frame, XCopyArea updates the source pixmap, then the GL quad texture automatically reflects the new content (via persistent EGLImage reference). Fallback: if DRI3 is unavailable, x11_refresh_image() routes to the XShm path from Phase 1.

**Tech Stack:** EGL 1.4+, OpenGL 2.1+, DRI3 extension (xcb_dri3_*), libxcb, GTK3/GDK

---

## File Structure

| File | Responsibility |
|------|-----------------|
| **egl.c** (new) | EGL context init/cleanup, EGLImage creation, GL state (shader, quad VAO/VBO), fullscreen quad rendering |
| **egl.h** (new) | Public API: `egl_init()`, `egl_create_image_from_fd()`, `egl_draw_quad()`, `egl_cleanup()` |
| **x11.c** (modify) | DRI3 pixmap export (`dri3_export_pixmap()`), routing in `x11_refresh_image()`, cleanup |
| **meson.build** (modify) | Add EGL, GL, xcb-dri3 dependencies |
| **squint.h** (modify) | Add `egl_ctx` to global state struct (opaque handle) |

---

## Task Breakdown

### Task 1: Add EGL and GL dependencies to meson.build

**Files:**
- Modify: `meson.build`

- [ ] **Step 1: Open meson.build and check current dependencies**

Read the `meson.build` file to see where dependencies are declared (around line 11 where 'xcb' was added in Phase 1).

- [ ] **Step 2: Add EGL, GL, and DRI3 dependencies**

After the existing `'xcb'` line, add three new dependencies:

```meson
dependencies = [
  dependency('gtk+-3.0'),
  dependency('gdk-x11-3.0'),
  dependency('xcb'),
  dependency('egl'),           # NEW
  dependency('gl'),            # NEW
  dependency('xcb-dri3'),      # NEW
  # ... rest of dependencies
]
```

- [ ] **Step 3: Verify syntax and run meson configure**

Run:
```bash
meson configure builddir
```

Expected: No errors about missing dependencies. If EGL/GL not found, the error message will suggest `apt-get install libgles2-mesa-dev libegl1-mesa-dev libxcb-dri3-dev` (or similar).

- [ ] **Step 4: Commit**

```bash
git add meson.build
git commit -m "build: add EGL, GL, and xcb-dri3 dependencies for Phase 2"
```

---

### Task 2: Create egl.h public header

**Files:**
- Create: `egl.h`

- [ ] **Step 1: Write the header file**

```c
#pragma once

#include <gdk/gdk.h>
#include <EGL/egl.h>
#include <GL/gl.h>

// Opaque EGL context handle
typedef struct egl_context egl_context_t;

// Initialize EGL on the destination display
// Returns opaque context handle on success, NULL on failure
egl_context_t* egl_init(GdkDisplay* gdisplay);

// Create an EGLImage from a DMA-BUF fd (from DRI3 pixmap export)
// fd is closed after import; the EGLImage holds the reference
// Returns GL texture name on success, 0 on failure
GLuint egl_create_image_from_fd(egl_context_t* ctx, int fd, int width, int height,
                               int stride, uint32_t drm_format);

// Render fullscreen quad with the given texture
void egl_draw_quad(egl_context_t* ctx, GLuint texture);

// Swap buffers (flip the EGL surface)
void egl_swap_buffers(egl_context_t* ctx);

// Cleanup and destroy EGL context
void egl_cleanup(egl_context_t* ctx);
```

- [ ] **Step 2: Commit**

```bash
git add egl.h
git commit -m "headers: add egl.h public API for Phase 2 EGL rendering"
```

---

### Task 3: Implement EGL context initialization (egl.c - part 1)

**Files:**
- Create: `egl.c`

- [ ] **Step 1: Write EGL init and struct definition**

```c
#include "egl.h"
#include <stdio.h>
#include <stdlib.h>
#include <gdk/gdkx11.h>

struct egl_context {
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    GLuint shader_program;
    GLuint vao, vbo;
    GdkWindow* gdkwin;
};

egl_context_t* egl_init(GdkDisplay* gdisplay) {
    // Get the GDK X11 display
    Display* x_display = gdk_x11_display_get_xdisplay(GDK_X11_DISPLAY(gdisplay));
    if (!x_display) {
        fprintf(stderr, "egl_init: failed to get X11 display\n");
        return NULL;
    }

    // Get EGL display from X11 display
    EGLDisplay egl_display = eglGetDisplay((EGLNativeDisplayType)x_display);
    if (egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "egl_init: eglGetDisplay failed\n");
        return NULL;
    }

    // Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor)) {
        fprintf(stderr, "egl_init: eglInitialize failed\n");
        return NULL;
    }
    fprintf(stderr, "egl_init: EGL %d.%d initialized\n", major, minor);

    // Bind OpenGL API
    eglBindAPI(EGL_OPENGL_API);

    // Choose EGL config (RGBA, 24-bit depth)
    EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(egl_display, config_attrs, &config, 1, &num_configs) || num_configs != 1) {
        fprintf(stderr, "egl_init: eglChooseConfig failed\n");
        eglTerminate(egl_display);
        return NULL;
    }

    // Create EGL context
    EGLint context_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 2,
        EGL_CONTEXT_MINOR_VERSION, 1,
        EGL_NONE
    };

    EGLContext egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attrs);
    if (egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "egl_init: eglCreateContext failed\n");
        eglTerminate(egl_display);
        return NULL;
    }

    // We'll create the EGL surface later when GTK window is realized
    egl_context_t* ctx = malloc(sizeof(egl_context_t));
    ctx->egl_display = egl_display;
    ctx->egl_context = egl_context;
    ctx->egl_surface = EGL_NO_SURFACE;
    ctx->gdkwin = NULL;
    ctx->shader_program = 0;
    ctx->vao = ctx->vbo = 0;

    return ctx;
}
```

- [ ] **Step 2: Build to verify no errors**

```bash
ninja -C builddir 2>&1 | head -20
```

Expected: Should compile without errors (warnings about unused functions are okay for now).

- [ ] **Step 3: Commit**

```bash
git add egl.c
git commit -m "egl: implement egl_init() context initialization"
```

---

### Task 4: Implement EGL surface creation and GL shader setup (egl.c - part 2)

**Files:**
- Modify: `egl.c`

- [ ] **Step 1: Add GL shader compilation helper**

Insert before the `egl_init()` function:

```c
static GLuint compile_shader(const char* source, GLenum type) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint log_length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        char* log = malloc(log_length);
        glGetShaderInfoLog(shader, log_length, NULL, log);
        fprintf(stderr, "egl: shader compile error (%s):\n%s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        free(log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint status;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        GLint log_length;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
        char* log = malloc(log_length);
        glGetProgramInfoLog(program, log_length, NULL, log);
        fprintf(stderr, "egl: program link error:\n%s\n", log);
        free(log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}
```

- [ ] **Step 2: Add GL state setup function**

Before `egl_init()`, add:

```c
static gboolean setup_gl_state(egl_context_t* ctx) {
    // Vertex shader: fullscreen quad with texture coordinates
    const char* vs_source = 
        "#version 120\n"
        "attribute vec2 position;\n"
        "attribute vec2 texcoord;\n"
        "varying vec2 frag_texcoord;\n"
        "void main() {\n"
        "  gl_Position = vec4(position, 0.0, 1.0);\n"
        "  frag_texcoord = texcoord;\n"
        "}\n";

    // Fragment shader: sample texture and output
    const char* fs_source =
        "#version 120\n"
        "varying vec2 frag_texcoord;\n"
        "uniform sampler2D texture0;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(texture0, frag_texcoord);\n"
        "}\n";

    GLuint vs = compile_shader(vs_source, GL_VERTEX_SHADER);
    GLuint fs = compile_shader(fs_source, GL_FRAGMENT_SHADER);
    if (!vs || !fs) return FALSE;

    GLuint program = link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!program) return FALSE;

    ctx->shader_program = program;

    // Create fullscreen quad VAO and VBO
    // Quad vertices: (-1,-1) to (1,1) with texcoords (0,0) to (1,1)
    float vertices[] = {
        -1.0f, -1.0f,  0.0f, 0.0f,  // bottom-left
         1.0f, -1.0f,  1.0f, 0.0f,  // bottom-right
         1.0f,  1.0f,  1.0f, 1.0f,  // top-right
        -1.0f,  1.0f,  0.0f, 1.0f   // top-left
    };

    glGenVertexArrays(1, &ctx->vao);
    glGenBuffers(1, &ctx->vbo);

    glBindVertexArray(ctx->vao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    GLint pos_attr = glGetAttribLocation(program, "position");
    GLint tex_attr = glGetAttribLocation(program, "texcoord");

    glEnableVertexAttribArray(pos_attr);
    glVertexAttribPointer(pos_attr, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    glEnableVertexAttribArray(tex_attr);
    glVertexAttribPointer(tex_attr, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    return TRUE;
}
```

- [ ] **Step 3: Add EGL surface creation helper**

Before `egl_init()`, add:

```c
static gboolean create_egl_surface(egl_context_t* ctx, GdkWindow* gdkwin) {
    Window x_window = gdk_x11_window_get_xid(gdkwin);

    EGLint surface_attrs[] = { EGL_NONE };
    EGLSurface surface = eglCreateWindowSurface(ctx->egl_display, NULL, 
                                               (EGLNativeWindowType)x_window, 
                                               surface_attrs);
    if (surface == EGL_NO_SURFACE) {
        fprintf(stderr, "egl: eglCreateWindowSurface failed\n");
        return FALSE;
    }

    if (!eglMakeCurrent(ctx->egl_display, surface, surface, ctx->egl_context)) {
        fprintf(stderr, "egl: eglMakeCurrent failed\n");
        eglDestroySurface(ctx->egl_display, surface);
        return FALSE;
    }

    ctx->egl_surface = surface;
    ctx->gdkwin = gdkwin;

    glViewport(0, 0, gdk_window_get_width(gdkwin), gdk_window_get_height(gdkwin));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    return setup_gl_state(ctx);
}
```

- [ ] **Step 4: Build and verify**

```bash
ninja -C builddir
```

Expected: Clean build with no new errors.

- [ ] **Step 5: Commit**

```bash
git add egl.c
git commit -m "egl: add GL shader compilation and VAO/VBO setup"
```

---

### Task 5: Implement DRI3 pixmap export (x11.c)

**Files:**
- Modify: `x11.c`

- [ ] **Step 1: Add DRI3 and EGL includes at top of x11.c**

```c
#include <xcb/dri3.h>
#include <xcb/sync.h>
#include "egl.h"
```

(These go with the other includes near the top of the file.)

- [ ] **Step 2: Add DRI3 helper function**

Add before `x11_init()`:

```c
// Export a pixmap as a DMA-BUF fd via DRI3
// Returns fd on success, -1 on failure
static int dri3_export_pixmap(xcb_connection_t* conn, xcb_pixmap_t pixmap, 
                              int width, int height, uint32_t* drm_format, int* stride) {
    // Query DRI3 version
    xcb_dri3_query_version_cookie_t qv_cookie = xcb_dri3_query_version(conn, 1, 0);
    xcb_dri3_query_version_reply_t* qv_reply = xcb_dri3_query_version_reply(conn, qv_cookie, NULL);
    if (!qv_reply || qv_reply->major_version < 1) {
        if (qv_reply) free(qv_reply);
        fprintf(stderr, "x11: DRI3 not available\n");
        return -1;
    }
    free(qv_reply);

    // Export pixmap as buffer
    xcb_dri3_buffer_from_pixmap_cookie_t exp_cookie = xcb_dri3_buffer_from_pixmap(conn, pixmap);
    xcb_dri3_buffer_from_pixmap_reply_t* exp_reply = xcb_dri3_buffer_from_pixmap_reply(conn, exp_cookie, NULL);
    if (!exp_reply) {
        fprintf(stderr, "x11: dri3_buffer_from_pixmap failed\n");
        return -1;
    }

    int fd = -1;
    int* fds = xcb_dri3_buffer_from_pixmap_reply_fds(conn, exp_reply);
    if (fds && *fds >= 0) {
        fd = fds[0];
        // Duplicate fd so we own a copy (original is freed by xcb)
        fd = dup(fd);
    }

    // Set output parameters (for EGLImage creation)
    *stride = exp_reply->stride;
    *drm_format = exp_reply->depth;  // Simplified: depth as format placeholder

    free(exp_reply);
    return fd;
}
```

- [ ] **Step 3: Add global DRI3 state in x11.c**

After the existing cross-server globals (around line 102), add:

```c
// DRI3 and EGL state for cross-GPU rendering
static gboolean dri3_available = FALSE;
static egl_context_t* egl_ctx = NULL;
static GLuint egl_texture = 0;
static int last_export_fd = -1;
```

- [ ] **Step 4: Update x11_init() to initialize EGL and test DRI3**

Find the `x11_init()` function and add this after the cross-server display initialization:

```c
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
```

- [ ] **Step 5: Build and verify**

```bash
ninja -C builddir
```

Expected: Clean build. If xcb-dri3 is not found, meson will report it; install with `apt-get install libxcb-dri3-dev`.

- [ ] **Step 6: Commit**

```bash
git add x11.c
git commit -m "x11: add DRI3 pixmap export and EGL initialization"
```

---

### Task 6: Implement EGLImage creation and quad rendering (egl.c - part 3)

**Files:**
- Modify: `egl.c`

- [ ] **Step 1: Add EGL image creation function**

Add after the existing functions:

```c
GLuint egl_create_image_from_fd(egl_context_t* ctx, int fd, int width, int height,
                               int stride, uint32_t drm_format) {
    if (!ctx || fd < 0) return 0;

    // EGLImage attributes for DMA-BUF import
    EGLint img_attrs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, drm_format,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
        EGL_NONE
    };

    EGLImageKHR image = eglCreateImageKHR(ctx->egl_display, EGL_NO_CONTEXT,
                                          EGL_LINUX_DMA_BUF_EXT, NULL, img_attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "egl: eglCreateImageKHR failed\n");
        return 0;
    }

    // Create GL texture from EGLImage
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    // Destroy EGLImage (texture now owns the reference)
    eglDestroyImageKHR(ctx->egl_display, image);

    fprintf(stderr, "egl: texture %u created from DMA-BUF fd\n", texture);
    return texture;
}
```

- [ ] **Step 2: Add quad rendering function**

Add after `egl_create_image_from_fd()`:

```c
void egl_draw_quad(egl_context_t* ctx, GLuint texture) {
    if (!ctx || !texture) return;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(ctx->shader_program);

    glBindVertexArray(ctx->vao);
    glBindTexture(GL_TEXTURE_2D, texture);

    glUniform1i(glGetUniformLocation(ctx->shader_program, "texture0"), 0);

    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(0);
}

void egl_swap_buffers(egl_context_t* ctx) {
    if (ctx && ctx->egl_surface != EGL_NO_SURFACE) {
        eglSwapBuffers(ctx->egl_display, ctx->egl_surface);
    }
}
```

- [ ] **Step 3: Add cleanup function**

Add after `egl_swap_buffers()`:

```c
void egl_cleanup(egl_context_t* ctx) {
    if (!ctx) return;

    if (ctx->shader_program) {
        glDeleteProgram(ctx->shader_program);
    }
    if (ctx->vao) {
        glDeleteVertexArrays(1, &ctx->vao);
    }
    if (ctx->vbo) {
        glDeleteBuffers(1, &ctx->vbo);
    }

    if (ctx->egl_surface != EGL_NO_SURFACE) {
        eglMakeCurrent(ctx->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(ctx->egl_display, ctx->egl_surface);
    }

    if (ctx->egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(ctx->egl_display, ctx->egl_context);
    }

    if (ctx->egl_display != EGL_NO_DISPLAY) {
        eglTerminate(ctx->egl_display);
    }

    free(ctx);
}
```

- [ ] **Step 4: Build and verify**

```bash
ninja -C builddir
```

Expected: Clean build.

- [ ] **Step 5: Commit**

```bash
git add egl.c
git commit -m "egl: implement EGLImage creation and fullscreen quad rendering"
```

---

### Task 7: Integrate EGL rendering path into x11_refresh_image() (x11.c)

**Files:**
- Modify: `x11.c`

- [ ] **Step 1: Modify x11_refresh_image() to route through EGL path**

Find the existing `x11_refresh_image()` function and replace the cross-server section (around lines 297-349) with:

```c
gboolean x11_refresh_image() {
    if (!enabled) return TRUE;

    // Capture from source display
    if (use_cross_server && src_display) {
        // XCopyArea from source root to persistent pixmap
        XGCValues gc_vals = {0};
        GC gc = XCreateGC(src_display, src_rect.width, 0, &gc_vals);
        
        XCopyArea(src_display, src_root, src_pixmap, gc,
                 src_rect.x, src_rect.y, src_rect.width, src_rect.height,
                 0, 0);

        XFreeGC(src_display, gc);
        XSync(src_display, False);  // Wait for blit

        // Try DRI3/EGL path first
        if (dri3_available && egl_ctx) {
            uint32_t drm_format = 0;
            int stride = 0;
            int fd = dri3_export_pixmap(src_xcb, src_pixmap, 
                                       src_rect.width, src_rect.height,
                                       &drm_format, &stride);
            if (fd >= 0) {
                // If this is the first frame, create texture from fd
                if (egl_texture == 0) {
                    egl_texture = egl_create_image_from_fd(egl_ctx, fd, 
                                                          src_rect.width, src_rect.height,
                                                          stride, drm_format);
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
                    egl_swap_buffers(egl_ctx);
                    return TRUE;
                }
            }
        }

        // Fallback to XShm if DRI3 unavailable
        #ifdef HAVE_MIT_SHM
        return x11_refresh_image_shm_fallback();
        #else
        fprintf(stderr, "x11: cross-server mode requires DRI3 or MIT-SHM\n");
        return FALSE;
        #endif
    }

    // Original single-server path (unchanged)
    // ... rest of function
}
```

- [ ] **Step 2: Add pixmap creation in x11_init()**

In `x11_init()`, after DRI3 check, add:

```c
    // Create persistent pixmap for DRI3 export
    if (dri3_available) {
        Pixmap depth = DefaultDepth(src_display, DefaultScreen(src_display));
        src_pixmap = XCreatePixmap(src_display, src_root,
                                  src_rect.width, src_rect.height, depth);
        fprintf(stderr, "x11: created source pixmap %lx\n", src_pixmap);
    }
```

- [ ] **Step 3: Add pixmap cleanup in x11_disable()**

In `x11_disable()`, add:

```c
    if (src_pixmap) {
        XFreePixmap(src_display, src_pixmap);
        src_pixmap = 0;
    }
    if (egl_ctx) {
        egl_cleanup(egl_ctx);
        egl_ctx = NULL;
        egl_texture = 0;
    }
```

- [ ] **Step 4: Add pixmap global declaration in x11.c**

Near the existing cross-server globals (line 102), add:

```c
static Pixmap src_pixmap = 0;
```

- [ ] **Step 5: Build and test**

```bash
ninja -C builddir
```

Expected: Clean build. All symbols should resolve.

- [ ] **Step 6: Commit**

```bash
git add x11.c
git commit -m "x11: integrate EGL/DRI3 path into x11_refresh_image() with XShm fallback"
```

---

### Task 8: Update squint.h to include egl.h forward declaration

**Files:**
- Modify: `squint.h`

- [ ] **Step 1: Add egl.h forward declaration**

At the top of `squint.h` (after GTK includes), add:

```c
// Forward declaration of EGL context (defined in egl.c)
typedef struct egl_context egl_context_t;
```

This allows x11.c to use `egl_context_t*` without polluting the header with EGL types.

- [ ] **Step 2: Verify build**

```bash
ninja -C builddir
```

Expected: Clean build.

- [ ] **Step 3: Commit**

```bash
git add squint.h
git commit -m "headers: add egl_context_t forward declaration"
```

---

### Task 9: Test with real hardware (manual verification)

**Files:**
- No files modified; manual testing phase

**Prerequisites:** Dual-GPU hardware (AMD + Intel iGPU), Xorg with DRI3 support, libxcb-dri3 installed

- [ ] **Step 1: Build the code**

```bash
ninja -C builddir
```

- [ ] **Step 2: Run igpu-server-up.sh to prepare hardware**

```bash
./tools/igpu-server-up.sh
```

This detaches the iGPU outputs from `:0` and starts `:1` bound to Intel.

- [ ] **Step 3: Launch Squint with cross-GPU mirroring**

```bash
./builddir/squint --src-display :0 --dst-display :1 -S 0 -D 1
```

- [ ] **Step 4: Verify mirror appears on iGPU monitor**

The iGPU monitor should show a mirror of AMD monitor 0. Verify:
- Image appears correctly (no corruption)
- Cursor follows source
- Updates occur smoothly
- No visual tearing

- [ ] **Step 5: Run performance benchmark**

While Squint is running, in another terminal:

```bash
# Get frame timing from Squint's stderr or internal counters
# (if Squint has timing logs enabled)
# Expected: >= 50 fps (capped) with <1ms latency
```

- [ ] **Step 6: Tear down and restore**

```bash
./tools/igpu-server-down.sh
```

- [ ] **Step 7: If all tests pass, document results**

No commit needed for manual testing — results go to the user's notes.

---

## Spec Coverage Checklist

✅ **EGL context initialization** — Task 3  
✅ **GL shader + VAO/VBO setup** — Task 4  
✅ **DRI3 pixmap export** — Task 5  
✅ **EGLImage creation from DMA-BUF fd** — Task 6  
✅ **Fullscreen quad rendering** — Task 6  
✅ **Integration into x11_refresh_image()** — Task 7  
✅ **XShm fallback when DRI3 unavailable** — Task 7  
✅ **Backward compatibility (single-server path unchanged)** — Task 7  
✅ **Dependencies added to meson.build** — Task 1  
✅ **Manual hardware testing** — Task 9
