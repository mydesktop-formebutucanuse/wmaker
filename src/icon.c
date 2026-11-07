/* icon.c - window icon and dock and appicon parent
 *
 *  Window Maker window manager
 *
 *  Copyright (c) 1997-2003 Alfredo K. Kojima
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
#include <X11/Xutil.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <wraster.h>
#include <sys/stat.h>

#include "WindowMaker.h"
#include "wcore.h"
#include "texture.h"
#include "window.h"
#include "icon.h"
#include "actions.h"
#include "stacking.h"
#include "application.h"
#include "defaults.h"
#include "appicon.h"
#include "wmspec.h"
#include "misc.h"
#include "startup.h"
#include "event.h"
#include "winmenu.h"

/**** Global varianebles ****/

#define MOD_MASK wPreferences.modifier_mask
#define CACHE_ICON_PATH "/" PACKAGE_TARNAME "/CachedPixmaps"
#define ICON_BORDER WMScaled(3)

static void miniwindowExpose(WObjDescriptor *desc, XEvent *event);
static void miniwindowMouseDown(WObjDescriptor *desc, XEvent *event);
static void miniwindowDblClick(WObjDescriptor *desc, XEvent *event);

static WIcon *icon_create_core(WScreen *scr, int coord_x, int coord_y);

static void set_dockapp_in_icon(WIcon *icon);
static RImage *get_window_image_from_wm_hints(WWindow *wwin);

static void icon_update_pixmap(WIcon *icon, RImage *image);
static void unset_icon_image(WIcon *icon);
static void set_icon_image(WIcon *icon, RImage *image, const char *path, Bool client);

/****** Notification Observers ******/

static void appearanceObserver(void *self, WMNotification *notif)
{
	WIcon *icon = (WIcon *) self;
	uintptr_t flags = (uintptr_t)WMGetNotificationClientData(notif);

	if ((flags & WTextureSettings) || (flags & WFontSettings)) {
		/* If the rimage exists, update the icon, else create it */
		if (icon->file_image)
			update_icon_pixmap(icon);
		else
			wIconPaint(icon);
	}

	/* so that the appicon expose handlers will paint the appicon specific
	 * stuff */
	XClearArea(dpy, icon->core->window, 0, 0, icon->core->width, icon->core->height, True);
}

static void tileObserver(void *self, WMNotification *notif)
{
	WIcon *icon = (WIcon *) self;

	/* Parameter not used, but tell the compiler that it is ok */
	(void) notif;

	update_icon_pixmap(icon);

	XClearArea(dpy, icon->core->window, 0, 0, 1, 1, True);
}

/************************************/

static int getSize(Drawable d, unsigned int *w, unsigned int *h, unsigned int *dep)
{
	if (d == None) {
		if (w)
			*w = 0;
		if (h)
			*h = 0;
		if (dep)
			*dep = 0;
		return 0;
	}

	Window rjunk;
	int xjunk, yjunk;
	unsigned int bjunk;

	return XGetGeometry(dpy, d, &rjunk, &xjunk, &yjunk, w, h, &bjunk, dep);
}

WIcon *icon_create_for_wwindow(WWindow *wwin)
{
	WScreen *scr = wwin->screen_ptr;
	WIcon *icon;

	icon = icon_create_core(scr, wwin->icon_x, wwin->icon_y);

	icon->owner = wwin;
	if (wwin->wm_hints && (wwin->wm_hints->flags & IconWindowHint)) {
		if (wwin->client_win == wwin->main_window) {
			WApplication *wapp;
			/* do not let miniwindow steal app-icon's icon window */
			wapp = wApplicationOf(wwin->client_win);
			if (!wapp || wapp->app_icon == NULL)
				icon->icon_win = wwin->wm_hints->icon_window;
		} else {
			icon->icon_win = wwin->wm_hints->icon_window;
		}
	}
#ifdef NO_MINIWINDOW_TITLES
	icon->show_title = 0;
#else
	icon->show_title = 1;
#endif

	wIconChangeTitle(icon, wwin);
	icon->tile_type = TILE_NORMAL;

	set_icon_image_from_database(icon, wwin->wm_instance, wwin->wm_class, NULL);
	/* Update the icon, because icon could be NULL */
	wIconUpdate(icon);

	WMAddNotificationObserver(appearanceObserver, icon, WNIconAppearanceSettingsChanged, icon);
	WMAddNotificationObserver(tileObserver, icon, WNIconTileSettingsChanged, icon);

	return icon;
}

WIcon *icon_create_for_dock(WScreen *scr, const char *command, const char *wm_instance, const char *wm_class, int tile)
{
	WIcon *icon;

	icon = icon_create_core(scr, 0, 0);
	icon->tile_type = tile;

	set_icon_image_from_database(icon, wm_instance, wm_class, command);
	/* Update the icon, because icon could be NULL */
	wIconUpdate(icon);

	WMAddNotificationObserver(appearanceObserver, icon, WNIconAppearanceSettingsChanged, icon);
	WMAddNotificationObserver(tileObserver, icon, WNIconTileSettingsChanged, icon);

	return icon;
}

static WIcon *icon_create_core(WScreen *scr, int coord_x, int coord_y)
{
	WIcon *icon;

	icon = wmalloc(sizeof(WIcon));
	icon->core = wCoreCreateTopLevel(scr,
					 coord_x,
					 coord_y,
					 wPreferences.icon_size,
					 wPreferences.icon_size,
					 0, scr->w_depth, scr->w_visual, scr->w_colormap,
					 scr->white_pixel);

	/* will be overriden if this is a application icon */
	icon->core->descriptor.handle_mousedown = miniwindowMouseDown;
	icon->core->descriptor.handle_expose = miniwindowExpose;
	icon->core->descriptor.parent_type = WCLASS_MINIWINDOW;
	icon->core->descriptor.parent = icon;

	icon->core->stacking = wmalloc(sizeof(WStacking));
	icon->core->stacking->above = NULL;
	icon->core->stacking->under = NULL;
	icon->core->stacking->window_level = NORMAL_ICON_LEVEL;
	icon->core->stacking->child_of = NULL;

	/* Icon image */
	icon->file = NULL;
	icon->file_image = NULL;
	icon->image_file = NULL;
	icon->client_image = 0;
	icon->icon_win = None;

	return icon;
}

void wIconDestroy(WIcon *icon)
{
	WCoreWindow *core = icon->core;
	WScreen *scr = core->screen_ptr;

	WMRemoveNotificationObserver(icon);

	if (icon->handlerID)
		WMDeleteTimerHandler(icon->handlerID);

	if (icon->icon_win) {
		int x = 0, y = 0;

		if (icon->owner) {
			x = icon->owner->icon_x;
			y = icon->owner->icon_y;
		}
		XUnmapWindow(dpy, icon->icon_win);
		XReparentWindow(dpy, icon->icon_win, scr->root_win, x, y);
	}
	if (icon->icon_name)
		XFree(icon->icon_name);

	if (icon->pixmap)
		XFreePixmap(dpy, icon->pixmap);

	if (icon->mini_preview)
		XFreePixmap(dpy, icon->mini_preview);

	unset_icon_image(icon);
	if (icon->file)
		wfree(icon->file);

	wCoreDestroy(icon->core);
	wfree(icon);
}

static void drawIconTitleBackground(WScreen *scr, Pixmap pixmap, int height)
{
	int t = WMScaled(1);	/* line thickness */

	XFillRectangle(dpy, pixmap, scr->icon_title_texture->normal_gc, 0, 0, wPreferences.icon_size, height + t);
	XFillRectangle(dpy, pixmap, scr->icon_title_texture->light_gc, 0, 0, wPreferences.icon_size, t);
	XFillRectangle(dpy, pixmap, scr->icon_title_texture->light_gc, 0, 0, t, height + t);
	XFillRectangle(dpy, pixmap, scr->icon_title_texture->dim_gc,
		       wPreferences.icon_size - t, 0, t, height + t);
}

/* draw the dashed frame of selected icons, with the GUI scale factor applied */
static void drawSelectionFrame(WIcon *icon)
{
	WScreen *scr = icon->core->screen_ptr;
	int t = WMScaled(1);	/* line thickness */

	XDrawRectangle(dpy, icon->core->window, scr->icon_select_gc, t / 2, t / 2,
		       icon->core->width - t, icon->core->height - t);
}

static void icon_update_pixmap(WIcon *icon, RImage *image)
{
	RImage *tile;
	Pixmap pixmap;
	int x, y, sx, sy;
	unsigned w, h;
	int theight = 0;
	WScreen *scr = icon->core->screen_ptr;

	switch (icon->tile_type) {
	case TILE_NORMAL:
		tile = RCloneImage(scr->icon_tile);
		break;
	case TILE_CLIP:
		tile = RCloneImage(scr->clip_tile);
		break;
	case TILE_DRAWER:
		tile = RCloneImage(scr->drawer_tile);
		break;
	default:
		/*
		 * The icon has always rigth value, this case is
		 * only to avoid a compiler warning with "tile"
		 * "may be used uninitialized"
		 */
		wwarning("Unknown tile type: %d.\n", icon->tile_type);
		tile = RCloneImage(scr->icon_tile);
	}

	if (image) {
		w = (image->width > wPreferences.icon_size)
		    ? wPreferences.icon_size : image->width;
		x = (wPreferences.icon_size - w) / 2;
		sx = (image->width - w) / 2;

		if (icon->show_title)
			theight = WMFontHeight(scr->icon_title_font);

		h = (image->height + theight > wPreferences.icon_size
		     ? wPreferences.icon_size - theight : image->height);
		y = theight + (wPreferences.icon_size - theight - h) / 2;
		sy = (image->height - h) / 2;

		RCombineArea(tile, image, sx, sy, w, h, x, y);
	}

	if (icon->shadowed) {
		RColor color;

		color.red = scr->icon_back_texture->light.red >> 8;
		color.green = scr->icon_back_texture->light.green >> 8;
		color.blue = scr->icon_back_texture->light.blue >> 8;
		color.alpha = 150;	/* about 60% */
		RClearImage(tile, &color);
	}

	if (icon->highlighted) {
		RColor color;

		color.red = color.green = color.blue = 0;
		color.alpha = 160;
		RLightImage(tile, &color);
	}

	if (!RConvertImage(scr->rcontext, tile, &pixmap))
		wwarning(_("error rendering image:%s"), RMessageForError(RErrorCode));

	RReleaseImage(tile);

	/* Draw the icon's title background (without text) */
	if (icon->show_title)
		drawIconTitleBackground(scr, pixmap, theight);

	icon->pixmap = pixmap;
}

/* The name, or NULL if it is empty */
static char *non_empty_name(char *name)
{
	if (name && name[0] == '\0') {
		XFree(name);
		return NULL;
	}

	return name;
}

/*
 * The title of a miniwindow is the icon name of its window, else the name of
 * the window, as its titlebar shows it: a client may set each of them through
 * the EWMH property only, or through the ICCCM one only.
 */
void wIconChangeTitle(WIcon *icon, WWindow *wwin)
{
	Window win;
	char *name;

	if (!icon || !wwin)
		return;

	win = wwin->client_win;

	/* Remove the previous icon title */
	if (icon->icon_name != NULL)
		XFree(icon->icon_name);

	name = non_empty_name(wNETWMGetIconName(win));
	if (!name && wGetIconName(dpy, win, &name))
		name = non_empty_name(name);
	if (!name)
		name = non_empty_name(wNETWMGetWindowName(win));
	if (!name && wFetchName(dpy, win, &name))
		name = non_empty_name(name);

	icon->icon_name = name;
}

RImage *wIconValidateIconSize(RImage *icon, int max_size)
{
	RImage *nimage;

	if (!icon)
		return NULL;

	/* We should hold "ICON_BORDER" (~2) pixels to include the icon border */
	if (((max_size + ICON_BORDER) < icon->width) ||
	    ((max_size + ICON_BORDER) < icon->height)) {
		if (icon->width > icon->height)
			nimage = RScaleImage(icon, max_size - ICON_BORDER,
					     (icon->height * (max_size - ICON_BORDER) / icon->width));
		else
			nimage = RScaleImage(icon, (icon->width * (max_size - ICON_BORDER) / icon->height),
					     max_size - ICON_BORDER);
		RReleaseImage(icon);
		icon = nimage;
	}

	return icon;
}

/*
 * Applies the GUI scale factor (GSScaleFactor) to an icon image that comes
 * from a source drawn for a regular (unscaled) display (an image file, the
 * icon pixmap of a client...), and then makes sure it fits in max_size.
 * The image passed is released, a new one is returned.
 */
RImage *wIconScaleImage(RImage *icon, int max_size)
{
	RImage *scaled;

	if (!icon)
		return NULL;

	scaled = WMScaleImageByFactor(icon);
	if (scaled) {
		RReleaseImage(icon);
		icon = scaled;
	}

	return wIconValidateIconSize(icon, max_size);
}

/*
 * Set the icon file of the user for an icon, and show it. Returns 0 if the
 * file can't be found or loaded. The next wIconUpdate shows the client icon
 * again, unless client icons are ignored.
 */
int wIconChangeImageFile(WIcon *icon, const char *file)
{
	WScreen *scr = icon->core->screen_ptr;
	char *path;
	RImage *image = NULL;

	/* If no new image, don't do nothing */
	if (!file)
		return 1;

	/* Find the new image */
	path = FindImage(wPreferences.icon_path, file);
	if (!path)
		return 0;

	image = get_rimage_from_file(scr, path, wPreferences.icon_size);
	if (!image) {
		wfree(path);
		return 0;
	}

	if (icon->file)
		wfree(icon->file);
	icon->file = path;

	set_icon_image(icon, image, path, False);
	update_icon_pixmap(icon);

	return 1;
}

static char *get_name_for_wwin(WWindow *wwin)
{
	return get_name_for_instance_class(wwin->wm_instance, wwin->wm_class);
}

char *get_name_for_instance_class(const char *wm_instance, const char *wm_class)
{
	char *suffix;
	int len;

	if (wm_class && wm_instance) {
		len = strlen(wm_class) + strlen(wm_instance) + 2;
		suffix = wmalloc(len);
		snprintf(suffix, len, "%s.%s", wm_instance, wm_class);
	} else if (wm_class) {
		len = strlen(wm_class) + 1;
		suffix = wmalloc(len);
		snprintf(suffix, len, "%s", wm_class);
	} else if (wm_instance) {
		len = strlen(wm_instance) + 1;
		suffix = wmalloc(len);
		snprintf(suffix, len, "%s", wm_instance);
	} else {
		return NULL;
	}

	return suffix;
}

static char *get_icon_cache_path(void)
{
	const char *prefix;
	char *path;
	int ret;

	prefix = wuserdatapath();
	path = wstrconcat(prefix, CACHE_ICON_PATH "/");

	/* If the folder exists, exit */
	if (access(path, F_OK) == 0)
		return path;

	/* Create the folder */
	ret = wmkdirhier(path);

	/* Exit 1 on success, 0 on failure */
	if (ret == 1)
		return path;

	/* Fail */
	wfree(path);
	return NULL;
}

static RImage *get_wwindow_image_from_wmhints(WWindow *wwin)
{
	RImage *image = NULL;
	XWMHints *hints = wwin->wm_hints;

	if (hints && (hints->flags & IconPixmapHint) && hints->icon_pixmap != None)
		image = RCreateImageFromDrawable(wwin->screen_ptr->rcontext,
						 hints->icon_pixmap,
						 (hints->flags & IconMaskHint)
						 ? hints->icon_mask : None);

	return image;
}

/*
 * wIconStore--
 * 	Stores the client supplied icon at CACHE_ICON_PATH
 * and returns the path for that icon. Returns NULL if there is no
 * client supplied icon or on failure.
 *
 * Side effects:
 * 	New directories might be created.
 */
char *wIconStore(WIcon *icon)
{
	char *path, *dir_path, *file, *filename;
	int len = 0;
	RImage *image = NULL;
	WWindow *wwin = icon->owner;

	if (!wwin)
		return NULL;

	dir_path = get_icon_cache_path();
	if (!dir_path)
		return NULL;

	file = get_name_for_wwin(wwin);
	if (!file) {
		wfree(dir_path);
		return NULL;
	}

	/* Create the file name */
	len = strlen(file) + 5;
	filename = wmalloc(len);
	snprintf(filename, len, "%s.xpm", file);
	wfree(file);

	/* Create the full path, including the filename */
	len = strlen(dir_path) + strlen(filename) + 1;
	path = wmalloc(len);
	snprintf(path, len, "%s%s", dir_path, filename);
	wfree(dir_path);

	/* If icon exists, exit */
	if (access(path, F_OK) == 0) {
		wfree(path);
		return filename;
	}

	if (wwin->net_icon_image)
		image = RRetainImage(wwin->net_icon_image);
	else
		image = get_wwindow_image_from_wmhints(wwin);

	if (!image) {
		wfree(path);
		wfree(filename);
		return NULL;
	}

	if (!RSaveImage(image, path, "XPM")) {
		wfree(filename);
		filename = NULL;
	}

	wfree(path);
	RReleaseImage(image);

	return filename;
}

/* Whether a file is a client icon saved by wIconStore */
Bool wIconIsCachedFile(const char *path)
{
	char *cachepath;
	Bool cached;

	if (!path)
		return False;

	cachepath = wstrconcat(wuserdatapath(), CACHE_ICON_PATH "/");
	cached = (strncmp(path, cachepath, strlen(cachepath)) == 0);
	wfree(cachepath);

	return cached;
}

void remove_cache_icon(char *filename)
{
	if (wIconIsCachedFile(filename))
		unlink(filename);
}

static void cycleColor(void *data)
{
	WIcon *icon = (WIcon *) data;
	WScreen *scr = icon->core->screen_ptr;
	XGCValues gcv;

	icon->step--;
	gcv.dash_offset = WMScaled(icon->step);
	XChangeGC(dpy, scr->icon_select_gc, GCDashOffset, &gcv);

	drawSelectionFrame(icon);
	icon->handlerID = WMAddTimerHandler(COLOR_CYCLE_DELAY, cycleColor, icon);
}

void wIconSetHighlited(WIcon *icon, Bool flag)
{
	if (icon->highlighted == flag)
		return;

	icon->highlighted = flag;
	update_icon_pixmap(icon);
}

void wIconSelect(WIcon *icon)
{
	icon->selected = !icon->selected;

	if (icon->selected) {
		icon->step = 0;
		if (!wPreferences.dont_blink)
			icon->handlerID = WMAddTimerHandler(10, cycleColor, icon);
		else
			drawSelectionFrame(icon);
	} else {
		if (icon->handlerID) {
			WMDeleteTimerHandler(icon->handlerID);
			icon->handlerID = NULL;
		}
		XClearArea(dpy, icon->core->window, 0, 0, icon->core->width, icon->core->height, True);
	}
}

/* Forget the image shown */
static void unset_icon_image(WIcon *icon)
{
	if (icon->file_image) {
		RReleaseImage(icon->file_image);
		icon->file_image = NULL;
	}

	if (icon->image_file) {
		wfree(icon->image_file);
		icon->image_file = NULL;
	}

	icon->client_image = 0;
}

/*
 * Show an image (the reference is taken over): path is the file it comes
 * from, if any, and client tells if it comes from the client.
 */
static void set_icon_image(WIcon *icon, RImage *image, const char *path, Bool client)
{
	unset_icon_image(icon);

	icon->file_image = image;
	if (path)
		icon->image_file = wstrdup(path);
	icon->client_image = client;
}

void set_icon_minipreview(WIcon *icon, RImage *image)
{
	Pixmap tmp;
	RImage *scaled_mini_preview;
	WScreen *scr = icon->core->screen_ptr;

	scaled_mini_preview = RSmoothScaleImage(image, wPreferences.minipreview_size - 2 * MINIPREVIEW_BORDER,
	                                  wPreferences.minipreview_size - 2 * MINIPREVIEW_BORDER);

	if (RConvertImage(scr->rcontext, scaled_mini_preview, &tmp)) {
		if (icon->mini_preview != None)
			XFreePixmap(dpy, icon->mini_preview);
		icon->mini_preview = tmp;
	}
	RReleaseImage(scaled_mini_preview);
}

/*
 * Choosing the image of an icon
 *
 * Every icon showing an application or a window (appicon, miniwindow, and
 * the switch panel and window list entries through wIconGetImageForWindow)
 * follows the same order:
 *
 *  1. The client icon (icon window, _NET_WM_ICON, WM_HINTS icon pixmap),
 *     unless client icons are ignored ("Ignore client supplied icon").
 *  2. The icon file the user set.
 *  3. The default icon.
 *
 * The icon file is kept while a client icon is shown, so it comes back as
 * soon as client icons are ignored.
 *
 * An application ignores the client icons when its group leader or any of
 * its windows does: the leader is often a hidden window with a class of its
 * own the user never sets anything for. Its icon file is the one of the
 * leader (or of the dock), else the one of its windows. A window without an
 * icon of its own shows the icon of its application, if it has the class of
 * the application.
 */

/* The application the icon is the appicon of (if it is running) */
static WApplication *icon_application(WIcon *icon)
{
	WApplication *wapp;

	if (!icon->owner)
		return NULL;

	wapp = wApplicationOf(icon->owner->main_window);
	if (wapp && wapp->main_window_desc == icon->owner)
		return wapp;

	return NULL;
}

Bool wIconIgnoresClientIcons(WIcon *icon)
{
	WApplication *wapp;

	if (!icon->owner)
		return False;

	wapp = icon_application(icon);
	if (wapp)
		return wApplicationIgnoresClientIcons(wapp);

	return WFLAGP(icon->owner, always_user_icon);
}

/* The client icon of a window, other than an icon window */
static RImage *get_window_client_image(WWindow *wwin)
{
	if (wwin->net_icon_image)
		return RRetainImage(wwin->net_icon_image);

	if (wwin->wm_hints && (wwin->wm_hints->flags & IconPixmapHint))
		return get_window_image_from_wm_hints(wwin);

	return NULL;
}

static Bool set_icon_image_from_client(WIcon *icon, WWindow *wwin)
{
	RImage *image;

	/* An icon window is shown as is (see update_icon_pixmap) */
	if (icon->icon_win != None) {
		set_icon_image(icon, get_window_image_from_x11(icon->icon_win), NULL, True);
		return True;
	}

	image = get_window_client_image(wwin);
	if (!image)
		return False;

	set_icon_image(icon, image, NULL, True);
	return True;
}

/* Show the image of a file, unless it is the one shown already */
static Bool set_icon_image_from_file(WIcon *icon, const char *path)
{
	RImage *image;

	if (icon->file_image && icon->image_file && strcmp(icon->image_file, path) == 0)
		return True;

	image = get_rimage_from_file(icon->core->screen_ptr, path, wPreferences.icon_size);
	if (!image)
		return False;

	set_icon_image(icon, image, path, False);
	return True;
}

static RImage *get_default_icon_image(WScreen *scr)
{
	if (!scr->def_icon_rimage)
		scr->def_icon_rimage = get_default_image(scr);

	return RRetainImage(scr->def_icon_rimage);
}

/*
 * The icon file of an application: the one of the leader unless it is just
 * a client icon saved for the dock, else the one of its windows (those
 * ignoring the client icons first, the most recently focused first).
 */
static char *get_application_icon_file(WIcon *icon, WApplication *wapp)
{
	WScreen *scr = icon->core->screen_ptr;
	WWindow *wwin;
	char *path = NULL;
	int pass;

	if (icon->file && !wIconIsCachedFile(icon->file))
		return wstrdup(icon->file);

	for (pass = 0; pass < 2 && !path; pass++) {
		for (wwin = scr->focused_window; wwin && !path; wwin = wwin->prev) {
			if (wwin->main_window != wapp->main_window)
				continue;

			if (!WFLAGP(wwin, always_user_icon) == (pass == 0))
				continue;

			path = get_icon_filename(wwin->wm_instance, wwin->wm_class, NULL, False);
		}
	}

	if (!path && icon->file)
		path = wstrdup(icon->file);

	return path;
}

/*
 * The client icon of an application: the one of its leader, else the one of
 * its most recently focused window having one. The leader takes the client
 * icon of the window that started the application, but only if that window
 * set it before being mapped (kitty sets it later), and a hidden leader has
 * none of its own.
 */
static Bool set_application_image_from_client(WIcon *icon, WApplication *wapp)
{
	WWindow *wwin;
	RImage *image;

	if (set_icon_image_from_client(icon, icon->owner))
		return True;

	for (wwin = wapp->main_window_desc->screen_ptr->focused_window; wwin; wwin = wwin->prev) {
		if (wwin->main_window != wapp->main_window)
			continue;

		image = get_window_client_image(wwin);
		if (image) {
			set_icon_image(icon, image, NULL, True);
			return True;
		}
	}

	return False;
}

static void update_application_icon(WIcon *icon, WApplication *wapp)
{
	char *path;

	if (!wApplicationIgnoresClientIcons(wapp) && set_application_image_from_client(icon, wapp))
		return;

	path = get_application_icon_file(icon, wapp);
	if (path && set_icon_image_from_file(icon, path)) {
		wfree(path);
		return;
	}

	if (path)
		wfree(path);

	set_icon_image(icon, get_default_icon_image(icon->core->screen_ptr), NULL, False);
}

/*
 * The appicon of the application of a window, if a window without an icon
 * of its own can show it. The window must be of the class of the application:
 * an application may hold the windows of other programs (SharedAppIcon puts
 * together the windows whose group leaders have the same class), and those
 * show the default icon. When the window ignores the client icons, only an
 * icon file is fine.
 */
static WIcon *get_application_icon_for(WWindow *wwin)
{
	WApplication *wapp;
	WIcon *aicon;
	char *app_class;

	wapp = wApplicationOf(wwin->main_window);
	if (!wapp || !wapp->app_icon)
		return NULL;

	app_class = wapp->main_window_desc->wm_class;
	if (!wwin->wm_class || !app_class || strcmp(wwin->wm_class, app_class) != 0)
		return NULL;

	aicon = wapp->app_icon->icon;
	if (!aicon || !aicon->file_image)
		return NULL;

	if (aicon->image_file || (aicon->client_image && !WFLAGP(wwin, always_user_icon)))
		return aicon;

	return NULL;
}

/* The miniwindow of a window, or a dock icon not owned by a running application */
static void update_window_icon(WIcon *icon)
{
	WWindow *wwin = icon->owner;
	WIcon *aicon;

	if (wwin && !WFLAGP(wwin, always_user_icon) && set_icon_image_from_client(icon, wwin))
		return;

	if (icon->file && set_icon_image_from_file(icon, icon->file))
		return;

	aicon = wwin ? get_application_icon_for(wwin) : NULL;
	if (aicon) {
		set_icon_image(icon, RRetainImage(aicon->file_image), aicon->image_file, aicon->client_image);
		return;
	}

	set_icon_image(icon, get_default_icon_image(icon->core->screen_ptr), NULL, False);
}

void wIconUpdate(WIcon *icon)
{
	WApplication *wapp;

	if (!icon)
		return;

	wapp = icon_application(icon);
	if (wapp)
		update_application_icon(icon, wapp);
	else
		update_window_icon(icon);

	update_icon_pixmap(icon);
}

/*
 * The image of a window for the switch panel and the window list: the one
 * its miniwindow shows, or would show. The reference is the caller's.
 */
RImage *wIconGetImageForWindow(WWindow *wwin)
{
	RImage *image;
	WIcon *aicon;
	char *path;

	if (wwin->icon && wwin->icon->file_image)
		return RRetainImage(wwin->icon->file_image);

	if (!WFLAGP(wwin, always_user_icon)) {
		image = get_window_client_image(wwin);
		if (image)
			return image;
	}

	path = get_icon_filename(wwin->wm_instance, wwin->wm_class, NULL, False);
	if (path) {
		image = get_rimage_from_file(wwin->screen_ptr, path, wPreferences.icon_size);
		wfree(path);
		if (image)
			return image;
	}

	aicon = get_application_icon_for(wwin);
	if (aicon)
		return RRetainImage(aicon->file_image);

	return get_default_icon_image(wwin->screen_ptr);
}

void update_icon_pixmap(WIcon *icon)
{
	if (icon->pixmap != None)
		XFreePixmap(dpy, icon->pixmap);

	icon->pixmap = None;

	/* Create the pixmap */
	if (icon->file_image)
		icon_update_pixmap(icon, icon->file_image);

	/* If dockapp, put inside the icon */
	if (icon->icon_win != None) {
		/* file_image is NULL, because is docked app */
		icon_update_pixmap(icon, NULL);
		set_dockapp_in_icon(icon);
	}

	/* No pixmap, set default background */
	if (icon->pixmap != None)
		XSetWindowBackgroundPixmap(dpy, icon->core->window, icon->pixmap);

	/* Paint it */
	wIconPaint(icon);
}

/* Set the dockapp in the WIcon */
static void set_dockapp_in_icon(WIcon *icon)
{
	XWindowAttributes attr;
	WScreen *scr = icon->core->screen_ptr;
	unsigned int w, h, d;

	/* Reparent the dock application to the icon */

	/* We need the application size to center it
	 * and show in the correct position */
	if (!getSize(icon->icon_win, &w, &h, &d)) {
		wwarning("Drawable invalid, skip reparenting dock app to icon.");
		return;
	}

	/* Set the background pixmap */
	XSetWindowBackgroundPixmap(dpy, icon->core->window, scr->icon_tile_pixmap);

	/* Set the icon border */
	XSetWindowBorderWidth(dpy, icon->icon_win, 0);

	/* Put the dock application in the icon */
	XReparentWindow(dpy, icon->icon_win, icon->core->window,
			(wPreferences.icon_size - w) / 2,
			(wPreferences.icon_size - h) / 2);

	/* Show it and save */
	XMapWindow(dpy, icon->icon_win);
	XAddToSaveSet(dpy, icon->icon_win);

	/* Needed to move the icon clicking on the application part */
	if ((XGetWindowAttributes(dpy, icon->icon_win, &attr)) &&
	    (attr.all_event_masks & ButtonPressMask))
		wHackedGrabButton(Button1, MOD_MASK, icon->core->window, True,
				  ButtonPressMask, GrabModeSync, GrabModeAsync,
				  None, wPreferences.cursor[WCUR_ARROW]);
}

/* Get the RImage from the XWindow wm_hints */
static RImage *get_window_image_from_wm_hints(WWindow *wwin)
{
	RImage *image = NULL;
	unsigned int w, h, d;

	if (!wwin->wm_hints)
		return NULL;

	if (!getSize(wwin->wm_hints->icon_pixmap, &w, &h, &d)) {
		wwin->wm_hints->flags &= ~IconPixmapHint;
		return NULL;
	}

	image = get_wwindow_image_from_wmhints(wwin);
	if (!image)
		return NULL;

	/* Scale the icon and resize it to the wPreferences.icon_size size */
	image = wIconScaleImage(image, wPreferences.icon_size);

	return image;
}

/* This function updates in the screen the icon title */
static void update_icon_title(WIcon *icon)
{
	WScreen *scr = icon->core->screen_ptr;
	int x, l, w;
	char *tmp;

	/* draw the icon title */
	if (icon->show_title && icon->icon_name != NULL) {
		tmp = ShrinkString(scr->icon_title_font, icon->icon_name, wPreferences.icon_size - WMScaled(4));
		w = WMWidthOfString(scr->icon_title_font, tmp, l = strlen(tmp));

		if (w > icon->core->width - WMScaled(4))
			x = (icon->core->width - WMScaled(4)) - w;
		else
			x = (icon->core->width - w) / 2;

		WMDrawString(scr->wmscreen, icon->core->window, scr->icon_title_color,
			     scr->icon_title_font, x, WMScaled(1), tmp, l);
		wfree(tmp);
	}
}


void wIconPaint(WIcon *icon)
{
	if (!icon || !icon->core || !icon->core->screen_ptr)
		return;

	XClearWindow(dpy, icon->core->window);

	update_icon_title(icon);

	if (icon->selected)
		drawSelectionFrame(icon);
}

/******************************************************************/

static void miniwindowExpose(WObjDescriptor *desc, XEvent *event)
{
	/* Parameter not used, but tell the compiler that it is ok */
	(void) event;

	wIconPaint(desc->parent);
}

static void miniwindowDblClick(WObjDescriptor *desc, XEvent *event)
{
	WIcon *icon = desc->parent;

	/* Parameter not used, but tell the compiler that it is ok */
	(void) event;

	assert(icon->owner != NULL);

	wDeiconifyWindow(icon->owner);
}

static void miniwindowMouseDown(WObjDescriptor *desc, XEvent *event)
{
	WIcon *icon = desc->parent;
	WWindow *wwin = icon->owner;
	XEvent ev;
	int x = wwin->icon_x, y = wwin->icon_y;
	int dx = event->xbutton.x, dy = event->xbutton.y;
	int grabbed = 0;
	int clickButton = event->xbutton.button;
	Bool hasMoved = False;

	if (WCHECK_STATE(WSTATE_MODAL))
		return;

	if (IsDoubleClick(icon->core->screen_ptr, event)) {
		miniwindowDblClick(desc, event);
		return;
	}

	if (event->xbutton.button == Button1) {
		if (event->xbutton.state & MOD_MASK)
			wLowerFrame(icon->core);
		else
			wRaiseFrame(icon->core);
		if (event->xbutton.state & ShiftMask) {
			wIconSelect(icon);
			wSelectWindow(icon->owner, !wwin->flags.selected);
		}
	} else if (event->xbutton.button == Button3) {
		WObjDescriptor *desc;

		OpenMiniwindowMenu(wwin, event->xbutton.x_root, event->xbutton.y_root);

		/* allow drag select of menu */
		desc = &wwin->screen_ptr->window_menu->menu->descriptor;
		event->xbutton.send_event = True;
		(*desc->handle_mousedown) (desc, event);

		return;
	}

	if (XGrabPointer(dpy, icon->core->window, False, ButtonMotionMask
			 | ButtonReleaseMask | ButtonPressMask, GrabModeAsync,
			 GrabModeAsync, None, None, CurrentTime) != GrabSuccess) {
	}
	while (1) {
		WMMaskEvent(dpy, PointerMotionMask | ButtonReleaseMask | ButtonPressMask
			    | ButtonMotionMask | ExposureMask, &ev);
		switch (ev.type) {
		case Expose:
			WMHandleEvent(&ev);
			break;

		case MotionNotify:
			hasMoved = True;
			if (!grabbed) {
				if (abs(dx - ev.xmotion.x) >= MOVE_THRESHOLD
				    || abs(dy - ev.xmotion.y) >= MOVE_THRESHOLD) {
					XChangeActivePointerGrab(dpy, ButtonMotionMask
								 | ButtonReleaseMask | ButtonPressMask,
								 wPreferences.cursor[WCUR_MOVE], CurrentTime);
					grabbed = 1;
				} else {
					break;
				}
			}
			x = ev.xmotion.x_root - dx;
			y = ev.xmotion.y_root - dy;
			XMoveWindow(dpy, icon->core->window, x, y);
			break;

		case ButtonPress:
			break;

		case ButtonRelease:
			if (ev.xbutton.button != clickButton)
				break;

			if (wwin->icon_x != x || wwin->icon_y != y)
				wwin->flags.icon_moved = 1;

			XMoveWindow(dpy, icon->core->window, x, y);

			wwin->icon_x = x;
			wwin->icon_y = y;
			XUngrabPointer(dpy, CurrentTime);

			if (wPreferences.auto_arrange_icons)
				wArrangeIcons(wwin->screen_ptr, True);
			if (wPreferences.single_click && !hasMoved)
				miniwindowDblClick(desc, event);
			return;

		}
	}
}

/* Set the icon file of the database for an icon (wIconUpdate shows it) */
void set_icon_image_from_database(WIcon *icon, const char *wm_instance, const char *wm_class, const char *command)
{
	if (icon->file)
		wfree(icon->file);

	icon->file = get_icon_filename(wm_instance, wm_class, command, False);
}
