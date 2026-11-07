/*
 *  Window Maker window manager
 *
 *  Copyright (c) 1997-2003 Alfredo K. Kojima
 *  Copyright (c) 1998-2003 Dan Pascu
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
#include "wconfig.h"

#include <X11/Xlib.h>
#include <string.h>

#include "WindowMaker.h"
#include "menu.h"
#include "window.h"
#ifdef USER_MENU
#include "usermenu.h"
#endif				/* USER_MENU */
#include "appicon.h"
#include "icon.h"
#include "application.h"
#include "appmenu.h"
#include "properties.h"
#include "workspace.h"
#include "dock.h"
#include "defaults.h"


/******** Local variables ********/

static WWindow *makeMainWindow(WScreen * scr, Window window)
{
	WWindow *wwin;
	XWindowAttributes attr;

	if (!XGetWindowAttributes(dpy, window, &attr))
		return NULL;

	wwin = wWindowCreate();
	wwin->screen_ptr = scr;
	wwin->client_win = window;
	wwin->main_window = window;
	wwin->wm_hints = XGetWMHints(dpy, window);

	PropGetWMClass(window, &wwin->wm_class, &wwin->wm_instance);

	wDefaultFillAttributes(wwin->wm_instance, wwin->wm_class, &wwin->user_flags,
			       &wwin->defined_user_flags, True);

	XSelectInput(dpy, window, attr.your_event_mask | PropertyChangeMask | StructureNotifyMask);
	return wwin;
}

WApplication *wApplicationOf(Window window)
{
	WApplication *wapp;

	if (window == None)
		return NULL;
	if (XFindContext(dpy, window, w_global.context.app_win, (XPointer *) & wapp) != XCSUCCESS)
		return NULL;
	return wapp;
}

WApplication *wApplicationCreate(WWindow * wwin)
{
	WScreen *scr = wwin->screen_ptr;
	Window main_window = wwin->main_window;
	WApplication *wapp;
	WWindow *leader;

	if (main_window == None || main_window == scr->root_win)
		return NULL;

	{
		Window root;
		int foo;
		unsigned int bar;
		/* check if the window is valid */
		if (!XGetGeometry(dpy, main_window, &root, &foo, &foo, &bar, &bar, &bar, &bar))
			return NULL;
	}

	wapp = wApplicationOf(main_window);
	if (wapp) {
		wapp->refcount++;
		if (wapp->app_icon && wapp->app_icon->docked &&
		    wapp->app_icon->relaunching && wapp->main_window_desc->fake_group)
			wDockFinishLaunch(wapp->app_icon);

		return wapp;
	}

	wapp = wmalloc(sizeof(WApplication));

	wapp->refcount = 1;
	wapp->last_focused = NULL;
	wapp->urgent_bounce_timer = NULL;

	wapp->last_workspace = 0;

	wapp->main_window = main_window;
	wapp->main_window_desc = makeMainWindow(scr, main_window);
	if (!wapp->main_window_desc) {
		wfree(wapp);
		return NULL;
	}

	wapp->main_window_desc->fake_group = wwin->fake_group;
	wapp->main_window_desc->net_icon_image = RRetainImage(wwin->net_icon_image);

	leader = wWindowFor(main_window);
	if (leader)
		leader->main_window = main_window;

	wapp->menu = wAppMenuGet(scr, main_window);
#ifdef USER_MENU
	if (!wapp->menu)
		wapp->menu = wUserMenuGet(scr, wapp->main_window_desc);

	wAppMenuMap(wapp->menu, wwin);
#endif

	/* Set application wide attributes from the leader */
	wapp->flags.hidden = WFLAGP(wapp->main_window_desc, start_hidden);
	wapp->flags.emulated = WFLAGP(wapp->main_window_desc, emulate_appicon);

	/* application descriptor */
	XSaveContext(dpy, main_window, w_global.context.app_win, (XPointer) wapp);

	create_appicon_for_application(wapp, wwin);

	return wapp;
}

/*
 * The appicon ignores the client icons when the group leader or any window
 * of the application does. The leader is often a hidden window with a class
 * of its own (GTK names it after the program, and Firefox based browsers
 * call their windows "Navigator"), so the user never sets its attributes:
 * without this, any new window of the application (a Picture-in-Picture
 * one, for example) would bring the client icon back on the appicon.
 */
Bool wApplicationIgnoresClientIcons(WApplication *wapp)
{
	WWindow *wwin;

	if (WFLAGP(wapp->main_window_desc, always_user_icon))
		return True;

	for (wwin = wapp->main_window_desc->screen_ptr->focused_window; wwin; wwin = wwin->prev)
		if (wwin->main_window == wapp->main_window && WFLAGP(wwin, always_user_icon))
			return True;

	return False;
}

/* Choose the image of the appicon again (see wIconUpdate) */
void wApplicationUpdateIcon(WApplication *wapp)
{
	if (!wapp || !wapp->app_icon)
		return;

	wIconUpdate(wapp->app_icon->icon);
	wAppIconPaint(wapp->app_icon);

	/* Let the dock remember the icon */
	if (wapp->app_icon->docked && !WFLAGP(wapp->main_window_desc, no_appicon))
		save_appicon(wapp->app_icon);
}

/*
 * A window joined or left its application: update the appicon if the icon
 * settings of the window may change it (it ignores the client icons, or
 * the appicon shows an icon file other than the one of the leader, or no
 * icon of its own). The window must be in the window list only if it is
 * joining the application.
 */
void wApplicationUpdateIconFor(WWindow *wwin)
{
	WApplication *wapp = wApplicationOf(wwin->main_window);
	WIcon *icon;

	if (!wapp || !wapp->app_icon || wapp->main_window_desc == wwin)
		return;

	icon = wapp->app_icon->icon;
	if (WFLAGP(wwin, always_user_icon) ||
	    (!icon->client_image && !icon->image_file) ||
	    (icon->image_file && (!icon->file || strcmp(icon->image_file, icon->file) != 0)))
		wApplicationUpdateIcon(wapp);
}

void wApplicationDestroy(WApplication *wapp)
{
	WWindow *wwin;
	WScreen *scr;

	if (!wapp)
		return;

	wapp->refcount--;
	if (wapp->refcount > 0)
		return;

	if (wapp->urgent_bounce_timer) {
		WMDeleteTimerHandler(wapp->urgent_bounce_timer);
		wapp->urgent_bounce_timer = NULL;
	}
	if (wapp->flags.bouncing) {
		/* event.c:handleDestroyNotify forced this destroy
		   and thereby overlooked the bounce callback */
		wapp->refcount = 1;
		return;
	}

	scr = wapp->main_window_desc->screen_ptr;

	if (wapp == scr->wapp_list) {
		if (wapp->next)
			wapp->next->prev = NULL;
		scr->wapp_list = wapp->next;
	} else {
		if (wapp->next)
			wapp->next->prev = wapp->prev;
		if (wapp->prev)
			wapp->prev->next = wapp->next;
	}

	XDeleteContext(dpy, wapp->main_window, w_global.context.app_win);
	wAppMenuDestroy(wapp->menu);

	/* Remove application icon */
	removeAppIconFor(wapp);

	wwin = wWindowFor(wapp->main_window_desc->client_win);

	wWindowDestroy(wapp->main_window_desc);
	if (wwin) {
		/* undelete client window context that was deleted in
		 * wWindowDestroy */
		XSaveContext(dpy, wwin->client_win, w_global.context.client_win, (XPointer) & wwin->client_descriptor);
	}
	wfree(wapp);
}

void wApplicationActivate(WApplication *wapp)
{
	if (wapp->app_icon) {
		wIconSetHighlited(wapp->app_icon->icon, True);
		wAppIconPaint(wapp->app_icon);
	}
}

void wApplicationDeactivate(WApplication *wapp)
{
	if (wapp->app_icon) {
		wIconSetHighlited(wapp->app_icon->icon, False);
		wAppIconPaint(wapp->app_icon);
	}
}
