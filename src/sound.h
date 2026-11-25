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
 *  with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#ifndef WMSOUND_H_
#define WMSOUND_H_

/*
 * Event sounds, played through libcanberra by the name of the event in the
 * XDG sound theme naming specification ("power-plug", "window-attention"),
 * from the theme of SoundTheme, for the events listed in SoundEvents.
 */

void wSoundInitialize(void);
void wSoundEventsChanged(void);
Bool wSoundEventEnabled(const char *event_id);
void wSoundPlay(const char *event_id, const char *description);

#endif
