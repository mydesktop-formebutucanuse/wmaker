/* wmnotch.c - shades the camera notch of the screen
 *
 *  Window Maker window manager
 *
 *  Copyright (c) 2026 Window Maker Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Window Maker starts this program on a screen with a camera notch (see
 * src/notch.c), and tells it where the notch is with the _WINDOWMAKER_NOTCH
 * property of the root window.
 *
 * The top of the screen is shaded: black at the top edge, where the notch
 * is, and fading out further down. The shade lies over every window, and
 * what is under it shows through. There is no compositing manager to blend
 * it, so this program does that itself, for that strip of the screen only:
 * the windows that reach into the strip are redirected off-screen (the
 * server still draws them to the screen itself, where they are not under
 * the strip), and the strip is drawn from their contents, darkened, into
 * the overlay window of the Composite extension, which is above every other
 * window. Nothing else on the screen is touched.
 */

#include "config.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

/* The strip is drawn again at most this often, in milliseconds */
#define FRAME_PERIOD 8

/* Nothing sane is higher than this, in pixels */
#define MAX_HEIGHT 1024

/* The values of the _WINDOWMAKER_NOTCH property, see src/notch.c */
enum {
	NOTCH_X,
	NOTCH_Y,
	NOTCH_WIDTH,
	NOTCH_HEIGHT,
	NOTCH_FADE,
	NOTCH_COVERED,
	NOTCH_COUNT
};

/* A child of the root window; x and y are those of its border */
typedef struct NWindow {
	struct NWindow *below;
	Window id;
	int x, y, width, height, border;
	Visual *visual;
	Bool mapped;
	Bool input_only;
	Bool redirected;	/* its contents are kept off-screen for us */
	Bool stale;		/* its damage under the strip is to be forgotten */
	Damage damage;
	Pixmap pixmap;
	Picture picture;
} NWindow;

static const char *prog_name;
static Display *dpy;
static Window root;
static int depth;
static Visual *visual;
static int damage_event, shape_event;
static Atom notch_atom;
static Atom background_atom[2];

static NWindow *top_window;

/* What Window Maker asks for */
static long notch[NOTCH_COUNT];

/* The part of the screen that is shaded; nothing is while the width is 0 */
static XRectangle area;
/* Whether the windows show through the strip; it is plain black otherwise */
static Bool see_through;

static Window overlay;
static GC gc;
static Pixmap buffer;		/* what the overlay shows */
static Picture buffer_picture;
static Picture shade;		/* black, less opaque row after row */
static Pixmap background;	/* of the root window, None when unknown */
static Bool dirty;


static int ignore_error(Display *d, XErrorEvent *event)
{
	/*
	 * The windows belong to other programs, which destroy them when they
	 * like: a request that comes too late for one is just dropped.
	 */
	(void) d;
	(void) event;
	return 0;
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/*
 * How much of what is under the shade shows through its row y, out of
 * 65535, when it is len rows long: nothing at the top edge of the screen,
 * all of it in the last row. (3t - t^3) / 2 starts to clear right at the top
 * edge and ends flat, so that the end of the shade does not show as an edge.
 */
static unsigned int clearness(int y, int len)
{
	double t;

	if (len <= 1)
		return 0;

	t = (double) y / (len - 1);
	return (unsigned int) (65535.0 * t * (3.0 - t * t) / 2.0 + 0.5);
}

/* ---- The windows ---- */

static NWindow **find_window(Window id)
{
	NWindow **link;

	for (link = &top_window; *link != NULL; link = &(*link)->below) {
		if ((*link)->id == id)
			break;
	}
	return link;
}

static Bool in_area(const NWindow *w)
{
	if (area.width == 0 || !see_through || !w->mapped || w->input_only)
		return False;

	return w->x < area.x + area.width && w->x + w->width + 2 * w->border > area.x
		&& w->y < area.y + area.height && w->y + w->height + 2 * w->border > area.y;
}

static void forget_contents(NWindow *w)
{
	if (w->picture != None)
		XRenderFreePicture(dpy, w->picture);
	if (w->pixmap != None)
		XFreePixmap(dpy, w->pixmap);
	w->picture = None;
	w->pixmap = None;
}

/* Keeps the contents of the window off-screen while it is under the strip */
static void update_window(NWindow *w)
{
	Bool wanted = in_area(w);

	if (wanted && !w->redirected) {
		XCompositeRedirectWindow(dpy, w->id, CompositeRedirectAutomatic);
		/*
		 * Only what is drawn in a part that is not damaged yet is
		 * reported, and only the part under the strip is ever
		 * repaired: after the first report, whatever else the window
		 * draws costs nothing.
		 */
		w->damage = XDamageCreate(dpy, w->id, XDamageReportDeltaRectangles);
		XShapeSelectInput(dpy, w->id, ShapeNotifyMask);
		w->redirected = True;
	} else if (!wanted && w->redirected) {
		forget_contents(w);
		XShapeSelectInput(dpy, w->id, 0);
		XDamageDestroy(dpy, w->damage);
		XCompositeUnredirectWindow(dpy, w->id, CompositeRedirectAutomatic);
		w->damage = None;
		w->redirected = False;
	}

	if (wanted) {
		w->stale = True;
		dirty = True;
	}
}

static void add_window(Window id)
{
	XWindowAttributes attr;
	NWindow *w;

	if (id == overlay || *find_window(id) != NULL)
		return;

	if (!XGetWindowAttributes(dpy, id, &attr))
		return;

	w = calloc(1, sizeof(*w));
	if (w == NULL)
		return;

	w->id = id;
	w->x = attr.x;
	w->y = attr.y;
	w->width = attr.width;
	w->height = attr.height;
	w->border = attr.border_width;
	w->visual = attr.visual;
	w->mapped = (attr.map_state != IsUnmapped);
	w->input_only = (attr.class == InputOnly);

	/* A new window is above its siblings */
	w->below = top_window;
	top_window = w;

	update_window(w);
}

static void remove_window(Window id, Bool destroyed)
{
	NWindow **link = find_window(id);
	NWindow *w = *link;

	if (w == NULL)
		return;

	if (in_area(w))
		dirty = True;

	if (destroyed) {
		/* The server has freed all there was for the window */
		if (w->picture != None)
			XRenderFreePicture(dpy, w->picture);
		if (w->pixmap != None)
			XFreePixmap(dpy, w->pixmap);
	} else {
		w->mapped = False;
		update_window(w);
	}

	*link = w->below;
	free(w);
}

/* Puts the window above the given sibling, at the bottom when there is none */
static void restack_window(NWindow *w, Window above, Bool on_top)
{
	NWindow **link;

	if (on_top ? top_window == w : (above != None && w->below != NULL && w->below->id == above))
		return;

	link = find_window(w->id);
	*link = w->below;

	if (on_top) {
		link = &top_window;
	} else if (above == None) {
		for (link = &top_window; *link != NULL; link = &(*link)->below)
			;
	} else {
		link = find_window(above);
		if (*link == NULL)
			link = &top_window;
	}

	w->below = *link;
	*link = w;

	if (in_area(w))
		dirty = True;
}

static void read_windows(void)
{
	Window root_ret, parent_ret, *children;
	unsigned int count, i;

	if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &count))
		return;

	/* They come from the bottom up */
	for (i = 0; i < count; i++)
		add_window(children[i]);

	if (children != NULL)
		XFree(children);
}

/* ---- The strip ---- */

/* The pixmap that the program which set the desktop background left for others */
static void read_background(void)
{
	int i;

	background = None;

	for (i = 0; i < 2 && background == None; i++) {
		Atom type;
		int format;
		unsigned long count, after;
		unsigned char *data = NULL;

		if (XGetWindowProperty(dpy, root, background_atom[i], 0, 1, False, XA_PIXMAP,
				       &type, &format, &count, &after, &data) != Success)
			continue;

		if (data != NULL) {
			if (type == XA_PIXMAP && format == 32 && count == 1)
				background = *(Pixmap *) data;
			XFree(data);
		}
	}

	dirty = True;
}

static void free_strip(void)
{
	if (shade != None)
		XRenderFreePicture(dpy, shade);
	if (buffer_picture != None)
		XRenderFreePicture(dpy, buffer_picture);
	if (buffer != None)
		XFreePixmap(dpy, buffer);
	shade = None;
	buffer_picture = None;
	buffer = None;
}

static void make_strip(void)
{
	Pixmap pixmap;
	int y;

	buffer = XCreatePixmap(dpy, root, area.width, area.height, depth);
	buffer_picture = XRenderCreatePicture(dpy, buffer, XRenderFindVisualFormat(dpy, visual), 0, NULL);

	if (!see_through)
		return;

	pixmap = XCreatePixmap(dpy, root, area.width, area.height, 32);
	shade = XRenderCreatePicture(dpy, pixmap, XRenderFindStandardFormat(dpy, PictStandardARGB32), 0, NULL);
	XFreePixmap(dpy, pixmap);

	for (y = 0; y < area.height; y++) {
		XRenderColor color = { 0, 0, 0, 0 };

		color.alpha = 65535 - clearness(y, area.height);
		XRenderFillRectangle(dpy, PictOpSrc, shade, &color, 0, y, area.width, 1);
	}
}

/* Takes what Window Maker asks for, from the property of the root window */
static void read_notch(void)
{
	Atom type;
	int format, i;
	unsigned long count, after;
	unsigned char *data = NULL;
	XRectangle new_area = { 0, 0, 0, 0 };
	Bool new_see_through = False;
	NWindow *w;

	memset(notch, 0, sizeof(notch));
	if (XGetWindowProperty(dpy, root, notch_atom, 0, NOTCH_COUNT, False, XA_CARDINAL,
			       &type, &format, &count, &after, &data) == Success && data != NULL) {
		if (type == XA_CARDINAL && format == 32 && count == NOTCH_COUNT) {
			for (i = 0; i < NOTCH_COUNT; i++)
				notch[i] = ((long *) data)[i];
		}
		XFree(data);
	}

	if (notch[NOTCH_WIDTH] > 0 && notch[NOTCH_WIDTH] <= 32767
	    && notch[NOTCH_HEIGHT] >= 0 && notch[NOTCH_HEIGHT] <= MAX_HEIGHT
	    && notch[NOTCH_FADE] >= 0 && notch[NOTCH_FADE] <= MAX_HEIGHT
	    && notch[NOTCH_X] >= -32768 && notch[NOTCH_X] <= 32767
	    && notch[NOTCH_Y] >= -32768 && notch[NOTCH_Y] <= 32767) {
		/* Under a fullscreen window only the notch itself stays, black */
		new_see_through = !notch[NOTCH_COVERED];
		new_area.x = notch[NOTCH_X];
		new_area.y = notch[NOTCH_Y];
		new_area.height = notch[NOTCH_HEIGHT] + (new_see_through ? notch[NOTCH_FADE] : 0);
		if (new_area.height > 0)
			new_area.width = notch[NOTCH_WIDTH];
	}

	if (memcmp(&new_area, &area, sizeof(area)) == 0 && new_see_through == see_through)
		return;

	area = new_area;
	see_through = new_see_through;

	free_strip();
	if (area.width > 0)
		make_strip();

	XShapeCombineRectangles(dpy, overlay, ShapeBounding, 0, 0, &area, area.width > 0 ? 1 : 0, ShapeSet, Unsorted);

	for (w = top_window; w != NULL; w = w->below)
		update_window(w);

	dirty = True;
}

/* Draws the window as it is on the screen, where it is under the strip */
static void draw_window(NWindow *w)
{
	XserverRegion region;

	if (w->stale) {
		XRectangle rect = area;

		/* Its drawing under the strip is to be reported again from here on */
		rect.x -= w->x + w->border;
		rect.y -= w->y + w->border;
		region = XFixesCreateRegion(dpy, &rect, 1);
		XDamageSubtract(dpy, w->damage, region, None);
		XFixesDestroyRegion(dpy, region);
		w->stale = False;
	}

	if (w->picture == None) {
		XRenderPictFormat *format = XRenderFindVisualFormat(dpy, w->visual);

		if (format == NULL)
			return;

		w->pixmap = XCompositeNameWindowPixmap(dpy, w->id);
		w->picture = XRenderCreatePicture(dpy, w->pixmap, format, 0, NULL);
	}

	/* The window may be shaped */
	region = XFixesCreateRegionFromWindow(dpy, w->id, WindowRegionBounding);
	XFixesTranslateRegion(dpy, region, w->x + w->border - area.x, w->y + w->border - area.y);
	XFixesSetPictureClipRegion(dpy, buffer_picture, 0, 0, region);
	XFixesDestroyRegion(dpy, region);

	XRenderComposite(dpy, PictOpSrc, w->picture, None, buffer_picture, 0, 0, 0, 0,
			 w->x - area.x, w->y - area.y, w->width + 2 * w->border, w->height + 2 * w->border);
}

static void show_strip(void)
{
	XCopyArea(dpy, buffer, overlay, gc, 0, 0, area.width, area.height, area.x, area.y);
}

static void draw_strip(void)
{
	NWindow *w, **stack;
	int count, i;

	if (area.width == 0)
		return;

	XSetFillStyle(dpy, gc, FillSolid);
	XFillRectangle(dpy, buffer, gc, 0, 0, area.width, area.height);

	if (see_through) {
		if (background != None) {
			/* Tiled from the origin of the screen, as the root window is */
			XSetTile(dpy, gc, background);
			XSetTSOrigin(dpy, gc, -area.x, -area.y);
			XSetFillStyle(dpy, gc, FillTiled);
			XFillRectangle(dpy, buffer, gc, 0, 0, area.width, area.height);
		}

		count = 0;
		for (w = top_window; w != NULL; w = w->below)
			count++;

		stack = malloc((count + 1) * sizeof(*stack));
		if (stack != NULL) {
			count = 0;
			for (w = top_window; w != NULL; w = w->below) {
				if (w->redirected && in_area(w))
					stack[count++] = w;
			}

			for (i = count - 1; i >= 0; i--)
				draw_window(stack[i]);

			free(stack);
		}

		XFixesSetPictureClipRegion(dpy, buffer_picture, 0, 0, None);
		XRenderComposite(dpy, PictOpOver, shade, None, buffer_picture, 0, 0, 0, 0,
				 0, 0, area.width, area.height);
	}

	show_strip();
}

/* ---- The events ---- */

static void handle_configure(XConfigureEvent *event)
{
	NWindow *w = *find_window(event->window);
	Bool was_in_area;

	if (w == NULL)
		return;

	was_in_area = in_area(w);

	if (w->width != event->width || w->height != event->height || w->border != event->border_width) {
		/* The server keeps its contents in another pixmap from now on */
		forget_contents(w);
	}

	w->x = event->x;
	w->y = event->y;
	w->width = event->width;
	w->height = event->height;
	w->border = event->border_width;

	if (was_in_area)
		dirty = True;

	update_window(w);
	restack_window(w, event->above, False);
}

static void handle_damage(XDamageNotifyEvent *event)
{
	NWindow *w = *find_window(event->drawable);
	int x, y;

	if (w == NULL || !w->redirected)
		return;

	x = w->x + w->border + event->area.x;
	y = w->y + w->border + event->area.y;
	if (x < area.x + area.width && x + event->area.width > area.x
	    && y < area.y + area.height && y + event->area.height > area.y) {
		w->stale = True;
		dirty = True;
	}
}

static void handle_event(XEvent *event)
{
	NWindow *w;

	switch (event->type) {
	case CreateNotify:
		if (event->xcreatewindow.parent == root)
			add_window(event->xcreatewindow.window);
		break;

	case DestroyNotify:
		if (event->xdestroywindow.event == root)
			remove_window(event->xdestroywindow.window, True);
		break;

	case ReparentNotify:
		if (event->xreparent.event != root)
			break;
		if (event->xreparent.parent == root)
			add_window(event->xreparent.window);
		else
			remove_window(event->xreparent.window, False);
		break;

	case MapNotify:
	case UnmapNotify:
		/* Both events have the same members */
		if (event->xmap.event != root)
			break;
		w = *find_window(event->xmap.window);
		if (w != NULL) {
			if (in_area(w))
				dirty = True;
			/* A window that is mapped again gets another pixmap */
			forget_contents(w);
			w->mapped = (event->type == MapNotify);
			update_window(w);
		}
		break;

	case ConfigureNotify:
		if (event->xconfigure.event == root)
			handle_configure(&event->xconfigure);
		break;

	case CirculateNotify:
		if (event->xcirculate.event != root)
			break;
		w = *find_window(event->xcirculate.window);
		if (w != NULL)
			restack_window(w, None, event->xcirculate.place == PlaceOnTop);
		break;

	case PropertyNotify:
		if (event->xproperty.window != root)
			break;
		if (event->xproperty.atom == notch_atom)
			read_notch();
		else if (event->xproperty.atom == background_atom[0] || event->xproperty.atom == background_atom[1])
			read_background();
		break;

	case Expose:
		if (event->xexpose.window == overlay && event->xexpose.count == 0 && area.width > 0)
			show_strip();
		break;

	default:
		if (event->type == damage_event + XDamageNotify) {
			handle_damage((XDamageNotifyEvent *) event);
		} else if (event->type == shape_event + ShapeNotify) {
			w = *find_window(((XShapeEvent *) event)->window);
			if (w != NULL && in_area(w))
				dirty = True;
		}
		break;
	}
}

static void check_extensions(void)
{
	int event, error, major, minor;

	if (!XCompositeQueryExtension(dpy, &event, &error)
	    || !XCompositeQueryVersion(dpy, &major, &minor) || (major == 0 && minor < 3)) {
		fprintf(stderr, "%s: the display has no Composite extension with an overlay window\n", prog_name);
		exit(1);
	}

	if (!XDamageQueryExtension(dpy, &damage_event, &error) || !XDamageQueryVersion(dpy, &major, &minor)
	    || !XFixesQueryExtension(dpy, &event, &error) || !XFixesQueryVersion(dpy, &major, &minor)
	    || !XRenderQueryExtension(dpy, &event, &error)
	    || !XShapeQueryExtension(dpy, &shape_event, &error)) {
		fprintf(stderr, "%s: the display lacks the Damage, XFixes, Render or Shape extension\n", prog_name);
		exit(1);
	}

	if (XRenderFindVisualFormat(dpy, visual) == NULL
	    || XRenderFindStandardFormat(dpy, PictStandardARGB32) == NULL) {
		fprintf(stderr, "%s: the Render extension cannot draw on this screen\n", prog_name);
		exit(1);
	}
}

int main(int argc, char **argv)
{
	struct pollfd fds[2];
	struct stat st;
	int nfds = 1;
	long next_frame = 0;
	XEvent event;
	char cm_name[32];

	prog_name = argv[0];

	if (argc > 1) {
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
			printf("%s (Window Maker %s)\n", prog_name, VERSION);
			return 0;
		}
		printf("Usage: %s [OPTIONS]\n", prog_name);
		puts("Shades the camera notch at the top of the screen, over every window.");
		puts("Window Maker starts it by itself, where the screen has a notch.");
		puts("");
		puts("  -h, --help               display this help and exit");
		puts("  -v, --version            output version information and exit");
		return strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ? 0 : 1;
	}

	dpy = XOpenDisplay(NULL);
	if (dpy == NULL) {
		fprintf(stderr, "%s: could not open the display \"%s\"\n", prog_name, XDisplayName(NULL));
		return 1;
	}

	root = DefaultRootWindow(dpy);
	depth = DefaultDepth(dpy, DefaultScreen(dpy));
	visual = DefaultVisual(dpy, DefaultScreen(dpy));
	check_extensions();

	/*
	 * A compositing manager draws the whole screen in the overlay window,
	 * shaping it to the strip would hide everything else
	 */
	snprintf(cm_name, sizeof(cm_name), "_NET_WM_CM_S%d", DefaultScreen(dpy));
	if (XGetSelectionOwner(dpy, XInternAtom(dpy, cm_name, False)) != None) {
		fprintf(stderr, "%s: a compositing manager is running, it has to shade the notch itself\n", prog_name);
		return 1;
	}

	XSetErrorHandler(ignore_error);

	notch_atom = XInternAtom(dpy, "_WINDOWMAKER_NOTCH", False);
	background_atom[0] = XInternAtom(dpy, "_XROOTPMAP_ID", False);
	background_atom[1] = XInternAtom(dpy, "ESETROOT_PMAP_ID", False);

	/*
	 * The overlay window is above all the windows, whatever they do. Only
	 * the strip is kept of it, and the pointer goes through it.
	 */
	overlay = XCompositeGetOverlayWindow(dpy, root);
	XShapeCombineRectangles(dpy, overlay, ShapeBounding, 0, 0, NULL, 0, ShapeSet, Unsorted);
	XShapeCombineRectangles(dpy, overlay, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
	XSelectInput(dpy, overlay, ExposureMask);

	/* Black, on a TrueColor screen */
	gc = XCreateGC(dpy, root, 0, NULL);
	XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
	XSetGraphicsExposures(dpy, gc, False);

	XSelectInput(dpy, root, SubstructureNotifyMask | PropertyChangeMask);
	read_windows();
	read_background();
	read_notch();

	fds[0].fd = ConnectionNumber(dpy);
	fds[0].events = POLLIN;

	/* Window Maker holds the other end of the pipe: when it is gone, so are we */
	if (fstat(STDIN_FILENO, &st) == 0 && S_ISFIFO(st.st_mode)) {
		fds[1].fd = STDIN_FILENO;
		fds[1].events = POLLIN;
		nfds = 2;
	}

	for (;;) {
		int timeout = -1;

		while (XPending(dpy) > 0) {
			XNextEvent(dpy, &event);
			handle_event(&event);
		}

		if (dirty) {
			long now = now_ms();

			if (now >= next_frame) {
				draw_strip();
				dirty = False;
				next_frame = now + FRAME_PERIOD;
				continue;
			}
			timeout = next_frame - now;
		}

		fds[0].revents = 0;
		fds[1].revents = 0;
		if (poll(fds, nfds, timeout) < 0 && errno != EINTR)
			break;

		if (nfds == 2 && fds[1].revents != 0) {
			char discard[64];

			if (read(STDIN_FILENO, discard, sizeof(discard)) <= 0)
				break;
		}

		if (fds[0].revents & (POLLERR | POLLHUP))
			break;
	}

	/* The server takes back the overlay window and all the rest */
	XCloseDisplay(dpy);
	return 0;
}
