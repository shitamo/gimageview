/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001 Takuro Ashie
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
 * $Id: gimv_image_win.c,v 1.19 2004/12/20 15:57:07 makeinu Exp $
 */

#include <stdlib.h>
#include <gtk/gtk.h>

#include "gimageview.h"

#include "charset.h"
#include "cursors.h"
#include "gtk2-compat.h"
#include "gtkutils.h"
#include "gimv_comment_view.h"
#include "gimv_icon_stock.h"
#include "gimv_image_view.h"
#include "gimv_image_win.h"
#include "gimv_prefs_win.h"
#include "gimv_thumb_win.h"
#include "help.h"
#include "menu.h"
#include "gimv_print.h"
#include "prefs.h"

#ifdef ENABLE_EXIF
#   include "exif_view.h"
#endif /* ENABLE_EXIF */


#define IMGWIN_FULLSCREEN_HIDE_CURSOR_DELAY 1000

enum {
   SHOW_FULLSCREEN_SIGNAL,
   HIDE_FULLSCREEN_SIGNAL,
   LAST_SIGNAL
};

enum {
   IMG_FIRST,
   IMG_PREV,
   IMG_NEXT,
   IMG_LAST
};

typedef enum {
   MOUSE_PRESS_NONE,
   MOUSE_PRESS_NEXT,
   MOUSE_PRESS_PREV,
   MOUSE_PRESS_POPUP,
   MOUSE_PRESS_ZOOM_IN,
   MOUSE_PRESS_ZOOM_OUT,
   MOUSE_PRESS_FIT,
   MOUSE_PRESS_ROTATE_CCW,
   MOUSE_PRESS_ROTATE_CW,
   MOUSE_PRESS_NAVWIN,
   MOUSE_PRESS_UP,
   MOUSE_PRESS_DOWN,
   MOUSE_PRESS_LEFT,
   MOUSE_PRESS_RIGHT
} ImageViewPressType;


/* color components are 0 - 65535 (as GdkColor of GTK2) */
typedef struct {
   gint red, green, blue;
} ImageWinColor;


struct GimvImageWinPriv_Tag
{
   /* flags */
   GimvImageWinFlags flags;
   GimvImageViewPlayerVisibleType player_visible;

   /* window geometory */
   gint             win_x;
   gint             win_y;
   gint             win_width;
   gint             win_height;

   ImageWinColor   *fs_bg_color;

   guint            hide_cursor_timer_id;

   guint            slideshow_interval;  /* [msec] */
   gint             slideshow_timer_id;
};


static void     gimv_image_win_class_init           (GimvImageWinClass *klass);
static void     gimv_image_win_init                 (GimvImageWin *iw);
static void     gimv_image_win_dispose              (GObject *object);
static void     gimv_image_win_realize              (GtkWidget *widget);
static void     gimv_image_win_real_show_fullscreen (GimvImageWin *iw);
static void     gimv_image_win_real_hide_fullscreen (GimvImageWin *iw);

static void       create_imageview_menus            (GimvImageWin  *iw);
static GtkWidget *create_toolbar                    (GimvImageWin  *iw,
                                                     GtkWidget     *container);
static GtkWidget *create_player_toolbar             (GimvImageWin  *iw,
                                                     GtkWidget     *container);

static void     gimv_image_win_set_win_size         (GimvImageWin  *iw);
static void     gimv_image_win_set_window_title     (GimvImageWin  *iw);
static void     gimv_image_win_set_statusbar_info   (GimvImageWin  *iw);

static void     gimv_image_win_fullscreen_show      (GimvImageWin *iw);
static void     gimv_image_win_fullscreen_hide      (GimvImageWin *iw);

/* callback functions for menubar */
static void     cb_file_select          (gpointer      data,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_open_imagewin        (gpointer      data,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_open_thumbwin        (gpointer      data,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_toggle_buffer        (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_window_close         (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_alpha_checker        (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem *widget);
static void     cb_ignore_alpha         (GimvImageWin *iv,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_toggle_menubar       (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_toggle_player        (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_toggle_toolbar       (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_toggle_statusbar     (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_toggle_scrollbar     (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_switch_player        (GimvImageWin *iw,
                                         GimvImageViewPlayerVisibleType visible,
                                         GimvMenuItem    *widget);
static void     cb_toggle_maximize      (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_toggle_fullscreen    (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_fit_to_image         (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_edit_comment         (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem    *widget);
#ifdef ENABLE_EXIF
static void     cb_exif                 (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem    *menuitem);
#endif /* ENABLE_EXIF */
static void     cb_print           (GimvImageWin   *iw,
                                    guint           action,
                                    GimvMenuItem   *menuitem);
static void     cb_create_thumb         (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem    *widget);
static void     cb_options              (GimvImageWin *iw);
static void     cb_move_menu            (GimvImageWin *iw,
                                         guint         action,
                                         GimvMenuItem    *widget);

/* callback functions for toolbar */
static void     cb_toolbar_open_button      (GtkWidget    *widget);
static void     cb_toolbar_prefs_button     (GtkWidget    *widget,
                                             GimvImageWin *iw);
static void     cb_toolbar_prev_button      (GtkWidget    *widget,
                                             GimvImageWin *iw);
static void     cb_toolbar_next_button      (GtkWidget    *widget,
                                             GimvImageWin *iw);
static void     cb_toolbar_no_zoom          (GtkWidget    *widget,
                                             GimvImageWin *iw);
static void     cb_toolbar_zoom_in          (GtkWidget    *widget,
                                             GimvImageWin *iw);
static void     cb_toolbar_zoom_out         (GtkWidget    *widget,
                                             GimvImageWin *iw);
static void     cb_toolbar_zoom_fit         (GtkWidget    *widget,
                                             GimvImageWin *iw);
static void     cb_toolbar_zoom             (GtkWidget    *widget,
                                             GimvImageWin *iw);
static void     cb_toolbar_keep_aspect      (GtkWidget    *widget,
                                             GimvImageWin *iw);
static void     cb_toolbar_fit_window       (GtkWidget    *widget,
                                             GimvImageWin *iw);
static void     cb_toolbar_fullscreen       (GtkWidget    *widget,
                                             GimvImageWin *iw);
static gboolean cb_scale_spinner_key_press  (GtkWidget      *widget, 
                                             GimvEventKey    *event,
                                             GimvImageWin   *iw);
static void     cb_rotate_menu              (GtkWidget      *widget,
                                             GimvImageWin   *iw);
static gboolean cb_rotate_menu_button_press (GtkWidget      *widget,
                                             GimvEventButton *event,
                                             GimvImageWin   *iw);

/* callback functions for slideshow toolbar */
static void     cb_play_clicked     (GtkWidget      *button,
                                     GimvImageWin   *iw);
static void     cb_stop_clicked     (GtkWidget      *button,
                                     GimvImageWin   *iw);
static void     cb_prev_clicked     (GtkWidget      *button,
                                     GimvImageWin   *iw);
static void     cb_next_clicked     (GtkWidget      *button,
                                     GimvImageWin   *iw);
static void     cb_first_clicked    (GtkWidget      *button,
                                     GimvImageWin   *iw);
static void     cb_last_clicked     (GtkWidget      *button,
                                     GimvImageWin   *iw);
static gboolean cb_seekbar_pressed  (GtkWidget      *widget,
                                     GimvEventButton *event,
                                     GimvImageWin   *iw);
static gboolean cb_seekbar_released (GtkWidget      *widget,
                                     GimvEventButton *event,
                                     GimvImageWin   *iw);

/* other callback functions */
static void     cb_image_changed       (GimvImageView *iv,
                                        GimvImageWin *iw);
static void     cb_load_start          (GimvImageView *iv,
                                        GimvImageInfo *info,
                                        GimvImageWin *iw);
static void     cb_load_end            (GimvImageView *iv,
                                        GimvImageInfo *info,
                                        gboolean cancel,
                                        GimvImageWin *iw);
static void     cb_set_list            (GimvImageView  *iv,
                                        GimvImageWin   *iw);
static void     cb_unset_list          (GimvImageView  *iv,
                                        GimvImageWin   *iw);
static void     cb_rendered            (GimvImageView  *iv,
                                        GimvImageWin   *iw);
static void     cb_toggle_aspect       (GimvImageView  *iv,
                                        gboolean        keep_aspect,
                                        GimvImageWin   *iw);
static gboolean cb_draw_area_key_press (GtkWidget      *widget, 
                                        GimvEventKey    *event,
                                        GimvImageWin   *iw);
static gint     cb_imageview_pressed   (GimvImageView  *iv,
                                        GimvEventButton *event,
                                        GimvImageWin   *iw);
static gint     cb_imageview_clicked   (GimvImageView  *iv,
                                        GimvEventButton *event,
                                        GimvImageWin   *iw);

static gint     gimv_image_view_button_action (GimvImageView  *iv,
                                               GimvEventButton *event,
                                               gint            num);

/* for main menu */
GimvMenuEntry gimv_image_win_menu_items[] =
{
   {N_("/_File"),                        NULL,         NULL,              0,  "<Branch>"},
   {N_("/_File/_Open..."),               "<control>F", cb_file_select,    0,  NULL},
   {N_("/_File/Open _Image Window"),     "<control>I", cb_open_imagewin,  0,  NULL},
   {N_("/_File/Open _Thumbnail Window"), "<control>W", cb_open_thumbwin,  0,  NULL},
   {N_("/_File/---"),                    NULL,         NULL,              0,  "<Separator>"},
   {N_("/_File/Memory _Buffer"),         "<control>B", cb_toggle_buffer,  0,  "<ToggleItem>"},
   {N_("/_File/---"),                    NULL,         NULL,              0,  "<Separator>"},
   {N_("/_File/_Print..."),              "<control>P", cb_print,          0,  NULL},
   {N_("/_File/---"),                    NULL,         NULL,              0,  "<Separator>"},
   {N_("/_File/_Close"),                 "Q",          cb_window_close,   0,  NULL},
   {N_("/_File/_Quit"),                  "<control>C", gimv_quit,         0,  NULL},

   {N_("/_Edit"),                        NULL,         NULL,              0,  "<Branch>"},
   {N_("/_Edit/Edit _Comment..."),       NULL,         cb_edit_comment,   0,  NULL},
   {N_("/_Edit/Scan E_XIF Data..."),       NULL,       cb_exif,           0,  NULL},
   {N_("/_Edit/Create _Thumbnail"),      "<shift>T",   cb_create_thumb,   0,  NULL},
   {N_("/_Edit/---"),                    NULL,         NULL,              0,  "<Separator>"},
   {N_("/_Edit/_Options..."),            "<control>O", cb_options,        0,  NULL},

   {N_("/_View"),                        NULL,         NULL,              0,  "<Branch>"},

   {N_("/_Go"),                        NULL,         NULL,              0,  "<Branch>"},

   {N_("/M_ovie"),                       NULL,         NULL,              0,  "<Branch>"},

   {N_("/_Help"),                        NULL,         NULL,              0,  "<LastBranch>"},
   {NULL, NULL, NULL, 0, NULL},
};


/* for "View" sub menu */
GimvMenuEntry gimv_image_win_view_items [] =
{
   {N_("/tear"),          NULL,        NULL,                 0,  "<Tearoff>"},
   {N_("/_Zoom"),         NULL,        NULL,                 0,  "<Branch>"},
   {N_("/_Rotate"),       NULL,        NULL,                 0,  "<Branch>"},
   {N_("/Ignore _Alpha Channel"), NULL,cb_ignore_alpha,      0,  "<ToggleItem>"},
   {N_("/_Checkerboard Behind Transparency"), NULL, cb_alpha_checker, 0, "<ToggleItem>"},
   {N_("/---"),           NULL,        NULL,                 0,  "<Separator>"},
   {N_("/_Menubar"),     "M",         cb_toggle_menubar,    0,  "<ToggleItem>"},
   {N_("/_Toolbar"),     "N",         cb_toggle_toolbar,    0,  "<ToggleItem>"},
   {N_("/Slideshow _Player"), "<shift>N", cb_toggle_player,     0,  "<ToggleItem>"},
   {N_("/St_atusbar"),   "V",         cb_toggle_statusbar,  0,  "<ToggleItem>"},
   {N_("/_Scrollbar"),   "<shift>S",  cb_toggle_scrollbar,  0,  "<ToggleItem>"},
   {N_("/_Player"),       NULL,        NULL,                 0,              "<Branch>"},  
   {N_("/_Player/_Show"), NULL,        cb_switch_player,     GimvImageViewPlayerVisibleShow, "<RadioItem>"},  
   {N_("/_Player/_Hide"), NULL,        cb_switch_player,     GimvImageViewPlayerVisibleHide, "/Player/Show"},  
   {N_("/_Player/_Auto"), NULL,        cb_switch_player,     GimvImageViewPlayerVisibleAuto, "/Player/Hide"},  
   {N_("/---"),           NULL,        NULL,                 0,  "<Separator>"},
   {N_("/_View Modes"),   NULL,        NULL,                 0, "<Branch>"},
   {N_("/---"),           NULL,        NULL,                 0,  "<Separator>"},
   {N_("/_Fit to Image"), "I",         cb_fit_to_image,      0,  NULL},
   {N_("/Ma_ximize"),     "F",         cb_toggle_maximize,   0,  "<ToggleItem>"},
   {N_("/F_ullscreen"),  "<shift>F",  cb_toggle_fullscreen, 0,  "<ToggleItem>"},
   {NULL, NULL, NULL, 0, NULL},
};


GimvMenuEntry gimv_image_win_move_items [] =
{
   {N_("/_Next"),        "L",  cb_move_menu,  IMG_NEXT,  NULL},
   {N_("/_Previous"),    "H",  cb_move_menu,  IMG_PREV,  NULL},
   {N_("/_First"),       "K",  cb_move_menu,  IMG_FIRST, NULL},
   {N_("/_Last"),        "J",  cb_move_menu,  IMG_LAST,  NULL},
   {NULL, NULL, NULL, 0, NULL},
};


static guint           gimv_image_win_signals[LAST_SIGNAL] = {0};


static GList         *ImageWinList   = NULL;
static GimvImageWin  *shared_img_win = NULL;


G_DEFINE_TYPE (GimvImageWin, gimv_image_win, GTK_TYPE_WINDOW)


static void
gimv_image_win_class_init (GimvImageWinClass *klass)
{
   GObjectClass *gobject_class;
   GtkWidgetClass *widget_class;

   gobject_class = G_OBJECT_CLASS (klass);
   widget_class = GTK_WIDGET_CLASS (klass);

   gimv_image_win_signals[SHOW_FULLSCREEN_SIGNAL]
      = g_signal_new ("show_fullscreen",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvImageWinClass, show_fullscreen),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);
   gimv_image_win_signals[HIDE_FULLSCREEN_SIGNAL]
      = g_signal_new ("hide_fullscreen",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvImageWinClass, hide_fullscreen),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

   gobject_class->dispose = gimv_image_win_dispose;

   widget_class->realize  = gimv_image_win_realize;

   klass->show_fullscreen = gimv_image_win_real_show_fullscreen;
   klass->hide_fullscreen = gimv_image_win_real_hide_fullscreen;
}


static void
gimv_image_win_init (GimvImageWin *iw)
{
   GtkWidget *vbox, *hbox;
   GtkAdjustment *adj;

   iw->priv = g_new0 (GimvImageWinPriv, 1);

   iw->iv = GIMV_IMAGE_VIEW (gimv_image_view_new (NULL));

   iw->priv->flags = 0;
   iw->priv->flags |= GimvImageWinCreatingFlag;
   if (conf.imgwin_show_menubar)
      iw->priv->flags |= GimvImageWinShowMenuBarFlag;
   if (conf.imgwin_show_toolbar)
      iw->priv->flags |= GimvImageWinShowToolBarFlag;
   if (conf.imgwin_show_player)
      iw->priv->flags |= GimvImageWinShowPlayerFlag;
   if (conf.imgwin_show_statusbar)
      iw->priv->flags |= GimvImageWinShowStatusBarFlag;
   /*
   iw->flags              &= ~GimvImageWinMaximizeFlag;
   iw->hide_frame         &= ~GimvImageWinHideFrameFlag;
   iw->seekbar_dragging   &= ~GimvImageWinSeekBarDraggingFlag;
   iw->is_movie           &= ~GimvImageWinMovieFlag;
   */
   iw->priv->player_visible = conf.imgview_player_visible;
   gimv_image_view_set_player_visible (iw->iv, iw->priv->player_visible);

   iw->fullscreen                 = NULL;
   iw->priv->hide_cursor_timer_id = 0;
   iw->priv->slideshow_interval   = conf.slideshow_interval * 1000;
   iw->priv->slideshow_timer_id   = 0;
   if (conf.slideshow_repeat)
      iw->priv->flags |= GimvImageWinSlideShowRepeatFlag;

   /* set bg color */
   if (conf.imgwin_set_bg) {
      gimv_image_view_set_bg_color (iw->iv,
                                    conf.imgwin_bg_color[0],
                                    conf.imgwin_bg_color[1],
                                    conf.imgwin_bg_color[2]);
   }

   if (conf.imgwin_fullscreen_set_bg) {
      iw->priv->fs_bg_color = g_new0 (ImageWinColor, 1);
      iw->priv->fs_bg_color->red   = conf.imgwin_fullscreen_bg_color[0];
      iw->priv->fs_bg_color->green = conf.imgwin_fullscreen_bg_color[1];
      iw->priv->fs_bg_color->blue  = conf.imgwin_fullscreen_bg_color[2];
   } else {
      iw->priv->fs_bg_color = NULL;
   }

   /* set window properties */
   gimv_icon_stock_set_window_icon (GTK_WIDGET (iw), "gimv_icon");
   gtk_window_set_resizable (GTK_WINDOW(iw), TRUE);
   gtk_window_set_default_size (GTK_WINDOW(iw),
                                conf.imgwin_width, conf.imgwin_height);
   gimv_image_win_set_window_title (iw);

   /* Main vbox */
   vbox = gimv_vbox_new (FALSE, 0);
   iw->main_vbox = vbox;
   if (iw == shared_img_win)
      gtk_widget_set_name (iw->main_vbox, "SharedImageWin");
   else
      gtk_widget_set_name (iw->main_vbox, "ImageWin");
   gimv_container_add (GTK_WIDGET (iw), vbox);
   gtk_widget_show (vbox);

   /* Menu Bar */
   /* GTK4: GtkHandleBox is gone */
   iw->menubar_handle = gimv_vbox_new (FALSE, 0);
   gtk_widget_set_name (iw->menubar_handle, "MenuBarContainer");
   gimv_container_set_border_width (GTK_WIDGET (iw->menubar_handle), 2);
   gimv_box_pack_start(GTK_BOX(vbox), iw->menubar_handle, FALSE, FALSE, 0);

   if (!(iw->priv->flags & GimvImageWinShowMenuBarFlag))
      gtk_widget_hide (iw->menubar_handle);
   else
      gtk_widget_show (iw->menubar_handle);

   /* toolbar */
   /* GTK4: GtkHandleBox is gone */
   iw->toolbar_handle = gimv_vbox_new (FALSE, 0);
   gtk_widget_set_name (iw->toolbar_handle, "ToolBarContainer");
   gimv_container_set_border_width (GTK_WIDGET (iw->toolbar_handle), 2);
   gimv_box_pack_start (GTK_BOX(vbox), iw->toolbar_handle, FALSE, FALSE, 0);
   iw->toolbar = create_toolbar (iw, iw->toolbar_handle);
   gimv_container_add (GTK_WIDGET (iw->toolbar_handle), iw->toolbar);
   gimv_widget_show_all (iw->toolbar);

   gtkutil_toolbar_set_style (iw->toolbar, conf.imgwin_toolbar_style);

   if (!(iw->priv->flags & GimvImageWinShowToolBarFlag))
      gtk_widget_hide (iw->toolbar_handle);
   else
      gtk_widget_show (iw->toolbar_handle);

   /* player toolbar */
   iw->player_handle = gimv_vbox_new (FALSE, 0);
   gtk_widget_set_name (iw->player_handle, "PlayerToolBarContainer");
   gimv_container_set_border_width (GTK_WIDGET (iw->player_handle), 2);
   gimv_box_pack_start(GTK_BOX(vbox), iw->player_handle, FALSE, FALSE, 0);

   hbox = gimv_hbox_new (FALSE, 0);
   gimv_container_add (GTK_WIDGET (iw->player_handle), hbox);
   gtk_widget_show (hbox);

   iw->player_bar = create_player_toolbar(iw, iw->player_handle);
   gimv_box_pack_start (GTK_BOX (hbox), iw->player_bar, FALSE, FALSE, 0);
   gimv_widget_show_all (iw->player_bar);

   adj = gtk_adjustment_new (0.0, 0.0, 100.0, 0.1, 1.0, 1.0);

   /* draw area */
   gimv_box_pack_start (GTK_BOX (iw->main_vbox), GTK_WIDGET (iw->iv),
                       TRUE, TRUE, 0);
   gtk_widget_show (GTK_WIDGET (iw->iv));

   g_signal_connect (G_OBJECT (iw->iv), "image_changed",
                       G_CALLBACK (cb_image_changed), iw);
   g_signal_connect (G_OBJECT (iw->iv), "load_start",
                       G_CALLBACK (cb_load_start), iw);
   g_signal_connect (G_OBJECT (iw->iv), "load_end",
                       G_CALLBACK (cb_load_end), iw);
   g_signal_connect (G_OBJECT (iw->iv), "set_list",
                       G_CALLBACK (cb_set_list), iw);
   g_signal_connect (G_OBJECT (iw->iv), "unset_list",
                       G_CALLBACK (cb_unset_list), iw);
   g_signal_connect (G_OBJECT (iw->iv), "rendered",
                       G_CALLBACK (cb_rendered), iw);
   g_signal_connect (G_OBJECT (iw->iv), "toggle_aspect",
                       G_CALLBACK (cb_toggle_aspect), iw);
   g_signal_connect (G_OBJECT (iw->iv), "image_pressed",
                       G_CALLBACK (cb_imageview_pressed), iw);
   g_signal_connect (G_OBJECT (iw->iv), "image_clicked",
                       G_CALLBACK (cb_imageview_clicked), iw);
   gimv_event_connect (GTK_WIDGET (iw->iv->draw_area), GIMV_EVENT_KEY_PRESS, G_CALLBACK (cb_draw_area_key_press), iw);

   iw->player.seekbar = gtk_scale_new (GTK_ORIENTATION_HORIZONTAL, GTK_ADJUSTMENT (adj));
   gtk_scale_set_draw_value (GTK_SCALE (iw->player.seekbar), FALSE);
   gimv_box_pack_start (GTK_BOX (hbox), iw->player.seekbar, TRUE, TRUE, 0);
   gtk_widget_show (iw->player.seekbar);

   gimv_event_connect (GTK_WIDGET (iw->player.seekbar), GIMV_EVENT_BUTTON_PRESS, G_CALLBACK (cb_seekbar_pressed), iw);
   gimv_event_connect (GTK_WIDGET (iw->player.seekbar), GIMV_EVENT_BUTTON_RELEASE, G_CALLBACK (cb_seekbar_released), iw);

   gtkutil_toolbar_set_style (iw->player_bar,
                              conf.imgwin_toolbar_style);

   if (!(iw->priv->flags & GimvImageWinShowPlayerFlag))
      gtk_widget_hide (iw->player_handle);
   else
      gtk_widget_show (iw->player_handle);

   /* status bar */
   hbox = gimv_hbox_new (FALSE, 0);
   iw->status_bar_container = hbox;
   gimv_box_pack_end (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
   gtk_widget_show (hbox);

   iw->status_bar1 = gtk_statusbar_new ();
   gtk_widget_set_name (iw->status_bar1, "StatuBar1");
   gimv_container_set_border_width (iw->status_bar1, 1);
   gimv_widget_set_size(iw->status_bar1, 200, -1);
   gimv_box_pack_start (GTK_BOX (hbox), iw->status_bar1, TRUE, TRUE, 0);
   gtk_statusbar_push(GTK_STATUSBAR (iw->status_bar1), 1, _("New Window"));
   gtk_widget_show (iw->status_bar1);

   iw->status_bar2 = gtk_statusbar_new ();
   gtk_widget_set_name (iw->status_bar1, "StatuBar2");
   gimv_container_set_border_width (iw->status_bar2, 1);
   gimv_widget_set_size(iw->status_bar2, 50, -1);
   gimv_box_pack_start (GTK_BOX (hbox), iw->status_bar2, TRUE, TRUE, 0);
   gtk_widget_show (iw->status_bar2);

   iw->progressbar = gtk_progress_bar_new ();
   gtk_widget_set_name (iw->progressbar, "ProgressBar");
   gimv_box_pack_end (GTK_BOX (hbox), iw->progressbar, FALSE, FALSE, 0);
   gtk_widget_show (iw->progressbar);

   gimv_image_view_set_progressbar (iw->iv, iw->progressbar);

   gimv_image_win_player_set_sensitive_all (iw, FALSE);

   if (!(iw->priv->flags & GimvImageWinShowStatusBarFlag))
      gtk_widget_hide (iw->status_bar_container);
   else
      gtk_widget_show (iw->status_bar_container);

   /* create menus */
   create_imageview_menus (iw);

   iw->priv->flags &= ~GimvImageWinCreatingFlag;

   gtk_widget_grab_focus (iw->iv->draw_area);

   ImageWinList = g_list_append (ImageWinList, iw);
}


GtkWidget *
gimv_image_win_new (GimvImageInfo *info)
{
   GimvImageWin *iw = g_object_new (GIMV_TYPE_IMAGE_WIN, NULL);

   gimv_image_view_change_image (iw->iv, info);

   return GTK_WIDGET (iw);
}


static void
gimv_image_win_dispose (GObject *object)
{
   gboolean was_listed;
   GimvImageWin *iw = GIMV_IMAGE_WIN (object);

   if (iw->fullscreen) {
      if (iw->iv)
         gimv_image_view_unset_fullscreen (iw->iv);
      gimv_widget_destroy (iw->fullscreen);
      iw->fullscreen = NULL;
   }

   if (iw->priv) {
      if (iw->priv->flags & GimvImageWinSlideShowPlayingFlag)
         gimv_image_win_slideshow_stop (iw);

      if (iw->priv->hide_cursor_timer_id != 0)
         g_source_remove (iw->priv->hide_cursor_timer_id);
      iw->priv->hide_cursor_timer_id = 0;

      if (g_list_length (ImageWinList) == 1 && conf.imgwin_save_win_state)
         gimv_image_win_save_state (iw);

      g_free (iw->priv->fs_bg_color);
      iw->priv->fs_bg_color = NULL;

      g_free (iw->priv);
      iw->priv = NULL;
   }

   if (iw->iv) {
      /* priv is gone: no more callbacks from the image view (GTK2 only
         disconnected "set_list" and "unset_list") */
      g_signal_handlers_disconnect_by_data (G_OBJECT (iw->iv), iw);
      if (iw->iv->draw_area)
         gimv_event_disconnect_by_func (iw->iv->draw_area,
                                        G_CALLBACK (cb_draw_area_key_press),
                                        iw);
      iw->iv = NULL;
   }

   /* update linked list */
   was_listed = g_list_find (ImageWinList, iw) != NULL;
   ImageWinList = g_list_remove (ImageWinList, iw);

   if (iw == shared_img_win)
      shared_img_win = NULL;

   G_OBJECT_CLASS (gimv_image_win_parent_class)->dispose (object);

   /* quit when last window (dispose may run more than once) */
   if (was_listed && !gimv_image_win_get_list() && !gimv_thumb_win_get_list()) {
      gimv_quit();
   }
}


static void
gimv_image_win_realize (GtkWidget *widget)
{
   GimvImageWin *iw = GIMV_IMAGE_WIN (widget);

   /* must be set before the surface is created */
   if (iw->priv && (iw->priv->flags & GimvImageWinHideFrameFlag))
      gtk_window_set_decorated (GTK_WINDOW (iw), FALSE);

   GTK_WIDGET_CLASS (gimv_image_win_parent_class)->realize (widget);
}


static void
create_imageview_menus (GimvImageWin *iw)
{
   guint n_menu_items;
   GtkWidget *zoom_menu, *rotate_menu, *movie_menu, *vmode_menu;
   gboolean show_scrollbar, keep_buffer;

   /* main menu */
   n_menu_items = sizeof (gimv_image_win_menu_items)
      / sizeof (gimv_image_win_menu_items[0]) - 1;
   iw->menubar = menubar_create (GTK_WIDGET (iw),
                                 gimv_image_win_menu_items, n_menu_items,
                                 "<ImageWinMainMenu>", iw);
   gimv_container_add (GTK_WIDGET (iw->menubar_handle), iw->menubar);
   gtk_widget_show (iw->menubar);

   /* sub menu */
   n_menu_items = sizeof(gimv_image_win_view_items)
      / sizeof(gimv_image_win_view_items[0]) - 1;
   iw->view_menu = menu_create_items(GTK_WIDGET (iw),
                                     gimv_image_win_view_items,
                                     n_menu_items, "<ImageWinViewSubMenu>", iw);
   n_menu_items = sizeof(gimv_image_win_move_items)
      / sizeof(gimv_image_win_move_items[0]) - 1;
   iw->move_menu = menu_create_items(GTK_WIDGET (iw),
                                     gimv_image_win_move_items,
                                     n_menu_items, "<ImageWinMoveSubMenu>", iw);

   zoom_menu = gimv_image_view_create_zoom_menu (GTK_WIDGET (iw),
                                                 iw->iv,
                                                 "<ImageWinZoomSubMenu>");
   rotate_menu = gimv_image_view_create_rotate_menu (GTK_WIDGET (iw),
                                                     iw->iv,
                                                     "<ImageWinRotateSubMenu>");

   movie_menu = gimv_image_view_create_movie_menu (GTK_WIDGET (iw),
                                                   iw->iv,
                                                   "<ImageWinMovieSubMenu>");
   vmode_menu = gimv_image_view_create_view_modes_menu (GTK_WIDGET (iw),
                                                        iw->iv,
                                                        "<ImageWinViewModesSubMenu>");
   iw->help_menu = gimvhelp_create_menu (GTK_WIDGET (iw));

   /* attach sub menus to parent menu */
   menu_set_submenu (iw->menubar,   "/View",       iw->view_menu);
   menu_set_submenu (iw->menubar,   "/Go",       iw->move_menu);
   menu_set_submenu (iw->menubar,   "/Help",       iw->help_menu);
   menu_set_submenu (iw->view_menu, "/Zoom",       zoom_menu);
   menu_set_submenu (iw->view_menu, "/Rotate",     rotate_menu);
   menu_set_submenu (iw->menubar,   "/Movie",      movie_menu);
   menu_set_submenu (iw->view_menu, "/View Modes", vmode_menu);

   /* popup menu */
   gimv_image_view_set_menu_ptr (iw->iv, &iw->iv->imageview_popup,
                                 iw->view_menu);

   /* initialize menubar check items */
   g_object_get (G_OBJECT (iw->iv),
                   "show_scrollbar", &show_scrollbar,
                   "keep_buffer",    &keep_buffer,
                   NULL);

   menu_check_item_set_active (iw->menubar, "/File/Memory Buffer", keep_buffer);

   menu_check_item_set_active (iw->view_menu, "/Menubar",
                               iw->priv->flags & GimvImageWinShowMenuBarFlag);
   menu_check_item_set_active (iw->view_menu, "/Toolbar",
                               iw->priv->flags & GimvImageWinShowToolBarFlag);
   menu_check_item_set_active (iw->view_menu, "/Slideshow Player",
                               iw->priv->flags & GimvImageWinShowPlayerFlag);
   menu_check_item_set_active (iw->view_menu, "/Statusbar",
                               iw->priv->flags & GimvImageWinShowStatusBarFlag);
   menu_check_item_set_active (iw->view_menu, "/Scrollbar",  show_scrollbar);
   menu_check_item_set_active (iw->view_menu, "/Checkerboard Behind Transparency",
                               gimv_image_view_get_alpha_checker (iw->iv));
   menu_check_item_set_active (iw->view_menu, "/Maximize",
                               iw->priv->flags & GimvImageWinMaximizeFlag);
   menu_check_item_set_active (iw->view_menu, "/Fullscreen",
                               iw->priv->flags & GimvImageWinFullScreenFlag);

   menu_item_set_sensitive (iw->move_menu, "/Next",     FALSE);
   menu_item_set_sensitive (iw->move_menu, "/Previous", FALSE);
   menu_item_set_sensitive (iw->move_menu, "/First",    FALSE);
   menu_item_set_sensitive (iw->move_menu, "/Last",     FALSE);

   switch (iw->priv->player_visible) {
   case GimvImageViewPlayerVisibleShow:
      menu_check_item_set_active (iw->view_menu, "/Player/Show", TRUE);
      break;
   case GimvImageViewPlayerVisibleHide:
      menu_check_item_set_active (iw->view_menu, "/Player/Hide", TRUE);
      break;
   case GimvImageViewPlayerVisibleAuto:
   default:
      menu_check_item_set_active (iw->view_menu, "/Player/Auto", TRUE);
      break;
   }
}


/******************************************************************************
 *
 *   toolbar helpers (GTK4: GtkToolbar is gone).
 *
 ******************************************************************************/
static GtkWidget *
toolbar_append_item (GtkWidget *toolbar,
                     const gchar *text,
                     const gchar *tooltip,
                     GtkWidget *icon,
                     GCallback callback,
                     gpointer data)
{
   GtkWidget *button, *vbox, *label;

   button = gtk_button_new ();
   gtk_button_set_has_frame (GTK_BUTTON (button), FALSE);
   gtk_widget_set_focus_on_click (button, FALSE);

   vbox = gimv_vbox_new (FALSE, 0);
   gtk_button_set_child (GTK_BUTTON (button), vbox);
   if (icon)
      gimv_box_pack_start (GTK_BOX (vbox), icon, TRUE, TRUE, 0);
   label = gtk_label_new (text);
   gimv_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);

   if (tooltip)
      gtk_widget_set_tooltip_text (button, tooltip);
   if (callback)
      g_signal_connect (button, "clicked", callback, data);

   gimv_box_pack_start (GTK_BOX (toolbar), button, FALSE, FALSE, 0);

   return button;
}


static void
toolbar_append_space (GtkWidget *toolbar)
{
   GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_VERTICAL);

   gtk_widget_set_margin_start (sep, 4);
   gtk_widget_set_margin_end (sep, 4);
   gimv_box_pack_start (GTK_BOX (toolbar), sep, FALSE, FALSE, 0);
}


static void
toolbar_append_widget (GtkWidget *toolbar,
                       GtkWidget *widget,
                       const gchar *tooltip)
{
   if (tooltip)
      gtk_widget_set_tooltip_text (widget, tooltip);
   gtk_widget_set_valign (widget, GTK_ALIGN_CENTER);
   gimv_box_pack_start (GTK_BOX (toolbar), widget, FALSE, FALSE, 0);
}


static GtkWidget *
create_toolbar (GimvImageWin *iw, GtkWidget *container)
{
   GtkWidget *toolbar, *button, *spinner, *iconw, *menu;
   GtkAdjustment *adj;
   const gchar *rotate_labels[] = {
      N_("90 degrees counterclockwise"),
      N_("0 degrees"),
      N_("90 degrees clockwise"),
      N_("180 degrees"),
      NULL,
   };
   gfloat x_scale, y_scale;

   toolbar = gtkutil_create_toolbar ();

   /* file open button */
   iconw = gimv_icon_stock_get_widget ("nfolder");
   button = toolbar_append_item (toolbar,
                                 _("Open"),
                                 _("Open File"),
                                 iconw,
                                 G_CALLBACK (cb_toolbar_open_button),
                                 NULL);

   /* preference button */
   iconw = gimv_icon_stock_get_widget ("prefs");
   button = toolbar_append_item (toolbar,
                                 _("Prefs"),
                                 _("Preferences"),
                                 iconw,
                                 G_CALLBACK (cb_toolbar_prefs_button),
                                 iw);

   toolbar_append_space (toolbar);

   /* prev button */
   iconw = gimv_icon_stock_get_widget ("back");
   button = toolbar_append_item (toolbar,
                                 _("Prev"),
                                 _("Previous Image"),
                                 iconw,
                                 G_CALLBACK (cb_toolbar_prev_button),
                                 iw);
   iw->button.prev = button;
   /* gtk_widget_set_sensitive (button, FALSE); */

   /* next button */
   iconw = gimv_icon_stock_get_widget ("forward");
   button = toolbar_append_item (toolbar,
                                 _("Next"),
                                 _("Next Image"),
                                 iconw,
                                 G_CALLBACK (cb_toolbar_next_button),
                                 iw);
   iw->button.next = button;
   /* gtk_widget_set_sensitive (button, FALSE); */

   toolbar_append_space (toolbar);

   /* no zoom button */
   iconw = gimv_icon_stock_get_widget ("no_zoom");
   button = toolbar_append_item (toolbar,
                                 _("No Zoom"),
                                 _("No Zoom"),
                                 iconw,
                                 G_CALLBACK (cb_toolbar_no_zoom),
                                 iw);

   /* zoom in button */
   iconw = gimv_icon_stock_get_widget ("zoom_in");
   button = toolbar_append_item (toolbar,
                                 _("Zoom in"),
                                 _("Zoom in"),
                                 iconw,
                                 G_CALLBACK (cb_toolbar_zoom_in),
                                 iw);

   /* zoom out button */
   iconw = gimv_icon_stock_get_widget ("zoom_out");
   button = toolbar_append_item (toolbar,
                                 _("Zoom out"),
                                 _("Zoom out"),
                                 iconw,
                                 G_CALLBACK (cb_toolbar_zoom_out),
                                 iw);

   /* zoom fit button */
   iconw = gimv_icon_stock_get_widget ("zoom_fit");
   button = toolbar_append_item (toolbar,
                                 _("Zoom to fit"),
                                 _("Zoom to fit"),
                                 iconw,
                                 G_CALLBACK (cb_toolbar_zoom_fit),
                                 iw);

   g_object_get (G_OBJECT (iw->iv),
                   "x_scale", &x_scale,
                   "y_scale", &y_scale,
                   NULL);

   /* x scale spinner */
   adj = (GtkAdjustment *) gtk_adjustment_new (x_scale,
                                               GIMV_THUMB_WIN_MIN_THUMB_SIZE,
                                               GIMV_THUMB_WIN_MAX_THUMB_SIZE,
                                               1.0, 5.0, 0.0);
   spinner = gtkutil_create_spin_button (adj);
   gtk_widget_set_name (spinner, "XScaleSpinner");
   toolbar_append_widget (toolbar, spinner, _("X Scale"));
   g_signal_connect (G_OBJECT (adj), "value_changed",
                       G_CALLBACK (cb_toolbar_keep_aspect), iw);
   iw->button.xscale = spinner;
   gimv_event_connect (GTK_WIDGET (spinner), GIMV_EVENT_KEY_PRESS, G_CALLBACK(cb_scale_spinner_key_press), iw);

   /* y scale spinner */
   adj = (GtkAdjustment *) gtk_adjustment_new (y_scale,
                                               GIMV_THUMB_WIN_MIN_THUMB_SIZE,
                                               GIMV_THUMB_WIN_MAX_THUMB_SIZE,
                                               1.0, 5.0, 0.0);
   spinner = gtkutil_create_spin_button (adj);
   gtk_widget_set_name (spinner, "YScaleSpinner");
   toolbar_append_widget (toolbar, spinner, _("Y Scale"));
   iw->button.yscale = spinner;

   /* zoom button */
   iconw = gimv_icon_stock_get_widget ("zoom");
   button = toolbar_append_item (toolbar,
                                 _("Zoom"),
                                 _("Zoom"),
                                 iconw,
                                 G_CALLBACK (cb_toolbar_zoom),
                                 iw);

   toolbar_append_space (toolbar);

   /* rotate button */
   menu = create_option_menu (rotate_labels,
                              1, cb_rotate_menu, iw);
   gimv_event_connect (GTK_WIDGET (menu), GIMV_EVENT_BUTTON_PRESS, G_CALLBACK (cb_rotate_menu_button_press), iw);
   toolbar_append_widget (toolbar, menu, _("Rotate the image"));
   iw->button.rotate = menu;

   toolbar_append_space (toolbar);

   /* resize button */
   iconw = gimv_icon_stock_get_widget ("resize");
   button = toolbar_append_item (toolbar,
                                 _("Resize"),
                                 _("Fit Window Size to Image"),
                                 iconw,
                                 G_CALLBACK (cb_toolbar_fit_window),
                                 iw);

   /* fullscreen button */
   iconw = gimv_icon_stock_get_widget ("fullscreen");
   /* button = gtk_toggle_button_new (); */
   button = toolbar_append_item (toolbar,
                                 _("Full"),
                                 _("Fullscreen"),
                                 iconw,
                                 G_CALLBACK (cb_toolbar_fullscreen),
                                 iw);

   gtk_widget_set_sensitive (iw->button.prev, FALSE);
   gtk_widget_set_sensitive (iw->button.next, FALSE);

   return toolbar;
}


static GtkWidget *
create_player_toolbar (GimvImageWin *iw, GtkWidget *container)
{
   GtkWidget *toolbar;
   GtkWidget *button;
   GtkWidget *iconw;

   toolbar = gtkutil_create_toolbar ();

   /* previous button */
   iconw = gimv_icon_stock_get_widget ("prev_t");
   button = toolbar_append_item (toolbar,
                                 _("First"),
                                 _("First"),
                                 iconw,
                                 G_CALLBACK (cb_first_clicked),
                                 iw);
   iw->player.prev = button;

   /* Reverse button */
   iconw = gimv_icon_stock_get_widget ("rw");
   button = toolbar_append_item (toolbar,
                                 _("Prev"),
                                 _("Previous"),
                                 iconw,
                                 G_CALLBACK (cb_prev_clicked),
                                 iw);
   iw->player.rw = button;

   /* play button */
   iconw = gimv_icon_stock_get_widget ("play");
   button = toolbar_append_item (toolbar,
                                 _("Play"),
                                 _("Play Slideshow"),
                                 iconw,
                                 G_CALLBACK (cb_play_clicked),
                                 iw);
   iw->player.play = button;

   /* stop button */
   iconw = gimv_icon_stock_get_widget ("stop2");
   button = toolbar_append_item (toolbar,
                                 _("Stop"),
                                 _("Stop Slideshow"),
                                 iconw,
                                 G_CALLBACK (cb_stop_clicked),
                                 iw);
   iw->player.stop = button;

   /* Forward button */
   iconw = gimv_icon_stock_get_widget ("ff");
   button = toolbar_append_item (toolbar,
                                 _("Next"),
                                 _("Next"),
                                 iconw,
                                 G_CALLBACK (cb_next_clicked),
                                 iw);
   iw->player.fw = button;

   /* Next button */
   iconw = gimv_icon_stock_get_widget ("next_t");
   button = toolbar_append_item (toolbar,
                                 _("Last"),
                                 _("Last"),
                                 iconw,
                                 G_CALLBACK (cb_last_clicked),
                                 iw);
   iw->player.next = button;

   return toolbar;
}


static void
gimv_image_win_set_win_size (GimvImageWin *iw)
{
   gint x_size, y_size, width, height;
   gfloat x_scale, y_scale, tmp;
   GimvImageViewOrientation rotate;

   if (!GIMV_IS_IMAGE_WIN (iw)) return;
   if (iw->priv->flags & GimvImageWinMaximizeFlag) return;
   if (iw->priv->flags & GimvImageWinFullScreenFlag) return;

   g_object_get (G_OBJECT (iw->iv),
                   "x_scale", &x_scale,
                   "y_scale", &y_scale,
                   NULL);
   if (x_scale < 0.001 || y_scale < 0.001) return;

   rotate = gimv_image_view_get_orientation (iw->iv);
   if (rotate == 0 || rotate == 2) {
      width =  iw->iv->info->width;
      height = iw->iv->info->height;
   } else {
      tmp = x_scale;
      x_scale = y_scale;
      y_scale = tmp;
      width  = iw->iv->info->height;
      height = iw->iv->info->width;
   }

   x_size = width  * x_scale / 100.0 + 0.5;
   y_size = height * y_scale / 100.0 + 0.5;

   if (iw->priv->flags & GimvImageWinShowMenuBarFlag)
      y_size += gtk_widget_get_height (GTK_WIDGET (iw->menubar_handle));

   if (iw->priv->flags & GimvImageWinShowToolBarFlag)
      y_size += gtk_widget_get_height (GTK_WIDGET (iw->toolbar_handle));

   if (iw->priv->flags & GimvImageWinShowPlayerFlag)
      y_size += gtk_widget_get_height (GTK_WIDGET (iw->player_handle));

   if (iw->priv->flags & GimvImageWinShowStatusBarFlag)
      y_size += gtk_widget_get_height (GTK_WIDGET (iw->status_bar_container));

   if (gtk_widget_get_visible (GTK_WIDGET (iw->iv->player_container)))
      y_size += gtk_widget_get_height (GTK_WIDGET (iw->iv->player_container));

   /* GTK4: no room for the scrollbars: they are hidden when the image fits */

   if (x_size < 1)
      x_size = conf.imgwin_width;
   if (y_size < 1)
      y_size = conf.imgwin_height;

   /* GTK4: resize through the default size */
   gtk_window_set_default_size (GTK_WINDOW (iw), x_size, y_size);
   gimv_image_view_set_view_position (iw->iv, 0, 0);
   gimv_image_view_draw_image (iw->iv);
}


static void
gimv_image_win_set_window_title (GimvImageWin *iw)
{
   gchar buf[BUF_SIZE];
   const gchar *filename = NULL;
   gchar *dirname = NULL, *tmpstr1, *tmpstr2;
   gboolean keep_buffer;

   g_return_if_fail (iw);
   if (!g_list_find (ImageWinList, iw)) return;

   g_return_if_fail (iw->iv);
   if (!g_list_find (gimv_image_view_get_list(), iw->iv)) return;

   if (iw->iv->info && gimv_image_info_get_path (iw->iv->info)) {
      filename = g_basename (iw->iv->info->filename);
      dirname = g_path_get_dirname (iw->iv->info->filename);
   } else {
      gtk_window_set_title (GTK_WINDOW (iw), GIMV_PROG_NAME);
      return;
   }

   tmpstr1 = charset_to_internal (filename, conf.charset_filename,
                                  conf.charset_auto_detect_fn,
                                  conf.charset_filename_mode);
   tmpstr2 = charset_to_internal (dirname, conf.charset_filename,
                                  conf.charset_auto_detect_fn,
                                  conf.charset_filename_mode);

   g_object_get (G_OBJECT (iw->iv),
                   "keep_buffer", &keep_buffer,
                   NULL);

   if (tmpstr1 && tmpstr2 && *tmpstr1 && *tmpstr2) {
      if (keep_buffer)
         g_snprintf (buf, BUF_SIZE, _("%s (%s) - %s - "),
                     tmpstr1, tmpstr2, GIMV_PROG_NAME);
      else
         g_snprintf (buf, BUF_SIZE, _("* %s (%s) - %s - *"),
                     tmpstr1, tmpstr2, GIMV_PROG_NAME);
      gtk_window_set_title (GTK_WINDOW (iw), buf);
   } else {
      if (keep_buffer)
         g_snprintf (buf, BUF_SIZE, _("- %s -"), GIMV_PROG_NAME);
      else
         g_snprintf (buf, BUF_SIZE, _("* - %s - *"), GIMV_PROG_NAME);
      gtk_window_set_title (GTK_WINDOW (iw), buf);
   }

   g_free (tmpstr1);
   g_free (tmpstr2);
   g_free (dirname);
}


static void
gimv_image_win_set_statusbar_info (GimvImageWin *iw)
{
   gchar buf[BUF_SIZE], *tmpstr;
   const gchar *filename = _("NONE");
   const gchar *format = _("UNKNOWN");
   gint width = 0, height = 0;
   gboolean keep_buffer;

   g_return_if_fail (iw);
   if (!g_list_find (ImageWinList, iw)) return;

   g_return_if_fail (iw->iv);
   if (!g_list_find (gimv_image_view_get_list(), iw->iv)) return;

   if (iw->iv->info) {
      filename = gimv_image_info_get_path (iw->iv->info);
      format = gimv_image_info_get_format (iw->iv->info);
      gimv_image_info_get_image_size (iw->iv->info, &width, &height);
   }

   tmpstr = charset_to_internal (filename, conf.charset_filename,
                                 conf.charset_auto_detect_fn,
                                 conf.charset_filename_mode);

   g_object_get (G_OBJECT (iw->iv),
                   "keep_buffer", &keep_buffer,
                   NULL);

   if (tmpstr && *tmpstr) {
      if (keep_buffer)
         g_snprintf (buf, BUF_SIZE, _("%s (Buffer ON)"), tmpstr);
      else
         g_snprintf (buf, BUF_SIZE, _("%s (Buffer OFF)"), tmpstr);

      gtk_statusbar_push (GTK_STATUSBAR (iw->status_bar1), 1, buf);
   } else {
      if (keep_buffer)
         g_snprintf (buf, BUF_SIZE, _("(Buffer ON)"));
      else
         g_snprintf (buf, BUF_SIZE, _("(Buffer OFF)"));
      gtk_statusbar_push (GTK_STATUSBAR (iw->status_bar1), 1, buf);
   }
   g_free (tmpstr);

   g_snprintf (buf, BUF_SIZE, "%s %d x %d", format, width, height);
   gtk_statusbar_push (GTK_STATUSBAR (iw->status_bar2), 1, buf);
}



/******************************************************************************
 *
 *   Callback functions for menubar.
 *
 ******************************************************************************/
static void
cb_file_select (gpointer data, guint action, GimvMenuItem *widget)
{
   create_filebrowser (NULL);
}


static void
cb_open_imagewin (gpointer data, guint action, GimvMenuItem *widget)
{
   gimv_image_win_open_window (NULL);   
}


static void
cb_open_thumbwin (gpointer data, guint action, GimvMenuItem *widget)
{
   gimv_thumb_win_open_window();
}


static void
cb_toggle_buffer (GimvImageWin *iw, guint action, GimvMenuItem *widget)
{
   gboolean keep_buffer;

   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));

   if (gimv_menu_item_get_active (GIMV_MENU_ITEM (widget))) {
      gimv_image_view_load_image_buf (iw->iv);
      keep_buffer = TRUE;
   } else {
      keep_buffer = FALSE;
      gimv_image_view_free_image_buf (iw->iv);
   }

   g_object_set (G_OBJECT (iw->iv),
                   "keep_buffer", keep_buffer,
                   NULL);

   gimv_image_win_set_window_title (iw);
   gimv_image_win_set_statusbar_info (iw);
}


static void
cb_window_close (GimvImageWin *iw, guint action, GimvMenuItem *widget)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));

   if (iw->priv->flags & GimvImageWinCreatingFlag) return;

   if (iw->priv->flags & GimvImageWinFullScreenFlag) {
      menu_check_item_set_active (iw->view_menu, "/Fullscreen", FALSE);
      return; /* or close window completly? */
   }

   gimv_widget_destroy (GTK_WIDGET (iw));
}


static void
cb_alpha_checker (GimvImageWin *iw, guint action, GimvMenuItem *widget)
{
   gboolean active = gimv_menu_item_get_active (GIMV_MENU_ITEM (widget));

   g_return_if_fail (GIMV_IS_IMAGE_WIN (iw));

   conf.imgview_alpha_checker = active;
   gimv_image_view_set_alpha_checker (iw->iv, active);
}


static void
cb_ignore_alpha (GimvImageWin *iw, guint action, GimvMenuItem *widget)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));

   g_object_set (G_OBJECT (iw->iv),
                   "ignore_alpha", gimv_menu_item_get_active (GIMV_MENU_ITEM (widget)),
                   NULL);
   gimv_image_view_show_image (iw->iv);
}


static void
cb_toggle_menubar (GimvImageWin *iw, guint action, GimvMenuItem *widget)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));

   if (gimv_menu_item_get_active (GIMV_MENU_ITEM (widget))) {
      gtk_widget_show (iw->menubar_handle);
      iw->priv->flags |= GimvImageWinShowMenuBarFlag;
   } else {
      gtk_widget_hide (iw->menubar_handle);
      iw->priv->flags &= ~GimvImageWinShowMenuBarFlag;
   }
}


static void
cb_toggle_player (GimvImageWin *iw, guint action, GimvMenuItem *widget)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));

   if (gimv_menu_item_get_active (GIMV_MENU_ITEM (widget))) {
      gtk_widget_show (iw->player_handle);
      iw->priv->flags |= GimvImageWinShowPlayerFlag;
   } else {
      gtk_widget_hide (iw->player_handle);
      iw->priv->flags &= ~GimvImageWinShowPlayerFlag;
   }
}


static void
cb_toggle_toolbar (GimvImageWin *iw, guint action, GimvMenuItem *widget)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));

   if (gimv_menu_item_get_active (GIMV_MENU_ITEM (widget))) {
      gtk_widget_show (iw->toolbar_handle);
      iw->priv->flags |= GimvImageWinShowToolBarFlag;
   } else {
      gtk_widget_hide (iw->toolbar_handle);
      iw->priv->flags &= ~GimvImageWinShowToolBarFlag;
   }
}


static void
cb_toggle_statusbar (GimvImageWin *iw, guint action, GimvMenuItem *widget)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));

   if (gimv_menu_item_get_active (GIMV_MENU_ITEM (widget))) {
      gtk_widget_show (iw->status_bar_container);
      iw->priv->flags |= GimvImageWinShowStatusBarFlag;
   } else {
      gtk_widget_hide (iw->status_bar_container);
      iw->priv->flags &= ~GimvImageWinShowStatusBarFlag;
   }
}


static void
cb_toggle_scrollbar (GimvImageWin *iw, guint action, GimvMenuItem *widget)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));

   if (gimv_menu_item_get_active (GIMV_MENU_ITEM (widget))) {
      gimv_image_view_show_scrollbar (iw->iv);
   } else {
      gimv_image_view_hide_scrollbar (iw->iv);
   }
}


static void
cb_switch_player (GimvImageWin *iw,
                  GimvImageViewPlayerVisibleType visible,
                  GimvMenuItem     *widget)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iw->iv));

   gimv_image_view_set_player_visible (iw->iv, visible);
   iw->priv->player_visible = gimv_image_view_get_player_visible (iw->iv);
}


static void
cb_toggle_maximize (GimvImageWin *iw, guint action, GimvMenuItem *widget)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));

   /* GTK4: window geometry can't be controlled directly, use the window
      manager's maximize state instead */
   if (gimv_menu_item_get_active (GIMV_MENU_ITEM (widget))) {
      iw->priv->flags |= GimvImageWinMaximizeFlag;
      iw->priv->win_x = 0;
      iw->priv->win_y = 0;
      /* GTK4: the default size follows the (unmaximized) window size */
      gtk_window_get_default_size (GTK_WINDOW (iw),
                                   &iw->priv->win_width, &iw->priv->win_height);

      gtk_window_maximize (GTK_WINDOW (iw));
      gtk_window_present (GTK_WINDOW (iw));
   } else {
      gtk_window_unmaximize (GTK_WINDOW (iw));
      gtk_window_set_default_size (GTK_WINDOW (iw),
                                   iw->priv->win_width, iw->priv->win_height);
      iw->priv->flags &= ~GimvImageWinMaximizeFlag;
   }
}


static void
cb_toggle_fullscreen (GimvImageWin *iw, guint action, GimvMenuItem *widget)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));

   if (gimv_menu_item_get_active (GIMV_MENU_ITEM (widget))) {
      gimv_image_win_fullscreen_show (iw);
   } else {
      gimv_image_win_fullscreen_hide (iw);
   }
}


static void
cb_fit_to_image (GimvImageWin *iw, guint action, GimvMenuItem *widget)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));

   gimv_image_win_set_win_size (iw);
}


static void
cb_edit_comment (GimvImageWin *iw, guint action, GimvMenuItem *menuitem)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));
   g_return_if_fail (iw->iv);

   if (iw->iv->info)
      gimv_comment_view_create_window (iw->iv->info);
}


#ifdef ENABLE_EXIF
static void
cb_exif (GimvImageWin *iw, guint action, GimvMenuItem *menuitem)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));

   if (iw->iv->info)
      exif_view_create_window (gimv_image_info_get_path (iw->iv->info),
                               GTK_WINDOW (iw));
}
#endif /* ENABLE_EXIF */


static void
cb_create_thumb (GimvImageWin *iw,
                 guint        action,
                 GimvMenuItem   *widget)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));
   g_return_if_fail (iw->iv);

   gimv_image_view_create_thumbnail (iw->iv);
}


static void
cb_print (GimvImageWin *iw, guint action, GimvMenuItem *menuitem)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN (iw));
   g_return_if_fail (iw->iv);

   gimv_print_image_view (iw->iv, GTK_WINDOW (iw));
}


static void
cb_options (GimvImageWin *iw)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));

   gimv_prefs_win_open_idle ("/Image Window", GTK_WINDOW (iw));
}


static void
cb_move_menu (GimvImageWin *iw,
              guint        action,
              GimvMenuItem   *widget)
{
   gint num;

   g_return_if_fail (GIMV_IS_IMAGE_WIN(iw));

   switch (action) {
   case IMG_FIRST:
      gimv_image_view_nth (iw->iv, 0);
      break;
   case IMG_PREV:
      gimv_image_view_prev (iw->iv);
      break;
   case IMG_NEXT:
      gimv_image_view_next (iw->iv);
      break;
   case IMG_LAST:
      if (!gimv_image_view_has_list (iw->iv)) return;
      num = gimv_image_view_image_list_length (iw->iv);
      gimv_image_view_nth (iw->iv, num - 1);
      break;
   default:
      break;
   }
}


/******************************************************************************
 *
 *  callback functions for toolbar.
 *
 ******************************************************************************/
static void
cb_toolbar_open_button (GtkWidget *widget)
{
   create_filebrowser (NULL);
}


static void
cb_toolbar_prefs_button (GtkWidget *widget, GimvImageWin *iw)
{
   gimv_prefs_win_open_idle ("/Image Window", GTK_WINDOW (iw));
}


static void
cb_toolbar_prev_button  (GtkWidget *widget, GimvImageWin *iw)
{
   g_return_if_fail (iw);
   gimv_image_view_prev (iw->iv);
}


static void
cb_toolbar_next_button  (GtkWidget *widget, GimvImageWin *iw)
{
   g_return_if_fail (iw);
   gimv_image_view_next (iw->iv);
}


static void
cb_toolbar_no_zoom (GtkWidget *widget, GimvImageWin *iw)
{
   g_return_if_fail (iw);
   gimv_image_view_zoom_image (iw->iv, GIMV_IMAGE_VIEW_ZOOM_100, 0, 0);
}


static void
cb_toolbar_zoom_in (GtkWidget *widget, GimvImageWin *iw)
{
   g_return_if_fail (iw);
   gimv_image_view_zoom_image (iw->iv, GIMV_IMAGE_VIEW_ZOOM_IN, 0, 0);
}


static void
cb_toolbar_zoom_out (GtkWidget *widget, GimvImageWin *iw)
{
   g_return_if_fail (iw);
   gimv_image_view_zoom_image (iw->iv, GIMV_IMAGE_VIEW_ZOOM_OUT, 0, 0);
}


static void
cb_toolbar_zoom_fit (GtkWidget *widget, GimvImageWin *iw)
{
   g_return_if_fail (iw);
   gimv_image_view_zoom_image (iw->iv, GIMV_IMAGE_VIEW_ZOOM_FIT, 0, 0);
}


static void
cb_toolbar_zoom (GtkWidget *widget, GimvImageWin *iw)
{
   gfloat x_scale, y_scale;
   GtkSpinButton *xspin, *yspin;

   g_return_if_fail (iw);

   xspin = GTK_SPIN_BUTTON(iw->button.xscale);
   yspin = GTK_SPIN_BUTTON(iw->button.yscale);

   /* GTK4: the toolbar buttons don't take the focus, commit typed text */
   gtk_spin_button_update (xspin);
   gtk_spin_button_update (yspin);

   x_scale = gtk_spin_button_get_value (xspin);
   y_scale = gtk_spin_button_get_value (yspin);

   gimv_image_view_zoom_image (iw->iv, GIMV_IMAGE_VIEW_ZOOM_FREE, x_scale, y_scale);
}


static void
cb_toolbar_keep_aspect (GtkWidget *widget, GimvImageWin *iw)
{
   gfloat scale;
   gboolean keep_aspect;

   g_return_if_fail (iw);

   g_object_get (G_OBJECT (iw->iv),
                   "keep_aspect", &keep_aspect,
                   NULL);

   if (!keep_aspect)
      return;

   scale = gtk_spin_button_get_value (GTK_SPIN_BUTTON (iw->button.xscale));
   gtk_spin_button_set_value (GTK_SPIN_BUTTON (iw->button.yscale), scale);
}


static void
cb_toolbar_fit_window (GtkWidget *widget, GimvImageWin *iw)
{
   g_return_if_fail (iw);
   gimv_image_win_set_win_size (iw);
}


static void
cb_toolbar_fullscreen (GtkWidget *widget, GimvImageWin *iw)
{
   g_return_if_fail (iw);
   menu_check_item_set_active (iw->view_menu, "/Fullscreen",
                               !(iw->priv->flags & GimvImageWinFullScreenFlag));
}


static gboolean
cb_scale_spinner_key_press (GtkWidget *widget, 
                            GimvEventKey *event,
                            GimvImageWin *iw)
{
   g_return_val_if_fail (iw, FALSE);
        
   switch (event->keyval) {
   case GDK_KEY_Escape:
      gtk_window_set_focus (GTK_WINDOW (iw), NULL);
      return FALSE;
   }

   return FALSE;
}


static void
cb_rotate_menu (GtkWidget *widget, GimvImageWin *iw)
{
   gint angle;
   g_return_if_fail (GTK_IS_WIDGET (widget));
   angle = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (widget), "num"));
   switch (angle)
   {
   case 0:
      angle = 1;
      break;
   case 1:
      angle = 0;
      break;
   case 2:
      angle = 3;
      break;
   case 3:
      angle = 2;
      break;
   default:
      angle = 0;
      break;
   }

   gimv_image_view_rotate_image (iw->iv, angle);
}


static gboolean
cb_rotate_menu_button_press (GtkWidget *widget,
                             GimvEventButton *event,
                             GimvImageWin *iw)
{
   g_return_val_if_fail (GTK_IS_WIDGET (widget), FALSE);

   switch (event->button) {
   case 2:
      gimv_image_view_rotate_ccw (iw->iv);
      return TRUE;
   case 3:
      gimv_image_view_rotate_cw (iw->iv);
      return TRUE;
   default:
      break;
   }

   return FALSE;
}



/******************************************************************************
 *
 *  callback functions for slideshow player
 *
 ******************************************************************************/
static void
cb_play_clicked (GtkWidget *button, GimvImageWin *iw)
{
   g_return_if_fail (iw);

   gimv_image_win_slideshow_play (iw);
}


static void
cb_stop_clicked (GtkWidget *button, GimvImageWin *iw)
{
   g_return_if_fail (iw);

   gimv_image_win_slideshow_stop (iw);
}


static void
cb_prev_clicked (GtkWidget *button, GimvImageWin *iw)
{
   g_return_if_fail (iw);

   gimv_image_view_prev (iw->iv);
}


static void
cb_next_clicked (GtkWidget *button, GimvImageWin *iw)
{
   g_return_if_fail (iw);

   gimv_image_view_next (iw->iv);
}


static void
cb_first_clicked (GtkWidget *button, GimvImageWin *iw)
{
   g_return_if_fail (iw);

   gimv_image_view_nth (iw->iv, 0);
}


static void
cb_last_clicked (GtkWidget *button, GimvImageWin *iw)
{
   gint num;

   g_return_if_fail (iw);
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iw->iv));
   g_return_if_fail (gimv_image_view_has_list (iw->iv));

   num = gimv_image_view_image_list_length (iw->iv);
   gimv_image_view_nth (iw->iv, num - 1);
}


static gboolean
cb_seekbar_pressed (GtkWidget *widget,
                    GimvEventButton *event,
                    GimvImageWin *iw)
{
   g_return_val_if_fail (iw, FALSE);

   iw->priv->flags |= GimvImageWinSlideShowSeekBarDraggingFlag;

   return FALSE;
}


static gboolean
cb_seekbar_released (GtkWidget *widget,
                     GimvEventButton *event,
                     GimvImageWin *iw)
{
   GtkAdjustment *adj;
   gint pos;

   g_return_val_if_fail (iw, FALSE);

   adj = gtk_range_get_adjustment (GTK_RANGE (iw->player.seekbar));
   pos = gtk_adjustment_get_value (adj) + 0.5;

   gimv_image_view_nth (iw->iv, pos);

   iw->priv->flags &= ~GimvImageWinSlideShowSeekBarDraggingFlag;

   return FALSE;
}



/******************************************************************************
 *
 *  other callback functions.
 *
 ******************************************************************************/
static void
cb_image_changed (GimvImageView *iv, GimvImageWin *iw)
{
   g_return_if_fail (iw);

   if (!(iw->priv->flags & GimvImageWinSlideShowSeekBarDraggingFlag)) {
      gint pos;
      GtkAdjustment *adj;

      pos = gimv_image_view_image_list_position (iv);
      adj = gtk_range_get_adjustment (GTK_RANGE (iw->player.seekbar));
      gtk_adjustment_set_value (adj, pos);
   }

   /* set statu bar */
   gimv_image_win_set_statusbar_info (iw);
   /* set title */
   gimv_image_win_set_window_title (iw);
}


static void
cb_load_start (GimvImageView *iv, GimvImageInfo *info, GimvImageWin *iw)
{
   g_return_if_fail (iw);

   gimv_image_win_set_sensitive (iw, FALSE);
}


static void
cb_load_end (GimvImageView *iv,
             GimvImageInfo *info,
             gboolean cancel,
             GimvImageWin *iw)
{
   g_return_if_fail (iw);

   gimv_image_win_set_sensitive (iw, TRUE);
}


static void
cb_set_list (GimvImageView *iv, GimvImageWin *iw)
{
   gint num, pos;
   GtkAdjustment *adj;

   g_return_if_fail (iw);
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   gtk_widget_set_sensitive (iw->button.prev, TRUE);
   gtk_widget_set_sensitive (iw->button.next, TRUE);

   /* player */
   gtk_widget_set_sensitive (iw->player.play,    TRUE);
   gtk_widget_set_sensitive (iw->player.stop,    FALSE);
   gtk_widget_set_sensitive (iw->player.rw,      TRUE);
   gtk_widget_set_sensitive (iw->player.fw,      TRUE);
   gtk_widget_set_sensitive (iw->player.prev,    TRUE);
   gtk_widget_set_sensitive (iw->player.next,    TRUE);
   gtk_widget_set_sensitive (iw->player.seekbar, TRUE);

   num = gimv_image_view_image_list_length (iv);
   pos = gimv_image_view_image_list_position (iv);

   adj = gtk_range_get_adjustment (GTK_RANGE (iw->player.seekbar));

   gtk_adjustment_configure (adj,
                             pos,    /* value */
                             0.0,    /* lower */
                             num,    /* upper */
                             1.0,    /* step_increment */
                             1.0,    /* page_increment */
                             1.0);   /* page_size */

   menu_item_set_sensitive (iw->move_menu, "/Next",     TRUE);
   menu_item_set_sensitive (iw->move_menu, "/Previous", TRUE);
   menu_item_set_sensitive (iw->move_menu, "/First",    TRUE);
   menu_item_set_sensitive (iw->move_menu, "/Last",     TRUE);
}


static void
cb_unset_list (GimvImageView *iv, GimvImageWin *iw)
{
   GtkAdjustment *adj;

   g_return_if_fail (iw);
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   gtk_widget_set_sensitive (iw->button.prev, FALSE);
   gtk_widget_set_sensitive (iw->button.next, FALSE);

   /* player */
   gtk_widget_set_sensitive (iw->player.play,    FALSE);
   gtk_widget_set_sensitive (iw->player.stop,    FALSE);
   gtk_widget_set_sensitive (iw->player.rw,      FALSE);
   gtk_widget_set_sensitive (iw->player.fw,      FALSE);
   gtk_widget_set_sensitive (iw->player.prev,    FALSE);
   gtk_widget_set_sensitive (iw->player.next,    FALSE);
   gtk_widget_set_sensitive (iw->player.seekbar, FALSE);

   adj = gtk_range_get_adjustment (GTK_RANGE (iw->player.seekbar));
   gtk_adjustment_set_value (adj, 0);

   menu_item_set_sensitive (iw->move_menu, "/Next",     FALSE);
   menu_item_set_sensitive (iw->move_menu, "/Previous", FALSE);
   menu_item_set_sensitive (iw->move_menu, "/First",    FALSE);
   menu_item_set_sensitive (iw->move_menu, "/Last",     FALSE);
}


static void
cb_rendered (GimvImageView *iv, GimvImageWin *iw)
{
   gfloat x_scale, y_scale;
   GimvImageViewOrientation rotate;
   gint angle = 1;

   g_return_if_fail (iw);
   if (!g_list_find (ImageWinList, iw)) return;

   g_return_if_fail (iw->iv);
   if (!g_list_find (gimv_image_view_get_list(), iw->iv)) return;

   /* set statu bar */
   gimv_image_win_set_statusbar_info (iw);

   /* set title */
   gimv_image_win_set_window_title (iw);

   /* resize window */
   if (!(iw->priv->flags & GimvImageWinMaximizeFlag) && conf.imgwin_fit_to_image)
      gimv_image_win_set_win_size (iw);

   g_object_get (G_OBJECT (iw->iv),
                   "x_scale",     &x_scale,
                   "y_scale",     &y_scale,
                   "rotation", &rotate,
                   NULL);

   switch (rotate) {
   case GIMV_IMAGE_VIEW_ROTATE_0:
      angle = 1;
      break;
   case GIMV_IMAGE_VIEW_ROTATE_90:
      angle = 0;
      break;
   case GIMV_IMAGE_VIEW_ROTATE_180:
      angle = 3;
      break;
   case GIMV_IMAGE_VIEW_ROTATE_270:
      angle = 2;
      break;
   default:
      break;
   }

   if (iw->button.xscale)
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (iw->button.xscale), x_scale);
   if (iw->button.yscale)
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (iw->button.yscale), y_scale);
   if (iw->button.rotate)
      gimv_option_menu_set_history (GTK_WIDGET (iw->button.rotate), angle);
}


static void
cb_toggle_aspect (GimvImageView *iv, gboolean keep_aspect, GimvImageWin *iw)
{
   g_return_if_fail (iw);

   gtk_widget_set_sensitive (iw->button.yscale, !keep_aspect);
   if (keep_aspect)
      gtk_widget_hide (iw->button.yscale);
   else
      gtk_widget_show (iw->button.yscale);
}


static gboolean
cb_draw_area_key_press (GtkWidget *widget, 
                        GimvEventKey *event,
                        GimvImageWin *iw)
{
   g_return_val_if_fail (iw, FALSE);
        
   switch (event->keyval) {
   case GDK_KEY_Escape:
      if (iw->priv->flags & GimvImageWinMaximizeFlag) {
         menu_check_item_set_active (iw->view_menu, "/Maximize",
                                     !(iw->priv->flags & GimvImageWinMaximizeFlag));
      }
      return FALSE;
   default:
      break;
   }

   return FALSE;
}


static gint
gimv_image_view_button_action (GimvImageView *iv, GimvEventButton *event, gint num)
{
   gint mx, my;

   /* option (Movie and Audio): a playing movie keeps the clicks from moving
      on to another file or
      zooming (the popup menu and the like still work) */
   if (conf.imgview_movie_lock_mouse && gimv_image_view_playable_is_busy (iv)) {
      switch (abs (num)) {
      case MOUSE_PRESS_NEXT: case MOUSE_PRESS_PREV:
      case MOUSE_PRESS_ZOOM_IN: case MOUSE_PRESS_ZOOM_OUT: case MOUSE_PRESS_FIT:
      case MOUSE_PRESS_ROTATE_CCW: case MOUSE_PRESS_ROTATE_CW: case MOUSE_PRESS_NAVWIN:
      case MOUSE_PRESS_UP: case MOUSE_PRESS_DOWN: case MOUSE_PRESS_LEFT: case MOUSE_PRESS_RIGHT:
         return TRUE;
      default:
         break;
      }
   }

   gimv_image_view_get_view_position (iv, &mx, &my);

   switch (abs (num)) {
   case MOUSE_PRESS_NEXT:
      gimv_image_view_next (iv);
      break;
   case MOUSE_PRESS_PREV:
      gimv_image_view_prev (iv);
      break;
   case MOUSE_PRESS_POPUP:
      gimv_image_view_popup_menu (iv, event);
      break;
   case MOUSE_PRESS_ZOOM_IN:
      gimv_image_view_zoom_image (iv, GIMV_IMAGE_VIEW_ZOOM_IN, 0, 0);
      break;
   case MOUSE_PRESS_ZOOM_OUT:
      gimv_image_view_zoom_image (iv, GIMV_IMAGE_VIEW_ZOOM_OUT, 0, 0);
      break;
   case MOUSE_PRESS_FIT:
      gimv_image_view_zoom_image (iv, GIMV_IMAGE_VIEW_ZOOM_FIT, 0, 0);
      break;
   case MOUSE_PRESS_ROTATE_CCW:
      gimv_image_view_rotate_ccw (iv);
      break;
   case MOUSE_PRESS_ROTATE_CW:
      gimv_image_view_rotate_cw (iv);
      break;
   case MOUSE_PRESS_NAVWIN:
      gimv_image_view_open_navwin (iv, event->x_root, event->y_root);
      break;
   case MOUSE_PRESS_UP:
      my += 20;
      gimv_image_view_moveto (iv, mx, my);
      break;
   case MOUSE_PRESS_DOWN:
      my -= 20;
      gimv_image_view_moveto (iv, mx, my);
      break;
   case MOUSE_PRESS_LEFT:
      mx += 20;
      gimv_image_view_moveto (iv, mx, my);
      break;
   case MOUSE_PRESS_RIGHT:
      mx -= 20;
      gimv_image_view_moveto (iv, mx, my);
      break;
   default:
      break;
   }

   return TRUE;
}


static gint
cb_imageview_pressed (GimvImageView *iv, GimvEventButton *event, GimvImageWin *iw)
{
   gint num;

   g_return_val_if_fail (iv, TRUE);
   g_return_val_if_fail (event, TRUE);

   num = prefs_mouse_get_num_from_event (event, conf.imgview_mouse_button);
   if (num > 0)
      return gimv_image_view_button_action (iv, event, num);

   return TRUE;
}


static gint
cb_imageview_clicked (GimvImageView *iv, GimvEventButton *event, GimvImageWin *iw)
{
   gint num;

   g_return_val_if_fail (iv, TRUE);
   g_return_val_if_fail (event, TRUE);

   num = prefs_mouse_get_num_from_event (event, conf.imgview_mouse_button);
   if (num < 0)
      return gimv_image_view_button_action (iv, event, num);

   return TRUE;
}


/******************************************************************************
 *
 *   fullscreen related functions.
 *
 ******************************************************************************/
static void
show_cursor (GimvImageWin *iw)
{
   GdkCursor *cursor;

   g_return_if_fail (iw);
   g_return_if_fail (iw->iv);
   g_return_if_fail (iw->iv->draw_area);
   /* GTK4: cursors can be set on unmapped widgets */

   cursor = cursor_get (iw->iv->draw_area, CURSOR_HAND_OPEN);
   gtk_widget_set_cursor (iw->iv->draw_area, cursor);
   g_object_unref (cursor);
}


static void
hide_cursor (GimvImageWin *iw)
{
   GdkCursor *cursor;

   g_return_if_fail (iw);
   g_return_if_fail (iw->iv);
   g_return_if_fail (iw->iv->draw_area);
   /* GTK4: cursors can be set on unmapped widgets */

   cursor = cursor_get (iw->iv->draw_area, CURSOR_VOID);
   gtk_widget_set_cursor (iw->iv->draw_area, cursor);
   g_object_unref (cursor);
}


static gint 
timeout_hide_cursor (gpointer data)
{
   GimvImageWin *iw = data;

   hide_cursor (iw);
   iw->priv->hide_cursor_timer_id = 0;

   return FALSE;
}


/*
 *  GTK4 replacement of gtk_accel_groups_activate (): run the keyboard
 *  shortcuts of the image window (menu accelerators) for a key event which
 *  was received by the fullscreen window.
 */
static gboolean
activate_shortcuts_of_controller (GimvImageWin *iw,
                                  GtkEventController *controller,
                                  guint keyval, GdkModifierType state)
{
   GListModel *model;
   guint i, n;

   if (!GTK_IS_SHORTCUT_CONTROLLER (controller)) return FALSE;

   model = G_LIST_MODEL (controller);
   n = g_list_model_get_n_items (model);

   for (i = 0; i < n; i++) {
      GtkShortcut *shortcut = g_list_model_get_item (model, i);
      GtkShortcutTrigger *trigger = gtk_shortcut_get_trigger (shortcut);
      gboolean match = FALSE;

      if (GTK_IS_KEYVAL_TRIGGER (trigger)) {
         GtkKeyvalTrigger *kt = GTK_KEYVAL_TRIGGER (trigger);
         match = (gdk_keyval_to_lower (gtk_keyval_trigger_get_keyval (kt))
                  == gdk_keyval_to_lower (keyval))
            && (gtk_keyval_trigger_get_modifiers (kt)
                == (state & gtk_accelerator_get_default_mod_mask ()));
      }

      if (match) {
         gboolean done;
         done = gtk_shortcut_action_activate (gtk_shortcut_get_action (shortcut),
                                              GTK_SHORTCUT_ACTION_EXCLUSIVE,
                                              GTK_WIDGET (iw),
                                              gtk_shortcut_get_arguments (shortcut));
         g_object_unref (shortcut);
         if (done) return TRUE;
         continue;
      }

      g_object_unref (shortcut);
   }

   return FALSE;
}


static gboolean
activate_window_shortcuts (GimvImageWin *iw, GimvEventKey *event)
{
   GListModel *controllers;
   guint i, n;
   gboolean retval = FALSE;

   controllers = gtk_widget_observe_controllers (GTK_WIDGET (iw));
   n = g_list_model_get_n_items (controllers);

   for (i = 0; i < n && !retval; i++) {
      GtkEventController *controller = g_list_model_get_item (controllers, i);
      retval = activate_shortcuts_of_controller (iw, controller,
                                                 event->keyval, event->state);
      g_object_unref (controller);
   }

   g_object_unref (controllers);

   return retval;
}


static gboolean
cb_fullscreen_key_press (GtkWidget *widget, 
                         GimvEventKey *event,
                         GimvImageWin *iw)
{
   g_return_val_if_fail (iw, FALSE);
        
   switch (event->keyval) {
   case GDK_KEY_Escape:
      if (iw->priv->flags & GimvImageWinCreatingFlag) return TRUE;
      menu_check_item_set_active (iw->view_menu, "/Fullscreen", FALSE);
      return TRUE;
   default:
      break;
   }

   activate_window_shortcuts (iw, event);

   return TRUE;
}


static gboolean
cb_fullscreen_motion_notify (GtkWidget *widget, 
                             GimvEventMotion *mevent, 
                             gpointer data)
{
   GimvImageWin *iw = data;

   show_cursor (iw);

   if (iw->priv->hide_cursor_timer_id != 0)
      g_source_remove (iw->priv->hide_cursor_timer_id);
   iw->priv->hide_cursor_timer_id
      = g_timeout_add (IMGWIN_FULLSCREEN_HIDE_CURSOR_DELAY,
                         timeout_hide_cursor,
                         iw);

   return FALSE;
}


static void
set_menu_sensitive (GimvImageWin *iw, gboolean sensitive)
{
   g_return_if_fail (iw);

   menu_item_set_sensitive (iw->menubar,   "/File/Open...",     sensitive);
   menu_item_set_sensitive (iw->menubar,   "/File/Open Image Window",
                            sensitive);
   menu_item_set_sensitive (iw->menubar,   "/File/Open Thumbnail Window",
                            sensitive);
   menu_item_set_sensitive (iw->menubar,   "/File/Quit",        sensitive);
   menu_item_set_sensitive (iw->menubar,   "/Edit/Options...",  sensitive);
   menu_item_set_sensitive (iw->view_menu, "/Menubar",         sensitive);
   menu_item_set_sensitive (iw->view_menu, "/Toolbar",         sensitive);
   menu_item_set_sensitive (iw->view_menu, "/Slideshow Player",sensitive);
   menu_item_set_sensitive (iw->view_menu, "/Statusbar",       sensitive);
   menu_item_set_sensitive (iw->view_menu, "/Scrollbar",       sensitive);
   menu_item_set_sensitive (iw->view_menu, "/Fit to Image",     sensitive);
   menu_item_set_sensitive (iw->view_menu, "/Maximize",         sensitive);
   gtk_widget_set_sensitive (iw->help_menu, sensitive);
}


void
gimv_image_win_fullscreen_show (GimvImageWin *iw)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN (iw));

   if (iw->fullscreen) return;

   g_signal_emit (G_OBJECT (iw),
                  gimv_image_win_signals[SHOW_FULLSCREEN_SIGNAL], 0);
}


void
gimv_image_win_fullscreen_hide (GimvImageWin *iw)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN (iw));

   if (!iw->fullscreen) return;

   g_signal_emit (G_OBJECT (iw),
                  gimv_image_win_signals[HIDE_FULLSCREEN_SIGNAL], 0);
}


static void
gimv_image_win_real_show_fullscreen (GimvImageWin *iw)
{ 
   gint screen_width, screen_height;

   g_return_if_fail (iw);

   iw->priv->flags |= GimvImageWinFullScreenFlag;

   /* create window */
   iw->fullscreen = gimv_popup_window_new ();

   gimv_screen_get_size (&screen_width, &screen_height);
   gtk_window_set_default_size (GTK_WINDOW (iw->fullscreen),
                                screen_width, screen_height);

   gimv_event_connect (GTK_WIDGET (iw->fullscreen), GIMV_EVENT_KEY_PRESS, (GCallback) cb_fullscreen_key_press, iw);
   gimv_event_connect (GTK_WIDGET (iw->fullscreen), GIMV_EVENT_MOTION_NOTIFY, (GCallback) cb_fullscreen_motion_notify, iw);

   /* set draw widget */
   if (iw->priv->fs_bg_color) {
      ImageWinColor *color = NULL;

      /* save current color (NULL: the default background) */
      if (iw->iv->bg_color) {
         color = g_new0 (ImageWinColor, 1);
         color->red   = iw->iv->bg_color->red   * 65535.0 + 0.5;
         color->green = iw->iv->bg_color->green * 65535.0 + 0.5;
         color->blue  = iw->iv->bg_color->blue  * 65535.0 + 0.5;
      }
      g_object_set_data_full (G_OBJECT (iw->fullscreen),
                              "GimvImageWin::FullScreen::OrigColor",
                              color,
                              (GDestroyNotify) g_free);
      g_object_set_data (G_OBJECT (iw->fullscreen),
                         "GimvImageWin::FullScreen::BgColorChanged",
                         GINT_TO_POINTER (TRUE));

      /* set bg color */
      gimv_image_view_set_bg_color (iw->iv,
                                    iw->priv->fs_bg_color->red,
                                    iw->priv->fs_bg_color->green,
                                    iw->priv->fs_bg_color->blue);
   }

   /* GTK4: the window manager places a fullscreen window on the proper
      monitor (the Xinerama code of GTK2 version isn't needed any more) */
   gtk_window_fullscreen (GTK_WINDOW (iw->fullscreen));
   gtk_widget_show (iw->fullscreen);

   gimv_image_view_set_fullscreen (iw->iv, GTK_WINDOW (iw->fullscreen));

   set_menu_sensitive (iw, FALSE);

   /* GTK4: no keyboard grabs, the fullscreen window is modal and focused */
   gimv_grab_add (iw->fullscreen);
   gtk_window_present (GTK_WINDOW (iw->fullscreen));
   gtk_widget_grab_focus (GTK_WIDGET (iw->iv));

   /* enable auto hide cursor stuff */
   iw->priv->hide_cursor_timer_id
      = g_timeout_add (IMGWIN_FULLSCREEN_HIDE_CURSOR_DELAY,
                         timeout_hide_cursor,
                         iw);
}


static void
gimv_image_win_real_hide_fullscreen (GimvImageWin *iw)
{
   ImageWinColor *color;

   g_return_if_fail (iw);

   /* restore draw widget */
   color = g_object_get_data (G_OBJECT (iw->fullscreen),
                              "GimvImageWin::FullScreen::OrigColor");
   if (color) {
      gimv_image_view_set_bg_color (iw->iv, color->red, color->green, color->blue);
   } else if (g_object_get_data (G_OBJECT (iw->fullscreen),
                                 "GimvImageWin::FullScreen::BgColorChanged"))
   {
      /* no color was set before: back to the default background */
      g_free (iw->iv->bg_color);
      iw->iv->bg_color = NULL;
      gtk_widget_queue_draw (iw->iv->draw_area);
   }

   gimv_image_view_unset_fullscreen (iw->iv);

   gtk_widget_grab_focus (GTK_WIDGET (iw->iv));

   /* disable auto hide cursor stuff */
   if (iw->priv->hide_cursor_timer_id != 0)
      g_source_remove (iw->priv->hide_cursor_timer_id);
   iw->priv->hide_cursor_timer_id = 0;
   show_cursor (iw);

   /* restore sensitivity */
   set_menu_sensitive (iw, TRUE);

   gimv_widget_destroy (iw->fullscreen);
   iw->fullscreen = NULL;
   iw->priv->flags &= ~GimvImageWinFullScreenFlag;
}



/******************************************************************************
 *
 *   public functions.
 *
 ******************************************************************************/
GList *
gimv_image_win_get_list (void)
{
   return ImageWinList;
}


GimvImageWin *
gimv_image_win_get_shared_window (void)
{
   return shared_img_win;
}


void
gimv_image_win_player_set_sensitive_all (GimvImageWin *iw, gboolean sensitive)
{
   g_return_if_fail (iw);

   gtk_widget_set_sensitive (iw->player.prev,    sensitive);
   gtk_widget_set_sensitive (iw->player.rw,      sensitive);
   gtk_widget_set_sensitive (iw->player.play,    sensitive);
   gtk_widget_set_sensitive (iw->player.stop,    sensitive);
   gtk_widget_set_sensitive (iw->player.fw,      sensitive);
   gtk_widget_set_sensitive (iw->player.next,    sensitive);
   gtk_widget_set_sensitive (iw->player.seekbar, sensitive);
}


void
gimv_image_win_change_image (GimvImageWin *iw, GimvImageInfo *info)
{
   GList *node;
   gint default_zoom;

   g_return_if_fail (iw);

   node = g_list_find (ImageWinList, iw);
   if (!node) return;

   if (conf.imgwin_raise_window
       && gtk_widget_get_realized (GTK_WIDGET (iw))
       && !(iw->priv->flags & GimvImageWinFullScreenFlag))
   {
      gtk_window_present (GTK_WINDOW (iw));
   }

   gimv_image_view_change_image (iw->iv, info);

   node = g_list_find (ImageWinList, iw);
   if (!node) return;

   /* If imgview_default_zoom is set to any of the "FIT" options
    * (i.e, the image is to be fit to the window in some form), and
    * imgwin_fit_to_image is set (i.e, the window is to be fit to the
    * image), we have a problem - what window size to use?  If we're
    * maximized or running in full screen mode, there's no problem,
    * othewise use the window size in the configuration options.
    * Not a great solution; maybe there should be a warning to
    * the user if both options are set together.
    */

   g_object_get (G_OBJECT (iw->iv),
                   "default_zoom", &default_zoom,
                   NULL);
   if ((default_zoom > 1) && conf.imgwin_fit_to_image
       && !(iw->priv->flags & GimvImageWinMaximizeFlag))
   {
      gtk_window_set_default_size (GTK_WINDOW (iw),
                                   conf.imgwin_width,
                                   conf.imgwin_height);
   }
}


void
gimv_image_win_set_sensitive (GimvImageWin *iw, gboolean sensitive)
{
   g_return_if_fail (iw);

   gtk_widget_set_sensitive (iw->menubar, sensitive);
}


void
gimv_image_win_save_state (GimvImageWin *iw)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN (iw));

   if (iw->priv->flags & GimvImageWinNotSaveStateFlag) return;

   if (!(iw->priv->flags & GimvImageWinMaximizeFlag)) {
      /* GTK4: the default size follows the window size (the allocation is
         already gone while the window is destroyed) */
      gtk_window_get_default_size (GTK_WINDOW (iw), &conf.imgwin_width, &conf.imgwin_height);
   } else {
      conf.imgwin_width  = iw->priv->win_width;
      conf.imgwin_height = iw->priv->win_height;
   }

   conf.imgwin_show_menubar    = iw->priv->flags & GimvImageWinShowMenuBarFlag;
   conf.imgwin_show_toolbar    = iw->priv->flags & GimvImageWinShowToolBarFlag;
   conf.imgwin_show_player     = iw->priv->flags & GimvImageWinShowPlayerFlag;
   conf.imgwin_show_statusbar  = iw->priv->flags & GimvImageWinShowStatusBarFlag;
   conf.imgview_player_visible = iw->priv->player_visible;
   g_object_get (G_OBJECT (iw->iv),
                   "show_scrollbar",   &conf.imgview_scrollbar,
                   "continuance_play", &conf.imgview_movie_continuance,
                   NULL);
}


GimvImageWin *
gimv_image_win_open_window (GimvImageInfo *info)
{
   GimvImageWin *iw;

   iw = GIMV_IMAGE_WIN (gimv_image_win_new (info));
   gtk_widget_show (GTK_WIDGET (iw));

   return GIMV_IMAGE_WIN (iw);
}


GimvImageWin *
gimv_image_win_open_shared_window (GimvImageInfo *info)
{
   GimvImageWin *iw;

   if (shared_img_win) {
      iw = shared_img_win;
      gimv_image_win_change_image (iw, info);
   } else {
      iw = GIMV_IMAGE_WIN (gimv_image_win_new (info));
      shared_img_win = iw;
      gtk_widget_show (GTK_WIDGET (iw));
   }

   return iw;
}


GimvImageWin *
gimv_image_win_open_window_auto (GimvImageInfo *info)
{
   GimvImageWin *iw;

   if (conf.imgwin_open_new_win) {
      iw = gimv_image_win_open_window (info);
   } else {
      iw = gimv_image_win_open_shared_window (info);
   }

   return iw;
}


static gboolean
timeout_slideshow (GimvImageWin *iw)
{
   GList *current;
   GimvImageViewPlayableStatus status;

   current = gimv_image_view_image_list_current (iw->iv);

   if ((current
        && !g_list_next (current)
        && !(iw->priv->flags & GimvImageWinSlideShowRepeatFlag))
       || !(iw->priv->flags & GimvImageWinSlideShowPlayingFlag))
   {
      gimv_image_win_slideshow_stop (iw);
      return FALSE;
   }

   // Movies, in contrast to pictues, should not be interrupted during slideshows.
   status = gimv_image_view_playable_get_status(iw->iv);
   if (status != GimvImageViewPlayableDisable &&
       status != GimvImageViewPlayableStop)
   {
      return TRUE;
   }

   gimv_image_view_next (iw->iv);

   return TRUE;
}


void
gimv_image_win_slideshow_play (GimvImageWin *iw)
{
   g_return_if_fail (iw);

   if (iw->priv->slideshow_interval > 0) {
      iw->priv->flags |= GimvImageWinSlideShowPlayingFlag;
      iw->priv->slideshow_timer_id
         = g_timeout_add (iw->priv->slideshow_interval,
                            (GSourceFunc) timeout_slideshow, iw);
   }

   gtk_widget_set_sensitive (iw->player.play, FALSE);
   gtk_widget_set_sensitive (iw->player.stop, TRUE);
}


void
gimv_image_win_slideshow_stop (GimvImageWin *iw)
{
   g_return_if_fail (iw);

   if (!(iw->priv->flags & GimvImageWinSlideShowPlayingFlag)) return;

   iw->priv->flags &= ~GimvImageWinSlideShowPlayingFlag;

   if (iw->priv->slideshow_timer_id)
      g_source_remove (iw->priv->slideshow_timer_id);
   iw->priv->slideshow_timer_id = 0;

   gtk_widget_set_sensitive (iw->player.play, TRUE);
   gtk_widget_set_sensitive (iw->player.stop, FALSE);
}


void
gimv_image_win_slideshow_set_interval (GimvImageWin *iw, guint interval)
{
   g_return_if_fail (iw);

   iw->priv->slideshow_interval = interval;
   if (iw->priv->flags & GimvImageWinSlideShowPlayingFlag) {
      gimv_image_win_slideshow_stop (iw);
      gimv_image_win_slideshow_play (iw);
   }
}


void
gimv_image_win_slideshow_set_repeat (GimvImageWin *iw, gboolean repeat)
{
   g_return_if_fail (iw);

   if (repeat)
      iw->priv->flags |= GimvImageWinSlideShowRepeatFlag;
   else
      iw->priv->flags &= ~GimvImageWinSlideShowRepeatFlag;
}


void
gimv_image_win_show_menubar (GimvImageWin *iw, gboolean show)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN (iw));
   menu_check_item_set_active (iw->view_menu, "/Menubar", show);
}


void
gimv_image_win_show_toolbar (GimvImageWin *iw, gboolean show)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN (iw));
   menu_check_item_set_active (iw->view_menu, "/Toolbar", show);
}


void
gimv_image_win_show_player (GimvImageWin *iw, gboolean show)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN (iw));
   menu_check_item_set_active (iw->view_menu, "/Slideshow Player", show);
}


void
gimv_image_win_show_statusbar (GimvImageWin *iw, gboolean show)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN (iw));
   menu_check_item_set_active (iw->view_menu, "/Statusbar", show);
}


void
gimv_image_win_set_maximize (GimvImageWin *iw, gboolean show)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN (iw));
   menu_check_item_set_active (iw->view_menu, "/Maximize", show);
}


void
gimv_image_win_set_fullscreen (GimvImageWin *iw, gboolean show)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN (iw));
   menu_check_item_set_active (iw->view_menu, "/Fullscreen", show);
}


void
gimv_image_win_set_fullscreen_bg_color (GimvImageWin *iw,
                                        gint red, gint green, gint blue)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN (iw));

   if (!iw->priv->fs_bg_color)
      iw->priv->fs_bg_color = g_new0 (ImageWinColor, 1);

   iw->priv->fs_bg_color->red   = red;
   iw->priv->fs_bg_color->green = green;
   iw->priv->fs_bg_color->blue  = blue;
}


void
gimv_image_win_set_flags (GimvImageWin  *iw, GimvImageWinFlags flags)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN (iw));
   iw->priv->flags |= flags;
}


void
gimv_image_win_unset_flags (GimvImageWin  *iw, GimvImageWinFlags flags)
{
   g_return_if_fail (GIMV_IS_IMAGE_WIN (iw));
   iw->priv->flags &= ~flags;
}

