/* wconfig.h
 *
 *  Copyright (c) 2001 Dan Pascu
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

#ifndef WINGS_CONFIG_H_
#define WINGS_CONFIG_H_

#include "../config.h"

/* The messages of WINGs are not translated, the macro only marks them */
#define _(text) (text)

#ifdef HAVE_SECURE_GETENV
#define GETENV(x) secure_getenv((x))
#else
#define GETENV(x) getenv((x))
#endif


#endif /* WINGS_CONFIG_H_ */

