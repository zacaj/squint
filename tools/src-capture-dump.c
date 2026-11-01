/* src-capture-dump.c — replicate squint's exact source-capture step and dump it.
 *
 * XCopyArea(:0 root -> pixmap)  [what squint/dmabuf-test do]
 *   then XGetImage(pixmap) and write a PPM so we can see if it's real content
 *   or black. Isolates the SOURCE side from all DRI3/EGL machinery.
 *
 * build: gcc -O2 -o src-capture-dump src-capture-dump.c -lX11
 * run:   XAUTHORITY=/tmp/xauth-squint-combined ./src-capture-dump :0 0 0 1920 1080 out.ppm
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    const char* dpy_name = argc > 1 ? argv[1] : ":0";
    int x = argc > 2 ? atoi(argv[2]) : 0;
    int y = argc > 3 ? atoi(argv[3]) : 0;
    int w = argc > 4 ? atoi(argv[4]) : 1920;
    int h = argc > 5 ? atoi(argv[5]) : 1080;
    const char* out = argc > 6 ? argv[6] : "out.ppm";

    Display* dpy = XOpenDisplay(dpy_name);
    if (!dpy) { fprintf(stderr, "cannot open %s\n", dpy_name); return 1; }

    Window root = DefaultRootWindow(dpy);
    int depth = DefaultDepth(dpy, DefaultScreen(dpy));
    Pixmap px = XCreatePixmap(dpy, root, w, h, depth);
    GC gc = XCreateGC(dpy, px, 0, NULL);

    /* mode A: exact squint operation — XCopyArea root->pixmap, read pixmap */
    XCopyArea(dpy, root, px, gc, x, y, w, h, 0, 0);
    XSync(dpy, False);
    XImage* imgA = XGetImage(dpy, px, 0, 0, w, h, AllPlanes, ZPixmap);
    long nbA = 0;
    if (imgA) {
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            unsigned long p = XGetPixel(imgA, i, j);
            if ((p & 0xffffff)) nbA++;
        }
        XDestroyImage(imgA);
    }
    fprintf(stderr, "[A] XCopyArea(root)->pixmap: %ld/%ld non-black (%.1f%%)\n",
            nbA, (long)w*h, 100.0*nbA/((double)w*h));

    /* mode C: XCopyArea root->pixmap but with IncludeInferiors GC */
    XGCValues gcv; gcv.subwindow_mode = IncludeInferiors;
    GC gc2 = XCreateGC(dpy, px, GCSubwindowMode, &gcv);
    XCopyArea(dpy, root, px, gc2, x, y, w, h, 0, 0);
    XSync(dpy, False);
    XImage* imgC = XGetImage(dpy, px, 0, 0, w, h, AllPlanes, ZPixmap);
    long nbC = 0;
    if (imgC) {
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            unsigned long p = XGetPixel(imgC, i, j);
            if ((p & 0xffffff)) nbC++;
        }
        XDestroyImage(imgC);
    }
    fprintf(stderr, "[C] XCopyArea(root,IncludeInferiors)->pixmap: %ld/%ld non-black (%.1f%%)\n",
            nbC, (long)w*h, 100.0*nbC/((double)w*h));

    /* mode B: XGetImage DIRECTLY on root (what a screengrabber does) */
    XImage* img = XGetImage(dpy, root, x, y, w, h, AllPlanes, ZPixmap);
    if (!img) { fprintf(stderr, "[B] XGetImage(root) failed\n"); return 1; }

    /* count non-black pixels + write PPM */
    long nonblack = 0;
    FILE* f = fopen(out, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            unsigned long p = XGetPixel(img, i, j);
            unsigned char r = (p >> 16) & 0xff;
            unsigned char g = (p >> 8) & 0xff;
            unsigned char b = p & 0xff;
            if (r || g || b) nonblack++;
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    }
    fclose(f);
    fprintf(stderr, "captured %dx%d from %s root at +%d+%d: %ld/%ld non-black pixels (%.1f%%)\n",
            w, h, dpy_name, x, y, nonblack, (long)w*h, 100.0*nonblack/((double)w*h));
    return 0;
}
