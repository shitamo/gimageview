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
 * $Id: exif_view.c,v 1.23 2004/09/22 15:37:11 makeinu Exp $
 */

#include "gimageview.h"

#ifdef ENABLE_EXIF

#include <libexif/exif-data.h>

#include "exif_view.h"
#include "gimv_image.h"
#include "gimv_io_mem.h"
#include "gtkutils.h"
#include "gimv_icon_stock.h"
#include "gimv_image_loader.h"


typedef enum {
   COLUMN_TERMINATOR = -1,
   COLUMN_KEY,
   COLUMN_VALUE,
   N_COLUMN
} ListStoreColumn;


/******************************************************************************
 *
 *   Callback Functions.
 *
 ******************************************************************************/
static void
cb_exif_view_destroy (GtkWidget *widget, ExifView *ev)
{
   g_return_if_fail (ev);

   if (ev->exif_data)
      exif_data_unref (ev->exif_data);
   ev->exif_data = NULL;

   g_free (ev);
}


static void
cb_exif_window_close (GtkWidget *button, ExifView *ev)
{
   g_return_if_fail (ev);

   gimv_widget_destroy (ev->window);
}


/******************************************************************************
 *
 *   Other Private Functions.
 *
 ******************************************************************************/
static void
exif_view_content_list_set_data (GtkWidget *clist,
                                 ExifContent *content,
                                 ExifIfd ifd)
{
   const gchar *text[2];
   gchar value[1024];
   guint i;

   g_return_if_fail (clist);
   g_return_if_fail (content);

   {
      GtkTreeModel *model = gtk_tree_view_get_model (GTK_TREE_VIEW (clist));
      gtk_list_store_clear (GTK_LIST_STORE (model));
   }

   for (i = 0; i < content->count; i++) {
      /* system libexif: titles and values are translated by libexif */
      text[0] = exif_tag_get_title_in_ifd (content->entries[i]->tag, ifd);
      if (!text[0])
         text[0] = exif_tag_get_name (content->entries[i]->tag);
      text[1] = exif_entry_get_value (content->entries[i], value, sizeof (value));
      {
         GtkTreeModel *model = gtk_tree_view_get_model (GTK_TREE_VIEW (clist));
         GtkTreeIter iter;

         gtk_list_store_append (GTK_LIST_STORE (model), &iter);
         gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                             COLUMN_KEY,       text[0],
                             COLUMN_VALUE,     text[1],
                             COLUMN_TERMINATOR);
      }
   }
}


/******************************************************************************
 *
 *   Public Functions.
 *
 ******************************************************************************/
ExifView *
exif_view_create_window (const gchar *filename, GtkWindow *parent)
{
   ExifView *ev;
   GtkWidget *button;
   gchar buf[BUF_SIZE];

   g_return_val_if_fail (filename && *filename, NULL);

   ev = exif_view_create (filename, parent);
   if (!ev) return NULL;

   ev->window = gtk_dialog_new ();
   if (parent)
      gtk_window_set_transient_for (GTK_WINDOW (ev->window), parent);
   else
      gimv_window_set_default_transient (GTK_WINDOW (ev->window));
   g_snprintf (buf, BUF_SIZE, _("%s EXIF data"), filename);
   gtk_window_set_title (GTK_WINDOW (ev->window), buf); 
   gtk_window_set_default_size (GTK_WINDOW (ev->window), 500, 400);

   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_vbox (GTK_WIDGET (ev->window))),
                       ev->container,
                       TRUE, TRUE, 0);

   gimv_widget_show_all (ev->window);

   /* button */
   button = gtk_button_new_with_label (_("Close"));
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (ev->window))), 
                       button, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK (cb_exif_window_close), ev);
   gtk_window_set_default_widget (GTK_WINDOW (ev->window), button);
   gtk_widget_show (button);

   gtk_widget_grab_focus (button);

   gimv_icon_stock_set_window_icon (ev->window, "gimv_icon");

   return ev;
}


static GtkWidget *
exif_view_get_thumbnail (ExifData *edata)
{
   GtkWidget *image;
   GimvImageLoader *loader;
   GimvIO *gio;
   GimvImage *gimvimage;
   GdkTexture *pixmap = NULL;
   GdkTexture *bitmap = NULL;


   g_return_val_if_fail (edata, NULL);

   if (!edata->data) return NULL;
   if (edata->size <= 0) return NULL;

   loader = gimv_image_loader_new ();
   if (!loader) return NULL;

   gio = gimv_io_mem_new (NULL, "rb", GimvIOMemModeWrap);
   gimv_io_mem_wrap ((GimvIOMem *) gio, edata->data, edata->size, FALSE);
   gimv_image_loader_set_gio (loader, gio);

   gimv_image_loader_load (loader);

   gimvimage = gimv_image_loader_get_image (loader);
   if (!gimvimage) {
      gimv_image_loader_unref (loader);
      return NULL;
   }

   gimv_image_scale_get_pixmap (gimvimage,
                                gimv_image_width (gimvimage),
                                gimv_image_height (gimvimage),
                                &pixmap, &bitmap);

   gimv_image_loader_unref (loader);
   gimv_io_unref (gio);

   if (!pixmap) return NULL;

   image = gtk_picture_new_for_paintable (GDK_PAINTABLE (pixmap));
   gtk_picture_set_can_shrink (GTK_PICTURE (image), FALSE);

   if (pixmap)
      g_object_unref (pixmap);

   return image;
}


ExifView *
exif_view_create (const gchar *filename, GtkWindow *parent)
{
   ExifData *edata;
   ExifView *ev = NULL;
   ExifContent *contents[EXIF_IFD_COUNT];
   GtkWidget *notebook, *label;
   GtkWidget *vbox, *pixmap;
   gint i;

   gchar *titles[] = {
      N_("Tag"), N_("Value"),
   };

   g_return_val_if_fail (filename && *filename, NULL);

   /* system libexif: finds the EXIF data in JPEG (and a few other) files */
   edata = exif_data_new_from_file (filename);
   if (!edata) {
      gtkutil_message_dialog (_("Error!"), _("EXIF data not found."),
                              GTK_WINDOW (parent));
      return NULL;
   }

   ev = g_new0 (ExifView, 1);
   ev->exif_data = edata;

#if 0
   contents[0] = edata->ifd0;
   contents[1] = edata->ifd1;
   contents[2] = edata->ifd_exif;
   contents[3] = edata->ifd_gps;
   contents[4] = edata->ifd_interoperability;
#else
   for (i = 0; i < EXIF_IFD_COUNT; i++)
      contents[i] = edata->ifd[i];
#endif

   ev->container = gimv_vbox_new (FALSE, 0);
   g_signal_connect (G_OBJECT (ev->container), "destroy",
                       G_CALLBACK (cb_exif_view_destroy), ev);
   gtk_widget_show (ev->container);

   notebook = gtk_notebook_new ();
   gtk_notebook_set_scrollable (GTK_NOTEBOOK (notebook), TRUE);
   gimv_box_pack_start(GTK_BOX(ev->container), notebook, TRUE, TRUE, 0);
   gtk_widget_show (notebook);

   /* Tag Tables */
   for (i = 0; i < EXIF_IFD_COUNT; i++) {
      GtkWidget *scrolledwin, *clist;

      /* scrolled window & clist */
      label = gtk_label_new (_(exif_ifd_get_name(i)));
      scrolledwin = gimv_scrolled_window_new (NULL, NULL);
      gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(scrolledwin),
                                      GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
      gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scrolledwin), TRUE);
      gimv_container_set_border_width (GTK_WIDGET (scrolledwin), 5);
      gtk_notebook_append_page (GTK_NOTEBOOK(notebook),
                                scrolledwin, label);
      gtk_widget_show (scrolledwin);

      {
         GtkListStore *store;
         GtkTreeViewColumn *col;
         GtkCellRenderer *render;

         store = gtk_list_store_new (N_COLUMN, G_TYPE_STRING, G_TYPE_STRING);
         clist = gtk_tree_view_new_with_model (GTK_TREE_MODEL (store));
         gimv_tree_view_widen_column_resize (GTK_TREE_VIEW (clist));

         /* GTK4: gtk_tree_view_set_rules_hint () removed (no striped rows) */

         /* set column for key */
         col = gtk_tree_view_column_new();
         gtk_tree_view_column_set_resizable (col, TRUE);
         gtk_tree_view_column_set_title (col, _(titles[0]));
         render = gtk_cell_renderer_text_new ();
         gtk_tree_view_column_pack_start (col, render, TRUE);
         gtk_tree_view_column_add_attribute (col, render, "text", COLUMN_KEY);
         gtk_tree_view_append_column (GTK_TREE_VIEW (clist), col);

         /* set column for value */
         col = gtk_tree_view_column_new();
         gtk_tree_view_column_set_resizable (col, TRUE);
         gtk_tree_view_column_set_title (col, _(titles[1]));
         render = gtk_cell_renderer_text_new ();
         gtk_tree_view_column_pack_start (col, render, TRUE);
         gtk_tree_view_column_add_attribute (col, render, "text", COLUMN_VALUE);
         gtk_tree_view_append_column (GTK_TREE_VIEW (clist), col);
      }

      gimv_container_add (GTK_WIDGET (scrolledwin), clist);
      gtk_widget_show (clist);

      exif_view_content_list_set_data (clist, contents[i], i);
   }

   /* Thumbnail page */
   label = gtk_label_new (_("Thumbnail"));
   vbox = gimv_vbox_new (TRUE, 0);
   gtk_notebook_append_page (GTK_NOTEBOOK(notebook),
                             vbox, label);
   gtk_widget_show (vbox);

   pixmap = exif_view_get_thumbnail (edata);

   if (pixmap)
      gimv_box_pack_start (GTK_BOX (vbox), pixmap, TRUE, TRUE, 0);

   return ev;
}

#endif /* ENABLE_EXIF */
