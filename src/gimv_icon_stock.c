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
 * $Id: gimv_icon_stock.c,v 1.2 2003/06/13 09:43:30 makeinu Exp $
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_object.h"

#include "fileutil.h"
#include "gimv_icon_stock.h"
#include "gimv_xpm.h"
#include "gimageview.h"
#include "prefs.h"

/* common icons */
#include "pixmaps/gimv_icon.xpm"
#include "pixmaps/nfolder.xpm"
#include "pixmaps/paper.xpm"
#include "pixmaps/prefs.xpm"
#include "pixmaps/alert.xpm"
#include "pixmaps/question.xpm"
#include "pixmaps/image.xpm"
#include "pixmaps/archive.xpm"
#include "pixmaps/small_archive.xpm"
#include "pixmaps/folder48.xpm"
#ifdef ENABLE_SPLASH
#   include "pixmaps/gimageview.xpm"
#endif
/* icons for image window */
#include "pixmaps/back.xpm"
#include "pixmaps/forward.xpm"
#include "pixmaps/no_zoom.xpm"
#include "pixmaps/zoom_in.xpm"
#include "pixmaps/zoom_out.xpm"
#include "pixmaps/zoom_fit.xpm"
#include "pixmaps/zoom.xpm"
#include "pixmaps/rotate.xpm"
#if 0
#include "pixmaps/rotate0.xpm"
#include "pixmaps/rotate1.xpm"
#include "pixmaps/rotate2.xpm"
#include "pixmaps/rotate3.xpm"
#endif
#include "pixmaps/resize.xpm"
#include "pixmaps/fullscreen.xpm"
#include "pixmaps/nav_button.xpm"

/* player */
#include "pixmaps/play.xpm"
#include "pixmaps/pause.xpm"
#include "pixmaps/stop2.xpm"
#include "pixmaps/ff.xpm"
#include "pixmaps/rw.xpm"
#include "pixmaps/next_t.xpm"
#include "pixmaps/prev_t.xpm"
#include "pixmaps/eject.xpm"

/* icons for thumbnail window */
#include "pixmaps/refresh.xpm"
#include "pixmaps/skip.xpm"
#include "pixmaps/stop.xpm"
#include "pixmaps/leftarrow.xpm"
#include "pixmaps/rightarrow.xpm"
#include "pixmaps/close.xpm"
#include "pixmaps/small_close.xpm"

/* icons for directory view */
#include "pixmaps/folder.xpm"
#include "pixmaps/folder-link.xpm"
#include "pixmaps/folder-open.xpm"
#include "pixmaps/folder-link-open.xpm"
#include "pixmaps/folder-go.xpm"
#include "pixmaps/folder-up.xpm"
#include "pixmaps/folder-lock.xpm"
#include "pixmaps/small_home.xpm"
#include "pixmaps/small_up.xpm"
#include "pixmaps/small_refresh.xpm"
#include "pixmaps/dotfile.xpm"


static GimvIconStockEntry default_icons [] = {
   {"gimv_icon",         gimv_icon_xpm},
   {"nfolder",           nfolder_xpm},
   {"paper",             paper_xpm},
   {"prefs",             prefs_xpm},
   {"alert",             alert_xpm},
   {"question",          question_xpm},
   {"image",             image_xpm},
   {"archive",           archive_xpm},
   {"small_archive",     small_archive_xpm},
   {"folder48",          folder48_xpm},
#ifdef ENABLE_SPLASH
   {"gimageview",        gimageview_xpm},
#endif

   {"back",              back_xpm},
   {"forward",           forward_xpm},
   {"no_zoom",           no_zoom_xpm},
   {"zoom_in",           zoom_in_xpm},
   {"zoom_out",          zoom_out_xpm},
   {"zoom_fit",          zoom_fit_xpm},
   {"zoom",              zoom_xpm},
   {"rotate",            rotate_xpm},
#if 0
   {"rotate0",           rotate0_xpm},
   {"rotate1",           rotate1_xpm},
   {"rotate2",           rotate2_xpm},
   {"rotate3",           rotate3_xpm},
#endif
   {"resize",            resize_xpm},
   {"fullscreen",        fullscreen_xpm},
   {"nav-button",        nav_button_xpm},

   {"play",              play_xpm},
   {"pause",             pause_xpm},
   {"stop2",             stop2_xpm},
   {"ff",                ff_xpm},
   {"rw",                rw_xpm},
   {"next_t",            next_t_xpm},
   {"prev_t",            prev_t_xpm},
   {"eject",             eject_xpm},

   {"refresh",           refresh_xpm},
   {"skip",              skip_xpm},
   {"stop",              stop_xpm},
   {"leftarrow",         leftarrow_xpm},
   {"rightarrow",        rightarrow_xpm},
   {"close",             close_xpm},
   {"small_close",       small_close_xpm},

   {"folder",            folder_xpm},
   {"folder-go",         folder_go_xpm},
   {"folder-up",         folder_up_xpm},
   {"folder-link",       folder_link_xpm},
   {"folder-open",       folder_open_xpm},
   {"folder-link-open",  folder_link_open_xpm},
   {"folder-lock",       folder_lock_xpm},
   {"small_home",        small_home_xpm},
   {"small_up",          small_up_xpm},
   {"small_refresh",     small_refresh_xpm},
   {"dotfile",           dotfile_xpm},
};
static gint default_icons_num
= sizeof (default_icons) / sizeof (default_icons[0]);


/*
 *  GTK4 port: icons of the desktop icon theme.  Buttons whose gimv icon has
 *  a freedesktop counterpart show the theme's icon (conf.use_theme_icons),
 *  the others and pictures used as data (directory tree, thumbnails) keep
 *  the gimv icons.
 */
static const struct {
   const gchar *name;
   const gchar *theme_name;
} theme_icons[] = {
   {"nfolder",       "document-open"},
   {"prefs",         "preferences-system"},
   {"back",          "go-previous"},
   {"forward",       "go-next"},
   {"leftarrow",     "go-previous"},
   {"rightarrow",    "go-next"},
   {"no_zoom",       "zoom-original"},
   {"zoom_in",       "zoom-in"},
   {"zoom_out",      "zoom-out"},
   {"zoom_fit",      "zoom-fit-best"},
   {"fullscreen",    "view-fullscreen"},
   {"refresh",       "view-refresh"},
   {"small_refresh", "view-refresh"},
   {"skip",          "go-jump"},
   {"stop",          "process-stop"},
   {"close",         "window-close"},
   {"small_close",   "window-close"},
   {"small_home",    "go-home"},
   {"small_up",      "go-up"},
   {"play",          "media-playback-start"},
   {"pause",         "media-playback-pause"},
   {"stop2",         "media-playback-stop"},
   {"ff",            "media-seek-forward"},
   {"rw",            "media-seek-backward"},
   {"next_t",        "media-skip-forward"},
   {"prev_t",        "media-skip-backward"},
   {"eject",         "media-eject"},
   {"alert",         "dialog-warning"},
   {"question",      "dialog-question"},
};

/* icon files: ~/.gimv/icons/<iconset>, ICONDIR/<iconset>, ICONDIR/default */
static gchar       *icondirs[4]     = { NULL, NULL, NULL, NULL };
static GHashTable  *icons           = NULL;

static const gchar *icon_file_exts[] = { ".png", ".svg", ".xpm" };


gboolean
gimv_icon_stock_init (const gchar *iconset)
{
   gint n = 0;

   if (!iconset || !*iconset) iconset = DEFAULT_ICONSET;

   g_free (icondirs[0]);
   g_free (icondirs[1]);
   g_free (icondirs[2]);
   icondirs[n++] = g_build_filename (g_get_home_dir (), GIMV_RC_DIR, "icons",
                                     iconset, NULL);
   icondirs[n++] = g_build_filename (ICONDIR, iconset, NULL);
   if (strcmp (iconset, DEFAULT_ICONSET))
      icondirs[n++] = g_build_filename (ICONDIR, DEFAULT_ICONSET, NULL);
   icondirs[n] = NULL;

   if (!icons)
      icons = g_hash_table_new (g_str_hash, g_str_equal);

   return TRUE;
}


/* an icon file of the icon set, or NULL */
static gchar *
icon_file_find (const gchar *icon_name)
{
   gint i;
   guint j;

   if (!icondirs[0]) gimv_icon_stock_init (NULL);

   for (i = 0; icondirs[i]; i++) {
      for (j = 0; j < G_N_ELEMENTS (icon_file_exts); j++) {
         gchar *path = g_strconcat (icondirs[i], "/", icon_name,
                                    icon_file_exts[j], NULL);
         if (file_exists (path)) return path;
         g_free (path);
      }
   }

   return NULL;
}


static GdkPixbuf *
icon_file_load (const gchar *path)
{
   if (g_str_has_suffix (path, ".xpm"))
      return gimv_xpm_pixbuf_new_from_file (path);
   return gdk_pixbuf_new_from_file (path, NULL);
}


static GdkPixbuf *
icon_builtin_load (const gchar *icon_name)
{
   gint i;

   for (i = 0; i < default_icons_num; i++) {
      if (!strcmp (icon_name, default_icons[i].name))
         return gimv_xpm_pixbuf_new_from_data (
            (const gchar * const *) default_icons[i].xpm_data);
   }
   return NULL;
}


/*
 *  The name of the theme icon to show for a gimv icon, or NULL: theme icons
 *  are off, the icon set has its own file, or the theme has no such icon.
 *  Plain names first (full color themes), then the -symbolic ones.
 */
static const gchar *
theme_icon_name (const gchar *icon_name)
{
   static gchar buf[128];
   GtkIconTheme *theme;
   GdkDisplay *display = gdk_display_get_default ();
   gchar *file;
   guint i;

   if (!conf.use_theme_icons || !display) return NULL;

   for (i = 0; i < G_N_ELEMENTS (theme_icons); i++)
      if (!strcmp (icon_name, theme_icons[i].name)) break;
   if (i >= G_N_ELEMENTS (theme_icons)) return NULL;

   file = icon_file_find (icon_name);
   if (file) {
      /* only the icon set chosen by the user counts, not the fallback
         directory of the default set */
      gboolean own = !g_str_has_prefix (file, ICONDIR "/" DEFAULT_ICONSET "/")
         || (conf.iconset && !strcmp (conf.iconset, DEFAULT_ICONSET));
      g_free (file);
      if (own) return NULL;
   }

   theme = gtk_icon_theme_get_for_display (display);
   if (gtk_icon_theme_has_icon (theme, theme_icons[i].theme_name))
      return theme_icons[i].theme_name;

   g_snprintf (buf, sizeof (buf), "%s-symbolic", theme_icons[i].theme_name);
   if (gtk_icon_theme_has_icon (theme, buf))
      return buf;

   return NULL;
}


/* theme icons in about the size of the gimv icon they replace */
static gint
theme_icon_size (GimvIcon *icon)
{
   gint size = 16;

   if (icon && icon->pixmap)
      size = MAX (gdk_texture_get_width (icon->pixmap),
                  gdk_texture_get_height (icon->pixmap));

   if (size <= 16) return 16;
   if (size <= 26) return 24;
   if (size <= 40) return 32;
   return 48;
}


/*
 *  GTK4 port: gimv icons drawn only in grays (black outlines like "zoom" or
 *  "dotfile") can hardly be seen on the dark background of a dark theme.
 *  GimvGrayIcon is a symbolic paintable: GtkImage passes it the text color,
 *  and if that color is light the icon is drawn with inverted grays.
 */
#define GIMV_TYPE_GRAY_ICON (gimv_gray_icon_get_type ())
G_DECLARE_FINAL_TYPE (GimvGrayIcon, gimv_gray_icon, GIMV, GRAY_ICON, GObject)

struct _GimvGrayIcon
{
   GObject     parent;
   GdkTexture *texture;
   GdkTexture *inverted;
};

static void gimv_gray_icon_paintable_init (GdkPaintableInterface *iface);
static void gimv_gray_icon_symbolic_init  (GtkSymbolicPaintableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (GimvGrayIcon, gimv_gray_icon, G_TYPE_OBJECT,
   G_IMPLEMENT_INTERFACE (GDK_TYPE_PAINTABLE, gimv_gray_icon_paintable_init)
   G_IMPLEMENT_INTERFACE (GTK_TYPE_SYMBOLIC_PAINTABLE, gimv_gray_icon_symbolic_init))


static void
gimv_gray_icon_finalize (GObject *object)
{
   GimvGrayIcon *self = GIMV_GRAY_ICON (object);

   g_clear_object (&self->texture);
   g_clear_object (&self->inverted);
   G_OBJECT_CLASS (gimv_gray_icon_parent_class)->finalize (object);
}


static void
gimv_gray_icon_class_init (GimvGrayIconClass *klass)
{
   G_OBJECT_CLASS (klass)->finalize = gimv_gray_icon_finalize;
}


static void
gimv_gray_icon_init (GimvGrayIcon *self)
{
}


static void
gimv_gray_icon_snapshot (GdkPaintable *paintable, GdkSnapshot *snapshot,
                         double width, double height)
{
   gdk_paintable_snapshot (GDK_PAINTABLE (GIMV_GRAY_ICON (paintable)->texture),
                           snapshot, width, height);
}


static int
gimv_gray_icon_get_width (GdkPaintable *paintable)
{
   return gdk_texture_get_width (GIMV_GRAY_ICON (paintable)->texture);
}


static int
gimv_gray_icon_get_height (GdkPaintable *paintable)
{
   return gdk_texture_get_height (GIMV_GRAY_ICON (paintable)->texture);
}


static GdkPaintableFlags
gimv_gray_icon_get_flags (GdkPaintable *paintable)
{
   return GDK_PAINTABLE_STATIC_SIZE | GDK_PAINTABLE_STATIC_CONTENTS;
}


static void
gimv_gray_icon_paintable_init (GdkPaintableInterface *iface)
{
   iface->snapshot             = gimv_gray_icon_snapshot;
   iface->get_intrinsic_width  = gimv_gray_icon_get_width;
   iface->get_intrinsic_height = gimv_gray_icon_get_height;
   iface->get_flags            = gimv_gray_icon_get_flags;
}


static void
gimv_gray_icon_snapshot_symbolic (GtkSymbolicPaintable *paintable,
                                  GdkSnapshot *snapshot,
                                  double width, double height,
                                  const GdkRGBA *colors, gsize n_colors)
{
   GimvGrayIcon *self = GIMV_GRAY_ICON (paintable);
   GdkTexture *texture = self->texture;

   /* colors[0]: the foreground (text) color of the widget */
   if (n_colors > 0
       && 0.299 * colors[0].red + 0.587 * colors[0].green
          + 0.114 * colors[0].blue > 0.5)
   {
      texture = self->inverted;
   }

   gdk_paintable_snapshot (GDK_PAINTABLE (texture), snapshot, width, height);
}


static void
gimv_gray_icon_symbolic_init (GtkSymbolicPaintableInterface *iface)
{
   iface->snapshot_symbolic = gimv_gray_icon_snapshot_symbolic;
}


/* a GimvGrayIcon if all visible pixels of the pixbuf are gray, or NULL */
static GdkPaintable *
gray_icon_new (GdkPixbuf *pixbuf, GdkTexture *texture)
{
   GimvGrayIcon *self;
   GdkPixbuf *inverted;
   gint width, height, rowstride, x, y;
   guchar *pixels;

   if (!pixbuf || !texture) return NULL;
   if (!gdk_pixbuf_get_has_alpha (pixbuf)
       || gdk_pixbuf_get_n_channels (pixbuf) != 4
       || gdk_pixbuf_get_bits_per_sample (pixbuf) != 8)
   {
      return NULL;
   }

   inverted  = gdk_pixbuf_copy (pixbuf);
   width     = gdk_pixbuf_get_width (inverted);
   height    = gdk_pixbuf_get_height (inverted);
   rowstride = gdk_pixbuf_get_rowstride (inverted);
   pixels    = gdk_pixbuf_get_pixels (inverted);

   for (y = 0; y < height; y++) {
      guchar *p = pixels + y * rowstride;
      for (x = 0; x < width; x++, p += 4) {
         if (p[3] == 0) continue;
         if (MAX (MAX (p[0], p[1]), p[2]) - MIN (MIN (p[0], p[1]), p[2]) > 16) {
            g_object_unref (inverted);
            return NULL;
         }
         p[0] = 255 - p[0];
         p[1] = 255 - p[1];
         p[2] = 255 - p[2];
      }
   }

   self = g_object_new (GIMV_TYPE_GRAY_ICON, NULL);
   self->texture  = g_object_ref (texture);
   self->inverted = gimv_texture_new_for_pixbuf (inverted);
   g_object_unref (inverted);

   return GDK_PAINTABLE (self);
}


/* the paintable for icon widgets */
static GdkPaintable *
icon_widget_paintable (GimvIcon *icon)
{
   if (icon->widget_paintable) return icon->widget_paintable;
   return GDK_PAINTABLE (icon->pixmap);
}


GimvIcon *
gimv_icon_stock_get_icon (const gchar *icon_name)
{
   gchar *path = NULL;
   GimvIcon *icon;

   g_return_val_if_fail (icon_name, NULL);;

   icon = g_hash_table_lookup (icons, icon_name);

   if (icon) {
      return icon;
   }

   icon = g_new0 (GimvIcon, 1);
   icon->pixmap = NULL;
   icon->mask = NULL;
   icon->pixbuf = NULL;

   path = icon_file_find (icon_name);
   if (path)
      icon->pixbuf = icon_file_load (path);
   g_free (path);

   if (!icon->pixbuf)
      icon->pixbuf = icon_builtin_load (icon_name);

   /* GTK4: the "pixmap" is a texture made from the pixbuf, mask is NULL */
   if (icon->pixbuf)
      icon->pixmap = gimv_texture_new_for_pixbuf (icon->pixbuf);

   if (icon->pixmap) {
      icon->widget_paintable = gray_icon_new (icon->pixbuf, icon->pixmap);
      g_hash_table_insert (icons, (gchar *) icon_name, icon);
   } else {
      if (icon->pixbuf) g_object_unref (icon->pixbuf);
      g_free (icon);
      return NULL;
   }

   return icon;
}


/*
 *  GTK4: GtkImage shows paintables at icon size (16px by default), GTK2's
 *  GtkPixmap used the size of the pixmap.  Use the natural size: GtkImage
 *  with a pixel size for square icons, GtkPicture for other shapes.
 */
static GtkWidget *
icon_widget_new (GdkPaintable *paintable)
{
   gint width  = gdk_paintable_get_intrinsic_width (paintable);
   gint height = gdk_paintable_get_intrinsic_height (paintable);
   GtkWidget *widget;

   if (width == height) {
      widget = gtk_image_new_from_paintable (paintable);
      if (width > 0)
         gtk_image_set_pixel_size (GTK_IMAGE (widget), width);
   } else {
      widget = gtk_picture_new_for_paintable (paintable);
      gtk_picture_set_can_shrink (GTK_PICTURE (widget), FALSE);
   }

   return widget;
}


GtkWidget *
gimv_icon_stock_get_widget (const gchar *icon_name)
{
   GimvIcon *icon;
   GtkWidget *widget = NULL;

   g_return_val_if_fail (icon_name, NULL);

   icon = gimv_icon_stock_get_icon (icon_name);

   if (theme_icon_name (icon_name)) {
      widget = gtk_image_new_from_icon_name (theme_icon_name (icon_name));
      gtk_image_set_pixel_size (GTK_IMAGE (widget), theme_icon_size (icon));
   } else if (icon && icon->pixmap) {
      widget = icon_widget_new (icon_widget_paintable (icon));
   }

   return widget;
}


void
gimv_icon_stock_change_widget_icon (GtkWidget *widget, const gchar *icon_name)
{
   GimvIcon *icon;

   g_return_if_fail (widget);
   g_return_if_fail (icon_name && *icon_name);

   g_return_if_fail (GTK_IS_IMAGE (widget) || GTK_IS_PICTURE (widget));

   icon = gimv_icon_stock_get_icon (icon_name);

   if (GTK_IS_IMAGE (widget) && theme_icon_name (icon_name)) {
      gtk_image_set_from_icon_name (GTK_IMAGE (widget), theme_icon_name (icon_name));
      gtk_image_set_pixel_size (GTK_IMAGE (widget), theme_icon_size (icon));
      return;
   }

   if (!icon || !icon->pixmap) return;

   if (GTK_IS_PICTURE (widget)) {
      gtk_picture_set_paintable (GTK_PICTURE (widget),
                                 icon_widget_paintable (icon));
   } else {
      gtk_image_set_from_paintable (GTK_IMAGE (widget),
                                    icon_widget_paintable (icon));
      gtk_image_set_pixel_size (GTK_IMAGE (widget),
                                MAX (gdk_texture_get_width (icon->pixmap),
                                     gdk_texture_get_height (icon->pixmap)));
   }
}


/*
 *  GTK4: windows can only use themed icon names.  If the icon theme knows
 *  an icon of that name it is used, otherwise the application icon "gimv".
 */
void
gimv_icon_stock_set_window_icon (GtkWidget *window, gchar *name)
{
   GtkIconTheme *theme;

   g_return_if_fail (GTK_IS_WINDOW (window));

   theme = gtk_icon_theme_get_for_display (gtk_widget_get_display (window));
   if (name && theme && gtk_icon_theme_has_icon (theme, name))
      gtk_window_set_icon_name (GTK_WINDOW (window), name);
   else
      gtk_window_set_icon_name (GTK_WINDOW (window), "gimv");
}


void
gimv_icon_stock_free_icon (const gchar *icon_name)
{
   GimvIcon *icon;

   g_return_if_fail (icon_name);

   icon = gimv_icon_stock_get_icon (icon_name);
   if (!icon) return;

   g_hash_table_remove (icons, icon_name);
   g_clear_object (&icon->widget_paintable);
   g_object_unref (icon->pixmap);
   if (icon->pixbuf)
      g_object_unref (icon->pixbuf);
   g_free (icon);
}


GdkPixbuf *
gimv_icon_stock_get_pixbuf  (const gchar *icon_name)
{
   GimvIcon *icon;
   gchar *path;

   g_return_val_if_fail (icon_name, NULL);

   icon = gimv_icon_stock_get_icon (icon_name);
   if (!icon) return NULL;
   if (icon->pixbuf) return icon->pixbuf;

   path = icon_file_find (icon_name);
   if (path)
      icon->pixbuf = icon_file_load (path);
   g_free (path);

   if (!icon->pixbuf)
      icon->pixbuf = icon_builtin_load (icon_name);

   return icon->pixbuf;
}


void
gimv_icon_stock_free_pixbuf (const gchar *icon_name)
{
   GimvIcon *icon;

   g_return_if_fail (icon_name);

   icon = gimv_icon_stock_get_icon (icon_name);
   if (!icon) return;

   g_object_unref (icon->pixbuf);
   icon->pixbuf = NULL;
}
