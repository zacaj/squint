#include "egl.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <gdk/gdkx.h>
#include <epoxy/gl.h>
#include <libdrm/drm_fourcc.h>

struct egl_context {
    EGLDisplay egl_display;
    EGLConfig egl_config;
    EGLContext egl_context;
    EGLSurface egl_surface;
    GLuint shader_program;
    GLuint vao, vbo;
    GLuint overlay_vao, overlay_vbo;  // dynamic quad for egl_draw_overlay
    GdkWindow* gdkwin;
};

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

    // Create fullscreen quad VAO and VBO.
    // The source X pixmap has its origin at the TOP-left (row 0 = top), but GL
    // clip space has -1 at the BOTTOM. So the texcoord V is flipped relative to
    // the vertex Y: screen-bottom (y=-1) samples the source's bottom row (v=1),
    // screen-top (y=+1) samples the source's top row (v=0). Without this flip
    // the mirrored image is upside-down.
    float vertices[] = {
        -1.0f, -1.0f,  0.0f, 1.0f,  // bottom-left  -> source bottom-left
         1.0f, -1.0f,  1.0f, 1.0f,  // bottom-right -> source bottom-right
         1.0f,  1.0f,  1.0f, 0.0f,  // top-right    -> source top-right
        -1.0f,  1.0f,  0.0f, 0.0f   // top-left     -> source top-left
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

    // Second VAO for egl_draw_overlay (e.g. the Wayland-source cursor): same
    // layout, vertex data rewritten per draw.
    glGenVertexArrays(1, &ctx->overlay_vao);
    glGenBuffers(1, &ctx->overlay_vbo);
    glBindVertexArray(ctx->overlay_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->overlay_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(pos_attr);
    glVertexAttribPointer(pos_attr, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(tex_attr);
    glVertexAttribPointer(tex_attr, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    return TRUE;
}

gboolean egl_bind_window(egl_context_t* ctx, GdkWindow* gdkwin) {
    // Idempotent: the destination window persists across source switches, but a
    // native window can hold only one EGLSurface -- calling
    // eglCreateWindowSurface a second time for it fails (EGL_BAD_ALLOC). If
    // we're already bound to this same window, just make our context current
    // and reuse the existing surface and GL state.
    if (ctx->egl_surface != EGL_NO_SURFACE && ctx->gdkwin == gdkwin) {
        return eglMakeCurrent(ctx->egl_display, ctx->egl_surface,
                              ctx->egl_surface, ctx->egl_context);
    }

    Window x_window = gdk_x11_window_get_xid(gdkwin);

    EGLint surface_attrs[] = { EGL_NONE };
    EGLSurface surface = eglCreateWindowSurface(ctx->egl_display, ctx->egl_config,
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

egl_context_t* egl_init(GdkDisplay* gdisplay) {
    // Get the GDK X11 display
    Display* x_display = gdk_x11_display_get_xdisplay(gdisplay);
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
    fprintf(stderr, "egl_init: EGL %d.%d initialized (vendor=%s)\n", major, minor,
            eglQueryString(egl_display, EGL_VENDOR));

    // Bind OpenGL API
    eglBindAPI(EGL_OPENGL_API);

    // Choose EGL config (RGBA, 24-bit depth)
    EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
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
    ctx->egl_config = config;
    ctx->egl_context = egl_context;
    ctx->egl_surface = EGL_NO_SURFACE;
    ctx->gdkwin = NULL;
    ctx->shader_program = 0;
    ctx->vao = ctx->vbo = 0;
    ctx->overlay_vao = ctx->overlay_vbo = 0;

    return ctx;
}

GLuint egl_create_image_from_fd(egl_context_t* ctx, int fd, int width, int height,
                               int stride, uint32_t drm_format, uint64_t modifier) {
    if (!ctx || fd < 0) return 0;

    // EGLImage attributes for DMA-BUF import. When the exporting driver
    // reported a real DRM format modifier (i.e. the buffer is tiled/compressed
    // rather than linear), we must pass it via the PLANE0_MODIFIER_LO/HI attrs
    // so the importing driver de-tiles correctly. Omitting it makes EGL assume
    // a linear layout, which produces a horizontally-streaked image from a
    // tiled AMD buffer.
    EGLint img_attrs[24];
    int n = 0;
    img_attrs[n++] = EGL_WIDTH;                    img_attrs[n++] = width;
    img_attrs[n++] = EGL_HEIGHT;                   img_attrs[n++] = height;
    img_attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;     img_attrs[n++] = drm_format;
    img_attrs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;    img_attrs[n++] = fd;
    img_attrs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; img_attrs[n++] = 0;
    img_attrs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; img_attrs[n++] = stride;
    if (modifier != DRM_FORMAT_MOD_INVALID) {
        img_attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        img_attrs[n++] = (EGLint)(modifier & 0xFFFFFFFF);
        img_attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        img_attrs[n++] = (EGLint)(modifier >> 32);
    }
    img_attrs[n++] = EGL_NONE;

    fprintf(stderr, "egl: creating image from fd=%d %dx%d stride=%d format=0x%x modifier=0x%llx\n",
            fd, width, height, stride, drm_format, (unsigned long long)modifier);

    EGLImageKHR image = eglCreateImageKHR(ctx->egl_display, EGL_NO_CONTEXT,
                                          EGL_LINUX_DMA_BUF_EXT, NULL, img_attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "egl: eglCreateImageKHR failed, eglGetError=0x%x\n", eglGetError());
        return 0;
    }

    // Create GL texture from EGLImage
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    {
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            fprintf(stderr, "egl: GL error 0x%x from glEGLImageTargetTexture2DOES\n", err);
        }
    }

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

static void check_gl_error(const char* where) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "egl: GL error 0x%x at %s\n", err, where);
    }
}

void egl_draw_quad(egl_context_t* ctx, GLuint texture) {
    if (!ctx || !texture) return;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    check_gl_error("clear");

    glUseProgram(ctx->shader_program);
    check_gl_error("useProgram");

    glBindVertexArray(ctx->vao);
    glBindTexture(GL_TEXTURE_2D, texture);
    check_gl_error("bind vao/texture");

    glUniform1i(glGetUniformLocation(ctx->shader_program, "texture0"), 0);

    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    check_gl_error("drawArrays");

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(0);
}

GLuint egl_create_texture(egl_context_t* ctx) {
    if (!ctx) return 0;
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

void egl_upload_texture(egl_context_t* ctx, GLuint texture, int width, int height,
                        int stride, GLenum format, const void* data) {
    if (!ctx || !texture || !data) return;
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, format,
                 GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    check_gl_error("upload_texture");
}

void egl_draw_overlay(egl_context_t* ctx, GLuint texture,
                      float x, float y, float w, float h) {
    if (!ctx || !texture || !ctx->overlay_vao) return;

    // Window fractions (top-left origin) -> clip space (bottom-left origin).
    float x0 = x * 2.0f - 1.0f, x1 = (x + w) * 2.0f - 1.0f;
    float y0 = 1.0f - y * 2.0f, y1 = 1.0f - (y + h) * 2.0f;
    float vertices[] = {
        x0, y1,  0.0f, 1.0f,
        x1, y1,  1.0f, 1.0f,
        x1, y0,  1.0f, 0.0f,
        x0, y0,  0.0f, 0.0f,
    };

    glUseProgram(ctx->shader_program);
    glBindVertexArray(ctx->overlay_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->overlay_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(ctx->shader_program, "texture0"), 0);

    // Cursor bitmaps are straight (non-premultiplied) alpha.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisable(GL_BLEND);
    check_gl_error("draw_overlay");

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(0);
}

void egl_swap_buffers(egl_context_t* ctx) {
    if (ctx && ctx->egl_surface != EGL_NO_SURFACE) {
        eglSwapBuffers(ctx->egl_display, ctx->egl_surface);
    }
}

void egl_delete_texture(egl_context_t* ctx, GLuint texture) {
    if (!ctx || !texture) return;
    // Make sure our context is current before touching GL objects -- the
    // caller may invoke this from teardown paths where another context could
    // be current. The texture holds the only remaining reference to the
    // imported DMA-BUF (the EGLImage was destroyed at import time), so deleting
    // it releases that buffer.
    eglMakeCurrent(ctx->egl_display, ctx->egl_surface, ctx->egl_surface, ctx->egl_context);
    glDeleteTextures(1, &texture);
}

char* egl_get_drm_device(egl_context_t* ctx) {
    if (!ctx || !epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_EXT_device_query")) return NULL;
    EGLAttrib dev = 0;
    if (!eglQueryDisplayAttribEXT(ctx->egl_display, EGL_DEVICE_EXT, &dev) || !dev) return NULL;
    const char* exts = eglQueryDeviceStringEXT((EGLDeviceEXT)dev, EGL_EXTENSIONS);
    if (!exts || !strstr(exts, "EGL_EXT_device_drm")) return NULL;
    const char* file = eglQueryDeviceStringEXT((EGLDeviceEXT)dev, EGL_DRM_DEVICE_FILE_EXT);
    return file ? g_strdup(file) : NULL;
}

gboolean egl_make_current(egl_context_t* ctx) {
    if (!ctx) return FALSE;
    return eglMakeCurrent(ctx->egl_display, ctx->egl_surface, ctx->egl_surface, ctx->egl_context);
}

void egl_unbind_window(egl_context_t* ctx) {
    if (!ctx) return;

    if (ctx->egl_surface != EGL_NO_SURFACE) {
        eglMakeCurrent(ctx->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(ctx->egl_display, ctx->egl_surface);
        ctx->egl_surface = EGL_NO_SURFACE;
    }
    ctx->gdkwin = NULL;
}

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
    if (ctx->overlay_vao) {
        glDeleteVertexArrays(1, &ctx->overlay_vao);
    }
    if (ctx->overlay_vbo) {
        glDeleteBuffers(1, &ctx->overlay_vbo);
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
