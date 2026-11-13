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
 * Video recording of a portion of the screen
 *
 * The PartialRecordKey shortcut lets the user drag a rectangle, like the
 * PartialCaptureKey one, and records it with ffmpeg in an mp4 file saved
 * next to the screenshots. A small red dot is shown inside the recorded
 * area, at the corner where the drag started. The same shortcut stops
 * the recording: ffmpeg is sent SIGINT, upon which it finishes the file.
 */

#include "wconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <X11/Xlib.h>
#ifdef USE_XSHAPE
#include <X11/extensions/shape.h>
#endif

#include <WINGs/WUtil.h>

#include "WindowMaker.h"
#include "screen.h"
#include "window.h"
#include "resources.h"
#include "event.h"
#include "main.h"
#include "screenrec.h"

#define RECORD_MARK_COLOR "red"
#define RECORD_MARK_SIZE WMScaled(12)	/* diameter of the dot */
#define RECORD_MARK_INSET WMScaled(6)	/* from the edges of the area */
#define RECORD_FRAMERATE "30"
/* how long Window Maker waits for ffmpeg to finish the file when it exits */
#define RECORD_FINISH_TIMEOUT_MS 10000

static struct {
	pid_t pid;		/* ffmpeg, 0 when not recording */
	Bool stopping;		/* SIGINT was sent, waiting for ffmpeg to finish */
	char *filepath;
	Window mark;		/* red dot in a corner of the recorded area */
} record;

/*
 * Shows the red dot inside the area (x, y, w, h), in the corner where the
 * drag started, so that it is near where the user looks.
 */
static void markShow(WScreen *scr, int x, int y, int w, int h, XPoint start)
{
	XSetWindowAttributes attribs;
	XColor color;
	const int d = RECORD_MARK_SIZE;
	const int m = RECORD_MARK_INSET;
	int mx, my;

	if (start.x <= x)
		mx = x + m;
	else
		mx = x + w - m - d;
	if (start.y <= y)
		my = y + m;
	else
		my = y + h - m - d;

	/* keep it inside of a small area */
	if (mx + d > x + w)
		mx = x + w - d;
	if (mx < x)
		mx = x;
	if (my + d > y + h)
		my = y + h - d;
	if (my < y)
		my = y;

	if (!wGetColor(scr, RECORD_MARK_COLOR, &color))
		color.pixel = scr->black_pixel;

	attribs.override_redirect = True;
	attribs.background_pixel = color.pixel;
	attribs.colormap = scr->w_colormap;

	record.mark = XCreateWindow(dpy, scr->root_win, mx, my, d, d, 0, scr->w_depth,
	                            CopyFromParent, scr->w_visual,
	                            CWOverrideRedirect | CWBackPixel | CWColormap, &attribs);

#ifdef USE_XSHAPE
	{
		Pixmap shape = XCreatePixmap(dpy, record.mark, d, d, 1);
		GC gc = XCreateGC(dpy, shape, 0, NULL);

		XSetForeground(dpy, gc, 0);
		XFillRectangle(dpy, shape, gc, 0, 0, d, d);
		XSetForeground(dpy, gc, 1);
		XFillArc(dpy, shape, gc, 0, 0, d, d, 0, 360 * 64);
		XShapeCombineMask(dpy, record.mark, ShapeBounding, 0, 0, shape, ShapeSet);
		XFreeGC(dpy, gc);
		XFreePixmap(dpy, shape);
	}
#endif

	XMapRaised(dpy, record.mark);
	XFlush(dpy);
}

static void markHide(void)
{
	if (record.mark != None) {
		XDestroyWindow(dpy, record.mark);
		record.mark = None;
		XFlush(dpy);
	}
}

static void recordDied(pid_t pid, unsigned int status, void *cdata)
{
	/* Parameter not used, but tell the compiler that it is ok */
	(void) cdata;

	if (pid != record.pid)
		return;

	/* ffmpeg ends with 255 when it is stopped by SIGINT */
	if (!record.stopping)
		werror(_("screen recording stopped, ffmpeg exited with status %u"), status);
	else if (status != 0 && status != 255)
		werror(_("ffmpeg exited with status %u, the video \"%s\" may be broken"), status, record.filepath);

	markHide();
	wfree(record.filepath);
	record.filepath = NULL;
	record.pid = 0;
	record.stopping = False;
}

static void recordChild(WScreen *scr, const char *filepath, int x, int y, int w, int h)
{
	sigset_t sigs;
	char size[32];
	char input[256];
	const char *display;
	int null_fd;

	SetupEnvironment(scr);

	/* Window Maker blocks or catches signals that ffmpeg must get */
	sigemptyset(&sigs);
	sigprocmask(SIG_SETMASK, &sigs, NULL);
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);

#ifdef HAVE_SETSID
	setsid();
#endif
#ifdef __linux__
	/* finish the file if Window Maker goes away without stopping us */
	prctl(PR_SET_PDEATHSIG, SIGINT);
#endif

	null_fd = open("/dev/null", O_RDONLY);
	if (null_fd >= 0) {
		dup2(null_fd, STDIN_FILENO);
		close(null_fd);
	}

	display = getenv("DISPLAY");
	if (!display)
		display = DisplayString(dpy);

	snprintf(size, sizeof(size), "%dx%d", w, h);
	snprintf(input, sizeof(input), "%s+%d,%d", display, x, y);

	execlp("ffmpeg", "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
	       "-f", "x11grab", "-framerate", RECORD_FRAMERATE,
	       "-video_size", size, "-i", input,
	       "-c:v", "libx264", "-preset", "veryfast", "-crf", "23",
	       "-pix_fmt", "yuv420p", "-movflags", "+faststart",
	       filepath, (char *)NULL);

	werror(_("could not execute \"%s\": %s"), "ffmpeg", strerror(errno));
	/* exec failed in child -- exit immediately without running parent cleanup */
	_exit(127);
}

static void recordStart(WScreen *scr)
{
	int x, y, w, h;
	XPoint start;
	pid_t pid;

	if (wScreenSelectArea(scr, False, &x, &y, &w, &h, &start) != SELECT_AREA_DONE)
		return;

	/* the H.264 encoder wants an even width and height */
	w &= ~1;
	h &= ~1;
	if (w < 2 || h < 2)
		return;

	record.filepath = wScreenCaptureFilePath(".mp4");
	if (!record.filepath)
		return;

	pid = fork();
	if (pid < 0) {
		werror(_("cannot fork a new process"));
		wfree(record.filepath);
		record.filepath = NULL;
		return;
	}
	if (pid == 0)
		recordChild(scr, record.filepath, x, y, w, h);

	record.pid = pid;
	record.stopping = False;
	wAddDeathHandler(pid, recordDied, NULL);

	markShow(scr, x, y, w, h, start);
}

void wScreenRecordStop(void)
{
	if (record.pid <= 0 || record.stopping)
		return;

	record.stopping = True;
	kill(record.pid, SIGINT);

	/* ffmpeg may take a moment to finish the file, show at once it is done */
	markHide();
}

void wScreenRecordFinish(void)
{
	int waited;

	if (record.pid <= 0)
		return;

	wScreenRecordStop();

	/*
	 * Wait for ffmpeg to write the end of the file: a second SIGINT, which
	 * it gets when we exit, would make it give up in the middle of that.
	 * The SIGCHLD handler may reap it first, waitpid() then fails.
	 */
	for (waited = 0; waited < RECORD_FINISH_TIMEOUT_MS; waited += 50) {
		pid_t ret = waitpid(record.pid, NULL, WNOHANG);

		if (ret == record.pid || (ret < 0 && errno != EINTR))
			break;
		usleep(50 * 1000);
	}

	record.pid = 0;
	record.stopping = False;
	wfree(record.filepath);
	record.filepath = NULL;
}

void wScreenRecordToggle(WScreen *scr)
{
	if (record.pid > 0) {
		wScreenRecordStop();
		return;
	}

	recordStart(scr);
}
