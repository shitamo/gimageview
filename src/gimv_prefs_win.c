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
 * $Id: gimv_prefs_win.c,v 1.8 2004/09/22 15:37:11 makeinu Exp $
 */

#include "gimv_prefs_win.h"

#include <stdlib.h>
#include <string.h>

#include "gimageview.h"

#include "fileutil.h"
#include "gimv_hpaned.h"
#include "gtk2-compat.h"
#include "gimv_list_view.h"
#include "gimv_icon_stock.h"
#include "help.h"
#include "prefs.h"
#include "prefs_ui/prefs_ui_common.h"
#include "prefs_ui/prefs_ui_etc.h"
#include "prefs_ui/prefs_ui_movie.h"
#include "prefs_ui/prefs_ui_plugin.h"
#include "prefs_ui/prefs_ui_progs.h"
#include "prefs_ui/prefs_ui_imagewin.h"
#include "prefs_ui/prefs_ui_thumbalbum.h"
#include "prefs_ui/prefs_ui_thumbwin.h"


typedef struct PrefsWin_Tag {
   GtkWidget *notebook;
   GtkWidget *tree;
   gboolean   ok_pressed;
} PrefsWin;


typedef struct GimvPrefsWinPagePrivate_Tag
{
   GimvPrefsWinPage *page;
   GtkWidget    *widget;
} GimvPrefsWinPagePrivate;


static GimvPrefsWinPage prefs_pages[] = {
   {N_("/Information"),                 0, NULL, NULL, gimvhelp_create_info_widget,  NULL},
   {N_("/Common"),                     0, NULL, NULL, prefs_common_page,            prefs_common_apply},
   {N_("/Common/Filtering"),           0, NULL, NULL, prefs_filter_page,            prefs_filter_apply},
   {N_("/Common/Character Set"),       0, NULL, NULL, prefs_charset_page,           prefs_charset_apply},

   {N_("/Image Window"),               0, NULL, NULL, prefs_imagewin_page,          NULL},
   {N_("/Image Window/Image"),         0, NULL, NULL, prefs_imagewin_image_page,    prefs_ui_imagewin_image_apply},
   {N_("/Image Window/Mouse Buttons"), 0, NULL, NULL, prefs_imagewin_mouse_page,    NULL},

   {N_("/Thumbnail Window"),           0, NULL, NULL, prefs_thumbwin_page,          prefs_ui_thumbwin_apply},
   /*
   {N_("/Thumbnail Window/Toolbar"),   0, NULL, NULL, prefs_thumbview_toolbar_page, NULL},
   */
   {N_("/Thumbnail Window/Tab"),                            0, NULL, NULL, prefs_thumbwin_tab_page,      prefs_ui_thumbwin_tab_apply},
   {N_("/Thumbnail Window/Thumbnail View"),                 0, NULL, NULL, prefs_thumbview_page,         NULL},
   {N_("/Thumbnail Window/Thumbnail View/Mouse Buttons"),   0, NULL, NULL, prefs_thumbview_mouse_page,   NULL},
   {N_("/Thumbnail Window/Thumbnail View/Album"),           0, NULL, NULL, prefs_ui_thumbalbum,          NULL},

   {N_("/Thumbnail Window/Directory View"),                 0, NULL, NULL, prefs_dirview_page,           prefs_ui_dirview_apply},
   {N_("/Thumbnail Window/Directory View/Mouse Buttons"),   0, NULL, NULL, prefs_dirview_mouse_page,     NULL},
   {N_("/Thumbnail Window/Preview"),                        0, NULL, NULL, prefs_preview_page,           prefs_ui_preview_apply},
   {N_("/Thumbnail Window/Preview/Mouse Buttons"),          0, NULL, NULL, prefs_preview_mouse_page,     NULL},

   {N_("/Movie and Audio"),            0, NULL, NULL, prefs_movie_page,               prefs_movie_apply},

   {N_("/Slideshow"),                 0, NULL, NULL, prefs_slideshow_page,           NULL},
   {N_("/Thumbnail Cache"),            0, NULL, NULL, prefs_cache_page,               NULL},
   {N_("/Comment"),                    0, NULL, NULL, prefs_comment_page,             prefs_comment_apply},
   {N_("/Search"),                     0, NULL, NULL, prefs_search_page,              NULL},
   {N_("/Drag and Drop"),              0, NULL, NULL, prefs_dnd_page,                 NULL},
   {N_("/External Program"),           0, NULL, NULL, prefs_progs_page,               NULL},
   {N_("/External Program/Scripts"),   0, NULL, NULL, prefs_scripts_page,             NULL},

   {N_("/Plugin"),                     0, NULL, NULL, prefs_ui_plugin,              prefs_ui_plugin_apply},
   {N_("/Plugin/Image Loader"),        0, NULL, NULL, prefs_ui_plugin_image_loader, NULL},
   {N_("/Plugin/Image Saver"),         0, NULL, NULL, prefs_ui_plugin_image_saver,  NULL},
   {N_("/Plugin/IO Stream"),           0, NULL, NULL, prefs_ui_plugin_io_stream,    NULL},
   {N_("/Plugin/Archiver"),            0, NULL, NULL, prefs_ui_plugin_archiver,     NULL},
   {N_("/Plugin/Thumbnail"),           0, NULL, NULL, prefs_ui_plugin_thumbnail,    NULL},
   {N_("/Plugin/Image View"),          0, NULL, NULL, prefs_ui_plugin_imageview,    NULL},
   {N_("/Plugin/Thumbnail View"),      0, NULL, NULL, prefs_ui_plugin_thumbview,    NULL},
};
static gint prefs_pages_num = sizeof (prefs_pages) / sizeof (prefs_pages[0]);

GList *prefs_pages_list = NULL;

Config *config_changed    = NULL;
Config *config_prechanged = NULL;


static GtkWidget *prefs_window;
static PrefsWin   prefs_win = {
   notebook:   NULL,
   tree:       NULL,
   ok_pressed: FALSE,
};
static GList *priv_page_list = NULL;


static GList *
get_page_entries_list (void)
{
   if (!prefs_pages_list) {
      gint i;

      for (i = 0; i < prefs_pages_num; i++)
         prefs_pages_list = g_list_append (prefs_pages_list, &prefs_pages[i]);
   }

   return prefs_pages_list;
}


gboolean
gimv_prefs_win_add_page_entry (GimvPrefsWinPage *page)
{
   g_return_val_if_fail (page, FALSE);

   get_page_entries_list ();
#warning FIXME: check path
   prefs_pages_list = g_list_append (prefs_pages_list, page);
   return TRUE;
}


/*******************************************************************************
 *
 *   Private functions
 *
 *******************************************************************************/
static void
prefs_win_apply_config (GimvPrefsWinAction action)
{
   GList *node;

   for (node = priv_page_list; node; node = g_list_next (node)) {
      GimvPrefsWinPagePrivate *priv = node->data;
      if (priv && priv->widget && priv->page && priv->page->apply_fn)
         priv->page->apply_fn (action);
   }
}


/* FIXME!! too stupid ;-( */
/* Damn Damn Damn XD */
static void
prefs_win_free_strings (gboolean value_changed)
{
   gint i;
   gchar **strings[] = {
      &conf.text_viewer,
      &conf.web_browser,
      &conf.plugin_search_dir_list,
      &conf.scripts_search_dir_list,
      &conf.imgview_mouse_button,
      &conf.preview_mouse_button,
      &conf.imgtype_disables,
      &conf.imgtype_user_defs,
      &conf.comment_key_list,
      &conf.charset_locale,
      &conf.charset_internal,
   };
   gint num1 = sizeof (strings) / sizeof (gchar **);
   gint num2 = sizeof (conf.progs) / sizeof (gchar *);

   for (i = 0; i < num1 + num2; i++) {
      gchar **src, **dest, **member;
      gulong offset;

      if (i < num1)
         member = strings[i];
      else if (i < num1 + num2)
         member = &conf.progs[i - num1];
      else
         break;

      offset = (gchar *) member - (gchar *) &conf;
      src  = G_STRUCT_MEMBER_P (config_prechanged, offset);
      dest = G_STRUCT_MEMBER_P (config_changed, offset);

      if (*src == *dest) continue;

      if (value_changed) {
         g_free (*src);
      } else {
         g_free (*dest);
      }
   }
}
/* END FIXME!! */


static void
prefs_win_create_page (GimvPrefsWinPagePrivate *priv)
{
   const gchar *title = NULL;
   GtkWidget *vbox, *label;

   if (!priv || !priv->page) return;
   if (priv->widget) return;

   /* translate page title */
   if (priv->page->path)
      title = fileutil_basename (_(priv->page->path));

   /* create page */
   if (priv->page->create_page_fn) {
      vbox = priv->page->create_page_fn ();
      label = gtk_label_new (title);
      gtk_notebook_append_page (GTK_NOTEBOOK(prefs_win.notebook),
                                vbox, label);
      priv->widget = vbox;
   }
}


static void
prefs_win_set_page (const gchar *path)
{
   GimvPrefsWinPagePrivate *priv = NULL;
   GList *node;
   gint num;

   if (!path || !*path) {
      if (!priv_page_list) return;
      priv = priv_page_list->data;
   } else {
      for (node = priv_page_list; node; node = g_list_next (node)) {
         priv = node->data;
         if (priv->page && !strcmp (path, priv->page->path)) 
            break;
         priv = NULL;
      }
   }

   if (!priv) {
      if (!priv_page_list) return;
      priv = priv_page_list->data;
   }

   if (!priv->widget) {
      prefs_win_create_page (priv);
   }
   if (priv->widget)
      gtk_widget_set_visible (priv->widget, TRUE);
   else
      return;

   num = gtk_notebook_page_num (GTK_NOTEBOOK (prefs_win.notebook),
                                priv->widget);
   if (num >= 0)
      gtk_notebook_set_current_page (GTK_NOTEBOOK (prefs_win.notebook), num);

}



/*******************************************************************************
 *
 *  functions for creating navigate tree
 *
 *******************************************************************************/
static void
get_icon (const gchar *name, GdkTexture **pixmap, GdkTexture **mask)
{
   GimvIcon *icon;

   if (!name || !*name) return;

   icon  = gimv_icon_stock_get_icon (name);
   if (icon) {
      if (pixmap)
         *pixmap = icon->pixmap;
      if (mask)
         *mask = icon->mask;
   } else {
      if (pixmap)
         *pixmap = NULL;
      if (mask)
         *mask = NULL;
   }
}



typedef enum {
   COLUMN_TERMINATOR = -1,
   COLUMN_ICON_OPEN,
   COLUMN_ICON_CLOSE,
   COLUMN_NAME,
   COLUMN_PRIV_DATA,
   N_COLUMN
} TreeStoreColumn;


/* the row of the page whose path is "path" in rows (and their children) */
static GimvRow *
find_row_by_path (GListModel *rows, const gchar *path)
{
   guint i, n = g_list_model_get_n_items (rows);

   for (i = 0; i < n; i++) {
      GimvRow *row = gimv_row_store_peek (rows, i), *found;
      GimvPrefsWinPagePrivate *priv;
      GListStore *children;

      priv = gimv_row_peek_pointer (row, COLUMN_PRIV_DATA);
      if (priv && !strcmp (priv->page->path, path))
         return row;

      children = gimv_row_get_children (row, FALSE);
      if (children) {
         found = find_row_by_path (G_LIST_MODEL (children), path);
         if (found) return found;
      }
   }

   return NULL;
}


static GimvRow *
prefs_win_navtree_get_parent (GListModel *root, GimvPrefsWinPagePrivate *priv)
{
   GimvRow *row;
   gchar *path;

   g_return_val_if_fail (priv, NULL);
   g_return_val_if_fail (priv->page, NULL);
   g_return_val_if_fail (priv->page->path, NULL);

   path = g_path_get_dirname (priv->page->path);
   row = find_row_by_path (root, path);
   g_free (path);

   return row;
}


static gboolean
cb_tree_key_press (GtkWidget *widget, GimvEventKey *event, gpointer data)
{
   GtkTreeListRow *tree_row;
   gboolean retval = FALSE;
   guint position;

   position = gimv_list_view_get_selected (widget);
   if (position == GTK_INVALID_LIST_POSITION) return FALSE;
   tree_row = g_list_model_get_item (G_LIST_MODEL (gimv_list_view_get_selection (widget)),
                                     position);
   if (!GTK_IS_TREE_LIST_ROW (tree_row)) {
      if (tree_row) g_object_unref (tree_row);
      return FALSE;
   }

   switch (event->keyval) {
   case GDK_KEY_KP_Enter:
   case GDK_KEY_Return:
   case GDK_KEY_ISO_Enter:
   case GDK_KEY_space:
      gtk_tree_list_row_set_expanded (tree_row,
                                      !gtk_tree_list_row_get_expanded (tree_row));
      retval = TRUE;
      break;
   case GDK_KEY_Right:
      gtk_tree_list_row_set_expanded (tree_row, TRUE);
      retval = TRUE;
      break;
   case GDK_KEY_Left:
      gtk_tree_list_row_set_expanded (tree_row, FALSE);
      retval = TRUE;
      break;
   default:
      break;
   }

   g_object_unref (tree_row);

   return retval;
}


static void
cb_tree_selection_changed (GtkSelectionModel *selection,
                           guint position, guint n_items, gpointer data)
{
   GimvPrefsWinPagePrivate *priv;
   GimvRow *row;

   if (!prefs_win.tree) return;

   row = gimv_list_view_get_selected_row (prefs_win.tree);
   if (!row) return;

   priv = gimv_row_peek_pointer (row, COLUMN_PRIV_DATA);

   g_return_if_fail (priv);
   g_return_if_fail (priv->page);

   prefs_win_set_page (priv->page->path);
}


/* the icon of a page (the open icon if it is expanded) and its name */
static void
navtree_cell_bind (GtkWidget *cell, GimvRow *row, GtkListItem *item,
                   gpointer data)
{
   GtkTreeListRow *tree_row = gimv_list_item_get_tree_row (item);
   gint icon_col = COLUMN_ICON_CLOSE;

   if (tree_row && gtk_tree_list_row_get_expanded (tree_row)
       && gimv_row_peek_object (row, COLUMN_ICON_OPEN))
      icon_col = COLUMN_ICON_OPEN;

   gimv_icon_text_cell_set_icon (cell, gimv_row_peek_value (row, icon_col));
   gtk_label_set_text (GTK_LABEL (gimv_icon_text_cell_get_label (cell)),
                       gimv_row_peek_string (row, COLUMN_NAME));
}


static GtkWidget *
navtree_cell_setup (gpointer data)
{
   GtkWidget *box, *picture, *label;

   box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);

   picture = gtk_picture_new ();
   gtk_picture_set_can_shrink (GTK_PICTURE (picture), FALSE);
   gtk_box_append (GTK_BOX (box), picture);
   g_object_set_data (G_OBJECT (box), "gimv-cell-image", picture);

   label = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_box_append (GTK_BOX (box), label);
   g_object_set_data (G_OBJECT (box), "gimv-cell-label", label);

   return box;
}


static GtkWidget *
prefs_win_create_navtree (void)
{
   GListStore *root;
   GtkTreeListModel *model;
   GtkWidget *tree;
   GList *node;

   /* GTK4: GtkTreeView (deprecated) -> GtkListView of a GtkTreeListModel */
   root = gimv_row_store_new ();
   model = gimv_tree_list_model_new (G_LIST_MODEL (root));
   tree = gimv_list_view_new (G_LIST_MODEL (model), GTK_SELECTION_BROWSE,
                              gimv_cell_factory_new (navtree_cell_setup,
                                                     navtree_cell_bind,
                                                     NULL, NULL, TRUE));
   prefs_win.tree = tree;

   gimv_event_connect (GTK_WIDGET (tree), GIMV_EVENT_KEY_PRESS, G_CALLBACK (cb_tree_key_press), NULL);
   g_signal_connect (gimv_list_view_get_selection (tree), "selection-changed",
                     G_CALLBACK (cb_tree_selection_changed), NULL);

   /* create pages */
   for (node = get_page_entries_list(); node; node = g_list_next (node)) {
      GimvPrefsWinPage *page = node->data;
      const gchar *title;
      GdkTexture *pixmap = NULL, *opixmap = NULL;
      GimvPrefsWinPagePrivate *priv;
      GimvRow *row, *parent_row;

      if (!page || !page->path) continue;

      /* translate page title */
      title = fileutil_basename (_(page->path));

      /* get node icon */
      get_icon (page->icon,      &pixmap,  NULL);
      get_icon (page->icon_open, &opixmap, NULL);

      /* set private data */
      priv = g_new0 (GimvPrefsWinPagePrivate, 1);
      priv->page   = page;
      priv->widget = NULL;

      priv_page_list = g_list_append (priv_page_list, priv);

      row = gimv_row_new (N_COLUMN,
                          GDK_TYPE_TEXTURE,
                          GDK_TYPE_TEXTURE,
                          G_TYPE_STRING,
                          G_TYPE_POINTER);
      gimv_row_set (row,
                    COLUMN_ICON_CLOSE,      pixmap,
                    COLUMN_ICON_OPEN,       opixmap,
                    COLUMN_NAME,            title,
                    COLUMN_PRIV_DATA,       priv,
                    COLUMN_TERMINATOR);

      parent_row = prefs_win_navtree_get_parent (G_LIST_MODEL (root), priv);
      if (parent_row)
         gimv_row_append_child (parent_row, row);
      else
         g_list_store_append (root, row);
      g_object_unref (row);
   }

   g_object_unref (root);

   return tree;
}




/*******************************************************************************
 *
 *   Callback functions for preference window.
 *
 *******************************************************************************/
static void
cb_prefs_win_destroy ()
{
   if (prefs_win.ok_pressed) {
      prefs_win_free_strings (TRUE);
      conf = *config_changed;
      prefs_win_apply_config (GIMV_PREFS_WIN_ACTION_OK);
   } else {
      prefs_win_free_strings (FALSE);
      conf = *config_prechanged;
      prefs_win_apply_config (GIMV_PREFS_WIN_ACTION_CANCEL);
   }

   g_free (config_changed);
   g_free (config_prechanged);
   config_changed       = NULL;
   config_prechanged    = NULL;

   prefs_window         = NULL;
   prefs_win.notebook   = NULL;
   prefs_win.tree       = NULL;
   prefs_win.ok_pressed = FALSE;

   g_list_foreach (priv_page_list, (GFunc) g_free, NULL);
   g_list_free (priv_page_list);
   priv_page_list = NULL;

   prefs_save_config ();
}



static void
cb_prefs_win_ok (GtkButton *button, gpointer data)
{
   prefs_win.ok_pressed = TRUE;
   gimv_widget_destroy (prefs_window);
}


static void
cb_prefs_win_apply (GtkButton *button, gpointer data)
{
   memcpy (&conf, config_changed, sizeof(Config));
   prefs_win_apply_config (GIMV_PREFS_WIN_ACTION_APPLY);
}


static void
cb_prefs_win_cancel (GtkButton *button, gpointer data)
{
   gimv_widget_destroy (prefs_window);
}




/*******************************************************************************
 *
 *   prefs_open_window:
 *      @ Create preference window. If already opened, raise it and return.
 *
 *   GtkWidget *prefs_window (global variable):
 *      Pointer to preference window.
 *
 *   Static PrefsWin prefs_win (local variable):
 *      store pointer to eache widget;
 *
 *******************************************************************************/
GtkWidget *
gimv_prefs_win_open (const gchar *path, GtkWindow *parent)
{
   GtkWidget *notebook, *pane, *scrolledwin, *navtree;

   /* if preference window is alredy opend, raise it and return */
   if (prefs_window) {
      gtk_window_present (GTK_WINDOW (prefs_window));
      return prefs_window;
   }

   prefs_win.ok_pressed = FALSE;

   /* allocate buffer for new config */
   config_prechanged = g_memdup2 (&conf, sizeof(Config));
   config_changed = g_memdup2 (&conf, sizeof(Config));

   /* create config window */
   prefs_window = gimv_dialog_new ();
   if (parent)
      gtk_window_set_transient_for (GTK_WINDOW (prefs_window), parent);
   else
      gimv_window_set_default_transient (GTK_WINDOW (prefs_window));
   /* GTK4: gtk_window_set_wmclass () removed */
   gtk_window_set_default_size (GTK_WINDOW(prefs_window), 600, 450);
   gtk_window_set_title (GTK_WINDOW (prefs_window), _("Preferences")); 
   g_signal_connect (G_OBJECT (prefs_window), "destroy",
                       G_CALLBACK(cb_prefs_win_destroy), NULL);

   /* pane */
   pane = gimv_hpaned_new ();
   gimv_container_set_border_width (GTK_WIDGET (pane), 5);
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_vbox (GTK_WIDGET (prefs_window))), 
                       pane, TRUE, TRUE, 0);
   gtk_widget_set_visible (pane, TRUE);

   notebook = prefs_win.notebook = gtk_notebook_new ();
   gtk_notebook_set_tab_pos (GTK_NOTEBOOK (notebook), GTK_POS_TOP);
   gtk_notebook_set_scrollable (GTK_NOTEBOOK (notebook), TRUE);
   /* gtk_notebook_popup_enable (GTK_NOTEBOOK (notebook)); */
   gtk_notebook_set_show_tabs (GTK_NOTEBOOK (notebook), FALSE);
   gtk_notebook_set_show_border (GTK_NOTEBOOK (notebook), FALSE);
   gtk_widget_set_visible (notebook, TRUE);

   /* scrolled window */
   scrolledwin = gimv_scrolled_window_new (NULL, NULL);
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(scrolledwin),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scrolledwin), TRUE);
   gimv_widget_set_size (scrolledwin, 170, -1);
   gtk_widget_set_visible (scrolledwin, TRUE);

   /* navigation tree */
   navtree = prefs_win_create_navtree ();
   gimv_container_add (GTK_WIDGET (scrolledwin), navtree);
   gtk_widget_set_visible (navtree, TRUE);

   /* GTK4: children that may shrink have no minimum size, which would let
      the default window size clip the pages (GTK2 always requested the
      children's size) */
   gimv_paned_pack1 (GTK_PANED (pane), scrolledwin, FALSE, FALSE);
   gimv_paned_pack2 (GTK_PANED (pane), notebook, TRUE, FALSE);

   /* button */
   {
      GtkWidget *action_area = gimv_dialog_get_action_area (prefs_window);
      GtkWidget *button;

      button = gtk_button_new_with_mnemonic (GIMV_STOCK_APPLY);
      gimv_box_pack_start (GTK_BOX (action_area), button, TRUE, TRUE, 0);
      g_signal_connect (button, "clicked",
                        G_CALLBACK (cb_prefs_win_apply), NULL);

      button = gtk_button_new_with_mnemonic (GIMV_STOCK_CANCEL);
      gimv_box_pack_start (GTK_BOX (action_area), button, TRUE, TRUE, 0);
      g_signal_connect (button, "clicked",
                        G_CALLBACK (cb_prefs_win_cancel), NULL);

      button = gtk_button_new_with_mnemonic (GIMV_STOCK_OK);
      gimv_box_pack_start (GTK_BOX (action_area), button, TRUE, TRUE, 0);
      g_signal_connect (button, "clicked",
                        G_CALLBACK (cb_prefs_win_ok), NULL);
   }

   /* GTK4: create all pages before showing, so that the notebook requests
      the size of the largest one: created on demand, a larger page grew the
      window and the window manager moved it when a page was selected */
   {
      GList *node;
      for (node = priv_page_list; node; node = g_list_next (node))
         prefs_win_create_page (node->data);
   }

   /* GTK4: window position (GTK_WIN_POS_CENTER) is up to the compositor */
   gtk_widget_set_visible (prefs_window, TRUE);
   gimv_icon_stock_set_window_icon (prefs_window, "prefs");

   prefs_win_set_page (NULL);

   return prefs_window;
}


static GtkWindow *auauauau_window = NULL;

static gboolean
idle_prefs_win_open (gpointer data)
{
   gchar *path = data;
   gimv_prefs_win_open (path, auauauau_window);
   auauauau_window = NULL;
   g_free (path);
   return FALSE;
}


void
gimv_prefs_win_open_idle (const gchar *path, GtkWindow *parent)
{
   gchar *str = NULL;

   if (path)
      str = g_strdup (path);

   auauauau_window = parent;

   g_idle_add (idle_prefs_win_open, str);
}


gboolean
gimv_prefs_win_is_opened (void)
{
   if (prefs_window)
      return TRUE;
   else
      return FALSE;
}


GtkWidget *
gimv_prefs_win_get (void)
{
   return prefs_window;
}
