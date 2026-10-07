/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001-2003 Takuro Ashie
 * Copyright (C) 2003 Frank Fischer <frank_fischer@gmx.de>
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
 * $Id: gimv_image_view.c,v 1.17 2004/09/29 06:13:50 makeinu Exp $
 */

#include <stdlib.h>
#include <string.h>

#include "gimageview.h"

#include "cursors.h"
#include "dnd.h"
#include "fileutil.h"
#include "gimv_image.h"
#include "gimv_anim.h"
#include "gimv_icon_stock.h"
#include "gimv_thumb.h"
#include "gimv_thumb_view.h"
#include "gimv_comment.h"
#include "gimv_image_view.h"
#include "gimv_image_win.h"
#include "gimv_nav_win.h"
#include "gtk2-compat.h"
#include "gtkutils.h"
#include "menu.h"
#include "gimv_print.h"
#include "prefs.h"

#ifdef ENABLE_EXIF

#include <libexif/exif-data.h>
#include <libexif/exif-utils.h>

#endif


#define MIN_IMAGE_WIDTH  8
#define MIN_IMAGE_HEIGHT 8
#define IMAGE_VIEW_ENABLE_SCALABLE_LOAD 1


typedef enum {
   IMAGE_CHANGED_SIGNAL,
   LOAD_START_SIGNAL,
   LOAD_END_SIGNAL,
   SET_LIST_SIGNAL,
   UNSET_LIST_SIGNAL,
   RENDERED_SIGNAL,
   TOGGLE_ASPECT_SIGNAL,
   TOGGLE_BUFFER_SIGNAL,
   THUMBNAIL_CREATED_SIGNAL,

   IMAGE_PRESSED_SIGNAL,
   IMAGE_RELEASED_SIGNAL,
   IMAGE_CLICKED_SIGNAL,

   LAST_SIGNAL
} GimvImageViewSignalType;


typedef enum {
   MOVIE_STOP,
   MOVIE_PLAY,
   MOVIE_PAUSE,
   MOVIE_FORWARD,
   MOVIE_REVERSE,
   MOVIE_EJECT
} GimvImageViewMovieMenu;


typedef enum
{
   GimvImageViewSeekBarDraggingFlag   = 1 << 0
} GimvImageViewPlayerFlags;


typedef struct GimvImageViewImageList_Tag
{
   GList                    *list;
   GList                    *current;
   gpointer                  owner;
   GimvImageViewNextFn       next_fn;
   GimvImageViewPrevFn       prev_fn;
   GimvImageViewNthFn        nth_fn;
   GimvImageViewRemoveListFn remove_list_fn;
   gpointer                  list_fn_user_data;
} GimvImageViewImageList;


struct GimvImageViewPrivate_Tag
{
   GtkWidget       *navwin;
   GtkWidget       *overlay;   /* GTK4: holds the navigator */

   /* image */
   GdkTexture       *pixmap;
   GdkTexture       *mask;

   /* image status */
   gint             x_pos;
   gint             y_pos;
   gint             width;
   gint             height;

   gfloat           x_scale;
   gfloat           y_scale;
   GimvImageViewOrientation rotate;

   gint             default_zoom;      /* should be defined as enum */
   gint             default_rotation;  /* should be defined as enum */
   GimvImageViewZoomType fit_to_frame;
   gboolean         keep_aspect;
   gboolean         ignore_alpha;
   gboolean         alpha_checker;   /* draw a checkerboard behind alpha */
   gboolean         has_alpha;       /* the shown image kept its alpha */
   gboolean         buffer;

   /* imageview status */
   gboolean         show_scrollbar;
   gboolean         continuance_play;

   /* player status */
   GimvImageViewPlayerFlags       player_flags;
   gboolean                       fit_pending;   /* fit when allocated */
   gdouble                        press_x, press_y;  /* where a button went down */
   gboolean                       size_unknown;  /* info had no size at load start */
   /* seek bar preview */
   gboolean                       preview_busy;  /* a request is open */
   gint64                         preview_want;  /* next request [ms], -1 */
   gint64                         preview_last;  /* last requested [ms] */
   GimvImageInfo                 *preview_info;
   GimvImageInfo                 *time_info;     /* file of time_length */
   guint                          time_length;   /* last known length [ms] */
   GimvImageViewPlayerVisibleType player_visible;

   /* image list and related functions */
   GimvImageViewImageList *image_list;

   /* information about image dragging */
   guint            button;
   gboolean         pressed;
   gboolean         dragging;
   gint             drag_startx;
   gint             drag_starty;
   gint             x_pos_drag_start;
   gint             y_pos_drag_start;

   /* */
   guint   load_end_signal_id;
   guint   loader_progress_update_signal_id;
   guint   loader_load_end_signal_id;

   GtkWindow *fullscreen;

   /* GTK4: cursor to restore after drag scrolling (replaces pointer grab) */
   GdkCursor *saved_cursor;
   /* GTK4: accumulated smooth scroll deltas */
   gdouble    scroll_acc_x;
   gdouble    scroll_acc_y;
};


/* object class methods */
static void gimv_image_view_class_init    (GimvImageViewClass *klass);
static void gimv_image_view_init          (GimvImageView *iv);
static void gimv_image_view_dispose       (GObject *object);
static void gimv_image_view_finalize      (GObject *object);

/* image view class methods */
static void gimv_image_view_image_changed (GimvImageView *iv);

/* call back functions for reference popup menu */
static void cb_alpha_checker               (GimvImageView   *iv,
                                            guint            action,
                                            GimvMenuItem    *widget);
static void cb_ignore_alpha                (GimvImageView   *iv,
                                            guint            action,
                                            GimvMenuItem       *widget);
static void cb_keep_aspect                 (GimvImageView   *iv,
                                            guint            action,
                                            GimvMenuItem       *widget);
static void cb_zoom                        (GimvImageView   *iv,
                                            GimvImageViewZoomType zoom,
                                            GimvMenuItem       *widget);
static void cb_rotate                      (GimvImageView   *iv,
                                            guint            action,
                                            GimvMenuItem       *widget);
static void cb_toggle_scrollbar            (GimvImageView   *iv,
                                            guint            action,
                                            GimvMenuItem       *widget);
static void cb_create_thumbnail            (GimvImageView   *iv,
                                            guint            action,
                                            GimvMenuItem       *widget);
static void cb_toggle_buffer               (GimvImageView   *iv,
                                            guint            action,
                                            GimvMenuItem       *widget);
static void cb_print                       (GimvImageView   *iv,
                                            guint            action,
                                            GimvMenuItem       *widget);

/* call back functions for player toolbar */
static void     cb_gimv_image_view_play   (GtkButton     *button,
                                           GimvImageView *iv);
static void     cb_gimv_image_view_stop   (GtkButton     *button,
                                           GimvImageView *iv);
static void     cb_gimv_image_view_fw     (GtkButton     *button,
                                           GimvImageView *iv);
static void     cb_gimv_image_view_rw     (GtkButton     *button,
                                           GimvImageView *iv);
static void     cb_gimv_image_view_eject  (GtkButton     *button,
                                           GimvImageView *iv);
static gboolean cb_seekbar_pressed  (GtkWidget      *widget,
                                     GimvEventButton *event,
                                     GimvImageView  *iv);
static gboolean cb_seekbar_released (GtkWidget      *widget,
                                     GimvEventButton *event,
                                     GimvImageView  *iv);

/* other call back functions */
static void     cb_destroy_loader          (GimvImageLoader   *loader,
                                            gpointer           data);
static void     cb_image_map               (GtkWidget         *widget,
                                            GimvImageView     *iv);
static gboolean cb_image_key_press         (GtkWidget         *widget,
                                            GimvEventKey       *event,
                                            GimvImageView     *iv);
static gboolean cb_image_button_press      (GtkWidget         *widget,
                                            GimvEventButton    *event,
                                            GimvImageView     *iv);
static gboolean cb_image_button_release    (GtkWidget         *widget, 
                                            GimvEventButton    *event,
                                            GimvImageView     *iv);
static gboolean cb_image_motion_notify     (GtkWidget         *widget, 
                                            GimvEventMotion    *event,
                                            GimvImageView     *iv);
static gboolean cb_image_scroll            (GtkWidget         *widget,
                                            GimvEventScroll   *event,
                                            GimvImageView     *iv);
static void     cb_scrollbar_value_changed (GtkAdjustment     *adj,
                                            GimvImageView     *iv);
static void     cb_nav_button_drag_begin   (GtkGestureDrag    *gesture,
                                            gdouble            x,
                                            gdouble            y,
                                            GimvImageView     *iv);
static void     cb_nav_button_drag_update  (GtkGestureDrag    *gesture,
                                            gdouble            dx,
                                            gdouble            dy,
                                            GimvImageView     *iv);
static void     cb_nav_button_drag_end     (GtkGestureDrag    *gesture,
                                            gdouble            dx,
                                            gdouble            dy,
                                            GimvImageView     *iv);

/* callback functions for movie menu */
static void cb_movie_menu                  (GimvImageView   *iv,
                                            GimvImageViewMovieMenu operation,
                                            GimvMenuItem       *widget);
static void cb_continuance                 (GimvImageView   *iv,
                                            guint            active,
                                            GimvMenuItem       *widget);
static void cb_change_view_mode            (GimvMenuItem    *item,
                                            gpointer         data);

/* other private functions */
static void gimv_image_view_calc_image_size  (GimvImageView   *iv);
static void gimv_image_view_rotate_render    (GimvImageView   *iv,
                                              GimvImageViewOrientation angle);

static void gimv_image_view_change_draw_widget     (GimvImageView     *iv,
                                                    const gchar       *label);
static void gimv_image_view_adjust_pos_in_frame    (GimvImageView     *iv,
                                                    gboolean           center);
static gint idle_gimv_image_view_change_image_info (gpointer           data);
static void gimv_image_view_get_request_size       (GimvImageView     *iv,
                                                    gint              *width_ret,
                                                    gint              *height_ret);

static GtkWidget *gimv_image_view_create_player_toolbar (GimvImageView *iv);


static gpointer parent_class = NULL;
static guint gimv_image_view_signals[LAST_SIGNAL] = {0};


extern GimvImageViewPlugin imageview_draw_vfunc_table;

GList *draw_area_list = NULL;


/* reference menu items */
GimvMenuEntry gimv_image_view_popup_items [] =
{
   {N_("/tear"),                  NULL,         NULL,                0, "<Tearoff>"},
   {N_("/_Zoom"),                 NULL,         NULL,                0, "<Branch>"},
   {N_("/_Rotate"),               NULL,         NULL,                0, "<Branch>"},
   {N_("/Ignore _Alpha Channel"), NULL,         cb_ignore_alpha,     0, "<ToggleItem>"},
   {N_("/_Checkerboard Behind Transparency"), NULL, cb_alpha_checker, 0, "<ToggleItem>"},
   {N_("/---"),                   NULL,         NULL,                0, "<Separator>"},
   {N_("/M_ovie"),                NULL,         NULL,                0, "<Branch>"},
   {N_("/---"),                   NULL,         NULL,                0, "<Separator>"},
   {N_("/_View Modes"),           NULL,         NULL,                0, "<Branch>"},
   {N_("/Show _Scrollbar"),       "<shift>S",   cb_toggle_scrollbar, 0, "<ToggleItem>"},
   {N_("/---"),                   NULL,         NULL,                0, "<Separator>"},
   {N_("/Create _Thumbnail"),     "<shift>T",   cb_create_thumbnail, 0, NULL},
   {N_("/Memory _Buffer"),        "<control>B", cb_toggle_buffer,    0, "<ToggleItem>"},
   {N_("/---"),                   NULL,         NULL,                0, "<Separator>"},
   {N_("/_Print..."),             NULL,         cb_print,            0, NULL},
   {NULL, NULL, NULL, 0, NULL},
};


/* for "Zoom" sub menu */
GimvMenuEntry gimv_image_view_zoom_items [] =
{
   {N_("/tear"),                NULL,        NULL,            0,           "<Tearoff>"},
   {N_("/Zoom _In"),            "S",         cb_zoom,         GIMV_IMAGE_VIEW_ZOOM_IN,     NULL},
   {N_("/Zoom _Out"),           "A",         cb_zoom,         GIMV_IMAGE_VIEW_ZOOM_OUT,    NULL},
   {N_("/_Fit to Window"),      "W",         cb_zoom,         GIMV_IMAGE_VIEW_ZOOM_FIT,    NULL},
   {N_("/_Fit _Width"),         "<shift>W",  cb_zoom,         GIMV_IMAGE_VIEW_ZOOM_FIT_WIDTH, NULL},
   {N_("/_Fit _Height"),        "<shift>H",  cb_zoom,         GIMV_IMAGE_VIEW_ZOOM_FIT_HEIGHT,NULL},
   {N_("/Keep _Aspect Ratio"),  "<shift>A",  cb_keep_aspect,  0,           "<ToggleItem>"},
   {N_("/---"),                 NULL,        NULL,            0,           "<Separator>"},
   {N_("/10%(_1)"),             "1",         cb_zoom,         GIMV_IMAGE_VIEW_ZOOM_10,     NULL},
   {N_("/25%(_2)"),             "2",         cb_zoom,         GIMV_IMAGE_VIEW_ZOOM_25,     NULL},
   {N_("/50%(_3)"),             "3",         cb_zoom,         GIMV_IMAGE_VIEW_ZOOM_50,     NULL},
   {N_("/75%(_4)"),             "4",         cb_zoom,         GIMV_IMAGE_VIEW_ZOOM_75,     NULL},
   {N_("/100%(_5)"),            "5",         cb_zoom,         GIMV_IMAGE_VIEW_ZOOM_100,    NULL},
   {N_("/125%(_6)"),            "6",         cb_zoom,         GIMV_IMAGE_VIEW_ZOOM_125,    NULL},
   {N_("/150%(_7)"),            "7",         cb_zoom,         GIMV_IMAGE_VIEW_ZOOM_150,    NULL},
   {N_("/175%(_8)"),            "8",         cb_zoom,         GIMV_IMAGE_VIEW_ZOOM_175,    NULL},
   {N_("/200%(_9)"),            "9",         cb_zoom,         GIMV_IMAGE_VIEW_ZOOM_200,    NULL},
   {NULL, NULL, NULL, 0, NULL},
};


/* for "Rotate" sub menu */
GimvMenuEntry gimv_image_view_rotate_items [] =
{
   {N_("/tear"),            NULL,  NULL,       0,           "<Tearoff>"},
   {N_("/Rotate 90 Degrees Clockwise"),  "R",   cb_rotate,  GIMV_IMAGE_VIEW_ROTATE_270,  NULL},
   {N_("/Rotate 90 Degrees Counterclockwise"), "E",   cb_rotate,  GIMV_IMAGE_VIEW_ROTATE_90,   NULL},
   {N_("/Rotate 180 Degrees"),    "D",   cb_rotate,  GIMV_IMAGE_VIEW_ROTATE_180,  NULL},
   {NULL, NULL, NULL, 0, NULL},
};


/* for "Movie" sub menu */
GimvMenuEntry gimv_image_view_playable_items [] =
{
   {N_("/tear"),             NULL,  NULL,           0,             "<Tearoff>"},
   {N_("/_Play"),            NULL,  cb_movie_menu,  MOVIE_PLAY,    NULL},
   {N_("/_Stop"),            NULL,  cb_movie_menu,  MOVIE_STOP,    NULL},
   {N_("/P_ause"),           NULL,  cb_movie_menu,  MOVIE_PAUSE,   NULL},
   {N_("/_Forward"),         NULL,  cb_movie_menu,  MOVIE_FORWARD, NULL},
   {N_("/_Reverse"),         NULL,  cb_movie_menu,  MOVIE_REVERSE, NULL},
   {N_("/---"),              NULL,  NULL,           0,             "<Separator>"},
   {N_("/_Continuous Play"),     NULL,  cb_continuance, 0,             "<ToggleItem>"},
   {N_("/---"),              NULL,  NULL,           0,             "<Separator>"},
   {N_("/_Eject"),           NULL,  cb_movie_menu,  MOVIE_EJECT, NULL},
   {NULL, NULL, NULL, 0, NULL},
};


static GList *GimvImageViewList = NULL;
static gboolean move_scrollbar_by_user = TRUE;
static void     update_scrollbar_visibility (GimvImageView *iv);


/****************************************************************************
 *
 *  Plugin Management
 *
 ****************************************************************************/
static gint
comp_func_priority (GimvImageViewPlugin *plugin1,
                    GimvImageViewPlugin *plugin2)
{
   g_return_val_if_fail (plugin1, 1);
   g_return_val_if_fail (plugin2, -1);

   return plugin1->priority_hint - plugin2->priority_hint;
}

gboolean
gimv_image_view_plugin_regist (const gchar *plugin_name,
                               const gchar *module_name,
                               gpointer impl,
                               gint     size)
{
   GimvImageViewPlugin *plugin = impl;

   g_return_val_if_fail (module_name, FALSE);
   g_return_val_if_fail (plugin, FALSE);
   g_return_val_if_fail (size > 0, FALSE);
   g_return_val_if_fail (plugin->if_version == GIMV_IMAGE_VIEW_IF_VERSION, FALSE);
   g_return_val_if_fail (plugin->label, FALSE);

   if (!draw_area_list)
      draw_area_list = g_list_append (draw_area_list,
                                      &imageview_draw_vfunc_table);

   draw_area_list = g_list_append (draw_area_list, plugin);
   draw_area_list = g_list_sort (draw_area_list,
                                 (GCompareFunc) comp_func_priority);

   return TRUE;
}


GList *
gimv_image_view_plugin_get_list (void)
{
   if (!draw_area_list)
      draw_area_list = g_list_append (draw_area_list,
                                      &imageview_draw_vfunc_table);
   return draw_area_list;
}


/****************************************************************************
 *
 *
 *
 ****************************************************************************/
G_DEFINE_TYPE (GimvImageView, gimv_image_view, GTK_TYPE_BOX)


enum {
  ARG_0,
  ARG_X_SCALE,
  ARG_Y_SCALE,
  ARG_ORIENTATION,
  ARG_DEFAULT_ZOOM,
  ARG_DEFAULT_ROTATION,
  ARG_KEEP_ASPECT,
  ARG_KEEP_BUFFER,
  ARG_IGNORE_ALPHA,
  ARG_SHOW_SCROLLBAR,
  ARG_CONTINUANCE_PLAY,
};


/* FIXME!! */
static void
gimv_image_view_set_property (GObject      *object,
                              guint         arg_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
   GimvImageView *iv = GIMV_IMAGE_VIEW(object);

   if (!iv->priv) return;

   switch (arg_id) {
   case ARG_X_SCALE:
      iv->priv->x_scale = g_value_get_float (value);
      break;
   case ARG_Y_SCALE:
      iv->priv->y_scale = g_value_get_float (value);
      break;
   case ARG_ORIENTATION:
      iv->priv->rotate = g_value_get_int (value);
      break;
   case ARG_DEFAULT_ZOOM:
      iv->priv->default_zoom = g_value_get_int (value);
      break;
   case ARG_DEFAULT_ROTATION:
      iv->priv->default_rotation = g_value_get_int (value);
      break;
   case ARG_KEEP_ASPECT:
      iv->priv->keep_aspect = g_value_get_boolean (value);
      break;
   case ARG_KEEP_BUFFER:
      iv->priv->buffer = g_value_get_boolean (value);
      break;
   case ARG_IGNORE_ALPHA:
      iv->priv->ignore_alpha = g_value_get_boolean (value);
      break;
   case ARG_SHOW_SCROLLBAR:
      iv->priv->show_scrollbar = g_value_get_boolean (value);
      break;
   case ARG_CONTINUANCE_PLAY:
      iv->priv->continuance_play = g_value_get_boolean (value);
      break;
   default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, arg_id, pspec);
      break;
   }
}

static void
gimv_image_view_get_property (GObject    *object,
                              guint       arg_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
   GimvImageView *iv = GIMV_IMAGE_VIEW(object);

   if (!iv->priv) return;

   switch (arg_id) {
   case ARG_X_SCALE:
      g_value_set_float (value, iv->priv->x_scale);
      break;
   case ARG_Y_SCALE:
      g_value_set_float (value, iv->priv->y_scale);
      break;
   case ARG_ORIENTATION:
      g_value_set_int (value, iv->priv->rotate);
      break;
   case ARG_DEFAULT_ZOOM:
      g_value_set_int (value, iv->priv->default_zoom);
      break;
   case ARG_DEFAULT_ROTATION:
      g_value_set_int (value, iv->priv->default_rotation);
      break;
   case ARG_KEEP_ASPECT:
      g_value_set_boolean (value, iv->priv->keep_aspect);
      break;
   case ARG_KEEP_BUFFER:
      g_value_set_boolean (value, iv->priv->buffer);
      break;
   case ARG_IGNORE_ALPHA:
      g_value_set_boolean (value, iv->priv->ignore_alpha);
      break;
   case ARG_SHOW_SCROLLBAR:
      g_value_set_boolean (value, iv->priv->show_scrollbar);
      break;
   case ARG_CONTINUANCE_PLAY:
      g_value_set_boolean (value, iv->priv->continuance_play);
      break;
   default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, arg_id, pspec);
      break;
   }
}


static void
gimv_image_view_class_init (GimvImageViewClass *klass)
{
   GObjectClass *gobject_class;

   gobject_class = G_OBJECT_CLASS (klass);
   parent_class = g_type_class_peek_parent (klass);

   gobject_class->set_property = gimv_image_view_set_property;
   gobject_class->get_property = gimv_image_view_get_property;
   gobject_class->dispose      = gimv_image_view_dispose;
   gobject_class->finalize     = gimv_image_view_finalize;

   g_object_class_install_property (gobject_class, ARG_X_SCALE,
      g_param_spec_float ("x_scale", NULL, NULL,
                          0.0, G_MAXFLOAT, 100.0, G_PARAM_READWRITE));
   g_object_class_install_property (gobject_class, ARG_Y_SCALE,
      g_param_spec_float ("y_scale", NULL, NULL,
                          0.0, G_MAXFLOAT, 100.0, G_PARAM_READWRITE));
   /*
    * GTK4: GtkBox implements GtkOrientable, so the GTK2 "orientation"
    * argument would clash with GtkOrientable:orientation.  It is called
    * "rotation" now.
    */
   g_object_class_install_property (gobject_class, ARG_ORIENTATION,
      g_param_spec_int ("rotation", NULL, NULL,
                        G_MININT, G_MAXINT, 0, G_PARAM_READWRITE));
   g_object_class_install_property (gobject_class, ARG_DEFAULT_ZOOM,
      g_param_spec_int ("default_zoom", NULL, NULL,
                        G_MININT, G_MAXINT, 0, G_PARAM_READWRITE));
   g_object_class_install_property (gobject_class, ARG_DEFAULT_ROTATION,
      g_param_spec_int ("default_rotation", NULL, NULL,
                        G_MININT, G_MAXINT, 0, G_PARAM_READWRITE));
   g_object_class_install_property (gobject_class, ARG_KEEP_ASPECT,
      g_param_spec_boolean ("keep_aspect", NULL, NULL,
                            FALSE, G_PARAM_READWRITE));
   g_object_class_install_property (gobject_class, ARG_KEEP_BUFFER,
      g_param_spec_boolean ("keep_buffer", NULL, NULL,
                            FALSE, G_PARAM_READWRITE));
   g_object_class_install_property (gobject_class, ARG_IGNORE_ALPHA,
      g_param_spec_boolean ("ignore_alpha", NULL, NULL,
                            FALSE, G_PARAM_READWRITE));
   g_object_class_install_property (gobject_class, ARG_SHOW_SCROLLBAR,
      g_param_spec_boolean ("show_scrollbar", NULL, NULL,
                            FALSE, G_PARAM_READWRITE));
   g_object_class_install_property (gobject_class, ARG_CONTINUANCE_PLAY,
      g_param_spec_boolean ("continuance_play", NULL, NULL,
                            FALSE, G_PARAM_READWRITE));

   gimv_image_view_signals[IMAGE_CHANGED_SIGNAL]
      = g_signal_new ("image_changed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvImageViewClass, image_changed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

   gimv_image_view_signals[LOAD_START_SIGNAL]
      = g_signal_new ("load_start",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvImageViewClass, load_start),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__POINTER,
                      G_TYPE_NONE, 1, G_TYPE_POINTER);

   gimv_image_view_signals[LOAD_END_SIGNAL]
      = g_signal_new ("load_end",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvImageViewClass, load_end),
                      NULL, NULL,
                      g_cclosure_marshal_generic,
                      G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_INT);

   gimv_image_view_signals[SET_LIST_SIGNAL]
      = g_signal_new ("set_list",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvImageViewClass, set_list),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

   gimv_image_view_signals[UNSET_LIST_SIGNAL]
      = g_signal_new ("unset_list",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvImageViewClass, unset_list),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

   gimv_image_view_signals[RENDERED_SIGNAL]
      = g_signal_new ("rendered",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvImageViewClass, rendered),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

   gimv_image_view_signals[TOGGLE_ASPECT_SIGNAL]
      = g_signal_new ("toggle_aspect",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvImageViewClass, toggle_aspect),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__INT,
                      G_TYPE_NONE, 1, G_TYPE_INT);

   gimv_image_view_signals[TOGGLE_BUFFER_SIGNAL]
      = g_signal_new ("toggle_buffer",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvImageViewClass, toggle_buffer),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__INT,
                      G_TYPE_NONE, 1, G_TYPE_INT);

   gimv_image_view_signals[THUMBNAIL_CREATED_SIGNAL]
      = g_signal_new ("thumbnail_created",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvImageViewClass, thumbnail_created),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__POINTER,
                      G_TYPE_NONE, 1, G_TYPE_POINTER);

   gimv_image_view_signals[IMAGE_PRESSED_SIGNAL]
      = g_signal_new ("image_pressed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (GimvImageViewClass, image_pressed),
                      NULL, NULL,
                      g_cclosure_marshal_generic,
                      G_TYPE_BOOLEAN, 1, G_TYPE_POINTER);

   gimv_image_view_signals[IMAGE_RELEASED_SIGNAL]
      = g_signal_new ("image_released",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (GimvImageViewClass, image_released),
                      NULL, NULL,
                      g_cclosure_marshal_generic,
                      G_TYPE_BOOLEAN, 1, G_TYPE_POINTER);

   gimv_image_view_signals[IMAGE_CLICKED_SIGNAL]
      = g_signal_new ("image_clicked",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (GimvImageViewClass, image_clicked),
                      NULL, NULL,
                      g_cclosure_marshal_generic,
                      G_TYPE_BOOLEAN, 1, G_TYPE_POINTER);

   klass->image_changed     = gimv_image_view_image_changed;
   klass->load_start        = NULL;
   klass->load_end          = NULL;
   klass->set_list          = NULL;
   klass->unset_list        = NULL;
   klass->rendered          = NULL;
   klass->toggle_aspect     = NULL;
   klass->toggle_buffer     = NULL;
   klass->thumbnail_created = NULL;
}


static void
gimv_image_view_init (GimvImageView *iv)
{
   GtkWidget *player, *event_box;

   /* initialize struct data based on config */
   iv->loader     = NULL;
   iv->image      = NULL;

   iv->hadj = iv->vadj  = NULL;

   iv->draw_area        = NULL;
   iv->draw_area_funcs  = NULL;

   iv->progressbar      = NULL;

   iv->bg_color         = NULL;

   iv->cursor           = NULL;
   iv->imageview_popup  = NULL;
   iv->zoom_menu        = NULL;
   iv->rotate_menu      = NULL;
   iv->movie_menu       = NULL;
   iv->view_modes_menu  = NULL;

   /* init private data */
   iv->priv = g_new0(GimvImageViewPrivate, 1);

   iv->priv->navwin     = NULL;

   iv->priv->pixmap     = NULL;
   iv->priv->mask       = NULL;

   iv->priv->x_pos      = 0;
   iv->priv->y_pos      = 0;
   iv->priv->width      = 0;
   iv->priv->height     = 0;

   iv->priv->show_scrollbar   = FALSE;
   iv->priv->default_zoom     = conf.imgview_default_zoom;
   iv->priv->default_rotation = conf.imgview_default_rotation;
   iv->priv->fit_to_frame     = 0;

   iv->priv->x_scale          = conf.imgview_scale;
   iv->priv->y_scale          = conf.imgview_scale;
   iv->priv->rotate     = 0;
   iv->priv->keep_aspect      = conf.imgview_keep_aspect;
   iv->priv->ignore_alpha     = FALSE;
   iv->priv->alpha_checker    = conf.imgview_alpha_checker;

   iv->priv->buffer           = conf.imgview_buffer;

   iv->priv->show_scrollbar   = conf.imgview_scrollbar;
   iv->priv->continuance_play = conf.imgview_movie_continuance;

   iv->priv->player_flags     = 0;
   iv->priv->player_visible   = GimvImageViewPlayerVisibleAuto;

   /* list */
   iv->priv->image_list = NULL;

   iv->priv->load_end_signal_id               = 0;
   iv->priv->loader_progress_update_signal_id = 0;
   iv->priv->loader_load_end_signal_id        = 0;

   iv->priv->fullscreen = NULL;

   iv->priv->saved_cursor = NULL;
   iv->priv->scroll_acc_x = 0.0;
   iv->priv->scroll_acc_y = 0.0;

   /* GTK2 GimvImageView was a GtkVBox */
   gtk_orientable_set_orientation (GTK_ORIENTABLE (iv),
                                   GTK_ORIENTATION_VERTICAL);

   /* create widgets */
   iv->loader = gimv_image_loader_new ();

   iv->table = gimv_table_new (2, 2, FALSE);
   gtk_widget_show (iv->table);
   /* GTK4: the navigator is shown in an overlay (see gimv_nav_win.h) */
   iv->priv->overlay = gtk_overlay_new ();
   gtk_overlay_set_child (GTK_OVERLAY (iv->priv->overlay), iv->table);
   gimv_box_pack_start (GTK_BOX (iv), iv->priv->overlay, TRUE, TRUE, 0);

   player = gimv_image_view_create_player_toolbar (iv);
   gimv_box_pack_start (GTK_BOX (iv), player, FALSE, FALSE, 2);
   /* GTK2: the player was not shown until a playable image was loaded */
   gtk_widget_set_visible (player, FALSE);

   gimv_image_view_change_draw_widget (iv, 0);

   iv->hadj = GTK_ADJUSTMENT (gtk_adjustment_new (0.0, 0.0, 0.0, 10.0, 10.0, 0.0));
   iv->hscrollbar = gtk_scrollbar_new (GTK_ORIENTATION_HORIZONTAL, iv->hadj);
   gtk_widget_show (iv->hscrollbar);

   iv->vadj = GTK_ADJUSTMENT (gtk_adjustment_new (0.0, 0.0, 0.0, 10.0, 10.0, 0.0));
   iv->vscrollbar = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL, iv->vadj);
   gtk_widget_show (iv->vscrollbar);

   event_box = gimv_event_box_new ();
   gtk_widget_set_name (event_box, "NavWinButton");
   gtk_widget_show (event_box);

   iv->nav_button = gimv_icon_stock_get_widget ("nav-button");
   gimv_container_add (GTK_WIDGET (event_box), iv->nav_button);
   gtk_widget_show (iv->nav_button);

   gimv_table_attach (GTK_WIDGET (iv->table), iv->vscrollbar, 1, 2, 0, 1, GIMV_FILL, GIMV_FILL, 0, 0);
   gimv_table_attach (GTK_WIDGET (iv->table), iv->hscrollbar, 0, 1, 1, 2, GIMV_FILL, GIMV_FILL, 0, 0);
   gimv_table_attach (GTK_WIDGET (iv->table), event_box, 1, 2, 1, 2, GIMV_FILL, GIMV_FILL, 0, 0);

   /* set signals */
   g_signal_connect (G_OBJECT (iv->hadj), "value_changed",
                       G_CALLBACK (cb_scrollbar_value_changed), iv);

   g_signal_connect (G_OBJECT (iv->vadj), "value_changed",
                       G_CALLBACK (cb_scrollbar_value_changed), iv);

   /* GTK4: press, drag and release on the button operate the navigator
      (the pointer stays grabbed by the button, see gimv_nav_win.h) */
   {
      GtkGesture *drag = gtk_gesture_drag_new ();
      gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), 0);
      g_signal_connect (drag, "drag-begin",
                        G_CALLBACK (cb_nav_button_drag_begin), iv);
      g_signal_connect (drag, "drag-update",
                        G_CALLBACK (cb_nav_button_drag_update), iv);
      g_signal_connect (drag, "drag-end",
                        G_CALLBACK (cb_nav_button_drag_end), iv);
      gtk_widget_add_controller (event_box, GTK_EVENT_CONTROLLER (drag));
   }

   /* add to list */
   GimvImageViewList = g_list_append (GimvImageViewList, iv);

   /* GTK4: scrollbars are shown only while the image does not fit (GTK2:
      always), see update_scrollbar_visibility() */
   gtk_widget_set_visible (iv->hscrollbar, FALSE);
   gtk_widget_set_visible (iv->vscrollbar, FALSE);
   gtk_widget_set_visible (iv->nav_button, FALSE);
}


GtkWidget*
gimv_image_view_new (GimvImageInfo *info)
{
   GimvImageView *iv = g_object_new (GIMV_TYPE_IMAGE_VIEW, NULL);

   if (info) {
      iv->info = gimv_image_info_ref (info);
   } else {
      iv->info = NULL;
   }

   /* load image */
   if (iv->info)
      g_idle_add (idle_gimv_image_view_change_image_info, iv);

   return GTK_WIDGET (iv);
}


static void
gimv_image_view_dispose (GObject *object)
{
   GimvImageView *iv = GIMV_IMAGE_VIEW (object);

   /* remove from list */
   GimvImageViewList = g_list_remove (GimvImageViewList, iv);

   /* GTK4: the menus are released with their window, which may happen
      after the image view has gone */
   {
      GtkWidget **menus[] = {
         &iv->imageview_popup, &iv->zoom_menu, &iv->rotate_menu,
         &iv->movie_menu, &iv->view_modes_menu,
      };
      guint i;
      for (i = 0; i < G_N_ELEMENTS (menus); i++) {
         /* non-NULL (weak pointer): the menu still exists */
         if (*menus[i])
            g_signal_handlers_disconnect_by_data (*menus[i], iv);
         gimv_image_view_set_menu_ptr (iv, menus[i], NULL);
      }
   }

   if (GTK_IS_WIDGET (iv->draw_area)) {
      GtkWidget *draw_area = g_object_ref (iv->draw_area);
      gimv_widget_destroy (draw_area);
      /* GTK4: the widget may still be referenced (e.g. by the event being
         dispatched); make sure its "destroy" handlers run while the image
         view still exists */
      g_object_run_dispose (G_OBJECT (draw_area));
      g_object_unref (draw_area);
   }
   iv->draw_area = NULL;

   if (iv->loader) {
      if (gimv_image_loader_is_loading (iv->loader)) {
         gimv_image_view_cancel_loading (iv);
         g_signal_connect (G_OBJECT (iv->loader), "load_end",
                             G_CALLBACK (cb_destroy_loader),
                             iv);
      } else {
         gimv_image_loader_unref (iv->loader);
      }
   }
   iv->loader = NULL;

   if (iv->image)
      gimv_image_unref (iv->image);
   iv->image = NULL;

   if (iv->bg_color)
      g_free (iv->bg_color);
   iv->bg_color = NULL;

   if (iv->cursor)
      g_object_unref (iv->cursor);
   iv->cursor = NULL;

   if (iv->info)
      gimv_image_info_unref (iv->info);
   iv->info = NULL;

   if (iv->priv) {
      if (iv->priv->navwin) {
         GtkWidget *parent = gtk_widget_get_parent (iv->priv->navwin);
         if (GTK_IS_OVERLAY (parent))
            gtk_overlay_remove_overlay (GTK_OVERLAY (parent), iv->priv->navwin);
         iv->priv->navwin = NULL;
      }

      if (iv->priv->pixmap) {
         gimv_image_free_pixmap_and_mask (iv->priv->pixmap, iv->priv->mask);
         iv->priv->pixmap = NULL;
         iv->priv->mask = NULL;
      }

      if (iv->priv->image_list)
         gimv_image_view_remove_list (iv, iv->priv->image_list->owner);
      iv->priv->image_list = NULL;

      if (iv->priv->saved_cursor)
         g_object_unref (iv->priv->saved_cursor);
      iv->priv->saved_cursor = NULL;
   }

   G_OBJECT_CLASS (parent_class)->dispose (object);
}


static void
gimv_image_view_finalize (GObject *object)
{
   GimvImageView *iv = GIMV_IMAGE_VIEW (object);

   /* private data is freed here, callbacks may still run while disposing */
   g_free (iv->priv);
   iv->priv = NULL;

   G_OBJECT_CLASS (parent_class)->finalize (object);
}


static void
gimv_image_view_image_changed (GimvImageView *iv)
{
   GimvImageViewPlayableIF *playable;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (!iv->draw_area_funcs) return;

   playable = iv->draw_area_funcs->playable;
   if (!playable) return;
   if (!playable->is_playable_fn) return;

   if (gimv_image_view_is_playable (iv)) {
      GimvImageViewPlayableStatus status;

      if (iv->priv->player_visible == GimvImageViewPlayerVisibleAuto)
         gtk_widget_show (iv->player_container);
      status = gimv_image_view_playable_get_status (iv);
      gimv_image_view_playable_set_status (iv, status);

   } else {
      if (iv->priv->player_visible == GimvImageViewPlayerVisibleAuto)
         gtk_widget_hide (iv->player_container);
      gimv_image_view_playable_set_status (iv, GimvImageViewPlayableDisable);
   }
}



/*****************************************************************************
 *
 *   Callback functions for reference popup menu.
 *
 *****************************************************************************/
static void
cb_alpha_checker (GimvImageView *iv, guint action, GimvMenuItem *widget)
{
   gboolean active = gimv_menu_item_get_active (GIMV_MENU_ITEM (widget));

   conf.imgview_alpha_checker = active;   /* the default for new views too */
   gimv_image_view_set_alpha_checker (iv, active);
}


static void
cb_ignore_alpha (GimvImageView *iv, guint action, GimvMenuItem *widget)
{
   iv->priv->ignore_alpha = gimv_menu_item_get_active (GIMV_MENU_ITEM (widget));
   gimv_image_view_show_image (iv);
}


static void
cb_keep_aspect (GimvImageView *iv, guint action, GimvMenuItem *widget)
{
   iv->priv->keep_aspect = gimv_menu_item_get_active (GIMV_MENU_ITEM (widget));

   g_signal_emit (G_OBJECT (iv),
                  gimv_image_view_signals[TOGGLE_ASPECT_SIGNAL], 0,
                  iv->priv->keep_aspect);
}


static void
cb_zoom (GimvImageView *iv, GimvImageViewZoomType zoom, GimvMenuItem *widget)
{
   gimv_image_view_zoom_image (iv, zoom, 0, 0);
}


static void
cb_rotate (GimvImageView *iv, guint rotate, GimvMenuItem *widget)
{
   guint angle;

   /* convert to absolute angle */
   angle = (iv->priv->rotate + rotate) % 4;

   gimv_image_view_rotate_image (iv, angle);
}


static void
cb_toggle_scrollbar (GimvImageView *iv, guint action, GimvMenuItem *widget)
{
   if (gimv_menu_item_get_active (GIMV_MENU_ITEM (widget))) {
      gimv_image_view_show_scrollbar (iv);
   } else {
      gimv_image_view_hide_scrollbar (iv);
   }
}


static void
cb_create_thumbnail (GimvImageView *iv, guint action, GimvMenuItem *widget)
{
   g_return_if_fail (iv);

   gimv_image_view_create_thumbnail (iv);
}


static void
cb_print (GimvImageView *iv, guint action, GimvMenuItem *widget)
{
   GtkRoot *root;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   root = gtk_widget_get_root (GTK_WIDGET (iv));
   gimv_print_image_view (iv, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL);
}


static void
cb_toggle_buffer (GimvImageView *iv, guint action, GimvMenuItem *widget)
{
   if (gimv_menu_item_get_active (GIMV_MENU_ITEM (widget))) {
      gimv_image_view_load_image_buf (iv);
      iv->priv->buffer = TRUE;
   }
   else {
      iv->priv->buffer = FALSE;
      gimv_image_view_free_image_buf (iv);
   }

   g_signal_emit (G_OBJECT (iv),
                  gimv_image_view_signals[TOGGLE_BUFFER_SIGNAL], 0,
                  iv->priv->buffer);
}



/*****************************************************************************
 *
 *   callback functions for movie menu.
 *
 *****************************************************************************/
static void
cb_movie_menu (GimvImageView *iv,
               GimvImageViewMovieMenu operation,
               GimvMenuItem *widget)
{
   g_return_if_fail (iv);

   switch (operation) {
   case MOVIE_PLAY:
      gimv_image_view_playable_play (iv);
      break;
   case MOVIE_STOP:
      gimv_image_view_playable_stop (iv);
      break;
   case MOVIE_PAUSE:
      gimv_image_view_playable_pause (iv);
      break;
   case MOVIE_FORWARD:
      gimv_image_view_playable_forward (iv);
      break;
   case MOVIE_REVERSE:
      gimv_image_view_playable_reverse (iv);
      break;
   case MOVIE_EJECT:
      gimv_image_view_playable_eject (iv);
      break;
   default:
      break;
   }
}




static void
cb_continuance (GimvImageView *iv, guint action, GimvMenuItem *widget)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));
   g_return_if_fail (GIMV_IS_MENU_ITEM (widget));

   iv->priv->continuance_play = gimv_menu_item_get_active (GIMV_MENU_ITEM (widget));
}


static void
cb_change_view_mode (GimvMenuItem *item, gpointer data)
{
   GimvImageView *iv = data;
   const gchar *label;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   label = g_object_get_data (G_OBJECT (item), "GimvImageView::ViewMode");
   gimv_image_view_change_view_mode (iv, label);
}



/*****************************************************************************
 *
 *   callback functions for player toolbar.
 *
 *****************************************************************************/
static void
cb_gimv_image_view_play (GtkButton *button, GimvImageView *iv)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   gimv_image_view_playable_play (iv);
}


static void
cb_gimv_image_view_stop (GtkButton *button, GimvImageView *iv)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   gimv_image_view_playable_stop (iv);
}


static void
cb_gimv_image_view_fw (GtkButton *button, GimvImageView *iv)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   gimv_image_view_playable_forward (iv);
}


static void
cb_gimv_image_view_eject (GtkButton *button, GimvImageView *iv)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   gimv_image_view_playable_eject (iv);
}


static void
cb_gimv_image_view_rw (GtkButton *button, GimvImageView *iv)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   gimv_image_view_playable_reverse (iv);
}


static gboolean
cb_seekbar_pressed (GtkWidget *widget,
                    GimvEventButton *event,
                    GimvImageView *iv)
{
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);

   iv->priv->player_flags |= GimvImageViewSeekBarDraggingFlag;

   return FALSE;
}


static gboolean
cb_seekbar_released (GtkWidget *widget,
                     GimvEventButton *event,
                     GimvImageView *iv)
{
   GtkAdjustment *adj;
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);

   adj = gtk_range_get_adjustment (GTK_RANGE (iv->player.seekbar));
   gimv_image_view_playable_seek (iv, gtk_adjustment_get_value (adj));

   iv->priv->player_flags &= ~GimvImageViewSeekBarDraggingFlag;

   return FALSE;
}



/*****************************************************************************
 *
 *   other callback functions.
 *
 *****************************************************************************/
static void
cb_destroy_loader (GimvImageLoader *loader, gpointer data)
{
   g_signal_handlers_disconnect_by_func (G_OBJECT (loader),
                                  G_CALLBACK (cb_destroy_loader),
                                  data);
   gimv_image_loader_unref (loader);
}


static void
cb_image_map (GtkWidget *widget, GimvImageView *iv)
{
   g_signal_handlers_disconnect_by_func (G_OBJECT (widget),
                                  G_CALLBACK (cb_image_map), iv);
   gimv_image_view_show_image (iv);
   gimv_image_view_playable_play (iv);
   g_signal_emit (G_OBJECT (iv),
                  gimv_image_view_signals[IMAGE_CHANGED_SIGNAL], 0);
}


static gboolean
cb_image_key_press (GtkWidget *widget, GimvEventKey *event, GimvImageView *iv)
{
   guint keyval, popup_key;
   GdkModifierType modval, popup_mod;
   gboolean move = FALSE;
   gint mx, my;

   g_return_val_if_fail (iv, FALSE);

   gimv_image_view_get_view_position (iv, &mx, &my);

   keyval = event->keyval;
   modval = event->state;

   popup_key = 0;
   popup_mod = 0;
   if (akey.common_popup_menu && *akey.common_popup_menu)
      gtk_accelerator_parse (akey.common_popup_menu, &popup_key, &popup_mod);

   if (popup_key && keyval == popup_key
       && (!popup_mod || (modval & popup_mod)))
   {
      gimv_image_view_popup_menu (iv, NULL);
   } else {
      switch (keyval) {
      case GDK_KEY_Left:
      case GDK_KEY_KP_Left:
      case GDK_KEY_KP_4:
         mx -= 20;
         move = TRUE;
         break;
      case GDK_KEY_Right:
      case GDK_KEY_KP_Right:
      case GDK_KEY_KP_6:
         mx += 20;
         move = TRUE;
         break;
      case GDK_KEY_Up:
      case GDK_KEY_KP_Up:
      case GDK_KEY_KP_8:
         my -= 20;
         move = TRUE;
         break;
      case GDK_KEY_Down:
      case GDK_KEY_KP_Down:
      case GDK_KEY_KP_2:
         my += 20;
         move = TRUE;
         break;
      case GDK_KEY_KP_5:
         mx = 0;
         my = 0;
         move = TRUE;
         break;
      case GDK_KEY_Page_Up:
      case GDK_KEY_KP_Page_Up:
      case GDK_KEY_KP_9:
         gimv_image_view_prev (iv);
         return TRUE;
      case GDK_KEY_space:
      case GDK_KEY_Page_Down:
      case GDK_KEY_KP_Space:
      case GDK_KEY_KP_Page_Down:
      case GDK_KEY_KP_3:
      case GDK_KEY_KP_0:
         gimv_image_view_next (iv);
         return TRUE;
      case GDK_KEY_Home:
      case GDK_KEY_KP_Home:
      case GDK_KEY_KP_7:
        gimv_image_view_nth (iv, 0);
         return TRUE;
      case GDK_KEY_End:
      case GDK_KEY_KP_End:
      case GDK_KEY_KP_1:
      {
         gint last;
         if (iv->priv->image_list) {
            last = g_list_length (iv->priv->image_list->list) - 1;
            if (last > 0)
               gimv_image_view_nth (iv, last);
         }
         return TRUE;
      }
      case GDK_KEY_KP_Add:
      case GDK_KEY_plus:
         gimv_image_view_zoom_image (iv, GIMV_IMAGE_VIEW_ZOOM_IN, 0, 0);
         return FALSE;
         break;
      case GDK_KEY_KP_Subtract:
      case GDK_KEY_minus:
         gimv_image_view_zoom_image (iv, GIMV_IMAGE_VIEW_ZOOM_OUT, 0, 0);
         return FALSE;
         break;
      case GDK_KEY_equal:
      case GDK_KEY_KP_Equal:
      case GDK_KEY_KP_Enter:
         gimv_image_view_zoom_image (iv, GIMV_IMAGE_VIEW_ZOOM_100, 0, 0);
         return FALSE;
         break;
      case GDK_KEY_KP_Divide:
         gimv_image_view_zoom_image (iv, GIMV_IMAGE_VIEW_ZOOM_FIT, 0, 0);
         return FALSE;
         break;
      }
   }

   if (move) {
      gimv_image_view_moveto (iv, mx, my);
      return TRUE;
   }

   return FALSE;
}


static gboolean
cb_image_button_press (GtkWidget *widget, GimvEventButton *event,
                       GimvImageView *iv)
{
   GdkCursor *cursor;
   gboolean retval = FALSE;

   g_return_val_if_fail (iv, FALSE);

   iv->priv->pressed = TRUE;
   iv->priv->button  = event->button;
   iv->priv->press_x = event->x;
   iv->priv->press_y = event->y;

   gtk_widget_grab_focus (widget);

   g_signal_emit (G_OBJECT (iv),
                  gimv_image_view_signals[IMAGE_PRESSED_SIGNAL], 0,
                  event, &retval);

   if (iv->priv->dragging)
      return FALSE;

   if (event->button == 1) {   /* scroll image */
      if (!iv->priv->pixmap)
         return FALSE;

      /*
       * GTK4: there is no explicit pointer grab any more; the button press
       * gives the widget an implicit grab.  Only change the cursor.
       */
      if (iv->priv->saved_cursor)
         g_object_unref (iv->priv->saved_cursor);
      iv->priv->saved_cursor = gtk_widget_get_cursor (widget);
      if (iv->priv->saved_cursor)
         g_object_ref (iv->priv->saved_cursor);
      cursor = cursor_get (widget, CURSOR_HAND_CLOSED);
      gtk_widget_set_cursor (widget, cursor);
      g_object_unref (cursor);

      iv->priv->drag_startx = event->x - iv->priv->x_pos;
      iv->priv->drag_starty = event->y - iv->priv->y_pos;
      iv->priv->x_pos_drag_start = iv->priv->x_pos;
      iv->priv->y_pos_drag_start = iv->priv->y_pos;

      return TRUE;

   }

   return FALSE;
}


static gboolean
cb_image_button_release  (GtkWidget *widget, GimvEventButton *event,
                          GimvImageView *iv)
{
   gboolean retval = FALSE;
   gboolean drag_scroll;

   /*
   if (event->button != 1)
      return FALSE;
   */

   drag_scroll = (iv->priv->pressed && iv->priv->button == 1);

   g_signal_emit (G_OBJECT (iv),
                  gimv_image_view_signals[IMAGE_RELEASED_SIGNAL], 0,
                  event, &retval);

   /* the handlers may have destroyed the widget */
   if (!iv->priv) return retval;

   if(iv->priv->pressed && !iv->priv->dragging)
      g_signal_emit (G_OBJECT (iv),
                     gimv_image_view_signals[IMAGE_CLICKED_SIGNAL], 0,
                     event, &retval);

   if (!iv->priv) return retval;

   iv->priv->button   = 0;
   iv->priv->pressed  = FALSE;
   iv->priv->dragging = FALSE;

   /* GTK4: replaces gdk_pointer_ungrab (): restore the cursor */
   if (drag_scroll && GTK_IS_WIDGET (widget)) {
      gtk_widget_set_cursor (widget, iv->priv->saved_cursor);
   }
   if (iv->priv->saved_cursor)
      g_object_unref (iv->priv->saved_cursor);
   iv->priv->saved_cursor = NULL;

   return retval;
}


static gboolean
cb_image_motion_notify (GtkWidget *widget, GimvEventMotion *event,
                        GimvImageView *iv)
{
   gint x, y, x_pos, y_pos, dx, dy;

   if (!iv->priv->pressed)
      return FALSE;

   x = event->x;
   y = event->y;

   x_pos = x - iv->priv->drag_startx;
   y_pos = y - iv->priv->drag_starty;

   /* a drag (not a click) once the pointer left the press position.  The
      drag_start* values are only set for button 1 on an image, so they
      can't be used for this: with them a middle click on a movie counted
      as a drag and "image_clicked" (e.g. the popup menu) never came. */
   dx = x - iv->priv->press_x;
   dy = y - iv->priv->press_y;
   if (!iv->priv->dragging && (abs (dx) > 2 || abs (dy) > 2))
      iv->priv->dragging = TRUE;

   /* scroll image */
   if (iv->priv->button == 1) {
      iv->priv->x_pos = x_pos;
      iv->priv->y_pos = y_pos;

      gimv_image_view_adjust_pos_in_frame (iv, FALSE);
   }

   gimv_image_view_draw_image (iv);

   return TRUE;
}


/*
 *  GTK2 (gtk2-compat) translated scroll events into button 4-7 press
 *  events, so the "image_pressed" handlers see the wheel as buttons.
 */
static gboolean
cb_image_scroll (GtkWidget *widget, GimvEventScroll *event,
                 GimvImageView *iv)
{
   GimvEventButton be;
   gdouble sx = 0.0, sy = 0.0;
   gboolean retval = FALSE;

   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);
   if (!iv->priv) return FALSE;

   memset (&be, 0, sizeof (be));

   /* smooth scrolling (touchpads): emit one "button" per scroll unit */
   if (event->event
       && gdk_scroll_event_get_direction (event->event) == GDK_SCROLL_SMOOTH)
   {
      iv->priv->scroll_acc_x += event->delta_x;
      iv->priv->scroll_acc_y += event->delta_y;
      if (iv->priv->scroll_acc_y <= -1.0) {
         be.button = 4;
         iv->priv->scroll_acc_y = 0.0;
      } else if (iv->priv->scroll_acc_y >= 1.0) {
         be.button = 5;
         iv->priv->scroll_acc_y = 0.0;
      } else if (iv->priv->scroll_acc_x <= -1.0) {
         be.button = 6;
         iv->priv->scroll_acc_x = 0.0;
      } else if (iv->priv->scroll_acc_x >= 1.0) {
         be.button = 7;
         iv->priv->scroll_acc_x = 0.0;
      } else {
         return TRUE;
      }
   } else {
      switch (event->direction) {
      case GDK_SCROLL_UP:
         be.button = 4;
         break;
      case GDK_SCROLL_DOWN:
         be.button = 5;
         break;
      case GDK_SCROLL_LEFT:
         be.button = 6;
         break;
      case GDK_SCROLL_RIGHT:
         be.button = 7;
         break;
      default:
         return FALSE;
      }
   }

   be.type  = GIMV_BUTTON_PRESS;
   be.time  = event->time;
   be.x     = event->x;
   be.y     = event->y;
   be.state = event->state;
   be.event = event->event;
   be.controller = event->controller;
   if (event->event && gdk_event_get_position (event->event, &sx, &sy)) {
      be.x_root = sx;
      be.y_root = sy;
   } else {
      be.x_root = event->x;
      be.y_root = event->y;
   }

   retval = cb_image_button_press (widget, &be, iv);

   /* a wheel "button" is never released */
   if (iv->priv && iv->priv->button == be.button) {
      iv->priv->pressed = FALSE;
      iv->priv->button  = 0;
   }

   return retval;
}


static void
cb_drag_data_received (GtkWidget *widget,
                       GimvDragContext *context,
                       gint x, gint y,
                       GimvSelectionData *seldata,
                       guint info,
                       guint32 time,
                       gpointer data)
{
   GimvImageView *iv = data;
   FilesLoader *files;
   GList *filelist;
   gchar *tmpstr;

   g_return_if_fail (iv || widget);

   filelist = dnd_get_file_list (seldata->data, seldata->length);

   if (filelist) {
      GimvImageInfo *info = gimv_image_info_get ((gchar *) filelist->data);

      if (!info) goto ERROR;

      gimv_image_view_change_image (iv, info);
      tmpstr = filelist->data;
      filelist = g_list_remove (filelist, tmpstr);
      g_free (tmpstr);

      if (filelist) {
         files = files_loader_new ();
         files->filelist = filelist;
         open_image_files_in_image_view (files);
         files->filelist = NULL;
         files_loader_delete (files);
      }
   }

ERROR:
   g_list_foreach (filelist, (GFunc) g_free, NULL);
   g_list_free (filelist);
}


static void
cb_scrollbar_value_changed (GtkAdjustment *adj, GimvImageView *iv)
{
   g_return_if_fail (iv);

   if (!move_scrollbar_by_user) return;

   if (iv->priv->width > gtk_widget_get_width (GTK_WIDGET (iv->draw_area)))
      iv->priv->x_pos = 0 - gtk_adjustment_get_value (iv->hadj);
   if (iv->priv->height > gtk_widget_get_height (GTK_WIDGET (iv->draw_area)))
      iv->priv->y_pos = 0 - gtk_adjustment_get_value (iv->vadj);

   gimv_image_view_draw_image (iv);
}


/* GTK4: widget coordinates -> coordinates of the overlay */
static gboolean
nav_button_to_overlay (GimvImageView *iv, GtkWidget *widget,
                       gdouble x, gdouble y, gdouble *ox, gdouble *oy)
{
   graphene_point_t p, q;

   graphene_point_init (&p, x, y);
   if (!gtk_widget_compute_point (widget, iv->priv->overlay, &p, &q))
      return FALSE;
   *ox = q.x;
   *oy = q.y;
   return TRUE;
}


static void
cb_nav_button_drag_begin (GtkGestureDrag *gesture,
                          gdouble x, gdouble y,
                          GimvImageView *iv)
{
   GtkWidget *widget;
   gdouble ox, oy;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
   gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

   if (!nav_button_to_overlay (iv, widget, x, y, &ox, &oy)) return;

   gimv_image_view_open_navwin (iv, ox, oy);
}


static void
cb_nav_button_drag_update (GtkGestureDrag *gesture,
                           gdouble dx, gdouble dy,
                           GimvImageView *iv)
{
   GtkWidget *widget;
   gdouble sx, sy, ox, oy;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (!iv->priv->navwin || !gtk_widget_get_visible (iv->priv->navwin))
      return;

   widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
   if (!gtk_gesture_drag_get_start_point (gesture, &sx, &sy)) return;
   if (!nav_button_to_overlay (iv, widget, sx + dx, sy + dy, &ox, &oy))
      return;

   gimv_nav_win_pointer_motion (GIMV_NAV_WIN (iv->priv->navwin), ox, oy);
}


static void
cb_nav_button_drag_end (GtkGestureDrag *gesture,
                        gdouble dx, gdouble dy,
                        GimvImageView *iv)
{
   guint button;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (!iv->priv->navwin || !gtk_widget_get_visible (iv->priv->navwin))
      return;

   button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
   gimv_nav_win_pointer_release (GIMV_NAV_WIN (iv->priv->navwin),
                                 button ? button : 1);
}


/*****************************************************************************
 *
 *   Private functions.
 *
 *****************************************************************************/
static void
gimv_image_view_calc_image_size (GimvImageView *iv)
{
   gint orig_width, orig_height;
   gint width, height;
   gint fwidth, fheight;
   gfloat x_scale, y_scale;

   /* image scale */
   if (iv->priv->rotate == 0 || iv->priv->rotate == 2) {
      x_scale = iv->priv->x_scale;
      y_scale = iv->priv->y_scale;
   } else {
      x_scale = iv->priv->y_scale;
      y_scale = iv->priv->x_scale;
   }

   if (iv->priv->rotate == 0 || iv->priv->rotate == 2) {
      orig_width  = iv->info->width;
      orig_height = iv->info->height;
   } else {
      orig_width  = iv->info->height;
      orig_height = iv->info->width;
   }

   gimv_image_view_get_image_frame_size (iv, &fwidth, &fheight);
   width  = orig_width;
   height = orig_height;

   /* GTK4: a draw area that has just replaced another one (e.g. the movie
      player's after a movie) has no size until the next layout (0x0).
      Fitting to that gives a dot; show the image at 100% for now and fit
      it again when the area gets its size (gimv_image_view_frame_resized) */
   iv->priv->fit_pending = FALSE;
   if (iv->priv->fit_to_frame && (fwidth <= 1 || fheight <= 1)) {
      iv->priv->fit_pending = TRUE;
      fwidth  = orig_width;
      fheight = orig_height;
   }

   /* calculate image size */
   switch (iv->priv->fit_to_frame) {
   case GIMV_IMAGE_VIEW_ZOOM_FIT:
   case GIMV_IMAGE_VIEW_ZOOM_FIT_ZOOM_OUT_ONLY:
   {
      if (width <= fwidth && height <= fheight
          && iv->priv->fit_to_frame == GIMV_IMAGE_VIEW_ZOOM_FIT_ZOOM_OUT_ONLY)
      {
         iv->priv->x_scale = 100.0;
         iv->priv->y_scale = 100.0;
      } else {
         iv->priv->x_scale = (gfloat) fwidth  / (gfloat) width  * 100.0;
         iv->priv->y_scale = (gfloat) fheight / (gfloat) height * 100.0;

         if (iv->priv->keep_aspect) {
            if (iv->priv->x_scale > iv->priv->y_scale) {
               iv->priv->x_scale = iv->priv->y_scale;
               width  = orig_width * iv->priv->x_scale / 100.0;
               height = fheight;
            } else {
               iv->priv->y_scale = iv->priv->x_scale;
               width  = fwidth;
               height = orig_height * iv->priv->y_scale / 100.0;
            }
         } else {
            width  = fwidth;
            height = fheight;
         }
      }

      break;
   }

   case GIMV_IMAGE_VIEW_ZOOM_FIT_WIDTH:
      iv->priv->x_scale = (gfloat) fwidth / (gfloat) width * 100.0;
      iv->priv->y_scale = iv->priv->x_scale;
      width  = orig_width  * iv->priv->x_scale/ 100.0;
      height = orig_height * iv->priv->x_scale / 100.0;
      break;

   case GIMV_IMAGE_VIEW_ZOOM_FIT_HEIGHT:
      iv->priv->y_scale = (gfloat) fheight / (gfloat) height * 100.0;
      iv->priv->x_scale = iv->priv->y_scale;
      width  = orig_width  * iv->priv->y_scale / 100.0;
      height = orig_height * iv->priv->y_scale / 100.0;
      break;

   default:
      width  = orig_width  * x_scale / 100.0;
      height = orig_height * y_scale / 100.0;
      break;
   }

   /* fitting is done once per image; keep it while it is pending */
   if (!iv->priv->fit_pending)
      iv->priv->fit_to_frame = 0;

   /*
    * zoom out if image is too big & window is not scrollable on
    * fullscreen mode
    */
   /*
   win_width  = hadj->page_size;
   win_height = vadj->page_size;

   if (im->fullscreen && (width > win_width || height > win_height)) {
         gfloat width_ratio, height_ratio;

         width_ratio = (gdouble) width / hadj->page_size;
         height_ratio = (gdouble) height / vadj->page_size;
         if (width_ratio > height_ratio) {
         width = win_width;
         height = (gdouble) height / (gdouble) width_ratio;
      } else {
         width = (gdouble) width / (gdouble) height_ratio;
         height = win_height;
      }
   }
   */

   if (width > MIN_IMAGE_WIDTH)
      iv->priv->width = width;
   else
      iv->priv->width = MIN_IMAGE_WIDTH;
   if (height > MIN_IMAGE_HEIGHT)
      iv->priv->height = height;
   else
      iv->priv->height = MIN_IMAGE_HEIGHT;
}


static void
gimv_image_view_change_draw_widget (GimvImageView *iv, const gchar *label)
{
   GList *node;
   GimvImageViewPlugin *vftable = NULL;
   const gchar *view_mode;

   g_return_if_fail (iv);

   if (iv->draw_area_funcs
       && iv->draw_area_funcs->is_supported_fn
       && iv->draw_area_funcs->is_supported_fn (iv, iv->info))
   {
      return;
   }

   /* if default movie view mode is specified */
   /* FIXME!! this code is ad-hoc */
   view_mode = conf.movie_default_view_mode;
   if (iv->info
       && (gimv_image_info_is_movie (iv->info) || gimv_image_info_is_audio (iv->info))
       && view_mode && *view_mode)
   {
      for (node = gimv_image_view_plugin_get_list();
           node;
           node = g_list_next (node))
      {
         GimvImageViewPlugin *table = node->data;

         if (!table) continue;

         if (!strcmp (view_mode, table->label)
             && table->is_supported_fn
             && table->is_supported_fn (iv, iv->info))
         {
            vftable = table;
            break;
         }
      }
   }

   /* fall down... */
   for (node = gimv_image_view_plugin_get_list();
        node && !vftable;
        node = g_list_next (node))
   {
      GimvImageViewPlugin *table = node->data;

      if (!table) continue;

      if (label && *label) {
         if (!strcmp (label, table->label)
             || !strcmp (GIMV_IMAGE_VIEW_DEFAULT_VIEW_MODE, table->label))
         {
            vftable = table;
            break;
         }
      } else {
         if (!table->is_supported_fn
             || table->is_supported_fn (iv, iv->info))
         {
            vftable = table;
            break;
         }
      }
   }

   if (!vftable)
      vftable = &imageview_draw_vfunc_table;
   g_return_if_fail (vftable->create_fn);
   if (iv->draw_area_funcs && !strcmp (iv->draw_area_funcs->label, vftable->label))
      return;
   if (iv->draw_area)
      gimv_widget_destroy (iv->draw_area);

   iv->draw_area = vftable->create_fn (iv);
   iv->draw_area_funcs = vftable;

   gtk_widget_show (iv->draw_area);

   gimv_event_connect_after (GTK_WIDGET (iv->draw_area), GIMV_EVENT_KEY_PRESS, G_CALLBACK(cb_image_key_press), iv);
   gimv_event_connect (GTK_WIDGET (iv->draw_area), GIMV_EVENT_BUTTON_PRESS, G_CALLBACK (cb_image_button_press), iv);
   gimv_event_connect (GTK_WIDGET (iv->draw_area), GIMV_EVENT_BUTTON_RELEASE, G_CALLBACK (cb_image_button_release), iv);
   gimv_event_connect (GTK_WIDGET (iv->draw_area), GIMV_EVENT_MOTION_NOTIFY, G_CALLBACK (cb_image_motion_notify), iv);
   gimv_event_connect (GTK_WIDGET (iv->draw_area), GIMV_EVENT_SCROLL, G_CALLBACK (cb_image_scroll), iv);

   if (iv->priv->fullscreen) {
      gimv_container_add (GTK_WIDGET (iv->priv->fullscreen), iv->draw_area);
   } else {
      gimv_table_attach (GTK_WIDGET (iv->table), iv->draw_area, 0, 1, 0, 1, GIMV_FILL | GIMV_EXPAND, GIMV_FILL | GIMV_EXPAND, 0, 0);
   }

   /* for droping file list */
   gimv_dnd_connect (GTK_WIDGET (iv->draw_area), GIMV_DND_DRAG_DATA_RECEIVED, G_CALLBACK (cb_drag_data_received), iv);

   dnd_dest_set (iv->draw_area, dnd_types_archive, dnd_types_archive_num);

   /* set flags */
   gtk_widget_set_focusable (GTK_WIDGET (iv->draw_area), TRUE);
}


static void
gimv_image_view_adjust_pos_in_frame (GimvImageView *iv, gboolean center)
{
   gint fwidth, fheight;

   if (center) {
      gimv_image_view_get_image_frame_size (iv, &fwidth, &fheight);

      if (fwidth < iv->priv->width) {
         if (conf.imgview_scroll_nolimit) {
            if (iv->priv->x_pos > fwidth)
               iv->priv->x_pos = 0;
            else if (iv->priv->x_pos < 0 - iv->priv->width)
               iv->priv->x_pos = 0 - iv->priv->width + fwidth;
         } else {
            if (iv->priv->x_pos > 0)
               iv->priv->x_pos = 0;
            else if (iv->priv->x_pos < 0 - iv->priv->width + fwidth)
               iv->priv->x_pos = 0 - iv->priv->width + fwidth;
         }
      } else {
         iv->priv->x_pos = (fwidth - iv->priv->width) / 2;
      }

      if (fheight < iv->priv->height) {
         if (conf.imgview_scroll_nolimit) {
            if (iv->priv->y_pos > fheight)
               iv->priv->y_pos = 0;
            else if (iv->priv->y_pos < 0 - iv->priv->height)
               iv->priv->y_pos = 0 - iv->priv->height + fheight;
         } else {
            if (iv->priv->y_pos > 0)
               iv->priv->y_pos = 0;
            else if (iv->priv->y_pos < 0 - iv->priv->height + fheight)
               iv->priv->y_pos = 0 - iv->priv->height + fheight;
         }
      } else {
         iv->priv->y_pos = (fheight - iv->priv->height) / 2;
      }
   } else {
      if (!conf.imgview_scroll_nolimit) {
         gimv_image_view_get_image_frame_size (iv, &fwidth, &fheight);

         if (iv->priv->width <= fwidth) {
            if (iv->priv->x_pos < 0)
               iv->priv->x_pos = 0;
            if (iv->priv->x_pos > fwidth - iv->priv->width)
               iv->priv->x_pos = fwidth - iv->priv->width;
         } else if (iv->priv->x_pos < fwidth - iv->priv->width) {
            iv->priv->x_pos = fwidth - iv->priv->width;
         } else if (iv->priv->x_pos > 0) {
            iv->priv->x_pos = 0;
         }

         if (iv->priv->height <= fheight) {
            if (iv->priv->y_pos < 0)
               iv->priv->y_pos = 0;
            if (iv->priv->y_pos > fheight - iv->priv->height)
               iv->priv->y_pos = fheight - iv->priv->height;
         } else if (iv->priv->y_pos < fheight - iv->priv->height) {
            iv->priv->y_pos = fheight - iv->priv->height;
         } else if (iv->priv->y_pos > 0) {
            iv->priv->y_pos = 0;
         }
      }
   }
}

#ifdef ENABLE_EXIF
static int
get_exif_rotation (GimvImageInfo *info)
{
   ExifData *edata;
   ExifEntry *entry;
   int rotate = 0;

   g_return_val_if_fail (info->filename && *(info->filename), 0);

   /* system libexif: exif_data_new_from_file () replaces the bundled
      jpeg_data_* helpers */
   edata = exif_data_new_from_file (info->filename);
   if (!edata) return 0;

   entry = exif_content_get_entry (edata->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
   if (entry && entry->format == EXIF_FORMAT_SHORT && entry->size >= 2) {
      switch (exif_get_short (entry->data, exif_data_get_byte_order (edata))) {
      case 3:            /* rotate 180 degrees */
         rotate = 2;
         break;
      case 6:            /* rotate 90 degrees clockwise */
         rotate = 3;
         break;
      case 8:            /* rotate 90 degrees counterclockwise */
         rotate = 1;
         break;
      default:           /* 1: as is; 2, 4, 5, 7: mirrored (not supported) */
         rotate = 0;
         break;
      }
   }

   exif_data_unref (edata);

   return rotate;
}

#endif


/*****************************************************************************
 *
 *   Public functions.
 *
 *****************************************************************************/
GList *
gimv_image_view_get_list (void)
{
   return GimvImageViewList;
}


void
gimv_image_view_change_image (GimvImageView *iv, GimvImageInfo *info)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   gimv_image_view_playable_stop (iv);

   gimv_image_view_change_image_info (iv, info);

   gimv_image_view_change_draw_widget (iv, NULL);
   if (!g_list_find (GimvImageViewList, iv)) return;

   if (gtk_widget_get_mapped (GTK_WIDGET (iv->draw_area))) {
      gimv_image_view_show_image (iv);
      gimv_image_view_playable_play (iv);
      g_signal_emit (G_OBJECT (iv),
                     gimv_image_view_signals[IMAGE_CHANGED_SIGNAL], 0);
   } else {
      g_signal_connect_after (G_OBJECT (iv->draw_area), "map",
                                G_CALLBACK (cb_image_map), iv);
   }
}


void
gimv_image_view_change_image_info (GimvImageView *iv, GimvImageInfo *info)
{
   g_return_if_fail (iv);

   if (!g_list_find (GimvImageViewList, iv)) return;

   /* free old image */
   if (iv->priv->pixmap)
      gimv_image_free_pixmap_and_mask (iv->priv->pixmap, iv->priv->mask);
   iv->priv->pixmap = NULL;
   iv->priv->mask = NULL;

   if (iv->image)
      gimv_image_unref (iv->image);
   iv->image = NULL;

   /* free old image info */
   if (iv->info)
      gimv_image_info_unref (iv->info);

   /* Reset fit_to_frame (zoom control) to its default value for a new image.
    * The numbers correspond to the order of options in the pulldown menu
    * in prefs_ui_imagewin.c
    */

   /*
    * 2004-09-29 Takuro Ashie <ashie@homa.ne.jp>
    *   We should define enum.
    */

   switch (iv->priv->default_zoom) {
   case 2:
      iv->priv->fit_to_frame = GIMV_IMAGE_VIEW_ZOOM_FIT;
      break;
   case 3:
      iv->priv->fit_to_frame = GIMV_IMAGE_VIEW_ZOOM_FIT_ZOOM_OUT_ONLY;
      break;
   case 4:
      iv->priv->fit_to_frame = GIMV_IMAGE_VIEW_ZOOM_FIT_WIDTH;
      break;
   case 5:
      iv->priv->fit_to_frame = GIMV_IMAGE_VIEW_ZOOM_FIT_HEIGHT;
      break;
   case 0:
      iv->priv->x_scale = conf.imgview_scale;
      iv->priv->y_scale = conf.imgview_scale;
   case 1:
   default:
      iv->priv->fit_to_frame = 0;
      break;
   }

   /* ditto for rotation */

   switch (iv->priv->default_rotation) {
   case 4:
      break;
   case 5:
#ifdef ENABLE_EXIF
      iv->priv->rotate = get_exif_rotation (info);
#else
      iv->priv->rotate = 0;
#endif
      break;
    default:
      iv->priv->rotate = iv->priv->default_rotation;
      break;
   }

   /* the rotation the user gave this image last time (comment file) */
   if (conf.imgview_remember_rotation && info
       && !gimv_image_info_is_movie (info) && !gimv_image_info_is_audio (info))
   {
      gint orientation;
      if (gimv_comment_get_rotation (info, &orientation))
         iv->priv->rotate = orientation;
   }

   /* suggestion from sheepman <sheepman@tcn.zaq.ne.jp> */
   iv->priv->x_pos = iv->priv->y_pos = 0;

   /* allocate memory for new image info*/
   if (info)
      iv->info = gimv_image_info_ref (info);
   else
      iv->info = NULL;
}


static gint
idle_gimv_image_view_change_image_info (gpointer data)
{
   GimvImageView *iv = data;
   GList *node;

   g_return_val_if_fail (data, FALSE);

   node = g_list_find (GimvImageViewList, iv);
   if (!node) return FALSE;

   gimv_image_info_ref (iv->info);
   gimv_image_view_change_image (iv, iv->info);

   return FALSE;
}


/*
 *  background color of the draw area: the color set by
 *  gimv_image_view_set_bg_color () or the theme's window background
 *  (GTK2: style->bg[GTK_STATE_NORMAL] of the draw area).
 */
static void
gimv_image_view_get_bg_rgba (GimvImageView *iv, GdkRGBA *color)
{
   GtkWidget *widget;

   g_return_if_fail (color);

   if (iv && iv->bg_color) {
      *color = *iv->bg_color;
      color->alpha = 1.0;
      return;
   }

   widget = iv && iv->draw_area ? iv->draw_area : GTK_WIDGET (iv);
   if (widget) {
      GtkStyleContext *context = gtk_widget_get_style_context (widget);
      if (gtk_style_context_lookup_color (context, "theme_bg_color", color)
          || gtk_style_context_lookup_color (context, "window_bg_color", color))
      {
         color->alpha = 1.0;
         return;
      }
   }

   gdk_rgba_parse (color, "#f6f5f4");
}


/*
 *  Paint the image view (background and image) with cairo.  Used by the
 *  draw function of the default draw area (image_view_draw.c).
 *  GTK4: replaces the direct drawing to the GdkWindow of GTK2.
 */
void
gimv_image_view_paint (GimvImageView *iv, cairo_t *cr,
                       gint width, gint height)
{
   GdkRGBA bg;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));
   g_return_if_fail (cr);

   if (!iv->priv) return;

   /* fill background by default bg color */
   gimv_image_view_get_bg_rgba (iv, &bg);
   gdk_cairo_set_source_rgba (cr, &bg);
   cairo_rectangle (cr, 0, 0, width, height);
   cairo_fill (cr);

   /* checkerboard under the transparent parts; it moves with the image */
   if (iv->priv->pixmap && iv->priv->has_alpha) {
      static cairo_pattern_t *checker = NULL;
      cairo_matrix_t matrix;

      if (!checker) {
         cairo_surface_t *tile;
         cairo_t *tcr;

         tile = cairo_image_surface_create (CAIRO_FORMAT_RGB24, 16, 16);
         tcr = cairo_create (tile);
         cairo_set_source_rgb (tcr, 0.6, 0.6, 0.6);
         cairo_paint (tcr);
         cairo_set_source_rgb (tcr, 0.4, 0.4, 0.4);
         cairo_rectangle (tcr, 0, 0, 8, 8);
         cairo_rectangle (tcr, 8, 8, 8, 8);
         cairo_fill (tcr);
         cairo_destroy (tcr);
         checker = cairo_pattern_create_for_surface (tile);
         cairo_pattern_set_extend (checker, CAIRO_EXTEND_REPEAT);
         cairo_pattern_set_filter (checker, CAIRO_FILTER_NEAREST);
         cairo_surface_destroy (tile);
      }

      cairo_matrix_init_translate (&matrix, -iv->priv->x_pos, -iv->priv->y_pos);
      cairo_pattern_set_matrix (checker, &matrix);
      cairo_set_source (cr, checker);
      cairo_rectangle (cr, iv->priv->x_pos, iv->priv->y_pos,
                       gdk_texture_get_width (iv->priv->pixmap),
                       gdk_texture_get_height (iv->priv->pixmap));
      cairo_fill (cr);
   }

   /* draw image */
   if (iv->priv->pixmap) {
      /* the alpha channel (GTK2: mask) is contained in the texture */
      gimv_cairo_draw_texture (cr, iv->priv->pixmap,
                               iv->priv->x_pos, iv->priv->y_pos);
   }
}


void
gimv_image_view_draw_image (GimvImageView *iv)
{
   if (!gtk_widget_get_mapped (GTK_WIDGET (iv))) return;
   if (!iv->draw_area) return;

   /* GTK4: drawing is done in the draw function; just request a redraw */
   gtk_widget_queue_draw (iv->draw_area);

   gimv_image_view_reset_scrollbar (iv);
}


void
gimv_image_view_show_image (GimvImageView *iv)
{
   gimv_image_view_rotate_image (iv, iv->priv->rotate);
}


static gboolean
check_can_draw_image (GimvImageView *iv)
{
   gboolean is_movie = FALSE;
   const gchar *newfile = NULL;

   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);
   g_return_val_if_fail (iv->draw_area, FALSE);
   g_return_val_if_fail (GTK_IS_DRAWING_AREA (iv->draw_area), FALSE);

   /* if (!gtk_widget_get_mapped (GTK_WIDGET (iv->draw_area))) return FALSE; */

#warning FIXME!!
   if (iv->info && gimv_image_info_is_movie (iv->info))
      is_movie = TRUE;

   if (is_movie) return TRUE;
   if (iv->image) return TRUE;

   if (iv->info)
      newfile = gimv_image_info_get_path (iv->info);

   if (newfile && *newfile) {
      if (!file_exists (newfile))
         g_print (_("File doesn't exist: %s\n"), newfile);
      else
         g_print(_("Not an image file (or an unsupported format): %s\n"), newfile);
   }

   /* clear draw area */
   gimv_image_view_draw_image (iv);

   return FALSE;
}




void
gimv_image_view_create_thumbnail (GimvImageView *iv)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (!iv->info) return;
   if (iv->info->flags & GIMV_IMAGE_INFO_MRL_FLAG) return;   /* Dose it need? */

   if (!iv->draw_area_funcs) return;
   if (!iv->draw_area_funcs->create_thumbnail_fn) return;

   iv->draw_area_funcs->create_thumbnail_fn (iv, conf.cache_write_type);
}


/*****************************************************************************
 *
 *   functions for creating/show/hide/setting status child widgets
 *
 *****************************************************************************/
void
gimv_image_view_change_view_mode (GimvImageView *iv,
                                  const gchar *label)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   gimv_image_view_change_image (iv, NULL);
   gimv_image_view_change_draw_widget (iv, label);
}


/* GTK4: replacement of gtk_toolbar_append_item () (icon only toolbar) */
static GtkWidget *
player_toolbar_append_item (GtkWidget   *toolbar,
                            const gchar *tooltip,
                            GtkWidget   *icon,
                            GCallback    callback,
                            gpointer     data)
{
   GtkWidget *button;

   button = gtk_button_new ();
   gtk_button_set_has_frame (GTK_BUTTON (button), FALSE);
   gtk_widget_set_focusable (button, FALSE);
   if (icon)
      gtk_button_set_child (GTK_BUTTON (button), icon);
   if (tooltip)
      gtk_widget_set_tooltip_text (button, tooltip);
   g_signal_connect (G_OBJECT (button), "clicked", callback, data);
   gtk_box_append (GTK_BOX (toolbar), button);

   return button;
}


static void
player_format_time (gchar *buf, gsize size, guint ms)
{
   guint sec = ms / 1000;

   if (sec >= 3600)
      g_snprintf (buf, size, "%u:%02u:%02u", sec / 3600, sec / 60 % 60, sec % 60);
   else
      g_snprintf (buf, size, "%02u:%02u", sec / 60, sec % 60);
}


/*
 *  The label shows the seek bar position (percent) converted to time with
 *  the length of the stream, so it is right for every player plugin and
 *  also while the seek bar is dragged.  The length is remembered because
 *  some players cannot tell it once stopped.
 */
static void
player_update_time (GimvImageView *iv)
{
   GimvImageViewPlayableStatus status;
   GtkAdjustment *adj;
   gchar cur[32], total[32], *text;
   guint len, pos;

   if (!GIMV_IS_IMAGE_VIEW (iv) || !iv->priv) return;
   if (!iv->player.time_label || !iv->player.seekbar) return;

   status = gimv_image_view_playable_get_status (iv);
   if (status == GimvImageViewPlayableDisable || !iv->info) {
      gtk_label_set_text (GTK_LABEL (iv->player.time_label), "");
      iv->priv->time_info   = NULL;
      iv->priv->time_length = 0;
      return;
   }

   if (iv->priv->time_info != iv->info) {
      iv->priv->time_info   = iv->info;
      iv->priv->time_length = 0;
   }

   len = gimv_image_view_playable_get_length (iv);
   if (len > 0)
      iv->priv->time_length = len;
   else
      len = iv->priv->time_length;

   adj = gtk_range_get_adjustment (GTK_RANGE (iv->player.seekbar));
   if (len > 0)
      pos = (guint) (gtk_adjustment_get_value (adj) * len / 100.0 + 0.5);
   else
      pos = gimv_image_view_playable_get_position (iv);

   player_format_time (cur, sizeof (cur), pos);
   if (len > 0)
      player_format_time (total, sizeof (total), len);
   else
      g_strlcpy (total, "--:--", sizeof (total));

   text = g_strdup_printf ("%s / %s", cur, total);
   gtk_label_set_text (GTK_LABEL (iv->player.time_label), text);
   g_free (text);
}


/******************************************************************************
 *
 *   seek bar preview: a popover over the seek bar with the frame and the
 *   time under the pointer.  Frames come from the playable plugin
 *   (preview_fn -> gimv_image_view_playable_set_preview); at most one
 *   request is open, the latest pointer position is asked for next.
 *
 ******************************************************************************/
#define PREVIEW_WIDTH  192
#define PREVIEW_HEIGHT 108

static void
player_preview_hide (GimvImageView *iv)
{
   if (!iv->priv) return;
   iv->priv->preview_want = -1;
   if (iv->player.preview && gtk_widget_get_visible (iv->player.preview))
      gtk_popover_popdown (GTK_POPOVER (iv->player.preview));
}


static void
player_preview_request (GimvImageView *iv, guint ms)
{
   GimvImageViewPlayableIF *playable = iv->draw_area_funcs->playable;

   if (iv->priv->preview_busy) {
      iv->priv->preview_want = ms;
      return;
   }
   if ((gint64) ms == iv->priv->preview_last) return;

   iv->priv->preview_busy = TRUE;
   iv->priv->preview_want = -1;
   iv->priv->preview_last = ms;
   playable->preview_fn (iv, ms);
}


static void
cb_preview_motion (GtkEventControllerMotion *controller,
                   gdouble x, gdouble y, GimvImageView *iv)
{
   GimvImageViewPlayableIF *playable;
   GimvImageViewPlayableStatus status;
   GdkRectangle rect, point;
   gboolean movie;
   gdouble frac;
   gchar buf[32];
   guint len, ms;

   if (!iv->priv || !iv->info || !iv->player.preview) return;

   movie = gimv_image_info_is_movie (iv->info);
   status = gimv_image_view_playable_get_status (iv);
   if ((!movie && !gimv_image_info_is_audio (iv->info))
       || status == GimvImageViewPlayableDisable
       || !gtk_widget_is_sensitive (iv->player.seekbar))
   {
      player_preview_hide (iv);
      return;
   }

   /* another file: forget the open request (its draw area may be gone) */
   if (iv->priv->preview_info != iv->info) {
      iv->priv->preview_info = iv->info;
      iv->priv->preview_busy = FALSE;
      iv->priv->preview_want = -1;
      iv->priv->preview_last = -1;
      gtk_picture_set_paintable (GTK_PICTURE (iv->player.preview_picture), NULL);
      gtk_widget_set_visible (iv->player.preview_picture, FALSE);
   }

   len = gimv_image_view_playable_get_length (iv);
   if (!len) len = iv->priv->time_length;
   if (!len) {
      player_preview_hide (iv);
      return;
   }

   gtk_range_get_range_rect (GTK_RANGE (iv->player.seekbar), &rect);
   if (rect.width < 2) return;
   frac = CLAMP ((x - rect.x) / (gdouble) rect.width, 0.0, 1.0);
   ms = (guint) (frac * len);

   player_format_time (buf, sizeof (buf), ms);
   gtk_label_set_text (GTK_LABEL (iv->player.preview_label), buf);

   point.x = (gint) x;
   point.y = 0;
   point.width = point.height = 1;
   gtk_popover_set_pointing_to (GTK_POPOVER (iv->player.preview), &point);
   if (!gtk_widget_get_visible (iv->player.preview))
      gtk_popover_popup (GTK_POPOVER (iv->player.preview));

   playable = iv->draw_area_funcs ? iv->draw_area_funcs->playable : NULL;
   if (movie && playable && playable->preview_fn
       && playable->is_seekable_fn && playable->is_seekable_fn (iv))
   {
      player_preview_request (iv, ms);
   }
}


static void
cb_preview_leave (GtkEventControllerMotion *controller, GimvImageView *iv)
{
   player_preview_hide (iv);
}


static void
cb_seekbar_destroy (GtkWidget *seekbar, GimvImageView *iv)
{
   if (iv->player.preview) {
      gtk_widget_unparent (iv->player.preview);
      iv->player.preview = NULL;
      iv->player.preview_picture = NULL;
      iv->player.preview_label = NULL;
   }
}


static void
player_preview_create (GimvImageView *iv)
{
   GtkWidget *popover, *vbox, *picture, *label;
   GtkEventController *motion;

   popover = gtk_popover_new ();
   gtk_popover_set_autohide (GTK_POPOVER (popover), FALSE);
   gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_TOP);
   gtk_widget_set_can_target (popover, FALSE);
   gtk_widget_set_can_focus (popover, FALSE);
   gtk_widget_add_css_class (popover, "gimv-seek-preview");

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
   gtk_popover_set_child (GTK_POPOVER (popover), vbox);

   picture = gtk_picture_new ();
   gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_CONTAIN);
   gtk_picture_set_can_shrink (GTK_PICTURE (picture), TRUE);
   gtk_widget_set_size_request (picture, PREVIEW_WIDTH, PREVIEW_HEIGHT);
   gtk_widget_set_visible (picture, FALSE);
   gtk_box_append (GTK_BOX (vbox), picture);

   label = gtk_label_new (NULL);
   gtk_widget_add_css_class (label, "numeric");
   gtk_box_append (GTK_BOX (vbox), label);

   gtk_widget_set_parent (popover, iv->player.seekbar);
   g_signal_connect (iv->player.seekbar, "destroy",
                     G_CALLBACK (cb_seekbar_destroy), iv);

   iv->player.preview         = popover;
   iv->player.preview_picture = picture;
   iv->player.preview_label   = label;

   iv->priv->preview_want = -1;
   iv->priv->preview_last = -1;

   motion = gtk_event_controller_motion_new ();
   /* "enter" carries the position too; no "motion" may follow it */
   g_signal_connect (motion, "enter",  G_CALLBACK (cb_preview_motion), iv);
   g_signal_connect (motion, "motion", G_CALLBACK (cb_preview_motion), iv);
   g_signal_connect (motion, "leave",  G_CALLBACK (cb_preview_leave), iv);
   gtk_widget_add_controller (iv->player.seekbar, motion);
}


void
gimv_image_view_playable_set_preview (GimvImageView *iv,
                                      GdkTexture    *frame,
                                      guint          pos)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (!iv->priv) return;
   iv->priv->preview_busy = FALSE;

   if (!iv->player.preview || !gtk_widget_get_visible (iv->player.preview))
      return;

   if (frame) {
      gtk_picture_set_paintable (GTK_PICTURE (iv->player.preview_picture),
                                 GDK_PAINTABLE (frame));
      gtk_widget_set_visible (iv->player.preview_picture, TRUE);
   } else {
      gtk_widget_set_visible (iv->player.preview_picture, FALSE);
   }

   /* the pointer moved on meanwhile */
   if (iv->priv->preview_want >= 0
       && iv->draw_area_funcs && iv->draw_area_funcs->playable
       && iv->draw_area_funcs->playable->preview_fn)
   {
      player_preview_request (iv, (guint) iv->priv->preview_want);
   }
}


static GtkWidget *
gimv_image_view_create_player_toolbar (GimvImageView *iv)
{
   GtkWidget *hbox, *toolbar;
   GtkWidget *button, *seekbar;
   GtkWidget *iconw;
   GtkAdjustment *adj;

   hbox = gimv_hbox_new (FALSE, 0);
   toolbar = gtkutil_create_toolbar ();
   /* GTK2: GTK_TOOLBAR_ICONS */
   gimv_box_pack_start (GTK_BOX (hbox), toolbar, FALSE, FALSE, 0);
   gtk_widget_show (toolbar);

   iv->player_container = hbox;
   iv->player_toolbar = toolbar;

   /* Reverse button */
   iconw = gimv_icon_stock_get_widget ("rw");
   button = player_toolbar_append_item (toolbar, _("Reverse"), iconw,
                                        G_CALLBACK (cb_gimv_image_view_rw), iv);
   gtk_widget_set_sensitive (button, FALSE);
   iv->player.rw = button;

   /* play button */
   iconw = gimv_icon_stock_get_widget ("play");
   button = player_toolbar_append_item (toolbar, _("Play"), iconw,
                                        G_CALLBACK (cb_gimv_image_view_play), iv);
   gtk_widget_set_sensitive (button, FALSE);
   iv->player.play = button;
   iv->player.play_icon = iconw;

   /* stop button */
   iconw = gimv_icon_stock_get_widget ("stop2");
   button = player_toolbar_append_item (toolbar, _("Stop"), iconw,
                                        G_CALLBACK (cb_gimv_image_view_stop), iv);
   gtk_widget_set_sensitive (button, FALSE);
   iv->player.stop = button;

   /* Forward button */
   iconw = gimv_icon_stock_get_widget ("ff");
   button = player_toolbar_append_item (toolbar, _("Forward"), iconw,
                                        G_CALLBACK (cb_gimv_image_view_fw), iv);
   gtk_widget_set_sensitive (button, FALSE);
   iv->player.fw = button;

   /* Eject button */
   iconw = gimv_icon_stock_get_widget ("eject");
   button = player_toolbar_append_item (toolbar, _("Eject"), iconw,
                                        G_CALLBACK (cb_gimv_image_view_eject), iv);
   iv->player.eject = button;
   gtk_widget_set_sensitive (button, FALSE);

   adj = gtk_adjustment_new (0.0, 0.0, 100.0, 0.1, 1.0, 1.0);
   seekbar = gtk_scale_new (GTK_ORIENTATION_HORIZONTAL, adj);
   gtk_scale_set_draw_value (GTK_SCALE (seekbar), FALSE);
   gimv_box_pack_start (GTK_BOX (hbox), seekbar, TRUE, TRUE, 0);
   gtk_widget_show (seekbar);
   iv->player.seekbar = seekbar;

   gimv_event_connect (GTK_WIDGET (iv->player.seekbar), GIMV_EVENT_BUTTON_PRESS, G_CALLBACK (cb_seekbar_pressed), iv);
   gimv_event_connect (GTK_WIDGET (iv->player.seekbar), GIMV_EVENT_BUTTON_RELEASE, G_CALLBACK (cb_seekbar_released), iv);

   player_preview_create (iv);

   /* elapsed / total time; follows the seek bar, also while dragging */
   iv->player.time_label = gtk_label_new (NULL);
   gtk_widget_add_css_class (iv->player.time_label, "numeric");
   gtk_widget_set_margin_start (iv->player.time_label, 4);
   gtk_widget_set_margin_end (iv->player.time_label, 6);
   gimv_box_pack_start (GTK_BOX (hbox), iv->player.time_label, FALSE, FALSE, 0);
   g_signal_connect_swapped (adj, "value-changed",
                             G_CALLBACK (player_update_time), iv);

   return hbox;
}


GtkWidget *
gimv_image_view_create_zoom_menu (GtkWidget *window,
                                  GimvImageView *iv,
                                  const gchar *path)
{
   GtkWidget *menu;
   guint n_menu_items;

   g_return_val_if_fail (window, NULL);
   g_return_val_if_fail (iv, NULL);
   g_return_val_if_fail (path && *path, NULL);

   n_menu_items = sizeof(gimv_image_view_zoom_items)
      / sizeof(gimv_image_view_zoom_items[0]) - 1;
   menu = menu_create_items(window, gimv_image_view_zoom_items,
                            n_menu_items, path, iv);
   gimv_image_view_set_menu_ptr (iv, &iv->zoom_menu, menu);
   menu_check_item_set_active (menu, "/Keep Aspect Ratio", iv->priv->keep_aspect);

   return menu;
}


GtkWidget *
gimv_image_view_create_rotate_menu (GtkWidget *window,
                                    GimvImageView *iv,
                                    const gchar *path)
{
   GtkWidget *menu;
   guint n_menu_items;

   g_return_val_if_fail (window, NULL);
   g_return_val_if_fail (iv, NULL);
   g_return_val_if_fail (path && *path, NULL);

   n_menu_items = sizeof(gimv_image_view_rotate_items)
      / sizeof(gimv_image_view_rotate_items[0]) - 1;
   menu = menu_create_items(window, gimv_image_view_rotate_items,
                            n_menu_items, path, iv);
   gimv_image_view_set_menu_ptr (iv, &iv->rotate_menu, menu);

   return menu;
}


/*
 *  GTK4: menus are released with their window, which may happen before or
 *  after the image view is disposed -- keep weak pointers to them.
 */
void
gimv_image_view_set_menu_ptr (GimvImageView *iv, GtkWidget **field,
                              GtkWidget *menu)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));
   g_return_if_fail (field);

   if (*field == menu) return;
   if (*field)
      g_object_remove_weak_pointer (G_OBJECT (*field), (gpointer *) field);
   *field = menu;
   if (menu)
      g_object_add_weak_pointer (G_OBJECT (menu), (gpointer *) field);
}


GtkWidget *
gimv_image_view_create_movie_menu (GtkWidget *window,
                                   GimvImageView *iv,
                                   const gchar *path)
{
   GtkWidget *menu;
   guint n_menu_items;
   GimvImageViewPlayableStatus status;

   n_menu_items = sizeof(gimv_image_view_playable_items)
      / sizeof(gimv_image_view_playable_items[0]) - 1;
   menu = menu_create_items(window, gimv_image_view_playable_items,
                            n_menu_items, path, iv);
   gimv_image_view_set_menu_ptr (iv, &iv->movie_menu, menu);

   menu_check_item_set_active (iv->movie_menu, "/Continuous Play",
                               iv->priv->continuance_play);
   status = gimv_image_view_playable_get_status (iv);
   gimv_image_view_playable_set_status (iv, status);


   return menu;
}


GtkWidget *
gimv_image_view_create_view_modes_menu (GtkWidget *window,
                                        GimvImageView *iv,
                                        const gchar *path)
{
   GtkWidget *menu;
   GList *node;

   menu = gimv_menu_new (window);
   gimv_image_view_set_menu_ptr (iv, &iv->view_modes_menu, menu);

   for (node = gimv_image_view_plugin_get_list(); node; node = g_list_next (node)) {
      GimvMenuItem *menu_item;
      GimvImageViewPlugin *vftable = node->data;

      if (!vftable) continue;

      menu_item = gimv_menu_append_item (menu, _(vftable->label),
                                         cb_change_view_mode, iv);
      g_object_set_data (G_OBJECT (menu_item),
                         "GimvImageView::ViewMode",
                         (gpointer) vftable->label);
   }


   return menu;
}


GtkWidget *
gimv_image_view_create_popup_menu (GtkWidget *window,
                                   GimvImageView *iv,
                                   const gchar *path)
{
   guint n_menu_items;

   g_return_val_if_fail (window, NULL);
   g_return_val_if_fail (iv, NULL);
   g_return_val_if_fail (path && *path, NULL);

   n_menu_items = sizeof(gimv_image_view_popup_items)
      / sizeof(gimv_image_view_popup_items[0]) - 1;
   gimv_image_view_set_menu_ptr (iv, &iv->imageview_popup,
                                 menu_create_items (window,
                                                    gimv_image_view_popup_items,
                                                    n_menu_items, path, iv));

   gimv_image_view_create_zoom_menu (window, iv, path);
   gimv_image_view_create_rotate_menu (window, iv, path);

   menu_set_submenu (iv->imageview_popup, "/Zoom",   iv->zoom_menu);
   menu_set_submenu (iv->imageview_popup, "/Rotate", iv->rotate_menu);

   menu_check_item_set_active (iv->imageview_popup, "/Checkerboard Behind Transparency",
                               iv->priv->alpha_checker);
   menu_check_item_set_active (iv->imageview_popup, "/Show Scrollbar",
                               iv->priv->show_scrollbar);
   menu_check_item_set_active (iv->imageview_popup, "/Memory Buffer",
                               iv->priv->buffer);

   gimv_image_view_create_movie_menu (window, iv, path);
   menu_set_submenu (iv->imageview_popup, "/Movie",  iv->movie_menu);

   gimv_image_view_create_view_modes_menu (window, iv, path);
   menu_set_submenu (iv->imageview_popup, "/View Modes",  iv->view_modes_menu);

   return iv->imageview_popup;
}


void
gimv_image_view_popup_menu (GimvImageView *iv, GimvEventButton *event)
{
   gdouble x = -1, y = -1;

   g_return_if_fail (iv);
   /* g_return_if_fail (event); */

   /* event coordinates are relative to the draw area */
   if (event) {
      x = event->x;
      y = event->y;
   }

   if (iv->imageview_popup)
      gimv_menu_popup (iv->imageview_popup,
                       iv->draw_area ? iv->draw_area : GTK_WIDGET (iv),
                       x, y);
}


void
gimv_image_view_set_alpha_checker (GimvImageView *iv, gboolean checker)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (iv->priv->alpha_checker == checker) return;
   iv->priv->alpha_checker = checker;

   if (iv->imageview_popup)
      menu_check_item_set_active (iv->imageview_popup,
                                  "/Checkerboard Behind Transparency", checker);

   /* the alpha channel may have been blended in already: load it again */
   if (iv->info && !gimv_image_info_is_movie (iv->info)
       && !gimv_image_info_is_audio (iv->info))
   {
      if (iv->image) {
         gimv_image_unref (iv->image);
         iv->image = NULL;
      }
      gimv_image_view_show_image (iv);
   }
}


void
gimv_image_view_set_continuance (GimvImageView *iv, gboolean continuance)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   iv->priv->continuance_play = continuance;
   if (iv->movie_menu)
      menu_check_item_set_active (iv->movie_menu, "/Continuous Play",
                                  continuance);
}


gboolean
gimv_image_view_get_alpha_checker (GimvImageView *iv)
{
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);
   return iv->priv->alpha_checker;
}


void
gimv_image_view_set_bg_color (GimvImageView *iv, gint red, gint green, gint brue)
{
   g_return_if_fail (iv);
   g_return_if_fail (iv->draw_area);

   /* the components are 0 - 65535 (GdkColor of GTK2) */
   if (!iv->bg_color)
      iv->bg_color = g_new0 (GdkRGBA, 1);
   iv->bg_color->red   = CLAMP (red,   0, 65535) / 65535.0;
   iv->bg_color->green = CLAMP (green, 0, 65535) / 65535.0;
   iv->bg_color->blue  = CLAMP (brue,  0, 65535) / 65535.0;
   iv->bg_color->alpha = 1.0;

   gtk_widget_queue_draw (iv->draw_area);
}


/* current background color of the draw area, components are 0 - 65535 */
void
gimv_image_view_get_bg_color (GimvImageView *iv,
                              gint *red, gint *green, gint *blue)
{
   GdkRGBA color;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   gimv_image_view_get_bg_rgba (iv, &color);
   if (red)   *red   = color.red   * 65535.0 + 0.5;
   if (green) *green = color.green * 65535.0 + 0.5;
   if (blue)  *blue  = color.blue  * 65535.0 + 0.5;
}


void
gimv_image_view_show_scrollbar (GimvImageView *iv)
{
   g_return_if_fail (iv);
   g_return_if_fail (iv->hscrollbar);
   g_return_if_fail (iv->vscrollbar);

   iv->priv->show_scrollbar = TRUE;
   update_scrollbar_visibility (iv);
}


void
gimv_image_view_hide_scrollbar (GimvImageView *iv)
{
   g_return_if_fail (iv);
   g_return_if_fail (iv->hscrollbar);
   g_return_if_fail (iv->vscrollbar);

   iv->priv->show_scrollbar = FALSE;
   update_scrollbar_visibility (iv);
}


void
gimv_image_view_set_progressbar (GimvImageView *iv, GtkWidget *progressbar)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   iv->progressbar = progressbar;
}


void
gimv_image_view_set_player_visible (GimvImageView *iv,
                                    GimvImageViewPlayerVisibleType type)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   switch (type) {
   case GimvImageViewPlayerVisibleHide:
      gtk_widget_hide (iv->player_container);
      iv->priv->player_visible = GimvImageViewPlayerVisibleHide;
      break;
   case GimvImageViewPlayerVisibleShow:
      gtk_widget_show (iv->player_container);
      iv->priv->player_visible = GimvImageViewPlayerVisibleShow;
      break;
   case GimvImageViewPlayerVisibleAuto:
   default:
      if (gimv_image_view_is_playable (iv)) {
         gtk_widget_show (iv->player_container);
      } else {
         gtk_widget_hide (iv->player_container);
      }
      iv->priv->player_visible = GimvImageViewPlayerVisibleAuto;
      break;
   }
}


GimvImageViewPlayerVisibleType
gimv_image_view_get_player_visible (GimvImageView *iv)
{
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), 0);
   return iv->priv->player_visible;
}


static gboolean
cb_navwin_button_release  (GtkWidget *widget,
                           GimvEventButton *event,
                           GimvImageView *iv)
{
   GimvNavWin *navwin;
   GimvImageViewZoomType zoom_type = -1;

   g_return_val_if_fail (GIMV_IS_NAV_WIN (widget), FALSE);
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);

   navwin = GIMV_NAV_WIN (widget);

   switch (event->button) {
   case 4:
      zoom_type = GIMV_IMAGE_VIEW_ZOOM_OUT;
      break;
   case 5:
      zoom_type = GIMV_IMAGE_VIEW_ZOOM_IN;
      break;
   default:
      break;
   }

   if (zoom_type >= 0) {
      gint vx, vy;

      gimv_event_block_by_func (widget,
                                G_CALLBACK (cb_navwin_button_release),
                                iv);
      gimv_image_view_zoom_image (iv, zoom_type, 0, 0);
      gimv_event_unblock_by_func (widget,
                                  G_CALLBACK (cb_navwin_button_release),
                                  iv);

      gimv_nav_win_set_orig_image_size (navwin,
                                        iv->priv->width,
                                        iv->priv->height);
      gimv_image_view_get_view_position (iv, &vx, &vy);
      gimv_nav_win_set_view_position (navwin, vx, vy);
   }

   return FALSE;
}


/* GTK4: the mouse wheel sends scroll events instead of button 4/5 */
static gboolean
cb_navwin_scroll (GtkWidget *widget,
                  GimvEventScroll *event,
                  GimvImageView *iv)
{
   GimvEventButton be;

   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);

   memset (&be, 0, sizeof (be));
   be.type   = GIMV_BUTTON_RELEASE;
   be.time   = event->time;
   be.x      = event->x;
   be.y      = event->y;
   be.state  = event->state;

   switch (event->direction) {
   case GDK_SCROLL_UP:
      be.button = 4;
      break;
   case GDK_SCROLL_DOWN:
      be.button = 5;
      break;
   default:
      return FALSE;
   }

   return cb_navwin_button_release (widget, &be, iv);
}


#define ZOOM_KEY_NUM 12

static void
zoom_key_parse (guint keys[ZOOM_KEY_NUM], GdkModifierType mods[ZOOM_KEY_NUM])
{
   gchar **keyconfs[ZOOM_KEY_NUM] = {
      &akey.imgwin_zoomin,
      &akey.imgwin_zoomout,
      &akey.imgwin_fit_img,
      &akey.imgwin_zoom10,
      &akey.imgwin_zoom25,
      &akey.imgwin_zoom50,
      &akey.imgwin_zoom75,
      &akey.imgwin_zoom100,
      &akey.imgwin_zoom125,
      &akey.imgwin_zoom150,
      &akey.imgwin_zoom175,
      &akey.imgwin_zoom200,
   };
   gint i;

   if (!keys) return;
   if (!mods) return;

   for (i = 0; i < ZOOM_KEY_NUM; i++) {
      gchar *keyconf;

      keys[i] = mods[i] = 0;
      if (!keyconfs[i] || !*keyconfs[i] || !**keyconfs[i]) continue;
      keyconf = *keyconfs[i];

      gtk_accelerator_parse (keyconf, &keys[i], &mods[i]);
   }
}


static gboolean
cb_navwin_key_press (GtkWidget *widget, 
                     GimvEventKey *event,
                     GimvImageView *iv)
{
   GimvNavWin *navwin;
   GimvImageViewZoomType zoom_type = -1;
   guint zoom_key[ZOOM_KEY_NUM], keyval;
   GdkModifierType zoom_mod[ZOOM_KEY_NUM], modval;

   g_return_val_if_fail (GIMV_IS_NAV_WIN (widget), FALSE);
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);

   navwin = GIMV_NAV_WIN (widget);

   keyval = event->keyval;
   modval = event->state;

   zoom_key_parse (zoom_key, zoom_mod);

   if (keyval == GDK_KEY_equal
              || (keyval == zoom_key[0] && (!zoom_mod[0] || (modval & zoom_mod[0]))))
   {
      zoom_type = GIMV_IMAGE_VIEW_ZOOM_IN;
   } else if (event->keyval == GDK_KEY_minus
              || (keyval == zoom_key[1] && (!zoom_mod[1] || (modval & zoom_mod[1])))) 
   {
      zoom_type = GIMV_IMAGE_VIEW_ZOOM_OUT;
   } else if (keyval == zoom_key[3] && (!zoom_mod[3] || (modval & zoom_mod[3]))) {
      zoom_type = GIMV_IMAGE_VIEW_ZOOM_10;
   } else if (keyval == zoom_key[4] && (!zoom_mod[4] || (modval & zoom_mod[4]))) {
      zoom_type = GIMV_IMAGE_VIEW_ZOOM_25;
   } else if (keyval == zoom_key[5] && (!zoom_mod[5] || (modval & zoom_mod[5]))) {
      zoom_type = GIMV_IMAGE_VIEW_ZOOM_50;
   } else if (keyval == zoom_key[6] && (!zoom_mod[6] || (modval & zoom_mod[6]))) {
      zoom_type = GIMV_IMAGE_VIEW_ZOOM_75;
   } else if (keyval == zoom_key[7] && (!zoom_mod[7] || (modval & zoom_mod[7]))) {
      zoom_type = GIMV_IMAGE_VIEW_ZOOM_100;
   } else if (keyval == zoom_key[8] && (!zoom_mod[8] || (modval & zoom_mod[8]))) {
      zoom_type = GIMV_IMAGE_VIEW_ZOOM_125;
   } else if (keyval == zoom_key[9] && (!zoom_mod[9] || (modval & zoom_mod[9]))) {
      zoom_type = GIMV_IMAGE_VIEW_ZOOM_150;
   } else if (keyval == zoom_key[10] && (!zoom_mod[10] || (modval & zoom_mod[10]))) {
      zoom_type = GIMV_IMAGE_VIEW_ZOOM_175;
   } else if (keyval == zoom_key[11] && (!zoom_mod[11] || (modval & zoom_mod[11]))) {
      zoom_type = GIMV_IMAGE_VIEW_ZOOM_200;
   }

   if (zoom_type >= 0) {
      gint vx, vy;

      gimv_event_block_by_func (widget,
                                G_CALLBACK (cb_navwin_key_press),
                                iv);
      gimv_image_view_zoom_image (iv, zoom_type, 0, 0);
      gimv_event_unblock_by_func (widget,
                                  G_CALLBACK (cb_navwin_key_press),
                                  iv);

      gimv_nav_win_set_orig_image_size (navwin,
                                        iv->priv->width,
                                        iv->priv->height);
      gimv_image_view_get_view_position (iv, &vx, &vy);
      gimv_nav_win_set_view_position (navwin, vx, vy);
   }

   return FALSE;
}


static void
cb_navwin_move (GimvNavWin *navwin, gint x, gint y, GimvImageView *iv)
{
   g_return_if_fail (GIMV_IS_NAV_WIN (navwin));
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   gimv_image_view_moveto (iv, x, y);
}


void
gimv_image_view_open_navwin (GimvImageView *iv, gint x_root, gint y_root)
{
   GtkWidget *navwin;
   GimvImage *image;
   GdkTexture *pixmap;
   GdkTexture *mask;
   gint src_width, src_height, dest_width, dest_height;
   gint fwidth, fheight;
   gint fpos_x, fpos_y;

   g_return_if_fail (iv);
   if (!iv->priv->pixmap) return;

   /* get pixmap for navigator */
   src_width  = gdk_texture_get_width (iv->priv->pixmap);
   src_height = gdk_texture_get_height (iv->priv->pixmap);
   image = gimv_image_create_from_texture (iv->priv->pixmap, 0, 0,
                                            src_width, src_height);
   g_return_if_fail (image);

   if (src_width > src_height) {
      dest_width  = GIMV_NAV_WIN_SIZE;
      dest_height = src_height * GIMV_NAV_WIN_SIZE / src_width;
   } else {
      dest_height = GIMV_NAV_WIN_SIZE;
      dest_width  = src_width * GIMV_NAV_WIN_SIZE / src_height;
   }

   gimv_image_scale_get_pixmap (image, dest_width, dest_height,
                                &pixmap, &mask);
   if (!pixmap) goto ERROR;

   gimv_image_view_get_image_frame_size (iv, &fwidth, &fheight);
   gimv_image_view_get_view_position (iv, &fpos_x, &fpos_y);

   /* open navigate window */
   if (iv->priv->navwin) {
      navwin = iv->priv->navwin;
      gimv_nav_win_set_pixmap (GIMV_NAV_WIN (navwin), pixmap, mask,
                               iv->priv->width, iv->priv->height);
      gimv_nav_win_set_view_size (GIMV_NAV_WIN (navwin), fwidth, fheight);
      gimv_nav_win_set_view_position (GIMV_NAV_WIN (navwin), fpos_x, fpos_y);
      gimv_nav_win_show (GIMV_NAV_WIN (navwin), x_root, y_root);
   } else {
      navwin = gimv_nav_win_new (pixmap, mask,
                                 iv->priv->width, iv->priv->height,
                                 fwidth, fheight,
                                 fpos_x, fpos_y);
      gimv_event_connect (GTK_WIDGET (navwin), GIMV_EVENT_BUTTON_RELEASE, G_CALLBACK (cb_navwin_button_release), iv);
      gimv_event_connect (GTK_WIDGET (navwin), GIMV_EVENT_KEY_PRESS, G_CALLBACK(cb_navwin_key_press), iv);
      gimv_event_connect (GTK_WIDGET (navwin), GIMV_EVENT_SCROLL, G_CALLBACK (cb_navwin_scroll), iv);
      g_signal_connect (G_OBJECT (navwin), "move",
                          G_CALLBACK (cb_navwin_move), iv);
      gtk_overlay_add_overlay (GTK_OVERLAY (iv->priv->overlay), navwin);
      gimv_nav_win_show (GIMV_NAV_WIN (navwin), x_root, y_root);
      iv->priv->navwin = navwin;
   }

   /* free */
   g_object_unref (pixmap);

ERROR:
   gimv_image_unref (image);
}


void
gimv_image_view_set_fullscreen (GimvImageView *iv, GtkWindow *fullscreen)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));
   g_return_if_fail (GTK_IS_WINDOW (fullscreen));

   if (iv->priv->fullscreen) return;
   iv->priv->fullscreen = fullscreen;

   /* GTK4: gtk_widget_reparent () is gone */
   g_object_ref (iv->draw_area);
   gimv_container_remove (iv->table, iv->draw_area);
   gtk_window_set_child (iv->priv->fullscreen, iv->draw_area);
   g_object_unref (iv->draw_area);
   gtk_widget_grab_focus (iv->draw_area);
}


void
gimv_image_view_unset_fullscreen (GimvImageView *iv)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (!iv->priv->fullscreen) return;

   /* GTK4: gtk_widget_reparent () is gone */
   g_object_ref (iv->draw_area);
   if (gtk_window_get_child (iv->priv->fullscreen) == iv->draw_area)
      gtk_window_set_child (iv->priv->fullscreen, NULL);
   gimv_table_attach (GTK_WIDGET (iv->table), iv->draw_area, 0, 1, 0, 1,
                      GIMV_FILL | GIMV_EXPAND, GIMV_FILL | GIMV_EXPAND, 0, 0);
   g_object_unref (iv->draw_area);
   iv->priv->fullscreen = NULL;
}



/*****************************************************************************
 *
 *  loading functions
 *
 *****************************************************************************/
void
gimv_image_view_free_image_buf (GimvImageView *iv)
{
   if (!iv)
      return;

   if (!iv->image) return;
   if (iv->priv->buffer) return;

#warning FIXME!!
   if (gimv_image_info_is_animation (iv->info)
       || gimv_image_info_is_movie (iv->info)
       || gimv_image_info_is_audio (iv->info))
   {
      return;
   }

   gimv_image_unref (iv->image);
   iv->image = NULL;
}
static gint
progress_timeout (gpointer data)
{
   /* GTK4: GtkProgress activity mode -> gtk_progress_bar_pulse () */
   gtk_progress_bar_pulse (GTK_PROGRESS_BAR (data));

   return (TRUE);
}


static void
cb_loader_progress_update (GimvImageLoader *loader, GimvImageView *iv)
{
   gimv_flush_events ();
}


static void
cb_loader_load_end (GimvImageLoader *loader, GimvImageView *iv)
{
   GimvImage *image, *rgb_image;

   g_return_if_fail (GIMV_IS_IMAGE_LOADER (loader));
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   image = gimv_image_loader_get_image (loader);
   if (!image) goto ERROR;
   gimv_image_ref (image);
   gimv_image_loader_unref_image (loader);

   /* keep the alpha channel and paint a checkerboard behind it
      (gimv_image_view_paint), or blend it with the background colour */
   iv->priv->has_alpha = FALSE;
   if (gimv_image_has_alpha (image) && !GIMV_IS_ANIM (image)
       && iv->priv->alpha_checker && !iv->priv->ignore_alpha)
   {
      iv->priv->has_alpha = TRUE;
   } else if (gimv_image_has_alpha (image) && !GIMV_IS_ANIM (image)) {
      gint bg_r = 255, bg_g = 255, bg_b = 255;
      GdkRGBA bg;

      gimv_image_view_get_bg_rgba (iv, &bg);
      bg_r = bg.red   * 255.0 + 0.5;
      bg_g = bg.green * 255.0 + 0.5;
      bg_b = bg.blue  * 255.0 + 0.5;
      
      rgb_image = gimv_image_rgba2rgb (image,
                                       bg_r, bg_g, bg_b,
                                       iv->priv->ignore_alpha);
      if (rgb_image) {
         gimv_image_unref (image);
         image = rgb_image;
      }
   }

   if (!g_list_find (GimvImageViewList, iv)) {
      gimv_image_unref (image);
      goto ERROR;
   } else {
      if (iv->image)
         gimv_image_unref (iv->image);
      iv->image = image;
   }

   /* the loader learned the image size: show it in the thumbnail views
      (detail view "Image size" column) */
   if (iv->priv->size_unknown && iv->info
       && iv->info->width > 0 && iv->info->height > 0)
   {
      iv->priv->size_unknown = FALSE;
      gimv_thumb_view_update_info (iv->info);
   }

   gimv_image_view_rotate_render (iv, iv->priv->rotate);

ERROR:
   g_signal_handlers_disconnect_by_func (G_OBJECT (iv->loader),
                                  G_CALLBACK (cb_loader_progress_update),
                                  iv);
   g_signal_handlers_disconnect_by_func (G_OBJECT (iv->loader),
                                  G_CALLBACK (cb_loader_load_end),
                                  iv);

   iv->priv->loader_progress_update_signal_id = 0;
   iv->priv->loader_load_end_signal_id        = 0;

   g_signal_emit (G_OBJECT (iv),
                  gimv_image_view_signals[LOAD_END_SIGNAL], 0,
                  iv->info, FALSE);
}


static gboolean
gimv_image_view_need_load (GimvImageView *iv)
{
   gint width, height;

   if (!iv->image) return TRUE;

#if IMAGE_VIEW_ENABLE_SCALABLE_LOAD

   if (iv->priv->rotate == 0 || iv->priv->rotate == 2) {
      width  = gimv_image_width (iv->image);
      height = gimv_image_height (iv->image);
   } else {
      width  = gimv_image_height (iv->image);
      height = gimv_image_width  (iv->image);
   }

   if ( width == iv->info->width
       &&  height == iv->info->height)
   {
      /* full scale image was already loaded */
      return FALSE;

   } else {
      gint req_width, req_height;

      gimv_image_view_get_request_size (iv, &req_width, &req_height);

      if (req_width >= 0 && req_height >= 0
          && width  >= req_width
          && height >= req_height)
      {
         /* the image is bigger than request size */
         return FALSE;
      }
   }

   return TRUE;

#endif /* IMAGE_VIEW_ENABLE_SCALABLE_LOAD */

   return FALSE;
}


static void
gimv_image_view_load_image_buf_start (GimvImageView *iv)
{
   const gchar *filename;
   gint req_width, req_height;

   g_return_if_fail (iv);

   if (!iv->info) return;
   if (!g_list_find (GimvImageViewList, iv)) return;

   if (!gimv_image_view_need_load (iv)) return;

#warning FIXME!!
   if (gimv_image_info_is_archive (iv->info)
       || gimv_image_info_is_movie (iv->info)
       || gimv_image_info_is_audio (iv->info))
   {
      return;
   }

   filename = gimv_image_info_get_path (iv->info);
   if (!filename || !*filename) return;

   iv->priv->size_unknown = (iv->info->width <= 0 || iv->info->height <= 0);

   g_signal_emit (G_OBJECT (iv),
                  gimv_image_view_signals[LOAD_START_SIGNAL], 0,
                  iv->info);

   if (gimv_image_info_is_in_archive (iv->info)) {
      guint timer = 0;

      /* set progress bar */
      if (iv->progressbar) {
         gtk_progress_bar_pulse (GTK_PROGRESS_BAR (iv->progressbar));
         timer = g_timeout_add (50,
                                  (GSourceFunc) progress_timeout,
                                  iv->progressbar);
         gimv_grab_add (iv->progressbar);
      }

      /* extract */
      gimv_image_info_extract_archive (iv->info);

      /* unset progress bar */
      if (iv->progressbar) {
         g_source_remove (timer);
         gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR(iv->progressbar), 0.0);
         gimv_grab_remove (iv->progressbar);
      }
   }

   /* load image buf */
   /* iv->loader->flags |= GIMV_IMAGE_LOADER_DEBUG_FLAG; */
   if (iv->priv->loader_progress_update_signal_id)
      g_signal_handler_disconnect (G_OBJECT (iv->loader),
                             iv->priv->loader_progress_update_signal_id);
   iv->priv->loader_progress_update_signal_id = 
      g_signal_connect (G_OBJECT (iv->loader), "progress_update",
                          G_CALLBACK (cb_loader_progress_update),
                             iv);
   if (iv->priv->loader_load_end_signal_id)
      g_signal_handler_disconnect (G_OBJECT (iv->loader),
                             iv->priv->loader_load_end_signal_id);
   iv->priv->loader_load_end_signal_id = 
      g_signal_connect (G_OBJECT (iv->loader), "load_end",
                          G_CALLBACK (cb_loader_load_end),
                          iv);

   gimv_image_loader_set_image_info (iv->loader, iv->info);
   gimv_image_loader_set_as_animation (iv->loader, TRUE);

#if IMAGE_VIEW_ENABLE_SCALABLE_LOAD
   gimv_image_view_get_request_size (iv, &req_width, &req_height);
   gimv_image_loader_set_size_request (iv->loader, req_width, req_height, TRUE);
#endif

   gimv_image_loader_load_start (iv->loader);
}


static void
cb_loader_load_restart (GimvImageLoader *loader, GimvImageView *iv)
{
   if (iv->priv->loader_load_end_signal_id)
      g_signal_handler_disconnect (G_OBJECT (iv->loader),
                             iv->priv->loader_load_end_signal_id);
   iv->priv->loader_load_end_signal_id = 0;
   gimv_image_view_load_image_buf_start (iv);
}


void
gimv_image_view_load_image_buf (GimvImageView *iv)
{
   if (gimv_image_loader_is_loading (iv->loader)) {
      if (iv->info == iv->loader->info) return;

      gimv_image_view_cancel_loading (iv);

      iv->priv->loader_load_end_signal_id
         = g_signal_connect (G_OBJECT (iv->loader), "load_end",
                               G_CALLBACK (cb_loader_load_restart),
                               iv);
   } else {
      gimv_image_view_load_image_buf_start (iv);
   }
}


gboolean
gimv_image_view_is_loading (GimvImageView *iv)
{
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);
   g_return_val_if_fail (iv->loader, FALSE);

   return gimv_image_loader_is_loading (iv->loader);
}


void
gimv_image_view_cancel_loading (GimvImageView *iv)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (!gimv_image_loader_is_loading (iv->loader)) return;

   gimv_image_loader_load_stop (iv->loader);

   if (iv->priv->loader_progress_update_signal_id)
      g_signal_handler_disconnect (G_OBJECT (iv->loader),
                             iv->priv->loader_progress_update_signal_id);
   iv->priv->loader_progress_update_signal_id = 0;

   if (iv->priv->loader_load_end_signal_id)
      g_signal_handler_disconnect (G_OBJECT (iv->loader),
                             iv->priv->loader_load_end_signal_id);
   iv->priv->loader_load_end_signal_id = 0;

   /*
   g_signal_emit (G_OBJECT (iv),
                    gimv_image_view_signals[LOAD_END_SIGNAL],
                    iv->info, TRUE);
   */
}



/*****************************************************************************
 *
 *   scalable interface functions
 *
 *****************************************************************************/
void
gimv_image_view_zoom_image (GimvImageView *iv, GimvImageViewZoomType zoom,
                            gfloat x_scale, gfloat y_scale)
{
   gint cx_pos, cy_pos, fwidth, fheight;
   gfloat src_x_scale, src_y_scale;

   g_return_if_fail (iv);

   src_x_scale = iv->priv->x_scale;
   src_y_scale = iv->priv->y_scale;

   iv->priv->fit_to_frame = 0;

   switch (zoom) {
   case GIMV_IMAGE_VIEW_ZOOM_IN:
      if (iv->priv->x_scale < GIMV_IMAGE_VIEW_MAX_SCALE
          && iv->priv->y_scale < GIMV_IMAGE_VIEW_MAX_SCALE)
      {
         iv->priv->x_scale = iv->priv->x_scale + GIMV_IMAGE_VIEW_MIN_SCALE;
         iv->priv->y_scale = iv->priv->y_scale + GIMV_IMAGE_VIEW_MIN_SCALE;
      }
      break;
   case GIMV_IMAGE_VIEW_ZOOM_OUT:
      if (iv->priv->x_scale > GIMV_IMAGE_VIEW_MIN_SCALE
          && iv->priv->y_scale > GIMV_IMAGE_VIEW_MIN_SCALE)
      {
         iv->priv->x_scale = iv->priv->x_scale - GIMV_IMAGE_VIEW_MIN_SCALE;
         iv->priv->y_scale = iv->priv->y_scale - GIMV_IMAGE_VIEW_MIN_SCALE;
      }
      break;
   case GIMV_IMAGE_VIEW_ZOOM_FIT:
   case GIMV_IMAGE_VIEW_ZOOM_FIT_ZOOM_OUT_ONLY:
   case GIMV_IMAGE_VIEW_ZOOM_FIT_WIDTH:
   case GIMV_IMAGE_VIEW_ZOOM_FIT_HEIGHT:
      iv->priv->fit_to_frame = zoom;
      break;
   case GIMV_IMAGE_VIEW_ZOOM_10:
      iv->priv->x_scale = iv->priv->y_scale =  10;
      break;
   case GIMV_IMAGE_VIEW_ZOOM_25:
      iv->priv->x_scale = iv->priv->y_scale =  25;
      break;
   case GIMV_IMAGE_VIEW_ZOOM_50:
      iv->priv->x_scale = iv->priv->y_scale =  50;
      break;
   case GIMV_IMAGE_VIEW_ZOOM_75:
      iv->priv->x_scale = iv->priv->y_scale =  75;
      break;
   case GIMV_IMAGE_VIEW_ZOOM_100:
      iv->priv->x_scale = iv->priv->y_scale = 100;
      break;
   case GIMV_IMAGE_VIEW_ZOOM_125:
      iv->priv->x_scale = iv->priv->y_scale = 125;
      break;
   case GIMV_IMAGE_VIEW_ZOOM_150:
      iv->priv->x_scale = iv->priv->y_scale = 150;
      break;
   case GIMV_IMAGE_VIEW_ZOOM_175:
      iv->priv->x_scale = iv->priv->y_scale = 175;
      break;
   case GIMV_IMAGE_VIEW_ZOOM_200:
      iv->priv->x_scale = iv->priv->y_scale = 200;
      break;	 
   case GIMV_IMAGE_VIEW_ZOOM_300:
      iv->priv->x_scale = iv->priv->y_scale = 300;
      break;	 
   case GIMV_IMAGE_VIEW_ZOOM_FREE:
      iv->priv->x_scale = x_scale;
      iv->priv->y_scale = y_scale;
      break;
   default:
      break;
   }

   gimv_image_view_get_image_frame_size (iv, &fwidth, &fheight);

   cx_pos = (iv->priv->x_pos - fwidth  / 2) * iv->priv->x_scale / src_x_scale;
   cy_pos = (iv->priv->y_pos - fheight / 2) * iv->priv->y_scale / src_y_scale;

   iv->priv->x_pos = cx_pos + fwidth  / 2;
   iv->priv->y_pos = cy_pos + fheight / 2;

   gimv_image_view_show_image (iv);
}


gboolean
gimv_image_view_get_image_size (GimvImageView   *iv,
                                gint        *width,
                                gint        *height)
{
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);

   if (width)
      *width = iv->priv->width;
   if (height)
      *height = iv->priv->height;

   return TRUE;
}


static void
gimv_image_view_get_request_size (GimvImageView *iv,
                                  gint *width_ret, gint *height_ret)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));
   g_return_if_fail (width_ret && height_ret);

   *width_ret  = -1;
   *height_ret = -1;

   if (iv->priv->fit_to_frame) {
      gint fwidth, fheight;

      gimv_image_view_get_image_frame_size (iv, &fwidth, &fheight);

      /* GTK4: a view in a window that was just created has no allocation
         yet (0x0, GTK2 reported at least 1x1 and allocated on show).
         Requesting that size makes scalable loaders (JPEG) decode at 1/8
         and the image is shown blocky, so load the full size instead. */
      if (fwidth <= 1 || fheight <= 1)
         return;

      if (iv->priv->rotate == 0 || iv->priv->rotate == 2) {
         *width_ret  = fwidth;
         *height_ret = fheight;
      } else {
         *width_ret  = fheight;
         *height_ret = fwidth;
      }

      return;

   } else if (iv->priv->width > MIN_IMAGE_WIDTH && iv->priv->height > MIN_IMAGE_HEIGHT) {
      *width_ret  = iv->info->width  * iv->priv->x_scale / 100.0;
      *height_ret = iv->info->height * iv->priv->y_scale / 100.0;
      return;
   } else {
      /*
       * the image was changed, so the image size may be unknown yet.
       * (iv->info will know it, but (currently) we don't trust it yet. )
       */
   }
}



/*****************************************************************************
 *
 *   rotatable interface functions
 *
 *****************************************************************************/
static void
gimv_image_view_rotate_render (GimvImageView *iv,
                               GimvImageViewOrientation angle)
{
   switch (angle) {
   case GIMV_IMAGE_VIEW_ROTATE_90:
      iv->image = gimv_image_rotate_90 (iv->image, TRUE);
      break;
   case GIMV_IMAGE_VIEW_ROTATE_180:
      iv->image = gimv_image_rotate_180 (iv->image);
      break;
   case GIMV_IMAGE_VIEW_ROTATE_270:
      iv->image = gimv_image_rotate_90 (iv->image, FALSE);
      break;
   default:
      break;
   }
}


typedef struct RotateData_Tag {
   GimvImageViewOrientation angle;
} RotateData;


static void
cb_gimv_image_view_rotate_load_end (GimvImageView *iv, GimvImageInfo *info,
                                    gboolean cancel, gpointer user_data)
{
   RotateData *data = user_data;
   GimvImageViewOrientation angle = data->angle, rotate_angle;
   gboolean can_draw;
   gchar *path, *cache;

   if (iv->priv->load_end_signal_id)
      g_signal_handler_disconnect (G_OBJECT (iv), iv->priv->load_end_signal_id);
   iv->priv->load_end_signal_id = 0;

   if (cancel) return;

   can_draw = check_can_draw_image (iv);
   if (!can_draw) goto func_end;

   /* rotate image */
   rotate_angle = (angle - iv->priv->rotate) % 4;
   if (rotate_angle < 0)
      rotate_angle = rotate_angle + 4;

   iv->priv->rotate = angle;
   gimv_image_view_rotate_render (iv, rotate_angle);

   /* set image size */
   gimv_image_view_calc_image_size (iv);

   /* image rendering */
   gimv_image_free_pixmap_and_mask (iv->priv->pixmap, iv->priv->mask);
   gimv_image_scale_get_pixmap (iv->image,
                                iv->priv->width, iv->priv->height,
                                &iv->priv->pixmap, &iv->priv->mask);

   /* reset image geometory */
   gimv_image_view_adjust_pos_in_frame (iv, TRUE);

   /* draw image */
   gimv_image_view_draw_image (iv);

   /* create thumbnail if not exist */
   path  = gimv_image_info_get_path_with_archive (iv->info);
   cache = gimv_thumb_find_thumbcache(path, NULL);
   if (path && *path && !cache) {
      gimv_image_view_create_thumbnail (iv);
   }
   g_free (path);
   g_free (cache);
   path  = NULL;
   cache = NULL;

   gimv_image_view_free_image_buf (iv);

func_end:
   if (iv->priv->load_end_signal_id)
      g_signal_handler_disconnect (G_OBJECT (iv), iv->priv->load_end_signal_id);
   iv->priv->load_end_signal_id = 0;

   g_signal_emit (G_OBJECT (iv), gimv_image_view_signals[RENDERED_SIGNAL], 0);
}


void
gimv_image_view_rotate_image (GimvImageView *iv, GimvImageViewOrientation angle)
{
   RotateData *data;

   /* rotated by the user (internal redraws pass the current angle):
      remember it for this image */
   if (conf.imgview_remember_rotation && iv->info && iv->priv
       && (gint) angle != iv->priv->rotate
       && !gimv_image_info_is_movie (iv->info)
       && !gimv_image_info_is_audio (iv->info))
   {
      gimv_comment_set_rotation (iv->info, angle);
   }

   if (iv->priv->load_end_signal_id)
      g_signal_handler_disconnect (G_OBJECT (iv), iv->priv->load_end_signal_id);

   data = g_new0 (RotateData, 1);
   data->angle = angle;

   if (gimv_image_view_need_load (iv)) {
      iv->priv->load_end_signal_id
         = g_signal_connect_data (G_OBJECT (iv), "load_end",
                                  G_CALLBACK (cb_gimv_image_view_rotate_load_end),
                                  data, (GClosureNotify) g_free,
                                  0);

      gimv_image_view_load_image_buf (iv);
   } else {
      cb_gimv_image_view_rotate_load_end (iv, iv->info, FALSE, data);
      g_free (data);
   }
}


void
gimv_image_view_rotate_ccw (GimvImageView *iv)
{
   guint angle;

   g_return_if_fail (iv);

   /* convert to absolute angle */
   angle = (iv->priv->rotate + 1) % 4;

   gimv_image_view_rotate_image (iv, angle);
}


void
gimv_image_view_rotate_cw (GimvImageView *iv)
{
   guint angle;

   g_return_if_fail (iv);

   /* convert to absolute angle */
   angle = (iv->priv->rotate + 3) % 4;

   gimv_image_view_rotate_image (iv, angle);
}


GimvImageViewOrientation
gimv_image_view_get_orientation (GimvImageView *iv)
{
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), 0);
   return iv->priv->rotate;
}



/*****************************************************************************
 *
 *   scrollable interface functions
 *
 *****************************************************************************/
void
gimv_image_view_get_image_frame_size (GimvImageView *iv, gint *width, gint *height)
{
   g_return_if_fail (width && height);

   *width = *height = 0;

   g_return_if_fail (iv);

   if (iv->priv->fullscreen) {
      *width  = gtk_widget_get_width (GTK_WIDGET (iv->priv->fullscreen));
      *height = gtk_widget_get_height (GTK_WIDGET (iv->priv->fullscreen));
   } else {      
      *width  = gtk_widget_get_width (GTK_WIDGET (iv->draw_area));
      *height = gtk_widget_get_height (GTK_WIDGET (iv->draw_area));
   }
}


/*
 *  gimv_image_view_frame_resized:
 *
 *  Called by draw areas when their size changes.  If the image was fitted
 *  before the area had a size, fit it again (from an idle callback).
 *  Returns TRUE if the caller need not redraw.
 */
static gboolean
idle_refit (gpointer data)
{
   GimvImageView *iv = data;

   if (iv->priv && iv->info && iv->draw_area
       && g_list_find (GimvImageViewList, iv))
   {
      gimv_image_view_show_image (iv);
   }

   return G_SOURCE_REMOVE;
}


gboolean
gimv_image_view_frame_resized (GimvImageView *iv)
{
   gint fwidth, fheight;

   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);

   if (!iv->priv || !iv->priv->fit_pending) return FALSE;
   if (!iv->info) return FALSE;

   gimv_image_view_get_image_frame_size (iv, &fwidth, &fheight);
   if (fwidth <= 1 || fheight <= 1) return FALSE;

   iv->priv->fit_pending = FALSE;

   /* the image buffer may be gone already (keep_buffer off): showing it
      again may reload the file, so not from inside the size allocation */
   g_idle_add_full (G_PRIORITY_HIGH_IDLE, idle_refit,
                    g_object_ref (iv), g_object_unref);

   return FALSE;
}


gboolean
gimv_image_view_get_view_position (GimvImageView *iv, gint *x, gint *y)
{
   g_return_val_if_fail (iv, FALSE);
   g_return_val_if_fail (x && y, FALSE);

   *x = 0 - iv->priv->x_pos;
   *y = 0 - iv->priv->y_pos;

   return TRUE;
}


void
gimv_image_view_moveto (GimvImageView *iv, gint x, gint y)
{
   g_return_if_fail (iv);

   iv->priv->x_pos = 0 - x;
   iv->priv->y_pos = 0 - y;

   gimv_image_view_adjust_pos_in_frame (iv, TRUE);

   gimv_image_view_draw_image (iv);
}


void
gimv_image_view_reset_scrollbar (GimvImageView *iv)
{
   gint fwidth, fheight;
   gdouble value, upper;

   g_return_if_fail (iv);
   g_return_if_fail (iv->draw_area);
   g_return_if_fail (iv->hadj);
   g_return_if_fail (iv->vadj);

   if (!iv->priv) return;

   fwidth  = gtk_widget_get_width  (GTK_WIDGET (iv->draw_area));
   fheight = gtk_widget_get_height (GTK_WIDGET (iv->draw_area));

   /* horizontal */
   if (iv->priv->x_pos < 0)
      value = 0 - iv->priv->x_pos;
   else
      value = 0;

   if (iv->priv->width > fwidth)
      upper = iv->priv->width;
   else
      upper = fwidth;

   move_scrollbar_by_user = FALSE;

   gtk_adjustment_configure (iv->hadj, value,
                             gtk_adjustment_get_lower (iv->hadj),
                             upper,
                             gtk_adjustment_get_step_increment (iv->hadj),
                             gtk_adjustment_get_page_increment (iv->hadj),
                             fwidth);

   /* vertical */
   if (iv->priv->y_pos < 0)
      value = 0 - iv->priv->y_pos;
   else
      value = 0;

   if (iv->priv->height > fheight)
      upper = iv->priv->height;
   else
      upper = fheight;

   gtk_adjustment_configure (iv->vadj, value,
                             gtk_adjustment_get_lower (iv->vadj),
                             upper,
                             gtk_adjustment_get_step_increment (iv->vadj),
                             gtk_adjustment_get_page_increment (iv->vadj),
                             fheight);

   move_scrollbar_by_user = TRUE;

   update_scrollbar_visibility (iv);
}


/* GTK4: like GTK_POLICY_AUTOMATIC, show each scrollbar only while the image
   does not fit (GTK2: always shown).  Decided from the size of the table,
   which does not change when the scrollbars come and go, so that showing
   one cannot hide it again on the next resize. */
static void
update_scrollbar_visibility (GimvImageView *iv)
{
   static gint vsb_width = 14, hsb_height = 14;   /* last seen thickness */
   gint avail_w, avail_h, img_w, img_h;
   gboolean need_h = FALSE, need_v = FALSE;

   if (!iv->priv || !iv->table || !iv->hscrollbar || !iv->vscrollbar) return;

   if (gtk_widget_get_visible (iv->vscrollbar)
       && gtk_widget_get_width (iv->vscrollbar) > 0)
      vsb_width = gtk_widget_get_width (iv->vscrollbar);
   if (gtk_widget_get_visible (iv->hscrollbar)
       && gtk_widget_get_height (iv->hscrollbar) > 0)
      hsb_height = gtk_widget_get_height (iv->hscrollbar);

   avail_w = gtk_widget_get_width  (iv->table);
   avail_h = gtk_widget_get_height (iv->table);

   if (iv->priv->show_scrollbar && !iv->priv->fullscreen) {
      if (avail_w <= 1 || avail_h <= 1) return;   /* not allocated yet */

      img_w = MAX (iv->priv->width, 0);
      img_h = MAX (iv->priv->height, 0);

      need_h = img_w > avail_w;
      need_v = img_h > avail_h;
      if (need_h && !need_v) need_v = img_h > avail_h - hsb_height;
      if (need_v && !need_h) need_h = img_w > avail_w - vsb_width;
   }

   if (gtk_widget_get_visible (iv->hscrollbar) != need_h)
      gtk_widget_set_visible (iv->hscrollbar, need_h);
   if (gtk_widget_get_visible (iv->vscrollbar) != need_v)
      gtk_widget_set_visible (iv->vscrollbar, need_v);
   /* the navigator button sits in the corner: only with both scrollbars */
   if (gtk_widget_get_visible (iv->nav_button) != (need_h && need_v))
      gtk_widget_set_visible (iv->nav_button, need_h && need_v);
}



/*****************************************************************************
 *
 *   playable interface functions
 *
 *****************************************************************************/
gboolean
gimv_image_view_is_playable (GimvImageView *iv)
{
   GimvImageViewPlayableIF *playable;

   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);

   if (!iv->info) return FALSE;
   if (!iv->draw_area_funcs || !iv->draw_area_funcs->playable) return FALSE;

   playable = iv->draw_area_funcs->playable;

   if (!playable->is_playable_fn) return FALSE;

   return playable->is_playable_fn (iv, iv->info);
}


void
gimv_image_view_playable_play (GimvImageView *iv)
{
   GimvImageViewPlayableIF *playable;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (!iv->draw_area_funcs || !iv->draw_area_funcs->playable) return;
   playable = iv->draw_area_funcs->playable;

   if (gimv_image_view_is_playable (iv)) {
      g_return_if_fail (playable->play_fn);
      playable->play_fn (iv);
   }
}


void
gimv_image_view_playable_stop (GimvImageView *iv)
{
   GimvImageViewPlayableIF *playable;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (!iv->draw_area_funcs || !iv->draw_area_funcs->playable) return;

   playable = iv->draw_area_funcs->playable;

   if (!playable->stop_fn) return;
   playable->stop_fn (iv);
}


void
gimv_image_view_playable_pause (GimvImageView *iv)
{
   GimvImageViewPlayableIF *playable;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (!iv->draw_area_funcs || !iv->draw_area_funcs->playable) return;

   playable = iv->draw_area_funcs->playable;

   if (!playable->pause_fn) return;
   playable->pause_fn (iv);
}


void
gimv_image_view_playable_forward (GimvImageView *iv)
{
   GimvImageViewPlayableIF *playable;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (!iv->draw_area_funcs || !iv->draw_area_funcs->playable) return;

   playable = iv->draw_area_funcs->playable;

   if (!playable->forward_fn) return;
   playable->forward_fn (iv);
}


void
gimv_image_view_playable_reverse (GimvImageView *iv)
{
   GimvImageViewPlayableIF *playable;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (!iv->draw_area_funcs || !iv->draw_area_funcs->playable) return;

   playable = iv->draw_area_funcs->playable;

   if (!playable->reverse_fn) return;
   playable->reverse_fn (iv);
}


void
gimv_image_view_playable_seek (GimvImageView *iv, guint pos)
{
   GimvImageViewPlayableIF *playable;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (!iv->draw_area_funcs || !iv->draw_area_funcs->playable) return;

   playable = iv->draw_area_funcs->playable;

   if (!playable->seek_fn) return;
   playable->seek_fn (iv, pos);
}


void
gimv_image_view_playable_eject (GimvImageView *iv)
{
   GimvImageViewPlayableIF *playable;


   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   if (!iv->draw_area_funcs || !iv->draw_area_funcs->playable) return;

   playable = iv->draw_area_funcs->playable;

   if (!playable->eject_fn) return;
   playable->eject_fn (iv);
}


GimvImageViewPlayableStatus
gimv_image_view_playable_get_status (GimvImageView *iv)
{
   GimvImageViewPlayableIF *playable;

   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), GimvImageViewPlayableDisable);

   if (!iv->draw_area_funcs || !iv->draw_area_funcs->playable)
      return GimvImageViewPlayableDisable;

   playable = iv->draw_area_funcs->playable;

   if (!playable->get_status_fn) return GimvImageViewPlayableDisable;
   return playable->get_status_fn (iv);
}


/*
 *  gimv_image_view_playable_is_busy:
 *
 *  TRUE while a movie or an audio file is playing or paused (not stopped),
 *  so the image and preview windows can ignore mouse actions like "next
 *  image" on it.  mplayer drew into its own X window, which swallowed the
 *  clicks; the GStreamer player is an ordinary widget.
 */
gboolean
gimv_image_view_playable_is_busy (GimvImageView *iv)
{
   GimvImageViewPlayableStatus status;

   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);

   if (!iv->info) return FALSE;
   if (!gimv_image_info_is_movie (iv->info)
       && !gimv_image_info_is_audio (iv->info))
   {
      return FALSE;
   }

   status = gimv_image_view_playable_get_status (iv);
   return status != GimvImageViewPlayableDisable
      && status != GimvImageViewPlayableStop;
}


guint
gimv_image_view_playable_get_length (GimvImageView *iv)
{
   GimvImageViewPlayableIF *playable;

   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), 0);

   if (!iv->draw_area_funcs || !iv->draw_area_funcs->playable) return 0;

   playable = iv->draw_area_funcs->playable;

   if (!playable->get_length_fn) return 0;
   return playable->get_length_fn (iv);
}


guint
gimv_image_view_playable_get_position (GimvImageView *iv)
{
   GimvImageViewPlayableIF *playable;

   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), 0);

   if (!iv->draw_area_funcs || !iv->draw_area_funcs->playable) return 0;

   playable = iv->draw_area_funcs->playable;

   if (!playable->get_position_fn) return 0;
   return playable->get_position_fn (iv);
}



/*****************************************************************************
 *
 *   list interface functions
 *
 *****************************************************************************/
void
gimv_image_view_set_list (GimvImageView       *iv,
                          GList           *list,
                          GList           *current,
                          gpointer         list_owner,
                          GimvImageViewNextFn  next_fn,
                          GimvImageViewPrevFn  prev_fn,
                          GimvImageViewNthFn   nth_fn,
                          GimvImageViewRemoveListFn remove_list_fn,
                          gpointer         list_fn_user_data)
{
   g_return_if_fail (iv);
   g_return_if_fail (list);
   g_return_if_fail (current);

   if (!g_list_find (GimvImageViewList, iv)) return;

   if (iv->priv->image_list)
      gimv_image_view_remove_list (iv, iv->priv->image_list->owner);

   iv->priv->image_list = g_new0 (GimvImageViewImageList, 1);

   iv->priv->image_list->list = list;
   if (!current)
      iv->priv->image_list->current = list;
   else
      iv->priv->image_list->current = current;

   iv->priv->image_list->owner             = list_owner;
   iv->priv->image_list->next_fn           = next_fn;
   iv->priv->image_list->prev_fn           = prev_fn;
   iv->priv->image_list->nth_fn            = nth_fn;
   iv->priv->image_list->remove_list_fn    = remove_list_fn;
   iv->priv->image_list->list_fn_user_data = list_fn_user_data;

   g_signal_emit (G_OBJECT (iv), gimv_image_view_signals[SET_LIST_SIGNAL], 0);
}


void
gimv_image_view_remove_list (GimvImageView *iv, gpointer list_owner)
{
   g_return_if_fail (iv);

   if (!iv->priv->image_list) return;

   if (iv->priv->image_list->remove_list_fn
       && iv->priv->image_list->owner == list_owner)
   {
      iv->priv->image_list->remove_list_fn
         (iv,
          iv->priv->image_list->owner,
          iv->priv->image_list->list_fn_user_data);
   }

   g_free (iv->priv->image_list);
   iv->priv->image_list = NULL;

   g_signal_emit (G_OBJECT (iv), gimv_image_view_signals[UNSET_LIST_SIGNAL], 0);
}


gint
gimv_image_view_image_list_length (GimvImageView *iv)
{
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), 0);
   if (!iv->priv->image_list) return 0;

   return g_list_length (iv->priv->image_list->list);
}


gint
gimv_image_view_image_list_position (GimvImageView *iv)
{
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), 0);
   if (!iv->priv->image_list) return 0;

   return g_list_position (iv->priv->image_list->list,
                           iv->priv->image_list->current);
}


GList *
gimv_image_view_image_list_current (GimvImageView *iv)
{
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), NULL);
   if (!iv->priv->image_list) return NULL;

   return iv->priv->image_list->current;
}


static gint
idle_gimv_image_view_next (gpointer data)
{
   GimvImageView *iv = data;

   g_return_val_if_fail (iv, FALSE);
   if (!iv->priv->image_list) return FALSE;
   if (!iv->priv->image_list->next_fn) return FALSE;

   iv->priv->image_list->current
      = iv->priv->image_list->next_fn (iv,
                                       iv->priv->image_list->owner,
                                       iv->priv->image_list->current,
                                       iv->priv->image_list->list_fn_user_data);

   return FALSE;
}


void
gimv_image_view_next (GimvImageView *iv)
{
   g_return_if_fail (iv);
   g_idle_add (idle_gimv_image_view_next, iv);
}


static gint
idle_gimv_image_view_prev (gpointer data)
{
   GimvImageView *iv = data;

   g_return_val_if_fail (iv, FALSE);
   if (!iv->priv->image_list) return FALSE;
   if (!iv->priv->image_list->prev_fn) return FALSE;

   iv->priv->image_list->current
      = iv->priv->image_list->prev_fn (iv,
                                       iv->priv->image_list->owner,
                                       iv->priv->image_list->current,
                                       iv->priv->image_list->list_fn_user_data);

   return FALSE;
}


void
gimv_image_view_prev (GimvImageView *iv)
{
   g_return_if_fail (iv);

   g_idle_add (idle_gimv_image_view_prev, iv);
}


typedef struct NthFnData_Tag {
   GimvImageView *iv;
   gint       nth;
} NthFnData;


static gint
idle_gimv_image_view_nth (gpointer data)
{
   NthFnData *nth_fn_data = data;
   GimvImageView *iv = data;
   gint nth;

   g_return_val_if_fail (nth_fn_data, FALSE);

   iv = nth_fn_data->iv;
   nth = nth_fn_data->nth;

   g_free (nth_fn_data);
   nth_fn_data = NULL;
   data = NULL;

   g_return_val_if_fail (iv, FALSE);
   if (!iv->priv->image_list) return FALSE;
   if (!iv->priv->image_list->nth_fn) return FALSE;

   iv->priv->image_list->current
      = iv->priv->image_list->nth_fn (iv,
                                      iv->priv->image_list->owner,
                                      iv->priv->image_list->list,
                                      nth,
                                      iv->priv->image_list->list_fn_user_data);

   return FALSE;
}


void
gimv_image_view_nth (GimvImageView *iv, guint nth)
{
   NthFnData *nth_fn_data;

   g_return_if_fail (iv);

   nth_fn_data = g_new (NthFnData, 1);
   nth_fn_data->iv  = iv;
   nth_fn_data->nth = nth;

   g_idle_add (idle_gimv_image_view_nth, nth_fn_data);
}


static GList *
next_image (GimvImageView *iv,
            gpointer list_owner,
            GList *current,
            gpointer data)
{
   GList *next, *node;

   g_return_val_if_fail (iv, NULL);
   g_return_val_if_fail (iv == list_owner, NULL);
   g_return_val_if_fail (current, NULL);

   if (!iv->priv->image_list) return NULL;

   node = g_list_find (iv->priv->image_list->list, current->data);
   g_return_val_if_fail (node, NULL);

   next = g_list_next (current);
   if (!next)
      next = current;
   g_return_val_if_fail (next, NULL);

   if (next != current) {
      gimv_image_view_change_image (iv, next->data);
   }

   return next;
}


static GList *
prev_image (GimvImageView *iv,
            gpointer list_owner,
            GList *current,
            gpointer data)
{
   GList *prev, *node;

   g_return_val_if_fail (iv, NULL);
   g_return_val_if_fail (iv == list_owner, NULL);
   g_return_val_if_fail (current, NULL);

   if (!iv->priv->image_list) return NULL;

   node = g_list_find (iv->priv->image_list->list, current->data);
   g_return_val_if_fail (node, NULL);

   prev = g_list_previous (current);
   if (!prev)
      prev = current;
   g_return_val_if_fail (prev, NULL);

   if (prev != current) {
      gimv_image_view_change_image (iv, prev->data);
   }

   return prev;
}


static GList *
nth_image (GimvImageView *iv,
           gpointer list_owner,
           GList *list,
           guint nth,
           gpointer data)
{
   GList *node;

   g_return_val_if_fail (iv, NULL);
   g_return_val_if_fail (iv == list_owner, NULL);

   if (!iv->priv->image_list) return NULL;

   node = g_list_nth (iv->priv->image_list->list, nth);
   g_return_val_if_fail (node, NULL);

   gimv_image_view_change_image (iv, node->data);

   return node;
}


static void
remove_list (GimvImageView *iv, gpointer list_owner, gpointer data)
{
   g_return_if_fail (iv == list_owner);

   if (!iv->priv->image_list) return;

   g_list_foreach (iv->priv->image_list->list, (GFunc) gimv_image_info_unref, NULL);
   g_list_free (iv->priv->image_list->list);
   iv->priv->image_list->list = NULL;
   iv->priv->image_list->current = NULL;
}


void
gimv_image_view_set_list_self (GimvImageView *iv, GList *list, GList *current)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));
   g_return_if_fail (list);
 
   if (!current || !g_list_find (list, current->data))
      current = list;

   gimv_image_view_set_list (iv, list, current, iv,
                       next_image,
                       prev_image,
                       nth_image,
                       remove_list,
                       iv);
}


gboolean
gimv_image_view_has_list (GimvImageView *iv)
{
   if (iv->priv->image_list) {
      return TRUE;
   } else {
      return FALSE;
   }
}

void
gimv_image_view_playable_set_status (GimvImageView *iv,
                                     GimvImageViewPlayableStatus status)
{
   GimvImageViewPlayableIF *playable;
   GimvMenuItem *play, *stop, *pause, *forward, *reverse, *eject;
   GtkWidget *ifactory;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));
   if (!iv->draw_area_funcs || !iv->draw_area_funcs->playable) {
      player_update_time (iv);
      return;
   }

   playable = iv->draw_area_funcs->playable;

   if (iv->movie_menu && GTK_IS_WIDGET (iv->movie_menu)) {
      ifactory = (iv->movie_menu);
      play     = gimv_menu_get_item (ifactory, "/Play");
      stop     = gimv_menu_get_item (ifactory, "/Stop");
      pause    = gimv_menu_get_item (ifactory, "/Pause");
      forward  = gimv_menu_get_item (ifactory, "/Forward");
      reverse  = gimv_menu_get_item (ifactory, "/Reverse");
      eject    = gimv_menu_get_item (ifactory, "/Eject");
   } else {
      play = stop = pause = forward = reverse = eject = NULL;
   }

   if (status == GimvImageViewPlayableDisable) {
      gimv_icon_stock_change_widget_icon  (iv->player.play_icon, "play");
      gtk_widget_set_sensitive (iv->player.play,    FALSE);
      gtk_widget_set_sensitive (iv->player.stop,    FALSE);
      gtk_widget_set_sensitive (iv->player.fw,      FALSE);
      gtk_widget_set_sensitive (iv->player.rw,      FALSE);
      gtk_widget_set_sensitive (iv->player.eject,   FALSE);
      gtk_widget_set_sensitive (iv->player.seekbar, FALSE);
      if (play)    gimv_menu_item_set_sensitive (play,    FALSE);
      if (stop)    gimv_menu_item_set_sensitive (stop,    FALSE);
      if (pause)   gimv_menu_item_set_sensitive (pause,   FALSE);
      if (forward) gimv_menu_item_set_sensitive (forward, FALSE);
      if (reverse) gimv_menu_item_set_sensitive (reverse, FALSE);
      if (eject)   gimv_menu_item_set_sensitive (eject, FALSE);
      gimv_image_view_playable_set_position (iv, 0.0);
      return;

   } else {
      gboolean enable;

      enable = playable->play_fn ? TRUE : FALSE;
      gtk_widget_set_sensitive (iv->player.play,  enable);
      if (play) gimv_menu_item_set_sensitive (play, enable);

      enable = playable->pause_fn ? TRUE : FALSE;
      if (pause) gimv_menu_item_set_sensitive (pause, enable);

      enable = playable->stop_fn ? TRUE : FALSE;
      gtk_widget_set_sensitive (iv->player.stop,  enable);
      if (stop) gimv_menu_item_set_sensitive (stop, enable);

      enable = playable->forward_fn ? TRUE : FALSE;
      gtk_widget_set_sensitive (iv->player.fw,    enable);
      if (forward) gimv_menu_item_set_sensitive (forward, enable);

      enable = playable->reverse_fn ? TRUE : FALSE;
      gtk_widget_set_sensitive (iv->player.rw,    enable);
      if (reverse) gimv_menu_item_set_sensitive (reverse, enable);

      enable = playable->eject_fn ? TRUE : FALSE;
      gtk_widget_set_sensitive (iv->player.eject, enable);
      if (eject) gimv_menu_item_set_sensitive (eject, enable);

      if (playable->is_seekable_fn
          && playable->is_seekable_fn (iv)
          && playable->seek_fn)
      {
         gtk_widget_set_sensitive (iv->player.seekbar, TRUE);
      }
   }

   switch (status) {
   case GimvImageViewPlayableStop:
      gimv_image_view_playable_set_position (iv, 0.0);
      gtk_widget_set_sensitive (iv->player.stop,    FALSE);
      gtk_widget_set_sensitive (iv->player.fw,      FALSE);
      gtk_widget_set_sensitive (iv->player.rw,      FALSE);
      if (stop)    gimv_menu_item_set_sensitive (stop,    FALSE);
      if (pause)   gimv_menu_item_set_sensitive (pause,   FALSE);
      if (forward) gimv_menu_item_set_sensitive (forward, FALSE);
      if (reverse) gimv_menu_item_set_sensitive (reverse, FALSE);
      break;
   case GimvImageViewPlayableForward:
   case GimvImageViewPlayableReverse:
      gtk_widget_set_sensitive (iv->player.fw,      FALSE);
      gtk_widget_set_sensitive (iv->player.rw,      FALSE);
      if (pause)   gimv_menu_item_set_sensitive (pause,   FALSE);
      if (forward) gimv_menu_item_set_sensitive (forward, FALSE);
      if (reverse) gimv_menu_item_set_sensitive (reverse, FALSE);
      break;
   case GimvImageViewPlayablePlay:
      if (!playable->pause_fn) {
         gtk_widget_set_sensitive (iv->player.play, FALSE);
         if (pause) gimv_menu_item_set_sensitive (pause, FALSE);
      }
      break;
   case GimvImageViewPlayablePause:
   default:
      break;
   }

   if (status == GimvImageViewPlayablePlay && playable->pause_fn) {
      gimv_icon_stock_change_widget_icon (iv->player.play_icon, "pause");
   } else {
      gimv_icon_stock_change_widget_icon (iv->player.play_icon, "play");
   }

   player_update_time (iv);
}


void
gimv_image_view_playable_set_position (GimvImageView *iv, gfloat pos)
{
   GtkAdjustment *adj;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   adj = gtk_range_get_adjustment (GTK_RANGE (iv->player.seekbar));

   if (iv->priv && !(iv->priv->player_flags & GimvImageViewSeekBarDraggingFlag)) {
      gtk_adjustment_set_value (adj, pos);
   }

}
