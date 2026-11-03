/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001-2002 Takuro Ashie
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
 * $Id: gimv_dupl_win.c,v 1.5 2004/04/08 13:39:25 makeinu Exp $
 */

#include "gimv_dupl_win.h"

#include "charset.h"
#include "fileutil.h"
#include "gimv_list_view.h"
#include "gimv_icon_stock.h"
#include "gimv_image_info.h"
#include "gimv_thumb.h"
#include "prefs.h"

struct GimvDuplWinPriv_Tag
{
   gint              thumbnail_size;

   GList            *thumb_list;

   gboolean          show_thumbnail;   /* or the icon */
   GListStore       *root;             /* rows of the tree (GimvRow) */
};

static void gimv_dupl_win_init       (GimvDuplWin      *sw);
static void gimv_dupl_win_class_init (GimvDuplWinClass *klass);
static void gimv_dupl_win_dispose    (GObject          *object);


static void cb_select_all_button      (GtkButton        *button,
                                       GimvDuplWin      *sw);
static void cb_finder_stop_button     (GtkWidget        *widget,
                                       GimvDuplWin      *sw);
static void cb_finder_start           (GimvDuplFinder   *finder,
                                       GimvDuplWin      *sw);
static void cb_finder_stop            (GimvDuplFinder   *finder,
                                       GimvDuplWin      *sw);
static void cb_finder_progress_update (GimvDuplFinder   *finder,
                                       GimvDuplWin      *sw);
static void cb_finder_found           (GimvDuplFinder   *finder,
                                       GimvDuplPair     *pair,
                                       GimvDuplWin      *sw);
static void cb_select_thumb           (GimvThumb        *thumb,
                                       GimvDuplWin      *sw);


typedef enum {
   COLUMN_TERMINATOR = -1,
   COLUMN_THUMBNAIL,
   COLUMN_ICON,
   COLUMN_NAME,
   COLUMN_ACCURACY,
   COLUMN_SIZE,
   COLUMN_MTIME,
   COLUMN_THUMBDATA,
   N_COLUMN
} TreeStoreColumn;

static void     cb_tree_selection_changed     (GtkSelectionModel *selection,
                                               guint         position,
                                               guint         n_items,
                                               GimvDuplWin  *sw);
static void     cb_change_to_thumbnail_button (GtkWidget    *button,
                                               GimvDuplWin  *sw);
static void     cb_change_to_icon_button      (GtkWidget    *button,
                                               GimvDuplWin  *sw);
static GtkWidget *name_cell_setup              (gpointer      data);
static void     name_cell_bind                (GtkWidget    *cell,
                                               GimvRow      *row,
                                               GtkListItem  *item,
                                               gpointer      data);
static GimvRow *find_row                      (GListModel   *rows,
                                               GimvThumb    *thumb);
static GimvRow *insert_node                   (GimvDuplWin  *sw,
                                               GimvRow      *parent,
                                               GimvThumb    *thumb,
                                               gfloat        similar);



gchar *simwin_titles[4] = {
   N_("Name"),
   N_("Accuracy"),
   N_("Size (byte)"),
   N_("Modification Time")
};
gint simwin_column_num = sizeof (simwin_titles) / sizeof (gchar *);

static GtkWindowClass *parent_class = NULL;


G_DEFINE_TYPE (GimvDuplWin, gimv_dupl_win, GTK_TYPE_WINDOW)


static void
gimv_dupl_win_init (GimvDuplWin *sw)
{
   GtkWidget *hbox;
   GtkWidget *scrolledwin, *radio, *button;
   gint i;

   sw->ctree           = NULL;
   sw->radio_thumb     = NULL;
   sw->radio_icon      = NULL;
   sw->select_button   = NULL;
   sw->progressbar     = NULL;

   sw->finder          = gimv_dupl_finder_new (NULL);
   sw->tv              = NULL;

   sw->priv            = g_new0 (GimvDuplWinPriv, 1);
   sw->priv->thumbnail_size  = 96;
   sw->priv->thumb_list      = NULL;
   sw->priv->show_thumbnail  = FALSE;
   sw->priv->root            = gimv_row_store_new ();

   /* window */
   gimv_dialog_init (GTK_WINDOW (sw));
   gtk_window_set_title (GTK_WINDOW (sw), _("Find Duplicates - Results")); 
   gtk_window_set_default_size (GTK_WINDOW (sw), 500, 400);
   /* GTK4: gtk_window_set_position (GTK_WIN_POS_MOUSE) is not available */

   /* ctree */
   scrolledwin = gimv_scrolled_window_new (NULL, NULL);
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(scrolledwin),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gimv_container_set_border_width (GTK_WIDGET (scrolledwin), 5);

   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_vbox (GTK_WIDGET (sw))),
                       scrolledwin, TRUE, TRUE, 0);

   {
      GtkTreeListModel *model;
      GtkColumnViewColumn *col;

      /* GTK4: GtkTreeView (deprecated) -> GtkColumnView of a
         GtkTreeListModel */
      model = gimv_tree_list_model_new (G_LIST_MODEL (sw->priv->root));
      sw->ctree = gimv_column_view_new (G_LIST_MODEL (model),
                                        GTK_SELECTION_SINGLE);
      gimv_container_add (GTK_WIDGET (scrolledwin), sw->ctree);

      /* name column: the thumbnail (or the icon) and the name */
      col = gimv_column_view_append (GTK_COLUMN_VIEW (sw->ctree),
                                     _(simwin_titles[0]),
                                     gimv_cell_factory_new (name_cell_setup,
                                                            name_cell_bind,
                                                            sw, NULL, TRUE));
      gtk_column_view_column_set_resizable (col, TRUE);

      /* other column */
      for (i = 1; i < simwin_column_num; i++) {
         col = gimv_column_view_append_text (GTK_COLUMN_VIEW (sw->ctree),
                                             _(simwin_titles[i]),
                                             COLUMN_NAME + i);
         gtk_column_view_column_set_resizable (col, TRUE);
      }

      g_signal_connect (gimv_list_view_get_selection (sw->ctree),
                        "selection-changed",
                        G_CALLBACK (cb_tree_selection_changed), sw);
   }

   /* button */
   hbox = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (sw))), 
                       hbox, TRUE, TRUE, 0);

   /* radio button */
   radio = gimv_radio_button_new_with_label (NULL, _("Thumbnail"));
   sw->radio_thumb = radio;
   g_signal_connect (G_OBJECT (radio), "toggled",
                       G_CALLBACK (cb_change_to_thumbnail_button), sw);
   gimv_box_pack_start (GTK_BOX (hbox), radio, FALSE, FALSE, 0);

   radio = gimv_radio_button_new_with_label_from_widget (GTK_WIDGET (radio), _("Icon"));
   sw->radio_icon  = radio;
   g_signal_connect (G_OBJECT (radio), "toggled",
                       G_CALLBACK (cb_change_to_icon_button), sw);
   gimv_box_pack_start (GTK_BOX (hbox), radio, FALSE, FALSE, 0);

   gimv_toggle_set_active (GTK_WIDGET (radio), TRUE);

   /* Select All */
   button = gtk_button_new_with_label (_("Select All"));
   sw->select_button = button;
   gimv_box_pack_start (GTK_BOX (hbox), button, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK (cb_select_all_button), sw);
   /* GTK4: SET (GTK_CAN_DEFAULT) removed */ (void) 0;
   gtk_widget_set_visible (button, TRUE);

   /* close button */
   button = gtk_button_new_with_label (_("Stop"));
   sw->stop_button = button;
   gimv_box_pack_start (GTK_BOX (hbox), button, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK (cb_finder_stop_button), sw);
   /* GTK4: SET (GTK_CAN_DEFAULT) removed */ (void) 0;
   gtk_widget_set_visible (button, TRUE);

   /* gtk_widget_grab_focus (button); */

   /* progress bar */
   sw->progressbar = gtk_progress_bar_new();
   gimv_box_pack_end (GTK_BOX (hbox), sw->progressbar, FALSE, FALSE, 0);

   /* finder */
   g_signal_connect (G_OBJECT (sw->finder), "start",
                       G_CALLBACK (cb_finder_start), sw);
   g_signal_connect (G_OBJECT (sw->finder), "stop",
                       G_CALLBACK (cb_finder_stop), sw);
   g_signal_connect (G_OBJECT (sw->finder), "progress_update",
                       G_CALLBACK (cb_finder_progress_update), sw);
   g_signal_connect (G_OBJECT (sw->finder), "found",
                       G_CALLBACK (cb_finder_found), sw);
}


static void
gimv_dupl_win_class_init (GimvDuplWinClass *klass)
{
   GObjectClass *gobject_class;

   gobject_class = (GObjectClass *) klass;
   parent_class = g_type_class_peek_parent (klass);

   gobject_class->dispose = gimv_dupl_win_dispose;
}


static void
gimv_dupl_win_dispose (GObject *object)
{
   GimvDuplWin *sw = GIMV_DUPL_WIN (object);

   g_return_if_fail (sw);

   if (sw->finder) {
      /* stop searching, the callbacks refer to this window */
      g_signal_handlers_disconnect_matched (G_OBJECT (sw->finder),
                                            G_SIGNAL_MATCH_DATA,
                                            0, 0, NULL, NULL, sw);
      gimv_dupl_finder_stop (sw->finder);
      g_object_unref (G_OBJECT (sw->finder));
      sw->finder = NULL;
   }

   if (sw->priv) {
      g_clear_object (&sw->priv->root);
      g_list_foreach (sw->priv->thumb_list, (GFunc) g_object_unref, NULL);
      g_list_free (sw->priv->thumb_list);
      sw->priv->thumb_list = NULL;
      g_free (sw->priv);
      sw->priv = NULL;
   }

   if (G_OBJECT_CLASS (parent_class)->dispose)
      G_OBJECT_CLASS (parent_class)->dispose (object);
}


static void
ghfunc_select_thumb (GimvThumb *thumb)
{
   gimv_thumb_view_set_selection (thumb, TRUE);
}


static void
cb_select_all_button (GtkButton *button, GimvDuplWin *sw)
{
   g_return_if_fail (sw);
   if (!sw->tv) return;

   gimv_thumb_view_set_selection_all (sw->tv, FALSE);
   g_list_foreach (sw->priv->thumb_list, (GFunc) ghfunc_select_thumb, NULL);
}


static void
cb_finder_stop_button (GtkWidget *widget, GimvDuplWin *sw)
{
   g_return_if_fail (sw);

   if (sw->finder)
      gimv_dupl_finder_stop (sw->finder);
}


static void
cb_finder_start (GimvDuplFinder *finder, GimvDuplWin *sw)
{
   gfloat progress;

   g_return_if_fail (GIMV_IS_DUPL_FINDER (finder));
   g_return_if_fail (sw);

   gtk_widget_set_sensitive (sw->stop_button, TRUE);

   gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (sw->progressbar), TRUE);
   gtk_progress_bar_set_text (GTK_PROGRESS_BAR (sw->progressbar),
                              _("Finding similar images..."));
   progress = gimv_dupl_finder_get_progress (finder);
   gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (sw->progressbar), progress);
}


static void
cb_finder_stop (GimvDuplFinder *finder, GimvDuplWin *sw)
{
   g_return_if_fail (GIMV_IS_DUPL_FINDER (finder));
   g_return_if_fail (sw);

   gtk_widget_set_sensitive (sw->stop_button, FALSE);

   gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (sw->progressbar), 0.0);
   /* gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (sw->progressbar), FALSE); */
   gtk_progress_bar_set_text (GTK_PROGRESS_BAR (sw->progressbar),
                              _("Completed"));
}


static void
cb_finder_progress_update (GimvDuplFinder *finder, GimvDuplWin *sw)
{
   gfloat progress;

   g_return_if_fail (GIMV_IS_DUPL_FINDER (finder));
   g_return_if_fail (sw);

   progress = gimv_dupl_finder_get_progress (finder);
   gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (sw->progressbar), progress);
}


static void
cb_finder_found (GimvDuplFinder *finder, GimvDuplPair *pair, GimvDuplWin *sw)
{
   g_return_if_fail (GIMV_IS_DUPL_FINDER (finder));
   g_return_if_fail (sw);
   g_return_if_fail (pair);

   gimv_dupl_win_set_thumb (sw, pair->thumb1, pair->thumb2, pair->similarity);
}


static void
cb_select_thumb (GimvThumb *thumb, GimvDuplWin *sw)
{
   g_return_if_fail (GIMV_IS_THUMB (thumb));
   g_return_if_fail (sw);

  if (conf.simwin_sel_thumbview) {
      gimv_thumb_view_set_selection_all (sw->tv, FALSE);
      gimv_thumb_view_set_selection (thumb, TRUE);
      gimv_thumb_view_adjust (sw->tv, thumb);
   }

   if (conf.simwin_sel_preview)
      gimv_thumb_view_open_image (sw->tv, thumb,
                                  GIMV_THUMB_VIEW_OPEN_IMAGE_PREVIEW);
   if (conf.simwin_sel_new_win)
      gimv_thumb_view_open_image (sw->tv, thumb,
                                  GIMV_THUMB_VIEW_OPEN_IMAGE_NEW_WIN);
   if (conf.simwin_sel_shared_win)
      gimv_thumb_view_open_image (sw->tv, thumb,
                                  GIMV_THUMB_VIEW_OPEN_IMAGE_SHARED_WIN);
}



static void
cb_tree_selection_changed (GtkSelectionModel *selection,
                           guint position, guint n_items, GimvDuplWin *sw)
{
   GimvRow *row;
   GimvThumb *thumb;

   g_return_if_fail (sw);

   if (!sw->tv) return;
   if (!sw->ctree) return;

   row = gimv_list_view_get_selected_row (sw->ctree);
   if (!row) return;

   thumb = gimv_row_peek_pointer (row, COLUMN_THUMBDATA);
   g_return_if_fail (GIMV_IS_THUMB (thumb));

   cb_select_thumb (thumb, sw);
}


static GtkWidget *
name_cell_setup (gpointer data)
{
   GtkWidget *box, *picture, *label;

   box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);

   picture = gtk_picture_new ();
   gtk_picture_set_can_shrink (GTK_PICTURE (picture), FALSE);
   gtk_widget_set_valign (picture, GTK_ALIGN_CENTER);
   gtk_box_append (GTK_BOX (box), picture);
   g_object_set_data (G_OBJECT (box), "gimv-cell-image", picture);

   label = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
   gtk_widget_set_hexpand (label, TRUE);
   gtk_box_append (GTK_BOX (box), label);
   g_object_set_data (G_OBJECT (box), "gimv-cell-label", label);

   return box;
}


static void
name_cell_bind (GtkWidget *cell, GimvRow *row, GtkListItem *item, gpointer data)
{
   GimvDuplWin *sw = data;
   gint icon_col = sw->priv && sw->priv->show_thumbnail
      ? COLUMN_THUMBNAIL : COLUMN_ICON;

   gimv_icon_text_cell_set_icon (cell, gimv_row_peek_value (row, icon_col));
   gtk_label_set_text (GTK_LABEL (gimv_icon_text_cell_get_label (cell)),
                       gimv_row_peek_string (row, COLUMN_NAME));
}


/* rebind the cells of rows (and their children) */
static void
refresh_rows (GListModel *rows)
{
   guint i, n = g_list_model_get_n_items (rows);

   for (i = 0; i < n; i++) {
      GimvRow *row = gimv_row_store_peek (rows, i);
      GListStore *children = gimv_row_get_children (row, FALSE);

      gimv_row_changed (row);
      if (children)
         refresh_rows (G_LIST_MODEL (children));
   }
}


static void
cb_change_to_thumbnail_button (GtkWidget *button, GimvDuplWin *sw)
{
   g_return_if_fail (sw);

   if (!gimv_toggle_get_active (button)) return;

   sw->priv->show_thumbnail = TRUE;
   refresh_rows (G_LIST_MODEL (sw->priv->root));
}


static void
cb_change_to_icon_button (GtkWidget *button, GimvDuplWin *sw)
{
   g_return_if_fail (sw);

   if (!gimv_toggle_get_active (button)) return;

   sw->priv->show_thumbnail = FALSE;
   refresh_rows (G_LIST_MODEL (sw->priv->root));
}


/* the row of thumb in rows (and their children) */
static GimvRow *
find_row (GListModel *rows, GimvThumb *thumb)
{
   guint i, n;

   g_return_val_if_fail (rows && thumb, NULL);

   n = g_list_model_get_n_items (rows);
   for (i = 0; i < n; i++) {
      GimvRow *row = gimv_row_store_peek (rows, i), *found;
      GListStore *children;

      if (gimv_row_peek_pointer (row, COLUMN_THUMBDATA) == thumb)
         return row;

      children = gimv_row_get_children (row, FALSE);
      if (children) {
         found = find_row (G_LIST_MODEL (children), thumb);
         if (found) return found;
      }
   }

   return NULL;
}


static GimvRow *
insert_node (GimvDuplWin *sw, GimvRow *parent,
             GimvThumb *thumb, gfloat similar)
{
   GimvRow *row;
   GdkTexture *thumb_pixmap, *icon_pixmap;
   GdkTexture *thumb_mask, *icon_mask;
   gchar *text[32], accuracy[32], *tmpstr;

   g_return_val_if_fail (GIMV_IS_THUMB (thumb), NULL);

   gimv_thumb_get_thumb (thumb, &thumb_pixmap, &thumb_mask);
   gimv_thumb_get_icon (thumb,  &icon_pixmap,  &icon_mask);

   text[0] = (gchar *) gimv_image_info_get_path (thumb->info);
   text[0] = charset_to_internal (text[0],
                                  conf.charset_filename,
                                  conf.charset_auto_detect_fn,
                                  conf.charset_filename_mode);

   if (similar > 0) {
      g_snprintf (accuracy, 32, "%2.1f%%", similar * 100);
      text[1] = accuracy;
   } else {
      text[1] = NULL;
   }

   tmpstr  = fileutil_size2str (thumb->info->st.st_size, FALSE);
   text[2] = charset_locale_to_internal (tmpstr);
   g_free (tmpstr);

   tmpstr  = fileutil_time2str (thumb->info->st.st_mtime);
   text[3] = charset_locale_to_internal (tmpstr);
   g_free (tmpstr);

   g_object_ref (G_OBJECT (thumb));
   sw->priv->thumb_list = g_list_append (sw->priv->thumb_list, thumb);

   row = gimv_row_new (N_COLUMN,
                       GDK_TYPE_TEXTURE,
                       GDK_TYPE_TEXTURE,
                       G_TYPE_STRING,
                       G_TYPE_STRING,
                       G_TYPE_STRING,
                       G_TYPE_STRING,
                       G_TYPE_POINTER);
   gimv_row_set (row,
                 COLUMN_THUMBNAIL,      thumb_pixmap,
                 COLUMN_ICON,           icon_pixmap,
                 COLUMN_NAME,           text[0],
                 COLUMN_ACCURACY,       text[1],
                 COLUMN_SIZE,           text[2],
                 COLUMN_MTIME,          text[3],
                 COLUMN_THUMBDATA,      thumb,
                 COLUMN_TERMINATOR);

   if (parent)
      gimv_row_append_child (parent, row);
   else
      g_list_store_append (sw->priv->root, row);
   g_object_unref (row);   /* the store keeps it */

   g_free (text[0]);
   g_free (text[2]);
   g_free (text[3]);

   return row;
}




/******************************************************************************
 *
 *   Public Functions.
 *
 ******************************************************************************/
GimvDuplWin *
gimv_dupl_win_new (gint thumbnail_size)
{
   GimvDuplWin *sw;

   sw = GIMV_DUPL_WIN (g_object_new (GIMV_TYPE_DUPL_WIN, NULL));

   /* FIXME */
   sw->priv->thumbnail_size = thumbnail_size;
   /* GTK4: shown by the caller (after setting the transient parent) */
   gimv_icon_stock_set_window_icon (GTK_WIDGET (sw), "gimv_icon");
   /* END FIXME */

   return sw;
}


void
gimv_dupl_win_set_relation (GimvDuplWin *sw, GimvThumbView *tv)
{
   g_return_if_fail (sw);
   g_return_if_fail (tv);

   sw->tv = tv;

   gtk_widget_set_sensitive (sw->select_button, TRUE);
}


void
gimv_dupl_win_unset_relation (GimvDuplWin *sw)
{
   g_return_if_fail (sw);

   sw->tv = NULL;

   gtk_widget_set_sensitive (sw->select_button, FALSE);
}


void
gimv_dupl_win_set_thumb (GimvDuplWin *sw,
                         GimvThumb *thumb1,
                         GimvThumb*thumb2,
                         gfloat     similar)
{
   g_return_if_fail (sw);
   g_return_if_fail (GIMV_IS_THUMB (thumb1));
   g_return_if_fail (GIMV_IS_THUMB (thumb2));

{
   GimvRow *parent;

   parent = find_row (G_LIST_MODEL (sw->priv->root), thumb1);
   if (!parent) {
      parent = insert_node (sw, NULL, thumb1, -1);

      /* select the first row, without opening it */
      if (gimv_list_view_get_selected (sw->ctree) == GTK_INVALID_LIST_POSITION) {
         GtkSelectionModel *selection = gimv_list_view_get_selection (sw->ctree);
         guint position;

         if (gimv_row_store_find (G_LIST_MODEL (selection), parent, &position)) {
            g_signal_handlers_block_by_func (selection,
                                             cb_tree_selection_changed,
                                             sw);
            gimv_list_view_select (sw->ctree, position);
            g_signal_handlers_unblock_by_func (selection,
                                               cb_tree_selection_changed,
                                               sw);
         }
      }
   }
   if (parent) {
      GtkSelectionModel *selection = gimv_list_view_get_selection (sw->ctree);
      GtkTreeListModel *model
         = GTK_TREE_LIST_MODEL (gtk_single_selection_get_model (GTK_SINGLE_SELECTION (selection)));

      insert_node (sw, parent, thumb2, similar);
      gimv_tree_list_model_expand_to (model, parent);
   }
}
}
