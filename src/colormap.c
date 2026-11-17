/* colormap.c - colormap handling code
 *
 *  Window Maker window manager
 *
 *  Copyright (c) 1998-2003 Alfredo K. Kojima
 *
 *  This code slightly based on fvwm code,
 *  Copyright (c) Rob Nation and others
 *  but completely rewritten.
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

#include "WindowMaker.h"
#include <X11/Xatom.h>

#include "window.h"
#include "framewin.h"
#include "colormap.h"


void wColormapInstallForWindow(WScreen * scr, WWindow * wwin)
{
	int i;
	XWindowAttributes attributes;
	int done = 0;
	Colormap colormap;

	scr->cmap_window = wwin;

	if (scr->root_colormap_install_count > 0) {
		scr->original_cmap_window = wwin;
		return;
	}

	/* install colormap for all windows of the client */
	if (wwin && wwin->cmap_window_no > 0 && wwin->cmap_windows) {
		for (i = wwin->cmap_window_no - 1; i >= 0; i--) {
			Window w;

			w = wwin->cmap_windows[i];
			if (w == wwin->client_win) {
				done = 1;
				colormap = wwin->colormap;
			} else {
				XGetWindowAttributes(dpy, w, &attributes);
				colormap = attributes.colormap;
			}
			if (colormap == None)
				colormap = scr->colormap;

			if (scr->current_colormap != colormap) {
				scr->current_colormap = colormap;
				/*
				 * ICCCM 2.0: some client requested permission
				 * to install colormaps by itself and we granted.
				 * So, we can't install any colormaps.
				 */
				if (!scr->flags.colormap_stuff_blocked)
					XInstallColormap(dpy, colormap);
			}
		}
	}

	/*
	 * This runs each time the pointer crosses into a window, so the client's
	 * colormap comes from wwin->colormap (kept current by ColormapNotify)
	 * rather than from the server, and the root's is scr->colormap.
	 */
	if (!done) {
		colormap = wwin ? wwin->colormap : None;
		if (colormap == None)
			colormap = scr->colormap;

		if (scr->current_colormap != colormap) {
			scr->current_colormap = colormap;
			if (!scr->flags.colormap_stuff_blocked)
				XInstallColormap(dpy, colormap);
		}
	}
}

void wColormapAllowClientInstallation(WScreen * scr, Bool starting)
{
	scr->flags.colormap_stuff_blocked = starting;
	/*
	 * Client stopped managing the colormap stuff. Restore the colormap
	 * that would be installed if the client did not request colormap
	 * stuff.
	 */
	if (!starting) {
		XInstallColormap(dpy, scr->current_colormap);
		XSync(dpy, False);
	}
}
