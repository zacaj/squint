/*
 * linear-pixmap-test.c — de-risk Path A for cross-GPU Squint.
 *
 * AMD (:0) tiles its framebuffer with a vendor modifier Intel can't import,
 * and DRI3 reports MOD_INVALID. The only format both GPUs share is LINEAR.
 * This test allocates a LINEAR buffer on AMD's own device (so AMD de-tiles
 * during XCopyArea), wraps it as an X pixmap, copies the root desktop into
 * it, then imports the SAME linear fd into Intel EGL and reads back what GL
 * sampled. If the readback is a clean desktop image (not horizontally
 * streaked), Path A works.
 *
 * libgbm has no -devel package here, so we dlopen libgbm.so.1 and declare the
 * handful of prototypes we need -- no new build dependency.
 *
 * build: gcc -O2 -o linear-pixmap-test linear-pixmap-test.c \
 *        -lX11 -lX11-xcb -lxcb -lxcb-dri3 -lEGL -lGL
 * run:   XAUTHORITY=/tmp/xauth-squint-combined ./linear-pixmap-test :0 :1 3072 1728 2400 972
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
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* ---- minimal vendored gbm declarations ---- */
struct gbm_device;
struct gbm_bo;
#define GBM_FORMAT_XRGB8888 0x34325258
#define GBM_BO_USE_RENDERING (1u << 2)
#define GBM_BO_USE_LINEAR    (1u << 4)
typedef struct gbm_device* (*pfn_gbm_create_device)(int fd);
typedef struct gbm_bo* (*pfn_gbm_bo_create)(struct gbm_device*, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags);
typedef int (*pfn_gbm_bo_get_fd)(struct gbm_bo*);
typedef uint32_t (*pfn_gbm_bo_get_stride)(struct gbm_bo*);
typedef uint64_t (*pfn_gbm_bo_get_modifier)(struct gbm_bo*);

#define DRM_FORMAT_MOD_LINEAR 0ULL

static const char* vsrc =
    "#version 120\n"
    "attribute vec2 pos; attribute vec2 uv; varying vec2 t;\n"
    "void main(){ t=uv; gl_Position=vec4(pos,0.0,1.0); }\n";
static const char* fsrc =
    "#version 120\n"
    "varying vec2 t; uniform sampler2D s;\n"
    "void main(){ gl_FragColor = texture2D(s, t); }\n";

static GLuint mkshader(GLenum type, const char* src){
    GLuint s=glCreateShader(type); glShaderSource(s,1,&src,NULL); glCompileShader(s);
    GLint ok; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){char l[512];glGetShaderInfoLog(s,512,NULL,l);fprintf(stderr,"shader: %s\n",l);}
    return s;
}

int main(int argc, char** argv){
    const char* src_name = argc>1?argv[1]:":0";
    const char* dst_name = argc>2?argv[2]:":1";
    int w = argc>3?atoi(argv[3]):3072;
    int h = argc>4?atoi(argv[4]):1728;
    int ox= argc>5?atoi(argv[5]):2400;
    int oy= argc>6?atoi(argv[6]):972;

    /* dlopen gbm */
    void* gbmlib = dlopen("libgbm.so.1", RTLD_NOW);
    if(!gbmlib){ fprintf(stderr,"dlopen libgbm.so.1 failed: %s\n", dlerror()); return 1; }
    pfn_gbm_create_device   gbm_create_device   = dlsym(gbmlib,"gbm_create_device");
    pfn_gbm_bo_create       gbm_bo_create       = dlsym(gbmlib,"gbm_bo_create");
    pfn_gbm_bo_get_fd       gbm_bo_get_fd       = dlsym(gbmlib,"gbm_bo_get_fd");
    pfn_gbm_bo_get_stride   gbm_bo_get_stride   = dlsym(gbmlib,"gbm_bo_get_stride");
    pfn_gbm_bo_get_modifier gbm_bo_get_modifier = dlsym(gbmlib,"gbm_bo_get_modifier");
    if(!gbm_create_device||!gbm_bo_create||!gbm_bo_get_fd||!gbm_bo_get_stride){
        fprintf(stderr,"dlsym gbm failed\n"); return 1; }

    /* source (AMD) */
    Display* src = XOpenDisplay(src_name);
    if(!src){fprintf(stderr,"open %s failed\n",src_name);return 1;}
    xcb_connection_t* xcb = XGetXCBConnection(src);
    Window root = DefaultRootWindow(src);

    /* get AMD device fd via DRI3 open */
    xcb_dri3_open_cookie_t oc = xcb_dri3_open(xcb, root, 0);
    xcb_dri3_open_reply_t* orep = xcb_dri3_open_reply(xcb, oc, NULL);
    if(!orep){fprintf(stderr,"xcb_dri3_open failed\n");return 1;}
    int* ofds = xcb_dri3_open_reply_fds(xcb, orep);
    int amd_fd = ofds[0];
    fprintf(stderr,"AMD device fd=%d\n", amd_fd);

    struct gbm_device* gdev = gbm_create_device(amd_fd);
    if(!gdev){fprintf(stderr,"gbm_create_device failed\n");return 1;}

    struct gbm_bo* bo = gbm_bo_create(gdev, w, h, GBM_FORMAT_XRGB8888,
                                      GBM_BO_USE_LINEAR|GBM_BO_USE_RENDERING);
    if(!bo){fprintf(stderr,"gbm_bo_create LINEAR failed\n");return 1;}
    uint32_t stride = gbm_bo_get_stride(bo);
    uint64_t mod = gbm_bo_get_modifier ? gbm_bo_get_modifier(bo) : 0;
    fprintf(stderr,"linear bo: stride=%u modifier=0x%llx (expect 0)\n",
            stride, (unsigned long long)mod);

    /* wrap bo as an X pixmap on :0 (consumes one fd) */
    int fd_for_x = gbm_bo_get_fd(bo);
    xcb_pixmap_t px = xcb_generate_id(xcb);
    uint32_t size = stride * h;
    xcb_void_cookie_t pc = xcb_dri3_pixmap_from_buffer(xcb, px, root, size,
                                w, h, stride, 24, 32, fd_for_x);
    xcb_generic_error_t* perr = xcb_request_check(xcb, pc);
    if(perr){fprintf(stderr,"pixmap_from_buffer failed err=%d\n", perr->error_code);return 1;}
    fprintf(stderr,"created linear X pixmap 0x%x\n", px);

    /* XCopyArea root -> linear pixmap with IncludeInferiors (grab children) */
    XGCValues gcv; gcv.subwindow_mode = IncludeInferiors;
    GC gc = XCreateGC(src, px, GCSubwindowMode, &gcv);
    XCopyArea(src, root, px, gc, ox, oy, w, h, 0, 0);
    XSync(src, False);
    fprintf(stderr,"copied root(%d,%d %dx%d) into linear pixmap\n", ox,oy,w,h);

    /* import the SAME linear buffer into Intel EGL */
    int fd_for_egl = gbm_bo_get_fd(bo);   /* fresh fd for the same bo */

    Display* dst = XOpenDisplay(dst_name);
    EGLDisplay ed = eglGetDisplay((EGLNativeDisplayType)dst);
    EGLint maj,min; eglInitialize(ed,&maj,&min);
    eglBindAPI(EGL_OPENGL_API);
    EGLint cfgattr[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
    EGLConfig cfg; EGLint nc; eglChooseConfig(ed,cfgattr,&cfg,1,&nc);
    EGLContext ectx = eglCreateContext(ed,cfg,EGL_NO_CONTEXT,NULL);
    EGLint pb[]={EGL_WIDTH,16,EGL_HEIGHT,16,EGL_NONE};
    EGLSurface esurf = eglCreatePbufferSurface(ed,cfg,pb);
    eglMakeCurrent(ed,esurf,esurf,ectx);

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR=(void*)eglGetProcAddress("eglCreateImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES=
        (void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    EGLint iattr[]={
        EGL_WIDTH,w, EGL_HEIGHT,h,
        EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd_for_egl,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
        EGL_NONE};
    EGLImageKHR img = eglCreateImageKHR(ed,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,NULL,iattr);
    if(img==EGL_NO_IMAGE_KHR){fprintf(stderr,"eglCreateImageKHR failed 0x%x\n",eglGetError());return 1;}
    GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D,img);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);

    /* render textured quad into an FBO at full res, read back */
    GLuint fbo,rt; glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glGenTextures(1,&rt); glBindTexture(GL_TEXTURE_2D,rt);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rt,0);
    glViewport(0,0,w,h);

    GLuint prog=glCreateProgram();
    glAttachShader(prog,mkshader(GL_VERTEX_SHADER,vsrc));
    glAttachShader(prog,mkshader(GL_FRAGMENT_SHADER,fsrc));
    glBindAttribLocation(prog,0,"pos"); glBindAttribLocation(prog,1,"uv");
    glLinkProgram(prog); glUseProgram(prog);
    /* fullscreen quad, uv flipped so readback is top-left origin */
    float quad[]={-1,-1, 0,1,  1,-1, 1,1,  -1,1, 0,0,  1,1, 1,0};
    GLuint vbo; glGenBuffers(1,&vbo); glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,0,16,(void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,2,GL_FLOAT,0,16,(void*)8);
    glBindTexture(GL_TEXTURE_2D,tex);
    glUniform1i(glGetUniformLocation(prog,"s"),0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex);
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glFinish();

    unsigned char* buf=malloc((size_t)w*h*4);
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,buf);
    long nb=0; for(long i=0;i<(long)w*h;i++) if(buf[i*4]||buf[i*4+1]||buf[i*4+2]) nb++;
    fprintf(stderr,"GL readback: %ld/%ld non-black (%.1f%%)\n", nb,(long)w*h,100.0*nb/((double)w*h));
    FILE* f=fopen(argc>7?argv[7]:"linear-gl.ppm","wb");
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(long i=0;i<(long)w*h;i++){fputc(buf[i*4],f);fputc(buf[i*4+1],f);fputc(buf[i*4+2],f);}
    fclose(f);
    fprintf(stderr,"wrote GL readback ppm\n");
    return 0;
}
