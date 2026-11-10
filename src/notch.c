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
 * is closed, that is to say when Window Maker exits or restarts.
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
#include "xinerama.h"
#include "event.h"
#include "main.h"
#include "notch.h"

/* The share of the height of the head that the strip or the fade may take */
#define NOTCH_MAX_SHARE 4

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

/* Tells if a fullscreen window is shown on the head of the notch */
static Bool notch_is_covered(WScreen *scr)
{
	WMRect *strip = &scr->notch.strip;
	int bottom = strip->pos.y + (int)strip->size.height;
	WWindow *wwin;

	if (scr->notch.head < 0)
		return False;

	for (wwin = scr->focused_window; wwin != NULL; wwin = wwin->prev) {
		WCoreWindow *core;

		if (!wwin->flags.fullscreen || !wwin->flags.mapped || wwin->frame == NULL)
			continue;

		/* A fullscreen window of that head starts at the bottom of the strip */
		core = wwin->frame->core;
		if (wwin->frame_x < strip->pos.x + (int)strip->size.width &&
		    wwin->frame_x + (int)core->width > strip->pos.x &&
		    wwin->frame_y <= bottom &&
		    wwin->frame_y + (int)core->height > bottom)
			return True;
	}

	return False;
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

	if (!scr->notch.initialized || scr->notch.head < 0 || scr->notch.check_pending)
		return;

	scr->notch.check_pending = 1;
	WMAddIdleHandler(notch_check_covered, scr);
}

void wNotchUpdate(WScreen *scr)
{
	if (!scr->notch.initialized)
		return;

	notch_find(scr);
	scr->notch.covered = notch_is_covered(scr);
	notch_publish(scr);

#ifdef USE_XCOMPOSITE
	if (scr->notch.head >= 0)
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

	/* The observers stay, they do nothing from now on */
	scr->notch.initialized = 0;
}
