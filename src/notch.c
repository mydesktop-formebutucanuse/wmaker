/*
 *  Window Maker window manager
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
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Camera notch of the screen
 *
 * Some laptops have a camera housing that is cut out of the top of the
 * panel. Such a panel is higher than 16:10 by the height of the housing, so
 * the strip of the screen at the height of the notch is found from the size
 * of the head alone.
 *
 * The strip is kept out of the area of the maximized and of the fullscreen
 * windows, and is shaded with a gradient that is black at the edge of the
 * screen and fades away below the strip, so that the notch melts into it.
 * The shade lies over the windows, which stay visible below it. Blending it
 * needs the contents of the windows, so it is drawn by a helper, wmnotch (see
 * util/wmnotch.c), which is started here and stops when its standard input
 * is closed, that is to say when Window Maker exits or restarts. With
 * NotchFade = 0 the strip is kept free but not shaded: the helper is not
 * started, or is stopped.
 *
 * While a fullscreen window is shown on the head, a video for instance, the
 * strip is plain black instead. This does not need the helper, which may not
 * be there: a black window is put over the strip, above all the others.
 *
 * The helper is told where the notch is with the _WINDOWMAKER_NOTCH property
 * of the root window, which is a list of 6 CARDINAL:
 *   x, y, width, height   strip of the screen at the height of the notch
 *   fade                  count of rows below the strip where the shade ends
 *   covered               1 while a fullscreen window is shown on the head,
 *                         the strip is then plain black and without fade
 */

#include "wconfig.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <WINGs/WUtil.h>

#include "WindowMaker.h"
#include "screen.h"
#include "window.h"
#include "framewin.h"
#include "actions.h"
#include "xinerama.h"
#include "event.h"
#include "main.h"
#include "stacking.h"
#include "wcore.h"
#include "notch.h"

#ifdef USE_XSHAPE
#include <X11/extensions/shape.h>
#endif

/* The share of the height of the head that the strip or the fade may take */
#define NOTCH_MAX_SHARE 4

/* The black window of the strip is above the menus and the panels */
#define NOTCH_COVER_LEVEL (WMScreensaverLevel - 1)

static Atom notch_atom = None;

/*
 * The height of the notch of a head, or 0 if it has none
 *
 * The notch is what a panel has more than 16:10. It takes between 2.1 % and
 * 2.5 % of the width on the known panels, where the 3:2 panels have 4.2 %
 * more than 16:10, so only what is between 1.5 % and 3.2 % is a notch.
 */
static int notch_height_for_rect(WMRect rect)
{
	int width = rect.size.width;
	int height = (int)rect.size.height - width * 10 / 16;

	if (height <= 0 || height * 1000 < width * 15 || height * 1000 > width * 32)
		return 0;

	return height;
}

/* Looks for the head that has the notch */
static void notch_find(WScreen *scr)
{
	WMRect rect;
	int head, height, fade, limit;

	scr->notch.head = -1;
	memset(&scr->notch.strip, 0, sizeof(scr->notch.strip));
	scr->notch.fade = 0;

	height = 0;
	if (wPreferences.notch_height > 0) {
		head = scr->xine_info.primary_head;
		if (head < 0 || head >= wXineramaHeads(scr))
			head = 0;
		height = wPreferences.notch_height;
	} else if (wPreferences.notch_height < 0) {
		for (head = 0; head < wXineramaHeads(scr); head++) {
			height = notch_height_for_rect(wGetRectForHead(scr, head));
			if (height > 0)
				break;
		}
	}
	if (height <= 0)
		return;

	rect = wGetRectForHead(scr, head);
	limit = rect.size.height / NOTCH_MAX_SHARE;
	if (height > limit)
		height = limit;
	if (height <= 0)
		return;

	if (wPreferences.notch_fade < 0)
		fade = (height * 3 + 2) / 4;
	else
		fade = wPreferences.notch_fade;
	if (fade > limit)
		fade = limit;

	scr->notch.head = head;
	scr->notch.strip.pos = rect.pos;
	scr->notch.strip.size.width = rect.size.width;
	scr->notch.strip.size.height = height;
	scr->notch.fade = fade;
}

/*
 * Tells if the window is shown as a fullscreen one on the head of the notch
 *
 * Besides the windows that are in the fullscreen state, these are the
 * windows whose contents cover the whole head, as some programs do by
 * themselves instead of asking for that state: the shade would lie over
 * their top rows. A window that has its titlebar in the head is not one.
 */
static Bool notch_window_covers(WScreen *scr, WWindow *wwin)
{
	WMRect *strip = &scr->notch.strip;
	WMRect head;
	int x, y, right, bottom, border;

	if (!wwin->flags.mapped || wwin->frame == NULL)
		return False;

	border = HAS_BORDER(wwin) ? scr->frame_border_width : 0;
	x = wwin->frame_x;
	y = wwin->frame_y;
	right = x + (int)wwin->frame->core->width + 2 * border;
	bottom = y + (int)wwin->frame->core->height + 2 * border;

	if (wwin->flags.fullscreen) {
		int strip_bottom = strip->pos.y + (int)strip->size.height;

		/* A fullscreen window of that head starts at the bottom of the strip */
		return x < strip->pos.x + (int)strip->size.width && right > strip->pos.x &&
		       y <= strip_bottom && bottom > strip_bottom;
	}

	/* The windows of the desktop cover the head too, below the others */
	if (wwin->frame->core->stacking->window_level < WMNormalLevel)
		return False;

	head = wGetRectForHead(scr, scr->notch.head);
	return x <= head.pos.x && y + wwin->frame->top_width <= head.pos.y &&
	       right >= head.pos.x + (int)head.size.width &&
	       bottom - wwin->frame->bottom_width >= head.pos.y + (int)head.size.height;
}

/* Tells if a fullscreen window is shown on the head of the notch */
static Bool notch_is_covered(WScreen *scr)
{
	WWindow *wwin;

	if (scr->notch.head < 0)
		return False;

	for (wwin = scr->focused_window; wwin != NULL; wwin = wwin->prev) {
		if (notch_window_covers(scr, wwin))
			return True;
	}

	return False;
}

/*
 * Shows the strip plain black while a fullscreen window is shown
 *
 * The helper does it as well, in its window that is above everything, but
 * it is not always running: the strip would show the desktop above the
 * fullscreen window.
 */
static void notch_cover_update(WScreen *scr)
{
	WMRect *strip = &scr->notch.strip;
	WCoreWindow *core = scr->notch.cover;

	if (scr->notch.head < 0 || !scr->notch.covered) {
		if (scr->notch.cover_mapped) {
			XUnmapWindow(dpy, core->window);
			scr->notch.cover_mapped = 0;
		}
		return;
	}

	if (core == NULL) {
		core = wCoreCreateTopLevel(scr, strip->pos.x, strip->pos.y, strip->size.width, strip->size.height,
		                           0, scr->w_depth, scr->w_visual, scr->w_colormap, scr->black_pixel);
		XSetWindowBackground(dpy, core->window, scr->black_pixel);

#if defined(USE_XSHAPE) && defined(ShapeInput)
		/* The pointer goes through it, to what it hides */
		if (w_global.xext.shape.supported)
			XShapeCombineRectangles(dpy, core->window, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
#endif

		core->stacking = wmalloc(sizeof(WStacking));
		core->stacking->above = NULL;
		core->stacking->under = NULL;
		core->stacking->window_level = NOTCH_COVER_LEVEL;
		core->stacking->child_of = NULL;
		AddToStackList(core);

		scr->notch.cover = core;
		scr->notch.cover_strip = *strip;
	} else if (memcmp(&scr->notch.cover_strip, strip, sizeof(*strip)) != 0) {
		wCoreConfigure(core, strip->pos.x, strip->pos.y, strip->size.width, strip->size.height);
		scr->notch.cover_strip = *strip;
	}

	if (!scr->notch.cover_mapped) {
		XMapWindow(dpy, core->window);
		scr->notch.cover_mapped = 1;
	}
}

/* Tells the helper where the notch is */
static void notch_publish(WScreen *scr)
{
	long data[6];

	if (notch_atom == None)
		notch_atom = XInternAtom(dpy, "_WINDOWMAKER_NOTCH", False);

	if (scr->notch.head < 0) {
		if (scr->notch.published)
			XDeleteProperty(dpy, scr->root_win, notch_atom);
		scr->notch.published = 0;
		return;
	}

	data[0] = scr->notch.strip.pos.x;
	data[1] = scr->notch.strip.pos.y;
	data[2] = scr->notch.strip.size.width;
	data[3] = scr->notch.strip.size.height;
	data[4] = scr->notch.fade;
	data[5] = scr->notch.covered;

	if (scr->notch.published && memcmp(data, scr->notch.data, sizeof(data)) == 0)
		return;

	XChangeProperty(dpy, scr->root_win, notch_atom, XA_CARDINAL, 32, PropModeReplace,
	                (unsigned char *)data, wlengthof(data));
	memcpy(scr->notch.data, data, sizeof(data));
	scr->notch.published = 1;
}

#ifdef USE_XCOMPOSITE
static void notch_helper_died(pid_t pid, unsigned int status, void *cdata)
{
	WScreen *scr = cdata;

	/* Parameter not used, but tell the compiler that it is ok */
	(void) status;

	/* Nothing to do for a helper that was stopped on purpose */
	if (scr->notch.helper_pid != pid)
		return;

	/* It is not started again, it would likely end the same way */
	close(scr->notch.helper_fd);
	scr->notch.helper_fd = -1;
	scr->notch.helper_pid = 0;
	scr->notch.helper_failed = 1;
	wwarning(_("\"%s\" has stopped, the notch of the screen is not shaded anymore"), "wmnotch");
}

/* Starts the helper that draws the shade, with a pipe as its standard input */
static void notch_helper_start(WScreen *scr)
{
	pid_t pid;
	int filedes[2];

	if (scr->notch.helper_pid > 0 || scr->notch.helper_failed)
		return;

	if (pipe(filedes) < 0) {
		werror(_("%s failed, can't shade the notch of the screen (%s)"), "pipe()", strerror(errno));
		scr->notch.helper_failed = 1;
		return;
	}

	pid = fork();
	if (pid < 0) {
		werror(_("%s failed, can't shade the notch of the screen (%s)"), "fork()", strerror(errno));
		close(filedes[0]);
		close(filedes[1]);
		scr->notch.helper_failed = 1;
		return;
	}

	if (pid == 0) {
		/* We don't need this side of the pipe in the child process */
		close(filedes[1]);

		SetupEnvironment(scr);

		close(STDIN_FILENO);
		if (dup2(filedes[0], STDIN_FILENO) < 0) {
			werror(_("%s failed, can't shade the notch of the screen (%s)"), "dup2()", strerror(errno));
			exit(1);
		}
		close(filedes[0]);

		execlp("wmnotch", "wmnotch", NULL);

		werror(_("could not execute \"%s\": %s"), "wmnotch", strerror(errno));
		exit(1);
	}

	/* We don't need this side of the pipe in the parent process */
	close(filedes[0]);

	/* The helper must see the pipe closed when Window Maker restarts */
	if (fcntl(filedes[1], F_SETFD, FD_CLOEXEC) < 0)
		wwarning(_("could not set close-on-exec flag for the communication file handle of \"%s\" (%s)"),
		         "wmnotch", strerror(errno));

	scr->notch.helper_fd = filedes[1];
	scr->notch.helper_pid = pid;

	wAddDeathHandler(pid, notch_helper_died, scr);
}

/* Stops the helper, which exits when its standard input is closed */
static void notch_helper_stop(WScreen *scr)
{
	if (scr->notch.helper_pid <= 0)
		return;

	close(scr->notch.helper_fd);
	scr->notch.helper_fd = -1;
	scr->notch.helper_pid = 0;
}
#endif	/* USE_XCOMPOSITE */

static void notch_check_covered(void *cdata)
{
	WScreen *scr = cdata;
	Bool covered;

	scr->notch.check_pending = 0;

	covered = notch_is_covered(scr);
	if (covered == scr->notch.covered)
		return;

	scr->notch.covered = covered;
	notch_publish(scr);
	notch_cover_update(scr);
}

/* The windows are to be looked at, when there is nothing else to do */
static void notch_schedule_check(WScreen *scr)
{
	if (!scr->notch.initialized || scr->notch.head < 0 || scr->notch.check_pending)
		return;

	scr->notch.check_pending = 1;
	WMAddIdleHandler(notch_check_covered, scr);
}

/*
 * A window was mapped, unmapped or has changed of state
 *
 * The windows are looked at when there is nothing else to do, because the
 * state of the window is not always settled when the notification is sent.
 */
static void notch_observer(void *self, WMNotification *notif)
{
	WScreen *scr = self;

	/* Parameter not used, but tell the compiler that it is ok */
	(void) notif;

	notch_schedule_check(scr);
}

void wNotchWindowMoved(WWindow *wwin)
{
	notch_schedule_check(wwin->screen_ptr);
}

void wNotchUpdate(WScreen *scr)
{
	if (!scr->notch.initialized)
		return;

	notch_find(scr);
	scr->notch.covered = notch_is_covered(scr);
	notch_publish(scr);
	notch_cover_update(scr);

#ifdef USE_XCOMPOSITE
	/* With NotchFade = 0 the strip is kept free, but not shaded */
	if (scr->notch.head >= 0 && wPreferences.notch_fade != 0)
		notch_helper_start(scr);
	else
		notch_helper_stop(scr);
#endif
}

void wNotchInit(WScreen *scr)
{
	if (scr->notch.initialized)
		return;

	scr->notch.initialized = 1;
	scr->notch.helper_fd = -1;

	/* The property of a previous instance may describe another screen */
	scr->notch.published = 1;

	WMAddNotificationObserver(notch_observer, scr, WMNManaged, NULL);
	WMAddNotificationObserver(notch_observer, scr, WMNUnmanaged, NULL);
	WMAddNotificationObserver(notch_observer, scr, WMNChangedWorkspace, NULL);
	WMAddNotificationObserver(notch_observer, scr, WMNChangedState, NULL);
	WMAddNotificationObserver(notch_observer, scr, WMNWorkspaceChanged, NULL);

	wNotchUpdate(scr);
}

void wNotchReserveArea(WScreen *scr, int head, WArea *area)
{
	int bottom;

	if (scr->notch.strip.size.height == 0 || head != scr->notch.head)
		return;

	bottom = scr->notch.strip.pos.y + (int)scr->notch.strip.size.height;
	if (area->y1 < bottom)
		area->y1 = bottom;
}

void wNotchClipRect(WScreen *scr, WMRect *rect)
{
	WMRect *strip = &scr->notch.strip;
	int bottom;

	if (strip->size.height == 0)
		return;

	/* Only if the strip is at the top of the rectangle */
	if (rect->pos.y != strip->pos.y ||
	    rect->pos.x >= strip->pos.x + (int)strip->size.width ||
	    rect->pos.x + (int)rect->size.width <= strip->pos.x ||
	    rect->size.height <= strip->size.height)
		return;

	bottom = strip->pos.y + (int)strip->size.height;
	rect->size.height -= bottom - rect->pos.y;
	rect->pos.y = bottom;
}

void wNotchCleanup(WScreen *scr)
{
	if (!scr->notch.initialized)
		return;

#ifdef USE_XCOMPOSITE
	notch_helper_stop(scr);
#endif

	scr->notch.head = -1;
	memset(&scr->notch.strip, 0, sizeof(scr->notch.strip));
	notch_publish(scr);

	if (scr->notch.cover != NULL) {
		RemoveFromStackList(scr->notch.cover);
		wCoreDestroy(scr->notch.cover);
		scr->notch.cover = NULL;
		scr->notch.cover_mapped = 0;
	}

	/* The observers stay, they do nothing from now on */
	scr->notch.initialized = 0;
}
