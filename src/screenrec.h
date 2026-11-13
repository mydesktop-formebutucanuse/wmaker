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

#ifndef WMSCREENREC_H_
#define WMSCREENREC_H_

#include "screen.h"

/* Selects a portion of the screen and starts to record it, or stops the recording */
void wScreenRecordToggle(WScreen *scr);

/* Stops the recording, if there is one; ffmpeg then finishes the video file */
void wScreenRecordStop(void);

/* Stops the recording and waits for ffmpeg to finish the file, for when Window Maker exits */
void wScreenRecordFinish(void);

#endif
