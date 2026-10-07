/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * Copyright (C) 2001-2002 the xine project
 * Copyright (C) 2002 Takuro Ashie
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * $Id: gimv_xine_priv.h,v 1.3 2003/07/12 15:53:31 makeinu Exp $
 *
 * the xine engine in a widget - implementation
 */

#ifndef __GIMV_XINE_PRIVATE_H__
#define __GIMV_XINE_PRIVATE_H__

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif /* HAVE_CONFIG_H */

#ifdef ENABLE_XINE

#include <xine.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pwd.h>
#include <sys/types.h>
#include <pthread.h>

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_object.h"


#if defined (GDK_WINDOWING_X11)
#  include <X11/Xlib.h>
#  include <X11/keysym.h>
#  include <X11/cursorfont.h>
#  include <X11/Xatom.h>
#  include <X11/extensions/XShm.h>
#  include <gdk/x11/gdkx.h>
#  include "gimv_x11_video_window.h"
#  define GIMV_XINE_DEFAULT_VISUAL_TYPE XINE_VISUAL_TYPE_X11
#else
#  define GIMV_XINE_DEFAULT_VISUAL_TYPE XINE_VISUAL_TYPE_NONE
#endif

#include "intl.h"
#include "gtk2-compat.h"



/*
 * config related constants
 */
#define CONFIG_LEVEL_BEG         0 /* => beginner */
#define CONFIG_LEVEL_ADV        10 /* advanced user */
#define CONFIG_LEVEL_EXP        20 /* expert */
#define CONFIG_LEVEL_MAS        30 /* motku */
#define CONFIG_LEVEL_DEB        40 /* debugger (only available in debug mode) */

#define CONFIG_NO_DESC          NULL
#define CONFIG_NO_HELP          NULL
#define CONFIG_NO_CB            NULL
#define CONFIG_NO_DATA          NULL


struct GimvXinePrivate_Tag
{
   xine_t                  *xine;

   xine_stream_t           *stream;
   xine_event_queue_t      *event_queue;
   double                   display_ratio;

   char                     configfile[256];

   char                    *video_driver_id;
   char                    *audio_driver_id;

   xine_video_port_t       *vo_driver;
   xine_audio_port_t       *ao_driver;

   int                      xpos, ypos;
   int                      oldwidth, oldheight;

   /* GTK4: widgets have no native windows any more.  On the X11 backend
    * the video is drawn into an X child window of the toplevel surface
    * (use_x11 == TRUE), on other backends xine's "raw" video driver hands
    * RGB frames to us which are drawn as a GdkTexture. */
   gboolean                 use_x11;
   gint                     alloc_width, alloc_height;

#if defined (GDK_WINDOWING_X11)
   Display                 *display;       /* xine's own connection */
   Display                 *gdk_display;   /* GDK's connection */
   int                      screen;
   Window                   video_window;
   int                      completion_event;
   gulong                   xevent_id;
   gint                     win_x, win_y, win_width, win_height;
   gboolean                 win_mapped;
   Colormap                 video_colormap;   /* GTK4 */

   pthread_t                thread;
#endif /* defined (GDK_WINDOWING_X11) */

   /* raw video output (non X11 backends) */
   GMutex                   frame_lock;
   guchar                  *frame_buf;
   gint                     frame_width, frame_height;
   gdouble                  frame_aspect;
   gboolean                 frame_changed;
   gboolean                 frame_idle_pending;
   GdkTexture              *frame_texture;

   int                       post_video_num;
   xine_post_t              *post_video;

   struct {
      xine_stream_t          *stream;
      xine_event_queue_t     *event_queue;
      int                     running;
      int                     current;
      int                     enabled; /* 0, 1:vpost, 2:vanim */

      char                  **mrls;
      int                     num_mrls;

      int                     post_plugin_num;
      xine_post_t            *post_output;
      int                     post_changed;

   } visual_anim;
};


typedef void (*GimvXinePrivScaleLineFn) (guchar *source, guchar *dest,
                                         gint width, gint step);

typedef struct GimvXinePrivImage_Tag {
   gint width;
   gint height;
   gint ratio_code;
   gint format;
   guchar *img, *y, *u, *v, *yuy2;

   gint u_width, v_width;
   gint u_height, v_height;

   GimvXinePrivScaleLineFn scale_line;
   unsigned long scale_factor;
} GimvXinePrivImage;


xine_t *gimv_xine_priv_get     (void);
void    gimv_xine_priv_release (xine_t *xine);


GimvXinePrivImage *gimv_xine_priv_image_new    (gint               imgsize);
void               gimv_xine_priv_image_delete (GimvXinePrivImage *image);
guchar            *gimv_xine_priv_yuv2rgb      (GimvXinePrivImage *image);

#endif /* ENABLE_XINE */

#endif /* __GIMV_XINE_PRIVATE_H__ */
