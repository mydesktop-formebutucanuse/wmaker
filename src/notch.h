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

#ifndef WMNOTCH_H_
#define WMNOTCH_H_

#include "screen.h"

/* Starts to handle the camera notch of the screen, if it has one */
void wNotchInit(WScreen *scr);

/* Looks again for the notch, after a change of the screen or of the options */
void wNotchUpdate(WScreen *scr);

/* Takes the strip of the notch out of the area that the windows of a head may use */
void wNotchReserveArea(WScreen *scr, int head, WArea *area);

/* Takes the strip of the notch out of the rectangle of a fullscreen window */
void wNotchClipRect(WScreen *scr, WMRect *rect);

/* Stops to handle the notch, when leaving the screen */
void wNotchCleanup(WScreen *scr);

#endif
