/* gnomefe misc defs. */

#ifndef _gnome_fe_h
#define _gnome_fe_h

#include <gdk/gdk.h>
#include <X11/Xlib.h>

/* Client data for Imagelib callbacks */
typedef struct fe_PixmapClientData {
  GdkPixmap *gdk_pixmap;
} fe_PixmapClientData;

typedef struct fe_ContextData {
    void *frame_or_view;
} fe_ContextData;

#endif /* _gnome_fe_h */
