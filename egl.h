#pragma once

#include <gdk/gdk.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>

// Opaque EGL context handle
typedef struct egl_context egl_context_t;

// Initialize EGL on the destination display
// Returns opaque context handle on success, NULL on failure
egl_context_t* egl_init(GdkDisplay* gdisplay);

// Bind an EGL surface to the given GDK window and make it current.
// Must be called once the destination window is realized, before any
// egl_create_image_from_fd/egl_draw_quad/egl_swap_buffers calls.
gboolean egl_bind_window(egl_context_t* ctx, GdkWindow* gdkwin);

// Create an EGLImage from a DMA-BUF fd (from DRI3 pixmap export)
// fd is closed after import; the EGLImage holds the reference
// modifier is the DRM format modifier from DRI3 1.2 (describes the tiled
// layout); pass DRM_FORMAT_MOD_INVALID to import as linear.
// Returns GL texture name on success, 0 on failure
GLuint egl_create_image_from_fd(egl_context_t* ctx, int fd, int width, int height,
                               int stride, uint32_t drm_format, uint64_t modifier);

// Render fullscreen quad with the given texture
void egl_draw_quad(egl_context_t* ctx, GLuint texture);

// Create an empty RGBA texture (linear filtering, clamp-to-edge) for CPU
// uploads via egl_upload_texture. Requires a current context.
GLuint egl_create_texture(egl_context_t* ctx);

// (Re)upload 32bpp pixels into a texture created by egl_create_texture.
// stride is in bytes; format is GL_RGBA or GL_BGRA.
void egl_upload_texture(egl_context_t* ctx, GLuint texture, int width, int height,
                        int stride, GLenum format, const void* data);

// Alpha-blend a texture over the current frame. x/y/w/h are fractions of the
// window (0..1, top-left origin). Call between egl_draw_quad and swap.
void egl_draw_overlay(egl_context_t* ctx, GLuint texture,
                      float x, float y, float w, float h);

// Swap buffers (flip the EGL surface)
void egl_swap_buffers(egl_context_t* ctx);

// Delete a texture created by egl_create_image_from_fd, releasing its
// reference to the underlying DMA-BUF. Call this when the source buffer is
// reallocated (e.g. a source-monitor switch) before importing a new one.
void egl_delete_texture(egl_context_t* ctx, GLuint texture);

// Destroy the surface bound by egl_bind_window without tearing down the
// context/shader program/VAO/VBO. Use this when the destination window is
// being torn down but the process (and its one-time EGL context) will keep
// running and rebind to a fresh window later via egl_bind_window -- e.g. a
// squint_disable()/squint_enable() cycle, as opposed to process shutdown.
void egl_unbind_window(egl_context_t* ctx);

// DRM primary node (e.g. /dev/dri/card1) of the GPU behind this context, or
// NULL if EGL can't tell (needs EGL_EXT_device_query + EGL_EXT_device_drm).
// g_free() the result.
char* egl_get_drm_device(egl_context_t* ctx);

// Make this context (and its bound window surface, if any) current again,
// e.g. after another EGL context in the process was used.
gboolean egl_make_current(egl_context_t* ctx);

// Cleanup and destroy EGL context
void egl_cleanup(egl_context_t* ctx);
