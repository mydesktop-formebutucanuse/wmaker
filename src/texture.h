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


#ifndef WMTEXTURE_H_
#define WMTEXTURE_H_

#include <X11/extensions/Xrender.h>

#include "screen.h"
#include "wcore.h"

/* texture relief */
#define WREL_RAISED	0
#define WREL_SUNKEN	1
#define WREL_FLAT	2
#define WREL_ICON	4
#define WREL_MENUENTRY	6

/* texture types */
#define WREL_BORDER_MASK	1

#define WTEX_SOLID 	((1<<1)|WREL_BORDER_MASK)
#define WTEX_HGRADIENT	((1<<2)|WREL_BORDER_MASK)
#define WTEX_VGRADIENT	((1<<3)|WREL_BORDER_MASK)
#define WTEX_DGRADIENT	((1<<4)|WREL_BORDER_MASK)
#define WTEX_MHGRADIENT	((1<<5)|WREL_BORDER_MASK)
#define WTEX_MVGRADIENT	((1<<6)|WREL_BORDER_MASK)
#define WTEX_MDGRADIENT	((1<<7)|WREL_BORDER_MASK)
#define WTEX_IGRADIENT	((1<<8)|WREL_BORDER_MASK)
#define WTEX_PIXMAP	(1<<10)
#define WTEX_THGRADIENT	((1<<11)|WREL_BORDER_MASK)
#define WTEX_TVGRADIENT	((1<<12)|WREL_BORDER_MASK)
#define WTEX_TDGRADIENT	((1<<13)|WREL_BORDER_MASK)
#define WTEX_FUNCTION	((1<<14)|WREL_BORDER_MASK)

/* pixmap subtypes */
#define WTP_TILE	2
#define WTP_SCALE	4
#define WTP_CENTER	6


typedef struct {
    short type;			       /* type of texture */
    char subtype;		       /* subtype of the texture */
    XColor color;		       /* default background color */
    GC gc;			       /* gc for the background color */
} WTexAny;


typedef struct WTexSolid {
    short type;
    char subtype;
    XColor normal;
    GC normal_gc;

    GC light_gc;
    GC dim_gc;
    GC dark_gc;

    XColor light;
    XColor dim;
    XColor dark;
} WTexSolid;


typedef struct WTexGradient {
    short type;
    char subtype;
    XColor normal;
    GC normal_gc;

    RColor color1;
    RColor color2;
} WTexGradient;


typedef struct WTexMGradient {
    short type;
    char subtype;
    XColor normal;
    GC normal_gc;

    RColor **colors;
} WTexMGradient;


typedef struct WTexIGradient {
    short type;
    char dummy;
    XColor normal;
    GC normal_gc;

    RColor colors1[2];
    RColor colors2[2];
    int thickness1;
    int thickness2;
} WTexIGradient;


typedef struct WTexPixmap {
    short type;
    char subtype;
    XColor normal;
    GC normal_gc;

    struct RImage *pixmap;
    Pixmap tile;		       /* the image on the X server, see wPaintTexture() */
} WTexPixmap;

typedef struct WTexTGradient {
    short type;
    char subtype;
    XColor normal;
    GC normal_gc;

    RColor color1;
    RColor color2;
    struct RImage *pixmap;
    int opacity;
} WTexTGradient;

typedef struct WTexFunction {
    short type;
    char subtype;
    XColor normal;
    GC normal_gc;

    void *handle;
    RImage *(*render) (int, char**, int, int, int);
    int argc;
    char **argv;
} WTexFunction;

typedef union WTexture {
    WTexAny any;
    WTexSolid solid;
    WTexGradient gradient;
    WTexIGradient igradient;
    WTexMGradient mgradient;
    WTexPixmap pixmap;
    WTexTGradient tgradient;
    WTexFunction function;
} WTexture;


WTexSolid *wTextureMakeSolid(WScreen*, XColor*);
WTexGradient *wTextureMakeGradient(WScreen*, int, const RColor*, const RColor*);
WTexMGradient *wTextureMakeMGradient(WScreen*, int, RColor**);
WTexTGradient *wTextureMakeTGradient(WScreen*, int, const RColor*, const RColor*, const char *, int);
WTexIGradient *wTextureMakeIGradient(WScreen*, int, const RColor[2], int, const RColor[2]);
WTexPixmap *wTextureMakePixmap(WScreen *scr, int style, const char *pixmap_file,
                               XColor *color);
void wTextureDestroy(WScreen*, WTexture*);
void wTexturePaint(WTexture *, Pixmap *, WCoreWindow*, int, int);
void wTextureRender(WScreen*, WTexture*, Pixmap*, int, int, int);
struct RImage *wTextureRenderImage(WTexture*, int, int, int);


void wTexturePaintTitlebar(struct WWindow *wwin, WTexture *texture, Pixmap *tdata,
                           int repaint);


#define FREE_PIXMAP(p) if ((p)!=None) XFreePixmap(dpy, (p)), (p)=None

/* drawing of lines with the GUI scale factor applied to their thickness */
void wDrawScaledLine(Drawable d, GC gc, int x1, int y1, int x2, int y2);
void wDrawScaledSegments(Drawable d, GC gc, XSegment *segs, int nsegs);
void wDrawScaledRectangle(Drawable d, GC gc, int x, int y, int width, int height);
void wOperateScaledLine(RImage *image, int operation, int x1, int y1, int x2, int y2, const RColor *color);
void wDrawScaledImageLine(RImage *image, int x1, int y1, int x2, int y2, const RColor *color);

void wDrawBevel(Drawable d, unsigned width, unsigned height,
                WTexSolid *texture, int relief);

/*
 * A texture being painted, with its relief and the lines drawn over it: by
 * the X server, or as an image rendered by Window Maker (see wPaintTexture())
 */
typedef struct WPaint {
    WScreen *scr;
    int width, height;
    struct RImage *image;	       /* the image rendered by Window Maker, or */
    Pixmap pixmap;		       /* the pixmap painted by the X server */
    Picture picture;
    GC invert_gc;
} WPaint;

XRenderPictFormat *wPaintFormat(WScreen *scr);
Bool wPaintTexture(WPaint *paint, WScreen *scr, WTexture *texture, int width, int height, int relief);
Bool wPaintPart(WPaint *paint, const WPaint *src, int x, int y, int width, int height);
void wPaintBevel(WPaint *paint, int bevel_type);
void wPaintOperateLine(WPaint *paint, int operation, int x1, int y1, int x2, int y2, const RColor *color);
void wPaintDrawLine(WPaint *paint, int x1, int y1, int x2, int y2, const RColor *color);
Pixmap wPaintFinish(WPaint *paint);
void wPaintRelease(WPaint *paint);

Picture wPictureFromImage(WScreen *scr, struct RImage *image);

#endif
