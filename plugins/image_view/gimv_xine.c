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
 * $Id: gimv_xine.c,v 1.6 2004/10/03 16:26:40 makeinu Exp $
 *
 * the xine engine in a widget - implementation
 */

/*
 * based on gtkxine.c,v 1.52 2002/12/22 23:12:20
 * (http://cvs.sourceforge.net/cgi-bin/viewcvs.cgi/xine/gnome-xine/src/gtkxine.c)
 */

/* indent -kr -i3 -psl -pcs */

#include "gimv_xine.h"

#ifdef ENABLE_XINE

#include <xine.h>

#if (XINE_MAJOR_VERSION >= 1) && (XINE_MINOR_VERSION >= 0) && (XINE_SUB_VERSION >= 0)

#include "gimv_xine_priv.h"
#include "gimv_xine_post.h"


enum {
   PLAY_SIGNAL,
   STOP_SIGNAL,
   PLAYBACK_FINISHED_SIGNAL,
   NEED_NEXT_MRL_SIGNAL,
   BRANCHED_SIGNAL,
   LAST_SIGNAL
};


#if defined(GDK_WINDOWING_X11)
/* missing stuff from X includes */
#  ifndef XShmGetEventBase
extern int XShmGetEventBase (Display *);
#  endif
#endif /* defined(GDK_WINDOWING_X11) */

static void gimv_xine_class_init    (GimvXineClass  *klass);
static void gimv_xine_init          (GimvXine       *gxine);

/* object class methods */
static void gimv_xine_dispose       (GObject        *object);
static void gimv_xine_finalize      (GObject        *object);

/* widget class methods */
static void gimv_xine_realize       (GtkWidget      *widget);
static void gimv_xine_unrealize     (GtkWidget      *widget);
static void gimv_xine_map           (GtkWidget      *widget);
static void gimv_xine_unmap         (GtkWidget      *widget);
static void gimv_xine_measure       (GtkWidget      *widget,
                                     GtkOrientation  orientation,
                                     int             for_size,
                                     int            *minimum,
                                     int            *natural,
                                     int            *minimum_baseline,
                                     int            *natural_baseline);
static void gimv_xine_snapshot      (GtkWidget      *widget,
                                     GtkSnapshot    *snapshot);
static void gimv_xine_size_allocate (GtkWidget      *widget,
                                     int             width,
                                     int             height,
                                     int             baseline);

static GtkWidgetClass *parent_class = NULL;
static guint gimv_xine_signals[LAST_SIGNAL] = {0};


GType
gimv_xine_get_type (void)
{
	static GType gimv_xine_type = 0;

   if (!gimv_xine_type) {
      static const GTypeInfo gimv_xine_info = {
         sizeof (GimvXineClass),
         NULL,               /* base_init */
         NULL,               /* base_finalize */
         (GClassInitFunc)    gimv_xine_class_init,
         NULL,               /* class_finalize */
         NULL,               /* class_data */
         sizeof (GimvXine),
         0,                  /* n_preallocs */
         (GInstanceInitFunc) gimv_xine_init,
      };

      gimv_xine_type = g_type_register_static (GTK_TYPE_WIDGET,
                                               "GimvXine",
                                               &gimv_xine_info,
                                               0);
   }

   return gimv_xine_type;
}


static void
gimv_xine_class_init (GimvXineClass *class)
{
   GObjectClass *gobject_class;
   GtkWidgetClass *widget_class;

   gobject_class = (GObjectClass *) class;
   widget_class = (GtkWidgetClass *) class;

   parent_class = g_type_class_peek_parent (class);

   gimv_xine_signals[PLAY_SIGNAL]
      = g_signal_new ("play",
                      G_TYPE_FROM_CLASS (gobject_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvXineClass, play),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

   gimv_xine_signals[STOP_SIGNAL]
      = g_signal_new ("stop",
                      G_TYPE_FROM_CLASS (gobject_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvXineClass, stop),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

   gimv_xine_signals[PLAYBACK_FINISHED_SIGNAL]
      = g_signal_new ("playback_finished",
                      G_TYPE_FROM_CLASS (gobject_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvXineClass, playback_finished),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

   /*
   gimv_xine_signals[NEED_NEXT_MRL_SIGNAL]
      = g_signal_new ("need_next_mrl",
                      G_TYPE_FROM_CLASS (gobject_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvXineClass, need_next_mrl),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__POINTER,
                      G_TYPE_NONE, 1, G_TYPE_POINTER);

   gimv_xine_signals[BRANCHED_SIGNAL]
      = g_signal_new ("branched",
                      G_TYPE_FROM_CLASS (gobject_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvXineClass, branched),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);
   */

   gobject_class->dispose      = gimv_xine_dispose;
   gobject_class->finalize     = gimv_xine_finalize;

   widget_class->realize       = gimv_xine_realize;
   widget_class->unrealize     = gimv_xine_unrealize;
   widget_class->map           = gimv_xine_map;
   widget_class->unmap         = gimv_xine_unmap;
   widget_class->measure       = gimv_xine_measure;
   widget_class->size_allocate = gimv_xine_size_allocate;
   widget_class->snapshot      = gimv_xine_snapshot;
}


static void
gimv_xine_init (GimvXine *this)
{
   GimvXinePrivate *priv;

   priv = this->private = g_new0 (GimvXinePrivate, 1);

   /*
    * create a new xine instance, load config values
    */

#if 0
   priv->xine = xine_new ();

   g_snprintf (priv->configfile, 255, "%s/.gimv/xinerc", getenv ("HOME"));
   xine_config_load (priv->xine, priv->configfile);

   xine_init (priv->xine);
#else
   priv->xine = gimv_xine_priv_get ();
#endif

   priv->stream               = NULL;
   priv->event_queue          = NULL;
   priv->vo_driver            = NULL;
   priv->ao_driver            = NULL;

   priv->oldwidth             = 0;
   priv->oldheight            = 0;

   priv->use_x11              = FALSE;
   g_mutex_init (&priv->frame_lock);
   priv->frame_buf            = NULL;
   priv->frame_texture        = NULL;
   priv->frame_aspect         = 0.0;

   gtk_widget_set_overflow (GTK_WIDGET (this), GTK_OVERFLOW_HIDDEN);
}


static void
gimv_xine_dispose (GObject *object)
{
   GimvXine *gtx = GIMV_XINE (object);

   g_return_if_fail (GIMV_IS_XINE (gtx));

   /* xine itself is released in finalize: the widget may still be
    * unrealized (which needs the xine stream) while chaining up. */

   if (G_OBJECT_CLASS (parent_class)->dispose)
      G_OBJECT_CLASS (parent_class)->dispose (object);
}


static void
gimv_xine_finalize (GObject *object)
{
   GimvXine *gtx = GIMV_XINE (object);
   GimvXinePrivate *priv;

   priv = gtx->private;

   if (priv) {
      /* exit xine */
      if (priv->xine)
#if 0
         xine_exit (priv->xine);
#else
         gimv_xine_priv_release (priv->xine);
#endif
      priv->xine = NULL;

      g_free (priv->video_driver_id);
      g_free (priv->audio_driver_id);

      g_mutex_clear (&priv->frame_lock);
      g_free (priv->frame_buf);
      g_clear_object (&priv->frame_texture);

      g_free (gtx->private);
      gtx->private = NULL;
   }

   if (G_OBJECT_CLASS (parent_class)->finalize)
      G_OBJECT_CLASS (parent_class)->finalize (object);
}


#if defined(GDK_WINDOWING_X11)
static void
dest_size_cb (void *gxine_gen,
              int video_width, int video_height,
              double video_pixel_aspect,
              int *dest_width, int *dest_height,
              double *dest_pixel_aspect)
{
   GimvXine *gxine = (GimvXine *) gxine_gen;
   GimvXinePrivate *priv;

   g_return_if_fail (GIMV_IS_XINE (gxine));
   priv = gxine->private;

   /* correct size with video_pixel_aspect */
   if (video_pixel_aspect >= priv->display_ratio)
      video_width =
         video_width * video_pixel_aspect / priv->display_ratio + .5;
   else
      video_height =
         video_height * priv->display_ratio / video_pixel_aspect + .5;

   /* GTK4: called from xine's video thread, use the cached allocation */
   *dest_width  = priv->alloc_width;
   *dest_height = priv->alloc_height;

   *dest_pixel_aspect = priv->display_ratio;
}


static void
frame_output_cb (void *gxine_gen,
                 int video_width, int video_height,
                 double video_pixel_aspect,
                 int *dest_x, int *dest_y,
                 int *dest_width, int *dest_height,
                 double *dest_pixel_aspect,
                 int *win_x, int *win_y)
{
   GimvXine *gxine = (GimvXine *) gxine_gen;
   GimvXinePrivate *priv;
   Window child;

   g_return_if_fail (GIMV_IS_XINE (gxine));
   priv = gxine->private;

   /* correct size with video_pixel_aspect */
   if (video_pixel_aspect >= priv->display_ratio)
      video_width =
         video_width * video_pixel_aspect / priv->display_ratio + .5;
   else
      video_height =
         video_height * priv->display_ratio / video_pixel_aspect + .5;

   *dest_x = 0;
   *dest_y = 0;

   /* GTK4: GdkWindow positions can't be queried any more, ask the X server
    * for the absolute position of our video window instead. */
   *win_x = 0;
   *win_y = 0;
   if (priv->display && priv->video_window)
      XTranslateCoordinates (priv->display, priv->video_window,
                             DefaultRootWindow (priv->display),
                             0, 0, win_x, win_y, &child);

   *dest_width  = priv->alloc_width;
   *dest_height = priv->alloc_height;

   *dest_pixel_aspect = priv->display_ratio;
}
#endif /* GDK_WINDOWING_X11 */


/*
 * GTK4: raw video output for backends where we can't give xine a native
 * window.  Called from xine's video output thread.
 */
static gboolean
idle_update_frame (gpointer data)
{
   GimvXine *gxine = GIMV_XINE (data);
   GimvXinePrivate *priv = gxine->private;
   GdkTexture *texture = NULL;

   if (!priv) {
      g_object_unref (gxine);
      return G_SOURCE_REMOVE;
   }

   g_mutex_lock (&priv->frame_lock);
   priv->frame_idle_pending = FALSE;
   if (priv->frame_changed && priv->frame_buf
       && priv->frame_width > 0 && priv->frame_height > 0)
   {
      GBytes *bytes;

      bytes = g_bytes_new (priv->frame_buf,
                           (gsize) priv->frame_width * priv->frame_height * 3);
      texture = gdk_memory_texture_new (priv->frame_width,
                                        priv->frame_height,
                                        GDK_MEMORY_R8G8B8,
                                        bytes,
                                        (gsize) priv->frame_width * 3);
      g_bytes_unref (bytes);
   }
   priv->frame_changed = FALSE;
   g_mutex_unlock (&priv->frame_lock);

   if (texture) {
      g_clear_object (&priv->frame_texture);
      priv->frame_texture = texture;
      gtk_widget_queue_draw (GTK_WIDGET (gxine));
   }

   g_object_unref (gxine);

   return G_SOURCE_REMOVE;
}


static void
raw_output_cb (void *user_data, int frame_format,
               int frame_width, int frame_height,
               double frame_aspect,
               void *data0, void *data1, void *data2)
{
   GimvXine *gxine = (GimvXine *) user_data;
   GimvXinePrivate *priv = gxine->private;
   gsize size;

   if (frame_format != XINE_VORAW_RGB || !data0) return;
   if (frame_width <= 0 || frame_height <= 0) return;

   size = (gsize) frame_width * frame_height * 3;

   g_mutex_lock (&priv->frame_lock);
   if (!priv->frame_buf
       || priv->frame_width  != frame_width
       || priv->frame_height != frame_height)
   {
      g_free (priv->frame_buf);
      priv->frame_buf = g_malloc (size);
   }
   memcpy (priv->frame_buf, data0, size);
   priv->frame_width   = frame_width;
   priv->frame_height  = frame_height;
   priv->frame_aspect  = frame_aspect;
   priv->frame_changed = TRUE;
   if (!priv->frame_idle_pending) {
      priv->frame_idle_pending = TRUE;
      g_idle_add (idle_update_frame, g_object_ref (gxine));
   }
   g_mutex_unlock (&priv->frame_lock);
}


static void
raw_overlay_cb (void *user_data, int num_ovl, raw_overlay_t *overlays_array)
{
   /* GTK4: overlays (OSD/subtitles) aren't rendered with the raw driver */
}


static xine_video_port_t *
load_video_out_driver (GimvXine *this)
{
#if defined(GDK_WINDOWING_X11)
   x11_visual_t vis;
   double res_h, res_v;
#endif /* defined(GDK_WINDOWING_X11) */
   raw_visual_t raw_vis;

   GimvXinePrivate *priv;
   const char *video_driver_id;
   xine_video_port_t *vo_driver;

   g_return_val_if_fail (GIMV_IS_XINE (this), NULL);
   priv = this->private;

   if (priv->video_driver_id) {
      video_driver_id = priv->video_driver_id;
   } else {
      /* try to init video with stored information */
      video_driver_id = xine_config_register_string (priv->xine,
                                                     "video.driver",
                                                     "auto",
                                                     "video driver to use",
                                                     NULL, 10, NULL, NULL);
   }

#if defined(GDK_WINDOWING_X11)
   if (priv->use_x11) {
      memset (&vis, 0, sizeof (vis));
      vis.display = priv->display;
      vis.screen  = priv->screen;
      vis.d       = priv->video_window;
      res_h       = (DisplayWidth (priv->display, priv->screen) * 1000
                     / DisplayWidthMM (priv->display, priv->screen));
      res_v       = (DisplayHeight (priv->display, priv->screen) * 1000
                     / DisplayHeightMM (priv->display, priv->screen));
      priv->display_ratio = res_v / res_h;

      if (fabs (priv->display_ratio - 1.0) < 0.01) {
         priv->display_ratio = 1.0;
      }

      vis.dest_size_cb = dest_size_cb;
      vis.frame_output_cb = frame_output_cb;
      vis.user_data = this;

      if (strcmp (video_driver_id, "auto")) {
         vo_driver = xine_open_video_driver (priv->xine,
                                             video_driver_id,
                                             XINE_VISUAL_TYPE_X11,
                                             (void *) &vis);
         if (vo_driver)
            return vo_driver;
         else
            g_print ("gtkxine: video driver %s failed.\n", video_driver_id);
      }

      return xine_open_video_driver (priv->xine, NULL,
                                     XINE_VISUAL_TYPE_X11,
                                     (void *) &vis);
   }
#endif /* defined(GDK_WINDOWING_X11) */

   /* GTK4: no native window available: let xine render RGB frames which
    * are drawn by gimv_xine_snapshot().  Only the "raw" driver supports
    * this visual type, so the configured driver is ignored here. */
   (void) video_driver_id;
   priv->display_ratio = 1.0;

   memset (&raw_vis, 0, sizeof (raw_vis));
   raw_vis.user_data         = this;
   raw_vis.supported_formats = XINE_VORAW_RGB;
   raw_vis.raw_output_cb     = raw_output_cb;
   raw_vis.raw_overlay_cb    = raw_overlay_cb;

   vo_driver = xine_open_video_driver (priv->xine, "raw",
                                       XINE_VISUAL_TYPE_RAW,
                                       (void *) &raw_vis);
   if (!vo_driver)
      g_print ("gtkxine: video driver raw failed.\n");

   return vo_driver;
}


static xine_audio_port_t *
load_audio_out_driver (GimvXine *this)
{
   GimvXinePrivate *priv;
   xine_audio_port_t *ao_driver;
   const char *audio_driver_id;

   g_return_val_if_fail (GIMV_IS_XINE (this), NULL);
   priv = this->private;

   if (priv->audio_driver_id)
      audio_driver_id = priv->audio_driver_id;
   else
      /* try to init audio with stored information */
      audio_driver_id = xine_config_register_string (priv->xine,
                                                     "audio.driver",
                                                     "auto",
                                                     "audio driver to use",
                                                     NULL, 10, NULL, NULL);

   if (!strcmp (audio_driver_id, "null"))
      return NULL;

   if (strcmp (audio_driver_id, "auto")) {
      ao_driver =
         xine_open_audio_driver (priv->xine, audio_driver_id, NULL);
      if (ao_driver)
         return ao_driver;
      else
         g_print ("audio driver %s failed\n", audio_driver_id);
   }

   /* autoprobe */
   return xine_open_audio_driver (priv->xine, NULL, NULL);
}


#if defined (GDK_WINDOWING_X11)
/* GTK4: gdk_window_add_filter () is gone, GdkX11Display::xevent is the
 * replacement. */
static gboolean
filter_xine_event (GdkX11Display *display, gpointer xevent, gpointer data)
{
   XEvent *event = xevent;
   GimvXine *this = GIMV_XINE (data);
   GimvXinePrivate *priv;

   g_return_val_if_fail (GIMV_IS_XINE (this), FALSE);
   priv = this->private;

   if (!priv->stream) return FALSE;

   switch (event->type) {
   case Expose:
      if (event->xexpose.window != priv->video_window)
         break;
      if (event->xexpose.count != 0)
         break;

      /* xine-lib 1.2: xine_gui_send_vo_data () is gone */
      if (priv->vo_driver)
         xine_port_send_gui_data (priv->vo_driver,
                                  XINE_GUI_SEND_EXPOSE_EVENT, event);
      break;

   default:
      break;
   }

   if (event->type == priv->completion_event) {
      if (priv->vo_driver)
         xine_port_send_gui_data (priv->vo_driver,
                                  XINE_GUI_SEND_COMPLETION_EVENT, event);
   }

   return FALSE;
}


/*
 * GTK4: keep the X child window at the position of the widget inside the
 * toplevel surface.
 */
static void
update_video_window_geometry (GimvXine *this)
{
   GimvXinePrivate *priv = this->private;
   GtkWidget *widget = GTK_WIDGET (this);
   GtkNative *native;
   graphene_point_t p;
   double sx = 0.0, sy = 0.0;
   gint x, y, width, height;

   if (!priv->use_x11 || !priv->video_window) return;

   native = gtk_widget_get_native (widget);
   if (!native) return;

   if (!gtk_widget_compute_point (widget, GTK_WIDGET (native),
                                  &GRAPHENE_POINT_INIT (0, 0), &p))
      return;
   gtk_native_get_surface_transform (native, &sx, &sy);

   x      = (gint) (p.x + sx);
   y      = (gint) (p.y + sy);
   width  = MAX (1, gtk_widget_get_width (widget));
   height = MAX (1, gtk_widget_get_height (widget));

   if (x == priv->win_x && y == priv->win_y
       && width == priv->win_width && height == priv->win_height)
   {
      return;
   }

   priv->win_x      = x;
   priv->win_y      = y;
   priv->win_width  = width;
   priv->win_height = height;

   XMoveResizeWindow (priv->gdk_display, priv->video_window,
                      x, y, width, height);
   XFlush (priv->gdk_display);
}


static gboolean
realize_x11_window (GimvXine *this)
{
   GimvXinePrivate *priv = this->private;
   GtkWidget *widget = GTK_WIDGET (this);
   GdkDisplay *gdisplay = gtk_widget_get_display (widget);
   GtkNative *native;
   GdkSurface *surface;
   Window parent;

   if (!GDK_IS_X11_DISPLAY (gdisplay)) return FALSE;

   native = gtk_widget_get_native (widget);
   if (!native) return FALSE;
   surface = gtk_native_get_surface (native);
   if (!surface || !GDK_IS_X11_SURFACE (surface)) return FALSE;

   parent = gdk_x11_surface_get_xid (surface);
   priv->gdk_display = gdk_x11_display_get_xdisplay (gdisplay);

   /*
    * create our own video window
    */
   priv->win_x = priv->win_y = -1;
   priv->win_width = priv->win_height = -1;
   priv->video_window
      = gimv_x11_create_video_window (priv->gdk_display, parent,
                                      gtk_widget_get_width (widget),
                                      gtk_widget_get_height (widget),
                                      &priv->video_colormap);
   XSelectInput (priv->gdk_display, priv->video_window, ExposureMask);

   /* GTK4: note that XInitThreads () should be called before GDK opens the
    * display; recent Xlib versions do this automatically. */
   if (!XInitThreads ()) {
      g_print ("gtkxine: XInitThreads failed - "
               "looks like you don't have a thread-safe xlib.\n");
      gimv_x11_destroy_video_window (priv->gdk_display, priv->video_window,
                                     priv->video_colormap);
      priv->video_window = 0;
      priv->video_colormap = None;
      return FALSE;
   }

   priv->display = XOpenDisplay (gdk_display_get_name (gdisplay));

   if (!priv->display) {
      g_print ("gtkxine: XOpenDisplay failed!\n");
      gimv_x11_destroy_video_window (priv->gdk_display, priv->video_window,
                                     priv->video_colormap);
      priv->video_window = 0;
      priv->video_colormap = None;
      return FALSE;
   }

   XLockDisplay (priv->display);

   priv->screen = DefaultScreen (priv->display);

   if (XShmQueryExtension (priv->display) == True) {
      priv->completion_event
         = XShmGetEventBase (priv->display) + ShmCompletion;
   } else {
      priv->completion_event = -1;
   }

   XSelectInput (priv->display, priv->video_window,
                 /* StructureNotifyMask | */ ExposureMask
                 /* | ButtonPressMask | PointerMotionMask */);

   XUnlockDisplay (priv->display);

   priv->xevent_id = g_signal_connect (gdisplay, "xevent",
                                       G_CALLBACK (filter_xine_event), this);

   priv->use_x11 = TRUE;
   update_video_window_geometry (this);

   return TRUE;
}


static void
unrealize_x11_window (GimvXine *this)
{
   GimvXinePrivate *priv = this->private;

   if (priv->xevent_id) {
      g_signal_handler_disconnect (gtk_widget_get_display (GTK_WIDGET (this)),
                                   priv->xevent_id);
      priv->xevent_id = 0;
   }

   if (priv->display) {
      XCloseDisplay (priv->display);
      priv->display = NULL;
   }

   if (priv->video_window && priv->gdk_display) {
      gimv_x11_destroy_video_window (priv->gdk_display, priv->video_window,
                                     priv->video_colormap);
      XFlush (priv->gdk_display);
   }
   priv->video_colormap = None;
   priv->video_window = 0;
   priv->win_mapped   = FALSE;
   priv->use_x11      = FALSE;
}
#endif /* defined (GDK_WINDOWING_X11) */


static gboolean
idle_playback_finished (gpointer data)
{
   GimvXine *gtx = GIMV_XINE (data);

   g_signal_emit (G_OBJECT (gtx),
                  gimv_xine_signals[PLAYBACK_FINISHED_SIGNAL], 0);
   g_object_unref (gtx);

   return G_SOURCE_REMOVE;
}


static void
event_listener (void *data, const xine_event_t * event)
{
    GimvXine *gtx = GIMV_XINE (data);

    g_return_if_fail (GIMV_IS_XINE (gtx));
    g_return_if_fail (event);

    switch (event->type)
    {
      case XINE_EVENT_UI_PLAYBACK_FINISHED:
	  /* GTK4: this runs in xine's listener thread, emit the signal in
	   * the main thread */
	  g_idle_add (idle_playback_finished, g_object_ref (gtx));
	  break;

      default:
	  break;
    }
}


static void
gimv_xine_realize (GtkWidget * widget)
{
   GimvXine *this;
   GimvXinePrivate *priv;

   g_return_if_fail (widget);
   g_return_if_fail (GIMV_IS_XINE (widget));

   this = GIMV_XINE (widget);
   priv = this->private;

   GTK_WIDGET_CLASS (parent_class)->realize (widget);

   priv->use_x11 = FALSE;

#if defined (GDK_WINDOWING_X11)
   /* GTK4: embedding a video window is only possible on X11: create an X
    * child window of the toplevel's surface.  Other backends fall back to
    * rendering xine's raw frames into a texture. */
   realize_x11_window (this);
#endif /* defined (GDK_WINDOWING_X11) */

   /*
    * load audio, video drivers
    */

   priv->vo_driver = load_video_out_driver (this);

   if (!priv->vo_driver) {
      g_print ("gtkxine: couldn't open video driver\n");
      return;
   }

   priv->ao_driver = load_audio_out_driver (this);

   /*
    * create a stream object
    */

   priv->stream = xine_stream_new (priv->xine, priv->ao_driver,
                                   priv->vo_driver);

   priv->event_queue = xine_event_new_queue (priv->stream);
   xine_event_create_listener_thread (priv->event_queue, event_listener,
                                      this);

   post_init(this);

   return;
}


static void
gimv_xine_unrealize (GtkWidget *widget)
{
   GimvXine *this;
   GimvXinePrivate *priv;

   g_return_if_fail (widget);
   g_return_if_fail (GIMV_IS_XINE (widget));

   this = GIMV_XINE (widget);
   priv = this->private;

   /*
    * stop the playback 
    */
   if (priv->stream) {
      gimv_xine_stop(this);
      xine_close (priv->stream);
      xine_event_dispose_queue (priv->event_queue);
      priv->event_queue = NULL;
      xine_dispose (priv->stream);
      priv->stream = NULL;
   }
   if (priv->visual_anim.post_output) {
      xine_post_dispose (priv->xine, priv->visual_anim.post_output);
      priv->visual_anim.post_output = NULL;
   }
   if (priv->post_video) {
      xine_post_dispose (priv->xine, priv->post_video);
      priv->post_video = NULL;
   }
   if (priv->ao_driver)
      xine_close_audio_driver(priv->xine, priv->ao_driver);
   if (priv->vo_driver)
      xine_close_video_driver(priv->xine, priv->vo_driver);
   priv->ao_driver = NULL;
   priv->vo_driver = NULL;

#if defined (GDK_WINDOWING_X11)
   /* stop event thread, destroy the video window */
   unrealize_x11_window (this);
#endif /* defined (GDK_WINDOWING_X11) */

   g_clear_object (&priv->frame_texture);

   /* save configuration */
   /* xine_config_save (priv->xine, priv->configfile); */

   GTK_WIDGET_CLASS (parent_class)->unrealize (widget);
}


static void
gimv_xine_map (GtkWidget *widget)
{
   GTK_WIDGET_CLASS (parent_class)->map (widget);

#if defined (GDK_WINDOWING_X11)
   {
      GimvXinePrivate *priv = GIMV_XINE (widget)->private;

      if (priv->use_x11 && priv->video_window && !priv->win_mapped) {
         update_video_window_geometry (GIMV_XINE (widget));
         XMapWindow (priv->gdk_display, priv->video_window);
         XFlush (priv->gdk_display);
         priv->win_mapped = TRUE;
         if (priv->stream)
            xine_port_send_gui_data (priv->vo_driver,
                                     XINE_GUI_SEND_VIDEOWIN_VISIBLE,
                                     (void *) 1);
      }
   }
#endif /* defined (GDK_WINDOWING_X11) */
}


static void
gimv_xine_unmap (GtkWidget *widget)
{
#if defined (GDK_WINDOWING_X11)
   {
      GimvXinePrivate *priv = GIMV_XINE (widget)->private;

      if (priv->use_x11 && priv->video_window && priv->win_mapped) {
         if (priv->stream)
            xine_port_send_gui_data (priv->vo_driver,
                                     XINE_GUI_SEND_VIDEOWIN_VISIBLE,
                                     (void *) 0);
         XUnmapWindow (priv->gdk_display, priv->video_window);
         XFlush (priv->gdk_display);
         priv->win_mapped = FALSE;
      }
   }
#endif /* defined (GDK_WINDOWING_X11) */

   GTK_WIDGET_CLASS (parent_class)->unmap (widget);
}


GtkWidget *
gimv_xine_new (const gchar *video_driver_id, const gchar *audio_driver_id)
{
   GtkWidget *this = GTK_WIDGET (g_object_new (gimv_xine_get_type (), NULL));
   GimvXinePrivate *priv;

   g_return_val_if_fail (GIMV_IS_XINE (this), NULL);
   priv = GIMV_XINE (this)->private;

   if (video_driver_id)
      priv->video_driver_id = g_strdup (video_driver_id);
   else
      priv->video_driver_id = NULL;

   if (audio_driver_id)
      priv->audio_driver_id = g_strdup (audio_driver_id);
   else
      priv->audio_driver_id = NULL;

   return this;
}


static void
gimv_xine_measure (GtkWidget      *widget,
                   GtkOrientation  orientation,
                   int             for_size,
                   int            *minimum,
                   int            *natural,
                   int            *minimum_baseline,
                   int            *natural_baseline)
{
   /* GTK2 version: requisition 8x8 */
   *minimum = *natural = 8;
}


/* GTK4: replaces the expose handler.  Paints the black background, and on
 * non-X11 backends the last frame delivered by the raw video driver. */
static void
gimv_xine_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
   GimvXine *this = GIMV_XINE (widget);
   GimvXinePrivate *priv = this->private;
   gint width  = gtk_widget_get_width (widget);
   gint height = gtk_widget_get_height (widget);
   GdkRGBA black = { 0.0, 0.0, 0.0, 1.0 };

   gtk_snapshot_append_color (snapshot, &black,
                              &GRAPHENE_RECT_INIT (0, 0, width, height));

#if defined (GDK_WINDOWING_X11)
   if (priv->use_x11) {
      /* the widget may have been moved without a new allocation */
      update_video_window_geometry (this);
      return;
   }
#endif /* defined (GDK_WINDOWING_X11) */

   if (priv->frame_texture && width > 0 && height > 0) {
      gint tw = gdk_texture_get_width (priv->frame_texture);
      gint th = gdk_texture_get_height (priv->frame_texture);
      gdouble aspect = priv->frame_aspect > 0.0
         ? priv->frame_aspect : (gdouble) tw / (gdouble) th;
      gdouble dw, dh;

      /* keep aspect ratio, fit into the widget */
      dw = width;
      dh = width / aspect;
      if (dh > height) {
         dh = height;
         dw = height * aspect;
      }

      gtk_snapshot_append_texture (snapshot, priv->frame_texture,
                                   &GRAPHENE_RECT_INIT ((width  - dw) / 2.0,
                                                        (height - dh) / 2.0,
                                                        dw, dh));
   }
}


static void
gimv_xine_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
   GimvXine *this;

   g_return_if_fail (widget);
   g_return_if_fail (GIMV_IS_XINE (widget));

   this = GIMV_XINE (widget);

   this->private->alloc_width  = width;
   this->private->alloc_height = height;

#if defined (GDK_WINDOWING_X11)
   if (gtk_widget_get_realized (GTK_WIDGET (widget)))
      update_video_window_geometry (this);
#endif /* defined (GDK_WINDOWING_X11) */
}


gint
gimv_xine_set_mrl (GimvXine *gtx, const gchar *mrl)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, FALSE);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), FALSE);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, FALSE);

   return xine_open (priv->stream, mrl);
}


static void
visual_anim_play(GimvXine *gtx)
{
   if(gtx->private->visual_anim.enabled == 2) {
      gtx->private->visual_anim.running = 1;
  }
}

static void
visual_anim_stop(GimvXine *gtx)
{
   if(gtx->private->visual_anim.enabled == 2) {
      xine_stop(gtx->private->visual_anim.stream);
      gtx->private->visual_anim.running = 0;
  }
}

gint
gimv_xine_play (GimvXine *gtx, gint pos, gint start_time)
{
   GimvXinePrivate *priv;
   gint retval;
   gboolean has_video;

   g_return_val_if_fail (gtx, -1);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), -1);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, -1);

#warning FIXME
   if(priv->visual_anim.post_changed
      && (xine_get_status (priv->stream) == XINE_STATUS_STOP))
   {
      post_rewire_visual_anim (gtx);
      priv->visual_anim.post_changed = 0;
   }

   has_video = xine_get_stream_info (priv->stream, XINE_STREAM_INFO_HAS_VIDEO);
   if (has_video)
      has_video = !xine_get_stream_info (priv->stream,
                                         XINE_STREAM_INFO_IGNORE_VIDEO);

   priv->visual_anim.enabled = 1;

   if ((has_video && priv->visual_anim.enabled == 1)
       && priv->visual_anim.running)
   {
      if (post_rewire_audio_port_to_stream(gtx, priv->stream))
         priv->visual_anim.running = 0;

   } else if (!has_video && (priv->visual_anim.enabled == 1)
              && (priv->visual_anim.running == 0)
              && priv->visual_anim.post_output)
   {
      if (post_rewire_audio_post_to_stream(gtx, priv->stream))
         priv->visual_anim.running = 1;

   } else if (has_video && priv->post_video && (priv->post_video_num > 0)) {
      post_rewire_video_post_to_stream(gtx, priv->stream);
   }

   retval = xine_play (priv->stream, pos, start_time);

   if (retval) {
      if(has_video) {
         if((priv->visual_anim.enabled == 2) && priv->visual_anim.running)
            visual_anim_stop(gtx);
      } else {
         if(!priv->visual_anim.running)
            visual_anim_play(gtx);
      }
      g_signal_emit (G_OBJECT(gtx),
                     gimv_xine_signals[PLAY_SIGNAL], 0);
   }

   return retval;
}


void
gimv_xine_set_speed (GimvXine *gtx, gint speed)
{
   gimv_xine_set_param (gtx, XINE_PARAM_SPEED, speed);
}


gint
gimv_xine_get_speed (GimvXine *gtx)
{
   return gimv_xine_get_param (gtx, XINE_PARAM_SPEED);
}


gint
gimv_xine_trick_mode (GimvXine *gtx, gint mode, gint value)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->stream, 0);

   /* GTK4 port: xine_trick_mode () was removed in libxine 1.2 (it was
    * an unimplemented stub returning 0 before). */
   (void) mode;
   (void) value;
   return 0;
}

static gint
gimv_xine_get_pos_length (GimvXine *gtx, gint *pos_stream,
                          gint *pos_time, gint *length_time)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->stream, 0);

   return xine_get_pos_length (priv->stream, pos_stream, pos_time,
                               length_time);
}


gint
gimv_xine_get_current_time (GimvXine *gtx)
{
   gint pos_stream = 0, pos_time = 0, length_time = 0;

   gimv_xine_get_pos_length (gtx, &pos_stream, &pos_time, &length_time);
   return pos_time;
}


gint
gimv_xine_get_stream_length (GimvXine *gtx)
{
   gint pos_stream = 0, pos_time = 0, length_time = 0;

   gimv_xine_get_pos_length (gtx, &pos_stream, &pos_time, &length_time);
   return length_time;
}


void
gimv_xine_stop (GimvXine *gtx)
{
   GimvXinePrivate *priv;

   g_return_if_fail (gtx);
   g_return_if_fail (GIMV_IS_XINE (gtx));

   priv = gtx->private;
   g_return_if_fail (priv->stream);

   xine_stop (priv->stream);

   g_signal_emit (G_OBJECT(gtx),
                  gimv_xine_signals[STOP_SIGNAL], 0);
}


gint
gimv_xine_get_error (GimvXine *gtx)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->stream, 0);

   return xine_get_error (priv->stream);
}


gint
gimv_xine_get_status (GimvXine *gtx)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->stream, 0);

   return xine_get_status (priv->stream);
}


gint
gimv_xine_is_playing (GimvXine *gtx)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, 0);

   if (xine_get_status (priv->stream) == XINE_STATUS_PLAY)
      return TRUE;
   else
      return FALSE;
}


void
gimv_xine_set_param (GimvXine *gtx, gint param, gint value)
{
   GimvXinePrivate *priv;

   g_return_if_fail (gtx);
   g_return_if_fail (GIMV_IS_XINE (gtx));

   priv = gtx->private;
   g_return_if_fail (priv->stream);

   xine_set_param (priv->stream, param, value);
}


gint
gimv_xine_get_param (GimvXine *gtx, gint param)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->stream, 0);

   return xine_get_param (priv->stream, param);
}


gint
gimv_xine_get_audio_lang (GimvXine *gtx, gint channel, gchar *lang)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->stream, 0);

   return xine_get_audio_lang (priv->stream, channel, lang);
}


gint
gimv_xine_get_spu_lang (GimvXine *gtx, gint channel, gchar *lang)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->stream, 0);

   return xine_get_spu_lang (priv->stream, channel, lang);
}


gint
gimv_xine_get_stream_info (GimvXine *gtx, gint info)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->stream, 0);

   return xine_get_stream_info (priv->stream, info);
}


const gchar *
gimv_xine_get_meta_info (GimvXine *gtx, gint info)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->stream, 0);

   return xine_get_meta_info (priv->stream, info);
}


gint
gimv_xine_get_current_frame (GimvXine *gtx,
                             gint *width,
                             gint *height,
                             gint *ratio_code, gint *format,
                             uint8_t *img)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->stream, 0);

   return xine_get_current_frame (priv->stream, width, height, ratio_code,
                                  format, img);
}


guchar *
gimv_xine_get_current_frame_rgb (GimvXine *gtx,
                                 gint *width_ret,
                                 gint *height_ret)
{
   GimvXinePrivate *priv;
   gint err = 0;
   GimvXinePrivImage *image;
   guchar *rgb = NULL;
   gint width, height;

   g_return_val_if_fail (gtx, NULL);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), NULL);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, NULL);
   g_return_val_if_fail (width_ret && height_ret, NULL);

   width  = xine_get_stream_info (priv->stream, XINE_STREAM_INFO_VIDEO_WIDTH);
   height = xine_get_stream_info (priv->stream, XINE_STREAM_INFO_VIDEO_HEIGHT);

   image = gimv_xine_priv_image_new (sizeof (guchar) * width * height * 2);

   err = xine_get_current_frame(priv->stream,
                                &image->width, &image->height,
                                &image->ratio_code,
                                &image->format,
                                image->img);

   if (err == 0) goto ERROR;

   /* the dxr3 driver does not allocate yuv buffers */
   /* image->u and image->v are always 0 for YUY2 */
   if (!image->img) goto ERROR;

   rgb = gimv_xine_priv_yuv2rgb (image);
   *width_ret  = image->width;
   *height_ret = image->height;

 ERROR:
   gimv_xine_priv_image_delete (image);
   return rgb;
}


gint
gimv_xine_get_log_section_count (GimvXine *gtx)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, 0);

   return xine_get_log_section_count (priv->xine);
}


gchar **
gimv_xine_get_log_names (GimvXine *gtx)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, NULL);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), NULL);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, NULL);

   return (gchar **) xine_get_log_names (priv->xine);
}

gchar **
gimv_xine_get_log (GimvXine *gtx, gint buf)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, NULL);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), NULL);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, NULL);

   return (gchar **) xine_get_log (priv->xine, buf);
}


void
gimv_xine_register_log_cb (GimvXine *gtx, xine_log_cb_t cb, void *user_data)
{
   GimvXinePrivate *priv;

   g_return_if_fail (gtx);
   g_return_if_fail (GIMV_IS_XINE (gtx));

   priv = gtx->private;
   g_return_if_fail (priv->xine);

   return xine_register_log_cb (priv->xine, cb, user_data);
}

gchar **
gimv_xine_get_browsable_input_plugin_ids (GimvXine *gtx)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, NULL);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), NULL);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, NULL);

   return (gchar **) xine_get_browsable_input_plugin_ids (priv->xine);
}


xine_mrl_t **
gimv_xine_get_browse_mrls (GimvXine *gtx,
                           const gchar *plugin_id,
                           const gchar *start_mrl, gint *num_mrls)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, NULL);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), NULL);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, NULL);

   return (xine_mrl_t **) xine_get_browse_mrls (priv->xine, plugin_id,
                                                start_mrl, num_mrls);
}


gchar **
gimv_xine_get_autoplay_input_plugin_ids (GimvXine *gtx)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, NULL);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), NULL);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, NULL);

   return (gchar **) xine_get_autoplay_input_plugin_ids (priv->xine);
}


gchar **
gimv_xine_get_autoplay_mrls (GimvXine *gtx,
                             const gchar *plugin_id, gint *num_mrls)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, NULL);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), NULL);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, NULL);

   return (gchar **) xine_get_autoplay_mrls (priv->xine, plugin_id,
                                             num_mrls);
}


gchar *
gimv_xine_get_file_extensions (GimvXine *gtx)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, NULL);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), NULL);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, NULL);

   return (gchar *) xine_get_file_extensions (priv->xine);
}


gchar *
gimv_xine_get_mime_types (GimvXine *gtx)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, NULL);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), NULL);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, NULL);

   return (gchar *) xine_get_mime_types (priv->xine);
}


const gchar *
gimv_xine_config_register_string (GimvXine *gtx,
                                  const gchar *key,
                                  const gchar *def_value,
                                  const gchar *description,
                                  const gchar *help,
                                  gint exp_level,
                                  xine_config_cb_t changed_cb,
                                  void *cb_data)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, NULL);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), NULL);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, NULL);

   return (gchar *) xine_config_register_string (priv->xine, key, def_value,
                                                 description, help,
                                                 exp_level, changed_cb,
                                                 cb_data);
}


gint
gimv_xine_config_register_range (GimvXine *gtx,
                                 const gchar *key,
                                 gint def_value,
                                 gint min, gint max,
                                 const gchar *description,
                                 const gchar *help,
                                 gint exp_level,
                                 xine_config_cb_t changed_cb, void *cb_data)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, 0);

   return xine_config_register_range (priv->xine, key, def_value, min, max,
                                      description, help,
                                      exp_level, changed_cb, cb_data);
}


gint
gimv_xine_config_register_enum (GimvXine *gtx,
                                const gchar *key,
                                gint def_value,
                                gchar **values,
                                const gchar *description,
                                const gchar *help,
                                gint exp_level,
                                xine_config_cb_t changed_cb, void *cb_data)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, 0);

   return xine_config_register_enum (priv->xine, key, def_value, values,
                                     description, help,
                                     exp_level, changed_cb, cb_data);
}


gint
gimv_xine_config_register_num (GimvXine *gtx,
                               const gchar *key,
                               gint def_value,
                               const gchar *description,
                               const gchar *help,
                               gint exp_level,
                               xine_config_cb_t changed_cb, void *cb_data)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, 0);

   return xine_config_register_num (priv->xine, key, def_value,
                                    description, help,
                                    exp_level, changed_cb, cb_data);
}


gint
gimv_xine_config_register_bool (GimvXine *gtx,
                                const gchar *key,
                                gint def_value,
                                const gchar *description,
                                const gchar *help,
                                gint exp_level,
                                xine_config_cb_t changed_cb, void *cb_data)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, 0);

   return xine_config_register_bool (priv->xine, key, def_value,
                                     description, help,
                                     exp_level, changed_cb, cb_data);
}


int
gimv_xine_config_get_first_entry (GimvXine *gtx, xine_cfg_entry_t *entry)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, 0);

   return xine_config_get_first_entry (priv->xine, entry);
}


int
gimv_xine_config_get_next_entry (GimvXine *gtx, xine_cfg_entry_t *entry)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, 0);

   return xine_config_get_next_entry (priv->xine, entry);
}


int
gimv_xine_config_lookup_entry (GimvXine *gtx,
                               const gchar *key, xine_cfg_entry_t *entry)
{
   GimvXinePrivate *priv;

   g_return_val_if_fail (gtx, 0);
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   priv = gtx->private;
   g_return_val_if_fail (priv->xine, 0);

   return xine_config_lookup_entry (priv->xine, key, entry);
}


void
gimv_xine_config_update_entry (GimvXine *gtx, xine_cfg_entry_t *entry)
{
   GimvXinePrivate *priv;

   g_return_if_fail (gtx);
   g_return_if_fail (GIMV_IS_XINE (gtx));

   priv = gtx->private;
   g_return_if_fail (priv->xine);

   xine_config_update_entry (priv->xine, entry);
}


void
gimv_xine_config_load (GimvXine *gtx, const gchar *cfg_filename)
{
   GimvXinePrivate *priv;

   g_return_if_fail (gtx);
   g_return_if_fail (GIMV_IS_XINE (gtx));

   priv = gtx->private;
   g_return_if_fail (priv->xine);

   xine_config_load (priv->xine, cfg_filename);
}


void
gimv_xine_config_save (GimvXine *gtx, const gchar *cfg_filename)
{
   GimvXinePrivate *priv;

   g_return_if_fail (gtx);
   g_return_if_fail (GIMV_IS_XINE (gtx));

   priv = gtx->private;
   g_return_if_fail (priv->xine);

   xine_config_save (priv->xine, cfg_filename);
}


void
gimv_xine_config_reset (GimvXine *gtx)
{
   GimvXinePrivate *priv;

   g_return_if_fail (gtx);
   g_return_if_fail (GIMV_IS_XINE (gtx));

   priv = gtx->private;
   g_return_if_fail (priv->xine);

   xine_config_reset (priv->xine);
}


void
gimv_xine_event_send (GimvXine *gtx, const xine_event_t *event)
{
   GimvXinePrivate *priv;

   g_return_if_fail (gtx);
   g_return_if_fail (GIMV_IS_XINE (gtx));

   priv = gtx->private;
   g_return_if_fail (priv->stream);

   xine_event_send (priv->stream, event);
}

#endif /* (XINE_MAJOR_VERSION >= 1) && (XINE_MINOR_VERSION >= 0) && (XINE_SUB_VERSION >= 0) */
#endif /* ENABLE_XINE */
