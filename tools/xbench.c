/*
 * xbench.c — cross-display pixel transfer bandwidth benchmark
 *
 * Runs two back-to-back tests and compares them:
 *   1. Standard: XGetImage → XPutImage (worst case, new alloc each frame)
 *   2. MIT-SHM:  XShmGetImage → XShmPutImage (shared memory, reused buffer)
 *
 * The SHM path is what a real cross-server Squint would use. Both servers
 * attach the same shm segment; the GPU DMAs directly into/out of it,
 * skipping the userspace buffer copy entirely.
 *
 * Build:
 *   gcc -O2 -o xbench xbench.c -lX11 -lXext
 *
 * Usage:
 *   xbench [src_display] [dst_display] [width] [height] [seconds]
 *   xbench :0 :1 3072 1728 10
 *
 * Defaults: src=:0  dst=:1  width=1920  height=1080  duration=10s
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void print_verdict(double fps) {
	if (fps >= 50)
		printf("VERDICT: excellent — can match Squint's 50fps cap\n");
	else if (fps >= 25)
		printf("VERDICT: viable — meets Squint's 25fps XDamage default\n");
	else if (fps >= 10)
		printf("VERDICT: marginal — usable with aggressive rate limiting (-l 10)\n");
	else
		printf("VERDICT: too slow — cross-server approach not viable; consider MST\n");
}

static double bench_standard(Display* src, Display* dst,
                             Window src_root, Pixmap dst_px, GC dst_gc,
                             int width, int height, int duration) {
	printf("\n[standard XGetImage/XPutImage]\n");

	/* warm-up */
	XImage* img = XGetImage(src, src_root, 0, 0, width, height, AllPlanes, ZPixmap);
	if (!img) { fprintf(stderr, "XGetImage failed\n"); return 0; }
	XPutImage(dst, dst_px, dst_gc, img, 0, 0, 0, 0, width, height);
	XSync(dst, False);

	long bytes_per_frame = (long)img->bytes_per_line * height;
	long frames = 0;
	double t0 = now_sec();
	double deadline = t0 + duration;

	while (now_sec() < deadline) {
		XDestroyImage(img);
		img = XGetImage(src, src_root, 0, 0, width, height, AllPlanes, ZPixmap);
		if (!img) break;
		XPutImage(dst, dst_px, dst_gc, img, 0, 0, 0, 0, width, height);
		XSync(dst, False);
		frames++;
	}
	XDestroyImage(img);

	double elapsed = now_sec() - t0;
	double fps  = frames / elapsed;
	double mbps = (fps * bytes_per_frame) / (1024.0 * 1024.0);
	printf("fps: %.1f   bandwidth: %.1f MB/s\n", fps, mbps);
	print_verdict(fps);
	return fps;
}

static double bench_shm(Display* src, Display* dst,
                        Window src_root, Window dst_root,
                        int width, int height, int duration) {
	printf("\n[MIT-SHM XShmGetImage/XShmPutImage]\n");

	if (!XShmQueryExtension(src)) { printf("MIT-SHM not available on src — skipping\n"); return 0; }
	if (!XShmQueryExtension(dst)) { printf("MIT-SHM not available on dst — skipping\n"); return 0; }

	/* Each display needs its own XShmSegmentInfo: shmseg is a per-server
	 * resource ID assigned by XShmAttach and must not be shared. The two
	 * structs share the same shmid/shmaddr (same physical memory). */
	XShmSegmentInfo src_shm = {0}, dst_shm = {0};

	XImage* src_img = XShmCreateImage(src,
	                                  DefaultVisual(src, DefaultScreen(src)),
	                                  DefaultDepth(src, DefaultScreen(src)),
	                                  ZPixmap, NULL, &src_shm, width, height);
	if (!src_img) { fprintf(stderr, "XShmCreateImage (src) failed\n"); return 0; }

	long bytes        = (long)src_img->bytes_per_line * height;
	src_shm.shmid     = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
	src_shm.shmaddr   = shmat(src_shm.shmid, NULL, 0);
	src_shm.readOnly  = False;
	src_img->data     = src_shm.shmaddr;

	/* dst image points to the same physical memory via dst_shm */
	dst_shm.shmid    = src_shm.shmid;
	dst_shm.shmaddr  = src_shm.shmaddr;
	dst_shm.readOnly = False;

	int dst_depth  = DefaultDepth(dst, DefaultScreen(dst));
	XImage* dst_img = XShmCreateImage(dst,
	                                  DefaultVisual(dst, DefaultScreen(dst)),
	                                  dst_depth,
	                                  ZPixmap, dst_shm.shmaddr, &dst_shm, width, height);
	if (!dst_img) { fprintf(stderr, "XShmCreateImage (dst) failed\n"); return 0; }

	XShmAttach(src, &src_shm);
	XSync(src, False);
	XShmAttach(dst, &dst_shm);
	XSync(dst, False);

	Pixmap dst_px = XCreatePixmap(dst, dst_root, width, height, dst_depth);
	GC dst_gc     = XCreateGC(dst, dst_px, 0, NULL);

	/* warm-up */
	XShmGetImage(src, src_root, src_img, 0, 0, AllPlanes);
	XSync(src, False);
	XShmPutImage(dst, dst_px, dst_gc, dst_img, 0, 0, 0, 0, width, height, False);
	XSync(dst, False);

	long frames = 0;
	double t0 = now_sec();
	double deadline = t0 + duration;

	while (now_sec() < deadline) {
		XShmGetImage(src, src_root, src_img, 0, 0, AllPlanes);
		XSync(src, False);
		XShmPutImage(dst, dst_px, dst_gc, dst_img, 0, 0, 0, 0, width, height, False);
		XSync(dst, False);
		frames++;
	}

	double elapsed = now_sec() - t0;
	double fps  = frames / elapsed;
	double mbps = (fps * bytes) / (1024.0 * 1024.0);
	printf("fps: %.1f   bandwidth: %.1f MB/s\n", fps, mbps);
	print_verdict(fps);

	XShmDetach(src, &src_shm);
	XShmDetach(dst, &dst_shm);
	XSync(src, False);
	XSync(dst, False);
	XDestroyImage(src_img);
	XDestroyImage(dst_img);
	shmdt(src_shm.shmaddr);
	shmctl(src_shm.shmid, IPC_RMID, NULL);
	XFreePixmap(dst, dst_px);
	XFreeGC(dst, dst_gc);
	return fps;
}

int main(int argc, char** argv) {
	const char* src_name = argc > 1 ? argv[1] : ":0";
	const char* dst_name = argc > 2 ? argv[2] : ":1";
	int width    = argc > 3 ? atoi(argv[3]) : 1920;
	int height   = argc > 4 ? atoi(argv[4]) : 1080;
	int duration = argc > 5 ? atoi(argv[5]) : 10;

	printf("xbench: src=%s dst=%s  rect=%dx%d  duration=%ds each\n",
	       src_name, dst_name, width, height, duration);

	Display* src = XOpenDisplay(src_name);
	if (!src) { fprintf(stderr, "cannot open src display %s\n", src_name); return 1; }
	Display* dst = XOpenDisplay(dst_name);
	if (!dst) { fprintf(stderr, "cannot open dst display %s\n", dst_name); return 1; }

	Window src_root = DefaultRootWindow(src);
	Window dst_root = DefaultRootWindow(dst);
	int dst_depth   = DefaultDepth(dst, DefaultScreen(dst));

	Pixmap std_dst_px = XCreatePixmap(dst, dst_root, width, height, dst_depth);
	GC     std_dst_gc = XCreateGC(dst, std_dst_px, 0, NULL);

	printf("warming up...");
	fflush(stdout);

	double fps_std = bench_standard(src, dst, src_root,
	                                std_dst_px, std_dst_gc,
	                                width, height, duration);
	double fps_shm = bench_shm(src, dst, src_root, dst_root,
	                            width, height, duration);

	printf("\n=== summary ===\n");
	printf("standard:  %.1f fps\n", fps_std);
	printf("MIT-SHM:   %.1f fps\n", fps_shm);
	if (fps_shm > 0 && fps_std > 0)
		printf("SHM speedup: %.1fx\n", fps_shm / fps_std);

	XFreePixmap(dst, std_dst_px);
	XFreeGC(dst, std_dst_gc);
	XCloseDisplay(src);
	XCloseDisplay(dst);
	return 0;
}
