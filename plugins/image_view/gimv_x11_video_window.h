/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001-2004 Takuro Ashie
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 *  GTK4 port: X child window used as the video output of the movie plugins.
 *
 *  GTK4 creates its X11 toplevels with a 32 bit ARGB visual when a
 *  compositing manager runs.  A child created with XCreateSimpleWindow ()
 *  inherits that visual, and XVideo (mplayer's and xine's default output)
 *  usually can't draw into depth 32 windows: the sound plays but the picture
 *  stays black.  GTK2 widget windows were 24 bit, so use a 24 bit TrueColor
 *  visual explicitly.
 */

#ifndef __GIMV_X11_VIDEO_WINDOW_H__
#define __GIMV_X11_VIDEO_WINDOW_H__

#include <X11/Xlib.h>
#include <X11/Xutil.h>

static inline Window
gimv_x11_create_video_window (Display *xdisplay, Window parent,
                              int width, int height, Colormap *colormap_ret)
{
   int screen = DefaultScreen (xdisplay);
   XVisualInfo vinfo;
   XSetWindowAttributes attr;
   unsigned long mask = CWBackPixel | CWBorderPixel;
   Visual *visual = CopyFromParent;
   int depth = CopyFromParent;

   *colormap_ret = None;

   attr.background_pixel = BlackPixel (xdisplay, screen);
   attr.border_pixel     = BlackPixel (xdisplay, screen);

   if (XMatchVisualInfo (xdisplay, screen, 24, TrueColor, &vinfo)) {
      visual = vinfo.visual;
      depth  = vinfo.depth;
      attr.background_pixel = 0;
      attr.border_pixel     = 0;
      attr.colormap = XCreateColormap (xdisplay, RootWindow (xdisplay, screen),
                                       visual, AllocNone);
      *colormap_ret = attr.colormap;
      mask |= CWColormap;
   }

   return XCreateWindow (xdisplay, parent, 0, 0,
                         width > 0 ? width : 1, height > 0 ? height : 1, 0,
                         depth, InputOutput, visual, mask, &attr);
}


static inline void
gimv_x11_destroy_video_window (Display *xdisplay, Window window,
                               Colormap colormap)
{
   if (window)
      XDestroyWindow (xdisplay, window);
   if (colormap != None)
      XFreeColormap (xdisplay, colormap);
}

#endif /* __GIMV_X11_VIDEO_WINDOW_H__ */
