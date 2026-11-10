# wm_i18n.m4 - Macro to check how the locale is initialized in WindowMaker
#
# Copyright (c) 2014-2015 Christophe CURIS
# Copyright (c) 2015-2021 The Window Maker Tean
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; see the file COPYING.


dnl WM_I18N_XLOCALE
dnl ---------------
dnl
dnl X11 needs to redefine the function 'setlocale' to properly initialize itself,
dnl we check if user wants to disable this behaviour or if it is not supported
AC_DEFUN_ONCE([WM_I18N_XLOCALE],
[m4_divert_push([INIT_PREPARE])dnl
 AC_ARG_ENABLE([xlocale],
    [AS_HELP_STRING([--disable-xlocale],
        [disable initialization of locale for X])],
    [AS_CASE([$enableval],
        [yes|no], [],
        [AC_MSG_ERROR([bad value '$enableval' for --disable-xlocale])])],
    [enable_xlocale=auto])
m4_divert_pop([INIT_PREPARE])dnl
AS_IF([test "x$enable_xlocale" != "xno"],
    [AC_CHECK_LIB([X11], [_Xsetlocale],
        [AC_DEFINE([X_LOCALE], [1],
            [defined if the locale is initialized by X window])],
        [AS_IF([test "x$enable_xlocale" = "xyes"],
            [AC_MSG_ERROR([support for X_LOCALE was explicitely requested, but X11 lacks the appropriate function])])],
        [$XLFLAGS $XLIBS]) ])
])
