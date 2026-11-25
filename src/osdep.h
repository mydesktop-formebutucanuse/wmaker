/*
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

#ifndef WMOSDEP_H_
#define WMOSDEP_H_

Bool GetCommandForPid(int pid, char ***argv, int *argc);

/*
 * Power supply, for the sounds of the charger (sound.c):
 *  wOSOpenPowerEvents  - a descriptor that becomes readable when the kernel
 *                        reports a change of a power supply, or -1
 *  wOSReadPowerEvents  - reads what is there: 1 if a charger changed,
 *                        0 if not, -1 if the descriptor is of no more use
 *  wOSMainsOnline      - 1 if a charger is plugged in, 0 if not, -1 if it
 *                        cannot be known
 */
int wOSOpenPowerEvents(void);
int wOSReadPowerEvents(int fd);
int wOSMainsOnline(void);

#endif
