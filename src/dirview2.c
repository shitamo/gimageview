/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
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
 * $Id: dirview2.c,v 1.38 2004/12/20 14:53:45 makeinu Exp $
 */

#include "dirview.h"


#include <string.h>

#include "charset.h"
#include "dirview_priv.h"
#include "dnd.h"
#include "fileutil.h"
#include "gfileutil.h"
#include "gtk_prop.h"
#include "gimv_icon_stock.h"
#include "gimv_list_view.h"
#include "gimv_thumb_win.h"
#include "menu.h"
#include "prefs.h"


/*
 *  GTK4: GtkTreeView/GtkTreeStore (deprecated) -> GtkListView of a
 *  GtkTreeListModel of GimvRow.  The subdirectories of a directory are read
 *  when it is expanded the first time (the GtkTreeStore version used dummy
 *  child rows for that); until then the row has an empty list of children,
 *  which makes it expandable.
 */
typedef enum {
   COLUMN_TERMINATOR = -1,
   COLUMN_LABEL,
   COLUMN_FULLPATH,
   COLUMN_ICON_OPEN,
   COLUMN_ICON_CLOSE,
   COLUMN_LOADED,      /* the subdirectories were read */
   N_COLUMN
} TreeStoreColumn;


/* callback functions */
static void       cb_dirview_destroyed           (GtkWidget      *widget,
                                                  DirView        *dv);
static gboolean   cb_button_press                (GtkWidget      *widget,
                                                  GimvEventButton *event,
                                                  DirView        *dv);
static gboolean   cb_button_release              (GtkWidget      *widget,
                                                  GimvEventButton *event,
                                                  DirView        *dv);
static gboolean   cb_scroll                      (GtkWidget      *widget,
                                                  GimvEventScroll *se,
                                                  DirView        *dv);
static gboolean   cb_key_press                   (GtkWidget      *widget,
                                                  GimvEventKey    *event,
                                                  DirView        *dv);
static void       dirview_load_subdirs           (DirView        *dv,
                                                  GimvRow        *row);


/* callback functions for popup menu */
static void       cb_open_thumbnail              (DirView        *dv,
                                                  ScanSubDirType  scan_subdir,
                                                  GimvMenuItem      *menuitem);
static void       cb_go_to_here                  (DirView        *dv,
                                                  guint           action,
                                                  GimvMenuItem      *menuitem);
static void       cb_refresh_dir_tree            (DirView        *dv,
                                                  guint           action,
                                                  GimvMenuItem      *menuitem);
static void       cb_file_property               (DirView        *tv,
                                                  guint           action,
                                                  GimvMenuItem      *menuitem);
static void       cb_mkdir                       (DirView        *dv,
                                                  guint           action,
                                                  GimvMenuItem      *menuitem);
static void       cb_rename_dir                  (DirView        *dv,
                                                  guint           action,
                                                  GimvMenuItem      *menuitem);
static void       cb_delete_dir                  (DirView        *dv,
                                                  guint           action,
                                                  GimvMenuItem      *menuitem);

/* Callback functions for toolbar buttons */
static void       cb_home_button                 (GtkWidget      *widget,
                                                  DirView        *dv);
static void       cb_up_button                   (GtkWidget      *widget,
                                                  DirView        *dv);
static void       cb_refresh_button              (GtkWidget      *widget,
                                                  DirView        *dv);
static void       cb_dotfile_button              (GtkWidget      *widget,
                                                  DirView        *dv);

/* Callback functions for DnD */
static void       cb_drag_data_get               (GtkWidget        *dirtree,
                                                  GimvDragContext   *context,
                                                  GimvSelectionData *seldata,
                                                  guint             info,
                                                  guint             time,
                                                  gpointer          data);
static void       cb_drag_data_received          (GtkWidget        *dirtree,
                                                  GimvDragContext   *context,
                                                  gint              x,
                                                  gint              y,
                                                  GimvSelectionData *seldata,
                                                  guint             info,
                                                  guint32           time,
                                                  gpointer          data);
static void       cb_drag_leave                  (GtkWidget        *dirtree,
                                                  GimvDragContext   *context,
                                                  guint             time,
                                                  gpointer          data);
static void       cb_drag_end                    (GtkWidget        *dirtree,
                                                  GimvDragContext   *context,
                                                  gpointer          data);
static void       cb_toolbar_drag_begin          (GtkWidget        *widget,
                                                  GimvDragContext   *context,
                                                  gpointer          data);
static gboolean   cb_drag_motion                 (GtkWidget *widget,
                                                  GimvDragContext *drag_context,
                                                  gint x,
                                                  gint y,
                                                  guint time,
                                                  gpointer          data);
static void       cb_toolbar_drag_data_get       (GtkWidget        *widget,
                                                  GimvDragContext   *context,
                                                  GimvSelectionData *seldata,
                                                  guint             info,
                                                  guint             time,
                                                  gpointer          data);
static void       cb_com_swap_drag_data_received (GtkWidget        *widget,
                                                  GimvDragContext   *context,
                                                  gint              x,
                                                  gint              y,
                                                  GimvSelectionData *seldata,
                                                  guint             info,
                                                  guint             time,
                                                  gpointer          data);

/* other private functions */
static void       get_icon_pixbufs               (void);

static void       dirview_create_treeview        (DirView        *dv,
                                                  const gchar    *root);
static GtkWidget *dirview_create_toolbar         (DirView        *dv);
static GimvRow    *insert_row                     (DirView        *dv,
                                                  GimvRow        *parent,
                                                  const gchar    *label,
                                                  const gchar    *fullpath);
static GimvRow    *get_iter_from_path             (DirView        *dv,
                                                  const gchar    *str);
static void       adjust_tree                    (DirView        *dv,
                                                  GimvRow        *row);
static void       adjust_tree_idle               (DirView        *dv,
                                                  GimvRow        *row);
static void       refresh_dir_tree               (DirView        *dv,
                                                  GimvRow        *parent);
static void       dirview_popup_menu             (DirView        *dv,
                                                  GimvEventButton *event);
static gboolean   dirview_button_action          (DirView        *dv,
                                                  GimvEventButton *event,
                                                  gint            num);


static GimvMenuEntry dirview_popup_items [] =
{
   {N_("/_Load Thumbnails"),                        NULL, cb_open_thumbnail, SCAN_SUB_DIR_NONE,     NULL},
   {N_("/Load Thumbnails Re_cursively"),            NULL, cb_open_thumbnail, SCAN_SUB_DIR,          NULL},
   {N_("/Load Thumbnails Recursively in _One Tab"), NULL, cb_open_thumbnail, SCAN_SUB_DIR_ONE_TAB,  NULL},
   {N_("/---"),                         NULL, NULL,                0,     "<Separator>"},
   {N_("/_Go Here"),                 NULL, cb_go_to_here,       0,     NULL},
   {N_("/_Refresh Tree"),               NULL, cb_refresh_dir_tree, 0,     NULL},
   {N_("/---"),                         NULL, NULL,                0,     "<Separator>"},
   {N_("/_Properties..."),                NULL, cb_file_property,    0,     NULL},
   {N_("/---"),                         NULL, NULL,                0,     "<Separator>"},
   {N_("/_Make Directory..."),          NULL, cb_mkdir,            0,     NULL},
   {N_("/Re_name Directory..."),        NULL, cb_rename_dir,       0,     NULL},
   {N_("/_Delete Directory..."),        NULL, cb_delete_dir,       0,     NULL},
   {NULL, NULL, NULL, 0, NULL},
};


static GdkPixbuf *folder      = NULL;
static GdkPixbuf *ofolder     = NULL;
static GdkPixbuf *lfolder     = NULL;
static GdkPixbuf *lofolder    = NULL;
static GdkPixbuf *go_folder   = NULL;
static GdkPixbuf *up_folder   = NULL;
static GdkPixbuf *lock_folder = NULL;



/******************************************************************************
 *
 *   tree model helpers
 *
 ******************************************************************************/
#define DIRVIEW_KEY       "gimv-dirview"
#define ROOT_STORE_KEY    "gimv-dirview-root-store"

static GListStore *
dirview_root_store (DirView *dv)
{
   return g_object_get_data (G_OBJECT (dv->dirtree), ROOT_STORE_KEY);
}


static GtkTreeListModel *
dirview_tree_model (DirView *dv)
{
   GtkSelectionModel *selection = gimv_list_view_get_selection (dv->dirtree);
   return GTK_TREE_LIST_MODEL (gtk_single_selection_get_model (GTK_SINGLE_SELECTION (selection)));
}


/* the row of the root directory */
static GimvRow *
dirview_get_root_row (DirView *dv)
{
   return gimv_row_store_peek (G_LIST_MODEL (dirview_root_store (dv)), 0);
}


static GimvRow *
dirview_get_selected_row (DirView *dv)
{
   return gimv_list_view_get_selected_row (dv->dirtree);
}


/* the row at widget coordinates x, y */
static GimvRow *
dirview_row_at (DirView *dv, gdouble x, gdouble y, guint *position)
{
   GimvRow *row = NULL;

   if (!gimv_list_view_get_row_at (dv->dirtree, x, y, position, &row))
      return NULL;
   return row;
}


static gboolean
dirview_row_is_expanded (DirView *dv, GimvRow *row)
{
   GtkTreeListRow *tree_row;
   gboolean expanded = FALSE;

   tree_row = gimv_tree_list_model_get_row_for (dirview_tree_model (dv), row, NULL);
   if (tree_row) {
      expanded = gtk_tree_list_row_get_expanded (tree_row);
      g_object_unref (tree_row);
   }

   return expanded;
}


/* read the subdirectories (once) and expand the row (and its parents) */
static void
dirview_expand_row (DirView *dv, GimvRow *row)
{
   g_return_if_fail (row);

   dirview_load_subdirs (dv, row);
   gimv_tree_list_model_expand_to (dirview_tree_model (dv), row);
}


static void
dirview_collapse_row (DirView *dv, GimvRow *row)
{
   GtkTreeListRow *tree_row;

   tree_row = gimv_tree_list_model_get_row_for (dirview_tree_model (dv), row, NULL);
   if (tree_row) {
      gtk_tree_list_row_set_expanded (tree_row, FALSE);
      g_object_unref (tree_row);
   }
}


/******************************************************************************
 *
 *   Callback functions
 *
 ******************************************************************************/
static void
cb_dirview_destroyed (GtkWidget *widget, DirView *dv)
{
   g_return_if_fail (dv);

   if (dv->priv->button_action_id)
      g_source_remove (dv->priv->button_action_id);
   dv->priv->button_action_id = 0;

   if (dv->priv->swap_com_id)
      g_source_remove (dv->priv->swap_com_id);
   dv->priv->swap_com_id = 0;

   if (dv->priv->adjust_tree_id)
      g_source_remove (dv->priv->adjust_tree_id);
   dv->priv->adjust_tree_id = 0;

   if (dv->popup_menu)
      gimv_menu_destroy (dv->popup_menu);
   dv->popup_menu = NULL;

   if (dv->dirtree) {
      g_object_remove_weak_pointer (G_OBJECT (dv->dirtree), (gpointer *) &dv->dirtree);
      g_object_set_data (G_OBJECT (dv->dirtree), DIRVIEW_KEY, NULL);
   }

   g_free (dv->root_dir);
   g_free (dv->priv);
   g_free (dv);
}


static gboolean
cb_button_press (GtkWidget *widget, GimvEventButton *event, DirView *dv)
{
   gint num;

   g_return_val_if_fail (dv, FALSE);
   g_return_val_if_fail (event, FALSE);

   num = prefs_mouse_get_num_from_event (event, conf.dirview_mouse_button);
   if (event->type == GIMV_2BUTTON_PRESS) {
      dv->priv->button_2pressed_queue = num;
   } else if (num > 0) {
      return dirview_button_action (dv, event, num);
   }

   return FALSE;
}


static gboolean
cb_button_release (GtkWidget *widget, GimvEventButton *event, DirView *dv)
{
   gint num;

   g_return_val_if_fail (dv, FALSE);
   g_return_val_if_fail (event, FALSE);

   if (dv->priv->button_2pressed_queue) {
      num = dv->priv->button_2pressed_queue;
      if (num > 0)
         num = 0 - num;
      dv->priv->button_2pressed_queue = 0;
   } else {
      num = prefs_mouse_get_num_from_event (event, conf.dirview_mouse_button);
   }
   if (num < 0)
      return dirview_button_action (dv, event, num);

   return FALSE;
}


static gboolean
cb_scroll (GtkWidget *widget, GimvEventScroll *se, DirView *dv)
{
   GimvEventButton be;
   gboolean retval = FALSE;
   gint num;

   g_return_val_if_fail (GTK_IS_WIDGET(widget), FALSE);

   memset (&be, 0, sizeof (be));
   be.type       = GDK_BUTTON_PRESS;
   be.time       = se->time;
   be.x          = se->x;
   be.y          = se->y;
   be.state      = se->state;
   be.x_root     = se->x;   /* GTK4: no root coordinates */
   be.y_root     = se->y;
   be.event      = se->event;
   be.controller = se->controller;
   switch ((se)->direction) {
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
      g_warning ("invalid scroll direction!");
      be.button = 0;
      break;
   }

   num = prefs_mouse_get_num_from_event (&be, conf.dirview_mouse_button);
   if (num > 0)
      retval = dirview_button_action (dv, &be, num);

   be.type = GDK_BUTTON_RELEASE;
   if (num < 0)
      retval = dirview_button_action (dv, &be, num);

   return retval;
}


static gboolean
cb_key_press (GtkWidget *widget, GimvEventKey *event, DirView *dv)
{
   guint keyval, popup_key;
   GdkModifierType modval, popup_mod;
   GimvRow *row;
   gboolean retval = FALSE;
   gchar *label, *path;

   g_return_val_if_fail (dv, FALSE);
   g_return_val_if_fail (GTK_IS_LIST_VIEW (widget), FALSE);

   keyval = event->keyval;
   modval = event->state;

   if (akey.common_popup_menu || *akey.common_popup_menu)
      gtk_accelerator_parse (akey.common_popup_menu, &popup_key, &popup_mod);
   else
      return FALSE;

   row = dirview_get_selected_row (dv);
   if (!row) return FALSE;
   g_object_ref (row);

   gimv_row_get (row,
                 COLUMN_LABEL,    &label,
                 COLUMN_FULLPATH, &path,
                 COLUMN_TERMINATOR);

   if (keyval == popup_key && (!popup_mod || (modval & popup_mod))) {
      dirview_popup_menu (dv, NULL);
   } else {
      switch (keyval) {
      case GDK_KEY_KP_Enter:
      case GDK_KEY_Return:
      case GDK_KEY_ISO_Enter:
      {
         if (!strcmp (label, ".") || !strcmp (label, "..")) {
            dirview_change_root (dv, path);
         } else {
            open_dir_images (path, dv->tw, NULL,
                             LOAD_CACHE, conf.scan_dir_recursive);
         }
         retval = TRUE;
         break;
      }
      case GDK_KEY_space:
         if (dirview_row_is_expanded (dv, row))
            dirview_collapse_row (dv, row);
         else
            dirview_expand_row (dv, row);
         retval = TRUE;
         break;
      case GDK_KEY_Right:
         if (modval & GDK_CONTROL_MASK) {
            dirview_change_root (dv, path);
         } else {
            dirview_expand_row (dv, row);
         }
         retval = TRUE;
         break;
      case GDK_KEY_Left:
         if (modval & GDK_CONTROL_MASK) {
            dirview_change_root_to_parent (dv);
         } else {
            dirview_collapse_row (dv, row);
         }
         retval = TRUE;
         break;
      case GDK_KEY_Up:
         if (modval & GDK_CONTROL_MASK) {
            dirview_change_root_to_parent (dv);
            retval = TRUE;
         }
         break;
      case GDK_KEY_Down:
         if (modval & GDK_CONTROL_MASK) {
            dirview_change_root (dv, path);
            retval = TRUE;
         }
         break;
      }
   }

   g_free (label);
   g_free (path);
   g_object_unref (row);

   return retval;
}


/* the replacement of the "row-expanded" handler: read the subdirectories of
   the directory of row (the first time it is expanded) */
static void
dirview_load_subdirs (DirView *dv, GimvRow *row)
{
   const gchar *parent_dir;
   GList *subdir_list = NULL, *node;
   GetDirFlags flags;

   g_return_if_fail (dv);
   g_return_if_fail (row);

   if (gimv_row_peek_boolean (row, COLUMN_LOADED)) return;
   gimv_row_set (row, COLUMN_LOADED, TRUE, COLUMN_TERMINATOR);

   parent_dir = gimv_row_peek_string (row, COLUMN_FULLPATH);
   if (!parent_dir) return;

   flags = GETDIR_FOLLOW_SYMLINK | GETDIR_READ_DOT;
   get_dir (parent_dir, flags, NULL, &subdir_list);

   if (dv->show_dotfile || conf.dirview_show_current_dir) {
      insert_row (dv, row, ".", parent_dir);
   }

   if (dv->show_dotfile || conf.dirview_show_parent_dir) {
      gchar *end, *grandparent = remove_slash(parent_dir);

      end = strrchr (grandparent, '/');
      if (end) *(end + 1) = '\0';

      insert_row (dv, row, "..", grandparent);

      g_free (grandparent);
   }

   for (node = subdir_list; node; node = g_list_next (node)) {
      gchar *path = node->data;
      gboolean dot_file_check;

      dot_file_check = fileutil_basename(path)[0] != '.'
         || (dv->show_dotfile && fileutil_basename(path)[0] == '.')
         || (conf.dirview_show_current_dir && !strcmp (fileutil_basename(path), "."))
         || (conf.dirview_show_parent_dir  && !strcmp (fileutil_basename(path), ".."));

      if (isdir (path) && dot_file_check) {
         insert_row (dv, row, fileutil_basename (path), path);
      }
   }
   g_list_foreach (subdir_list, (GFunc) g_free, NULL);
   g_list_free (subdir_list);

   /* the expander and the folder icon depend on the children */
   gimv_row_changed (row);
}


static void
cb_tree_row_expanded (GtkTreeListRow *tree_row, GParamSpec *pspec,
                      GtkWidget *dirtree)
{
   DirView *dv = g_object_get_data (G_OBJECT (dirtree), DIRVIEW_KEY);
   GimvRow *row;

   if (!dv) return;
   if (!gtk_tree_list_row_get_expanded (tree_row)) return;

   row = gimv_row_from_item (tree_row);
   if (row)
      dirview_load_subdirs (dv, row);
}



/******************************************************************************
 *
 *   Callback functions for popup menu
 *
 ******************************************************************************/
static void
cb_open_thumbnail (DirView *dv, ScanSubDirType scan_subdir, GimvMenuItem *menuitem)
{
   gchar *path;

   path = dirview_get_selected_path (dv);
   if (!path) return;

   open_dir_images (path, dv->tw, NULL, LOAD_CACHE, scan_subdir);

   g_free (path);
}


static void
cb_go_to_here (DirView *dv, guint action, GimvMenuItem *menuitem)
{
   gchar *path;

   path = dirview_get_selected_path (dv);
   if (!path) return;

   dirview_change_root (dv, path);

   g_free (path);
}


static void
cb_refresh_dir_tree (DirView *dv, guint action, GimvMenuItem *menuitem)
{
   GimvRow *row = dirview_get_selected_row (dv);

   if (!row) return;

   refresh_dir_tree (dv, row);
}


static void
cb_file_property (DirView *dv, guint action, GimvMenuItem *menuitem)
{
   GimvImageInfo *info;
   gchar *path, *tmpstr;

   g_return_if_fail (dv);

   tmpstr = dirview_get_selected_path (dv);
   if (!tmpstr) return;

   path = remove_slash (tmpstr);
   g_free(tmpstr);

   info = gimv_image_info_get (path);
   if (!info) {
      g_free (path);
      return;
   }

   dlg_prop_from_image_info (info, 0);

   gimv_image_info_unref (info);
   g_free (path);
}


static void
cb_mkdir (DirView *dv, guint action, GimvMenuItem *menuitem)
{
   gchar *parent_path;
   gboolean success;

   g_return_if_fail (dv);

   parent_path = dirview_get_selected_path (dv);
   if (!parent_path) return;

   success = make_dir_dialog (
      parent_path,
      GTK_WINDOW(gimv_widget_get_toplevel(dv->container)));

   if (success)
      refresh_dir_tree (dv, get_iter_from_path (dv, parent_path));

   g_free (parent_path);
}


static void
cb_rename_dir (DirView *dv, guint action, GimvMenuItem *menuitem)
{
   gboolean success;
   gchar *path;

   g_return_if_fail (dv);

   path = dirview_get_selected_path (dv);
   if (!path) return;

   success = rename_dir_dialog
      (path, GTK_WINDOW(gimv_widget_get_toplevel(dv->container)));

   if (success) {
      gchar *tmp_path, *parent_dir;

      tmp_path = remove_slash (path);
      parent_dir = g_path_get_dirname (tmp_path);

      refresh_dir_tree (dv, get_iter_from_path (dv, parent_dir));

      g_free (tmp_path);
      g_free (parent_dir);
   }
}


static void
cb_delete_dir (DirView *dv, guint action, GimvMenuItem *menuitem)
{
   gchar *path, *parent_dir;

   g_return_if_fail (dv);

   path = dirview_get_selected_path (dv);
   if (!path) return;

   if (path [strlen (path) - 1] == '/')
      path [strlen (path) - 1] = '\0';

   delete_dir (path, GTK_WINDOW(gimv_widget_get_toplevel(dv->container)));

   parent_dir = g_path_get_dirname (path);
   g_free (path);

   /* refresh dir tree */
   refresh_dir_tree (dv, get_iter_from_path (dv, parent_dir));

   g_free (parent_dir);
}



/******************************************************************************
 *
 *   Callback functions for toolbar buttons.
 *
 ******************************************************************************/
static void
cb_home_button (GtkWidget *widget, DirView *dv)
{
   g_return_if_fail (dv);

   dirview_go_home (dv);
}


static void
cb_up_button (GtkWidget *widget, DirView *dv)
{
   dirview_change_root_to_parent (dv);
}


static void
cb_refresh_button (GtkWidget *widget, DirView *dv)
{
   GimvRow *root;

   g_return_if_fail (widget && dv);

   root = dirview_get_root_row (dv);
   if (!root) return;

   refresh_dir_tree (dv, root);
}


static void
cb_dotfile_button (GtkWidget *widget, DirView *dv)
{
   GimvRow *root;

   g_return_if_fail (widget && dv);

   dv->show_dotfile = !dv->show_dotfile;

   root = dirview_get_root_row (dv);
   if (!root) return;

   refresh_dir_tree (dv, root);
}


/******************************************************************************
 *
 *   Callback functions for DnD
 *
 ******************************************************************************/
static void
cb_drag_data_get (GtkWidget *dirtree,
                  GimvDragContext *context,
                  GimvSelectionData *seldata,
                  guint info,
                  guint time,
                  gpointer data)
{
   DirView *dv = data;
   gchar *path, *urilist;

   g_return_if_fail (dv);

   path = dirview_get_selected_path(dv);
   if (!path) return;
   if (!*path) {
      g_free (path);
      return;
   }

   if (path [strlen (path) - 1] == '/')
      path [strlen (path) - 1] = '\0';
   urilist = g_strconcat ("file://", path, "\r\n", NULL);

   gimv_selection_data_set(seldata, seldata->target,
                          8, urilist, strlen(urilist));

   g_free (path);
   g_free (urilist);
}


static gboolean
cb_drag_motion (GtkWidget *widget,
                GimvDragContext *drag_context,
                gint x, gint y, guint time,
                gpointer data)
{
   DirView *dv = data;

   g_return_val_if_fail(GTK_IS_LIST_VIEW(widget), FALSE);
   g_return_val_if_fail(dv, FALSE);

   if (!dirview_row_at (dv, x, y, NULL)) {
      gimv_list_view_set_drop_highlight (widget, -1, -1);
      return FALSE;
   }

   /* GTK4: was done by the model drag dest of GtkTreeView (dropping into
      the directory) */
   gimv_list_view_set_drop_highlight (widget, x, y);
   gimv_drag_status (drag_context, GDK_ACTION_MOVE, time);

   return FALSE;
}


static void
cb_drag_leave (GtkWidget *dirtree, GimvDragContext *context,
               guint time, gpointer data)
{
   gimv_list_view_set_drop_highlight (dirtree, -1, -1);
}


static void
cb_drag_data_received (GtkWidget *dirtree,
                       GimvDragContext *context,
                       gint x, gint y,
                       GimvSelectionData *seldata,
                       guint info,
                       guint32 time,
                       gpointer data)
{
   DirView *dv = data;
   GimvRow *row;
   gchar *path = NULL;

   g_return_if_fail (dv);

   gimv_list_view_set_drop_highlight (dirtree, -1, -1);

   row = dirview_row_at (dv, x, y, NULL);
   if (!row) return;

   gimv_row_get (row,
                 COLUMN_FULLPATH, &path,
                 COLUMN_TERMINATOR);

   if (path && *path) {
      if (iswritable (path)) {
         dnd_file_operation (path, context, seldata, time, dv->tw);
         refresh_dir_tree (dv, dirview_get_root_row (dv));
      } else {
         gchar error_message[BUF_SIZE], *dir_internal;

         dir_internal = charset_to_internal (path,
                                             conf.charset_filename,
                                             conf.charset_auto_detect_fn,
                                             conf.charset_filename_mode);
         g_snprintf (error_message, BUF_SIZE,
                     _("Permission denied: %s"),
                     dir_internal);
         gtkutil_message_dialog (
            _("Error!"), error_message,
            GTK_WINDOW(gimv_widget_get_toplevel(dv->container)));

         g_free (dir_internal);
      }
   }

   g_free (path);
}


static void
cb_drag_end (GtkWidget *dirtree, GimvDragContext *context, gpointer data)
{
   DirView *dv = data;

   g_return_if_fail (dirtree && dv);

   refresh_dir_tree (dv, dirview_get_root_row (dv));
}


static void
cb_toolbar_drag_begin (GtkWidget *widget,
                       GimvDragContext *context,
                       gpointer data)
{
   GimvIcon *icon;

   icon = gimv_icon_stock_get_icon ("paper");
   if (icon && icon->pixmap)
      gimv_drag_set_icon_texture (context, icon->pixmap, 0, 0);
}


static void
cb_toolbar_drag_data_get (GtkWidget *widget,
                          GimvDragContext *context,
                          GimvSelectionData *seldata,
                          guint info,
                          guint time,
                          gpointer data)
{
   switch (info) {
   case TARGET_GIMV_COMPONENT:
      gimv_selection_data_set(seldata, seldata->target,
                             8, "dummy", strlen("dummy"));
      break;
   }
}


typedef struct SwapCom_Tag
{
   GimvThumbWin *tw;
   gint src;
   gint dest;
} SwapCom;


static gint
idle_thumbwin_swap_component (gpointer data)
{
   SwapCom *swap = data;
   gimv_thumb_win_swap_component (swap->tw, swap->src, swap->dest);
   return FALSE;
}


static void
cb_com_swap_drag_data_received (GtkWidget *widget,
                                GimvDragContext *context,
                                gint x, gint y,
                                GimvSelectionData *seldata,
                                guint info,
                                guint time,
                                gpointer data)
{
   GimvThumbWin *tw = data;
   GtkWidget *src_widget;
   gpointer p;
   gint src, dest;

   switch (info) {
   case TARGET_GIMV_COMPONENT:
      src_widget = gimv_drag_get_source_widget (context);
      if (!src_widget
          || gtk_widget_get_root (src_widget) != gtk_widget_get_root (widget))
      {
         return;
      }

      p = g_object_get_data (G_OBJECT (src_widget), "gimv-component");
      src = GPOINTER_TO_INT (p);
      if (!src) return;

      p = g_object_get_data (G_OBJECT (widget), "gimv-component");
      dest = GPOINTER_TO_INT (p);
      if (!dest) return;

      {
         SwapCom *swap = g_new0 (SwapCom, 1);
         swap->tw = tw;
         swap->src = src;
         swap->dest = dest;
         /* to avoid gtk's bug, exec redraw after exit this callback function */
         g_idle_add_full (/* GTK_PRIORITY_REDRAW */G_PRIORITY_LOW,
                          idle_thumbwin_swap_component, swap,
                          (GDestroyNotify) g_free);
      }

      break;

   default:
      break;
   }
}



/******************************************************************************
 *
 *   Other private  functions
 *
 ******************************************************************************/
static void
get_icon_pixbufs (void)
{
   if (!folder) { 
      folder = gimv_icon_stock_get_pixbuf ("folder");
      if (folder) g_object_ref (folder);
   }

   if (!ofolder) {
      ofolder = gimv_icon_stock_get_pixbuf ("folder-open");
      if (ofolder) g_object_ref (ofolder);
   }

   if (!lfolder) { 
      lfolder = gimv_icon_stock_get_pixbuf ("folder-link");
      if (lfolder) g_object_ref (lfolder);
   }

   if (!lofolder) {
      lofolder = gimv_icon_stock_get_pixbuf ("folder-link-open");
      if (lofolder) g_object_ref (lofolder);
   }

   if (!go_folder) {
      go_folder = gimv_icon_stock_get_pixbuf ("folder-go");
      if (go_folder) g_object_ref (go_folder);
   }

   if (!up_folder) {
      up_folder = gimv_icon_stock_get_pixbuf ("folder-up");
      if (up_folder) g_object_ref (up_folder);
   }

   if (!lock_folder) {
      lock_folder = gimv_icon_stock_get_pixbuf ("folder-lock");
      if (lock_folder) g_object_ref (lock_folder);
   }
}


/* the folder icon (open when the row is expanded) and the name */
static GtkWidget *
dirtree_cell_setup (gpointer data)
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
   gtk_box_append (GTK_BOX (box), label);
   g_object_set_data (G_OBJECT (box), "gimv-cell-label", label);

   return box;
}


static void
dirtree_cell_bind (GtkWidget *cell, GimvRow *row, GtkListItem *item,
                   gpointer data)
{
   GtkWidget **dirtree = data;
   GtkTreeListRow *tree_row = gimv_list_item_get_tree_row (item);
   GtkWidget *expander = gtk_widget_get_parent (cell);
   GListStore *children = gimv_row_get_children (row, FALSE);
   gboolean has_child, loaded, expanded = FALSE;
   gint icon_col = COLUMN_ICON_CLOSE;

   loaded    = gimv_row_peek_boolean (row, COLUMN_LOADED);
   has_child = children
      && (!loaded || g_list_model_get_n_items (G_LIST_MODEL (children)) > 0);

   if (tree_row)
      expanded = gtk_tree_list_row_get_expanded (tree_row);
   if (expanded && has_child)
      icon_col = COLUMN_ICON_OPEN;

   gimv_icon_text_cell_set_icon (cell, gimv_row_peek_value (row, icon_col));
   gtk_label_set_text (GTK_LABEL (gimv_icon_text_cell_get_label (cell)),
                       gimv_row_peek_string (row, COLUMN_LABEL));

   /* no expander for directories without subdirectories (GtkTreeView
      removed it when the dummy child row was removed) */
   if (GTK_IS_TREE_EXPANDER (expander))
      gtk_tree_expander_set_hide_expander (GTK_TREE_EXPANDER (expander),
                                           !has_child);

   /* read the subdirectories when the user expands the row */
   if (tree_row && *dirtree
       && !g_object_get_data (G_OBJECT (tree_row), "gimv-dirview-expanded"))
   {
      g_signal_connect_object (tree_row, "notify::expanded",
                               G_CALLBACK (cb_tree_row_expanded), *dirtree, 0);
      g_object_set_data (G_OBJECT (tree_row), "gimv-dirview-expanded",
                         GINT_TO_POINTER (1));
   }
}


static void
dirview_create_treeview (DirView *dv, const gchar *root)
{
   GListStore *store;
   GtkTreeListModel *model;
   GtkWidget **dirtree_ptr;
   GimvRow *root_row;

   get_icon_pixbufs();

   store = gimv_row_store_new ();
   model = gimv_tree_list_model_new (G_LIST_MODEL (store));

   dirtree_ptr = g_new0 (GtkWidget *, 1);
   dv->dirtree = gimv_list_view_new (G_LIST_MODEL (model), GTK_SELECTION_SINGLE,
                                     gimv_cell_factory_new (dirtree_cell_setup,
                                                            dirtree_cell_bind,
                                                            dirtree_ptr, g_free,
                                                            TRUE));
   *dirtree_ptr = dv->dirtree;
   gimv_list_view_enable_search (dv->dirtree, COLUMN_LABEL, NULL, NULL);
   g_object_add_weak_pointer (G_OBJECT (dv->dirtree), (gpointer *) dirtree_ptr);
   g_object_set_data_full (G_OBJECT (dv->dirtree), ROOT_STORE_KEY,
                           store, g_object_unref);
   g_object_set_data (G_OBJECT (dv->dirtree), DIRVIEW_KEY, dv);
   /* the tree may go before the container (its "destroy" comes after the
      children are gone, when the layout is changed) */
   g_object_add_weak_pointer (G_OBJECT (dv->dirtree), (gpointer *) &dv->dirtree);

   root_row = insert_row (dv, NULL, root, root);

   gimv_event_connect (GTK_WIDGET (dv->dirtree), GIMV_EVENT_BUTTON_PRESS, G_CALLBACK (cb_button_press), dv);
   gimv_event_connect (GTK_WIDGET (dv->dirtree), GIMV_EVENT_BUTTON_RELEASE, G_CALLBACK (cb_button_release), dv);
   gimv_event_connect (GTK_WIDGET (dv->dirtree), GIMV_EVENT_SCROLL, G_CALLBACK(cb_scroll), dv);
   gimv_event_connect (GTK_WIDGET (dv->dirtree), GIMV_EVENT_KEY_PRESS, G_CALLBACK (cb_key_press), dv);

   /*
    *  for DnD
    *  the drop row is highlighted by cb_drag_motion ().
    */
   gimv_drag_source_set (GTK_WIDGET (dv->dirtree),
                         GDK_BUTTON1_MASK
                         | GDK_BUTTON2_MASK
                         | GDK_BUTTON3_MASK,
                         dnd_types_uri,
                         dnd_types_uri_num,
                         GDK_ACTION_ASK  | GDK_ACTION_COPY
                         | GDK_ACTION_MOVE | GDK_ACTION_LINK);
   gimv_drag_dest_set (GTK_WIDGET (dv->dirtree), 0,
                       dnd_types_uri,
                       dnd_types_uri_num,
                       GDK_ACTION_ASK  | GDK_ACTION_COPY
                       | GDK_ACTION_MOVE | GDK_ACTION_LINK);
   gimv_dnd_connect (GTK_WIDGET (dv->dirtree), GIMV_DND_DRAG_DATA_GET, G_CALLBACK (cb_drag_data_get), dv);
   gimv_dnd_connect (GTK_WIDGET (dv->dirtree), GIMV_DND_DRAG_MOTION, G_CALLBACK (cb_drag_motion), dv);
   gimv_dnd_connect (GTK_WIDGET (dv->dirtree), GIMV_DND_DRAG_LEAVE, G_CALLBACK (cb_drag_leave), dv);
   gimv_dnd_connect (GTK_WIDGET (dv->dirtree), GIMV_DND_DRAG_DATA_RECEIVED, G_CALLBACK (cb_drag_data_received), dv);
   gimv_dnd_connect (GTK_WIDGET (dv->dirtree), GIMV_DND_DRAG_END, G_CALLBACK (cb_drag_end), dv);

   gimv_container_add (GTK_WIDGET (dv->scroll_win), dv->dirtree);
   gtk_widget_set_visible (dv->dirtree, TRUE);

   if (root_row)
      dirview_expand_row (dv, root_row);

   adjust_tree_idle (dv, NULL);
}


/* GTK4: replacement of gtk_toolbar_append_item () */
static GtkWidget *
toolbar_append_item (GtkWidget *toolbar,
                     const gchar *text,
                     const gchar *tooltip_text,
                     GtkWidget *icon,
                     GCallback callback,
                     gpointer data)
{
   GtkWidget *button, *vbox, *label;

   button = gtk_button_new ();
   gtk_widget_add_css_class (button, "flat");
   gtk_widget_set_focusable (button, FALSE);
   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
   if (icon)
      gtk_box_append (GTK_BOX (vbox), icon);
   label = gtk_label_new (text);
   gtk_box_append (GTK_BOX (vbox), label);
   gtk_button_set_child (GTK_BUTTON (button), vbox);
   if (tooltip_text)
      gtk_widget_set_tooltip_text (button, tooltip_text);
   g_signal_connect (G_OBJECT (button), "clicked", callback, data);
   gtk_box_append (GTK_BOX (toolbar), button);

   return button;
}


static GtkWidget *
dirview_create_toolbar (DirView *dv)
{
   GtkWidget *toolbar;
   GtkWidget *iconw;

   g_return_val_if_fail (dv, NULL);

   toolbar = gtkutil_create_toolbar ();

   /* file open button */
   iconw = gimv_icon_stock_get_widget ("small_home");
   toolbar_append_item (toolbar,
                        _("Home"),
                        _("Home"),
                        iconw,
                        G_CALLBACK (cb_home_button),
                        dv);

   /* preference button */
   iconw = gimv_icon_stock_get_widget ("small_up");
   toolbar_append_item (toolbar,
                        _("Up"),
                        _("Up"),
                        iconw,
                        G_CALLBACK (cb_up_button),
                        dv);

   /* refresh button */
   iconw = gimv_icon_stock_get_widget ("small_refresh");
   toolbar_append_item (toolbar,
                        _("Refresh"),
                        _("Refresh"),
                        iconw,
                        G_CALLBACK (cb_refresh_button),
                        dv);

   /* preference button */
   iconw = gimv_icon_stock_get_widget ("dotfile");
   toolbar_append_item (toolbar,
                        _("Dotfiles"),
                        _("Show/Hide dotfiles"),
                        iconw,
                        G_CALLBACK (cb_dotfile_button),
                        dv);

   gimv_widget_show_all (toolbar);
   gtkutil_toolbar_set_style (toolbar, 0); /* GTK_TOOLBAR_ICONS */

   return toolbar;
}


/*
 *  fullpath shouldn't be terminated with "/" character.
 *  The row is appended to the children of parent (to the root if NULL).
 */
static GimvRow *
insert_row (DirView *dv, GimvRow *parent,
            const gchar *label, const gchar *fullpath)
{
   GdkPixbuf *icon, *oicon;
   gchar *path, *tmpstr;
   gchar *label_internal;
   GimvRow *row;

   g_return_val_if_fail (dv, NULL);
   g_return_val_if_fail (label && *label, NULL);
   g_return_val_if_fail (fullpath && *fullpath, NULL);

   path = remove_slash (fullpath);

   if (!strcmp (label, ".")) {
      icon  = go_folder;
      oicon = go_folder;
   } else if (!strcmp (label, "..")) {
      icon  = up_folder;
      oicon = up_folder;
   } else if (access(path, R_OK)) {
      icon  = lock_folder;
      oicon = lock_folder;
   } else if (islink(path)) {
      icon  = lfolder;
      oicon = lofolder;
   } else if (isdir(path)) {
      icon  = folder;
      oicon = ofolder;
   } else {
      g_free (path);
      return NULL;
   }

   tmpstr = add_slash (path);
   g_free(path);
   path = tmpstr;

   /* convert charset */
   label_internal = charset_to_internal (label, conf.charset_filename,
                                         conf.charset_auto_detect_fn,
                                         conf.charset_filename_mode);

   row = gimv_row_new (N_COLUMN,
                       G_TYPE_STRING,
                       G_TYPE_STRING,
                       GDK_TYPE_PIXBUF,
                       GDK_TYPE_PIXBUF,
                       G_TYPE_BOOLEAN);
   gimv_row_set (row,
                 COLUMN_LABEL,      label_internal,
                 COLUMN_FULLPATH,   path,
                 COLUMN_ICON_OPEN,  oicon,
                 COLUMN_ICON_CLOSE, icon,
                 COLUMN_LOADED,     TRUE,
                 COLUMN_TERMINATOR);

   /* expandable: the subdirectories are read when it is expanded */
   if (strcmp (label, ".") && strcmp (label, "..") && !access(path, R_OK)) {
      gimv_row_get_children (row, TRUE);
      gimv_row_set (row, COLUMN_LOADED, FALSE, COLUMN_TERMINATOR);
   }

   if (parent)
      gimv_row_append_child (parent, row);
   else
      g_list_store_append (dirview_root_store (dv), row);
   g_object_unref (row);   /* the store keeps it */

   g_free (label_internal);
   g_free (path);

   return row;
}


static gboolean
is_in_view (DirView *dv, guint position)
{
   graphene_rect_t area;

   if (!gtk_widget_get_realized (dv->dirtree))
      return FALSE;

   if (!gimv_list_view_get_row_area (dv->dirtree, position, &area))
      return FALSE;

   if (area.origin.y >= 0 && area.origin.y < gtk_widget_get_height (dv->dirtree))
      return TRUE;
   else
      return FALSE;
}


/* select row (the root if NULL) and scroll to it */
static void
adjust_tree (DirView *dv, GimvRow *row)
{
   GtkTreeListRow *tree_row;
   guint position;

   if (!row)
      row = dirview_get_root_row (dv);
   if (!row) return;

   tree_row = gimv_tree_list_model_get_row_for (dirview_tree_model (dv), row,
                                                &position);
   if (!tree_row) return;
   g_object_unref (tree_row);

   gimv_list_view_select (dv->dirtree, position);
   if (!is_in_view (dv, position))
      gimv_list_view_scroll_to (dv->dirtree, position, FALSE, FALSE);
}


/* the row of the directory str (its parents are expanded) */
static GimvRow *
get_iter_from_path (DirView *dv, const gchar *str)
{
   GimvRow *root, *retval = NULL;
   GListModel *children;
   gchar *destpath;
   gint len_src, len_dest;
   guint i;

   g_return_val_if_fail (dv, NULL);

   if (!str) return NULL;
   destpath = add_slash (str);
   if (!destpath) return NULL;

   len_src  = strlen (dv->root_dir);
   len_dest = strlen (destpath);

   if (len_dest < len_src || strncmp (dv->root_dir, destpath, len_src)) {
      goto ERROR;
   }

   root = dirview_get_root_row (dv);
   if (!root) goto ERROR;

   if (!strcmp (destpath, dv->root_dir)) {
      g_free (destpath);
      return root;
   }

   dirview_expand_row (dv, root);
   children = G_LIST_MODEL (gimv_row_get_children (root, FALSE));

   for (i = 0; children && i < g_list_model_get_n_items (children); ) {
      GimvRow *row = gimv_row_store_peek (children, i);
      const gchar *path, *label;
      gint len;

      label = gimv_row_peek_string (row, COLUMN_LABEL);
      path  = gimv_row_peek_string (row, COLUMN_FULLPATH);

      len = strlen (path); /* path will be terminated by "/" */

      /* It is just the directory! */
      if (!strcmp (label, ".") || !strcmp (label, "..")) {
         i++;
      } else if (!strcmp (destpath, path)) {
         retval = row;
         break;

      /* It is the parent directory of the destpath */
      } else if (!strncmp (destpath, path, len)) {
         dirview_expand_row (dv, row);
         children = G_LIST_MODEL (gimv_row_get_children (row, FALSE));
         i = 0;

      /* No match. Search the next... */
      } else {
         i++;
      }
   }

   g_free (destpath);
   return retval;

 ERROR:
   g_free (destpath);
   return NULL;
}


struct AdjustTreeIdle {
   DirView     *dv;
   GimvRow     *row;
};


static void
adjust_tree_idle_free (struct AdjustTreeIdle *idle)
{
   if (idle->row)
      g_object_unref (idle->row);
   g_free (idle);
}


static gint
idle_adjust_tree (gpointer data)
{
   struct AdjustTreeIdle *idle = data;

   idle->dv->priv->adjust_tree_id = 0;
   adjust_tree (idle->dv, idle->row);

   return FALSE;
}


static void
adjust_tree_idle (DirView *dv, GimvRow *row)
{
   struct AdjustTreeIdle *idle;

   g_return_if_fail (dv && GTK_IS_LIST_VIEW (dv->dirtree));

   idle = g_new0(struct AdjustTreeIdle, 1);

   idle->dv = dv;
   idle->row = row ? g_object_ref (row) : NULL;

   if (dv->priv->adjust_tree_id)
      g_source_remove (dv->priv->adjust_tree_id);
   dv->priv->adjust_tree_id =
      g_idle_add_full (G_PRIORITY_DEFAULT,
                       idle_adjust_tree, idle,
                       (GDestroyNotify) adjust_tree_idle_free);
}


/* the replacement of gtk_tree_view_map_expanded_rows () */
static void
get_expanded_dirs (DirView *dv, GListModel *rows, GList **list)
{
   guint i, n = g_list_model_get_n_items (rows);

   for (i = 0; i < n; i++) {
      GimvRow *row = gimv_row_store_peek (rows, i);
      GListStore *children = gimv_row_get_children (row, FALSE);

      if (!children || !gimv_row_peek_boolean (row, COLUMN_LOADED)) continue;
      if (!dirview_row_is_expanded (dv, row)) continue;

      *list = g_list_append (*list,
                             g_strdup (gimv_row_peek_string (row, COLUMN_FULLPATH)));
      get_expanded_dirs (dv, G_LIST_MODEL (children), list);
   }
}


static void
refresh_dir_tree (DirView *dv, GimvRow *parent)
{
   gchar *root_dir, *selected_path;
   GList *expand_list = NULL, *node;
   GimvRow *row;

   g_return_if_fail (dv);

   selected_path = dirview_get_selected_path (dv);
   if (!selected_path) selected_path = g_strdup (dv->root_dir);

   /* get expanded directory list */
   get_expanded_dirs (dv, G_LIST_MODEL (dirview_root_store (dv)), &expand_list);

   /* replace root node */
   root_dir = g_strdup (dv->root_dir);
#if 1 /* almost same with dirview_change_root () */
   g_free (dv->root_dir);
   dv->root_dir = add_slash (root_dir);

   g_list_store_remove_all (dirview_root_store (dv));
   insert_row (dv, NULL, dv->root_dir, dv->root_dir);
#endif
   g_free (root_dir);

   /* restore expanded directory */
   for (node = expand_list; node; node = g_list_next (node)) {
      dirview_expand_dir (dv, node->data, FALSE);
   }

   g_list_foreach (expand_list, (GFunc) g_free, NULL);
   g_list_free (expand_list);

   /* adjust tree pos */
   row = get_iter_from_path (dv, selected_path);
   if (row)
      adjust_tree_idle (dv, row);

   g_free (selected_path);
}


static void
dirview_popup_menu (DirView *dv, GimvEventButton *event)
{
   GimvRow *row;
   gchar *path, *parent, *tmpstr, *label;

   GtkWidget *ifactory;
   GtkWidget *dirview_popup;
   GimvMenuItem *menuitem;
   gint n_menu_items;
   gdouble x, y;

   g_return_if_fail (dv);

   if (event) {
      row = dirview_row_at (dv, event->x, event->y, NULL);
      if (!row) return;

      x = event->x;
      y = event->y;
   } else {
      /* popup by key: use the selected row */
      graphene_rect_t area;
      guint position;

      position = gimv_list_view_get_selected (dv->dirtree);
      row = dirview_get_selected_row (dv);
      if (!row) return;

      /* was menu_calc_popup_position () */
      if (gimv_list_view_get_row_area (dv->dirtree, position, &area)) {
         x = MAX (area.origin.x, 0);
         y = MAX (area.origin.y + area.size.height, 0);
      } else {
         x = y = 0;
      }
   }

   gimv_row_get (row,
                 COLUMN_LABEL,    &label,
                 COLUMN_FULLPATH, &path,
                 COLUMN_TERMINATOR);

   tmpstr = remove_slash (path);
   parent = g_path_get_dirname (tmpstr);
   g_free (tmpstr);

   if (dv->popup_menu) {
      gimv_menu_destroy (dv->popup_menu);
      dv->popup_menu = NULL;
   }

   n_menu_items = sizeof(dirview_popup_items)
      / sizeof(dirview_popup_items[0]) -  1;
   dirview_popup = menu_create_items(NULL, dirview_popup_items,
                                     n_menu_items, "<DirViewPop>", dv);


   /* set sensitive */
   ifactory = (dirview_popup);

   if (!strcmp (label, ".") || !strcmp (label, ".."))
   {
      menuitem = gimv_menu_get_item (ifactory, "/Refresh Tree");
      gimv_menu_item_set_sensitive (menuitem, FALSE);
      menuitem = gimv_menu_get_item (ifactory, "/Make Directory...");
      gimv_menu_item_set_sensitive (menuitem, FALSE);
   }

   if (!iswritable (path)) {
      menuitem = gimv_menu_get_item (ifactory, "/Make Directory...");
      gimv_menu_item_set_sensitive (menuitem, FALSE);
   }

   if (!parent || !strcmp (parent, ".") || !iswritable (parent)
       || !strcmp (label, ".") || !strcmp (label, ".."))
   {
      menuitem = gimv_menu_get_item (ifactory, "/Rename Directory...");
      gimv_menu_item_set_sensitive (menuitem, FALSE);
      menuitem = gimv_menu_get_item (ifactory, "/Delete Directory...");
      gimv_menu_item_set_sensitive (menuitem, FALSE);
   }

   g_free (label);
   g_free (path);
   g_free (parent);

   /* popup menu */
   gimv_menu_popup (dirview_popup, dv->dirtree, x, y);

   dv->popup_menu = dirview_popup;
}


typedef struct ButtonActionData_Tag
{
   DirView *dv;
   gchar   *path, *label;
   gint     action_num;
} ButtonActionData;


static void
free_button_action_data (ButtonActionData *data)
{
   g_free (data->path);
   g_free (data->label);
   g_free (data);
}


static gboolean
idle_dirview_button_action (gpointer data)
{
   ButtonActionData *bdata = data;
   DirView *dv = bdata->dv;
   gchar *path = bdata->path, *label = bdata->label;

   dv->priv->button_action_id = 0;

   switch (abs (bdata->action_num)) {
   case MouseActLoadThumb:
      if (!strcmp (label, ".") || !strcmp (label, "..")) {
         dirview_change_root (dv, path);
      } else {
         open_dir_images (path, dv->tw, NULL, LOAD_CACHE, SCAN_SUB_DIR_NONE);
      }
      break;
   case MouseActLoadThumbRecursive:
      if (!strcmp (label, ".") || !strcmp (label, "..")) {
         dirview_change_root (dv, path);
      } else {
         open_dir_images (path, dv->tw, NULL, LOAD_CACHE, SCAN_SUB_DIR);
      }
      break;
   case MouseActLoadThumbRecursiveInOneTab:
      if (!strcmp (label, ".") || !strcmp (label, "..")) {
         dirview_change_root (dv, path);
      } else {
         open_dir_images (path, dv->tw, NULL, LOAD_CACHE, SCAN_SUB_DIR_ONE_TAB);
      }
      break;
   case MouseActChangeTop:
      dirview_change_root (dv, path);
      break;
   default:
      break;
   }

   free_button_action_data (bdata);

   return FALSE;
}


static gboolean
dirview_button_action (DirView *dv, GimvEventButton *event, gint num)
{
   gchar *path = NULL, *label = NULL;
   gboolean retval = FALSE;
   GimvRow *row;
   guint position;

   row = dirview_row_at (dv, event->x, event->y, &position);
   if (!row) return FALSE;

   gimv_list_view_select (dv->dirtree, position);

   gimv_row_get (row,
                 COLUMN_LABEL,    &label,
                 COLUMN_FULLPATH, &path,
                 COLUMN_TERMINATOR);

   switch (abs(num)) {
   case MouseActLoadThumb:
   case MouseActLoadThumbRecursive:
   case MouseActLoadThumbRecursiveInOneTab:
   case MouseActChangeTop:
   {
      ButtonActionData *data = g_new0 (ButtonActionData, 1);

      data->dv         = dv;
      data->path       = path;
      data->label      = label;
      data->action_num = num;

      dv->priv->button_action_id
         = g_idle_add (idle_dirview_button_action, data);

      return FALSE;
      break;
   }
   case MouseActPopupMenu:
      dirview_popup_menu (dv, event);
      if (num > 0) retval = TRUE;
      break;
   default:
      break;
   }

   g_free (path);
   g_free (label);

   return retval;
}



/******************************************************************************
 *
 *   Public  functions
 *
 ******************************************************************************/
void
dirview_change_root (DirView *dv, const gchar *root_dir)
{
   GimvRow *root_row;
   gchar *dest_dir;

   g_return_if_fail (dv);
   g_return_if_fail (root_dir && *root_dir);

   dest_dir = add_slash (root_dir);

   if (!isdir (dest_dir)) {
      g_free (dest_dir);
      return;
   }

   g_free (dv->root_dir);
   dv->root_dir = dest_dir;

   g_list_store_remove_all (dirview_root_store (dv));
   root_row = insert_row (dv, NULL, dv->root_dir, dv->root_dir);
   if (root_row)
      dirview_expand_row (dv, root_row);

   adjust_tree_idle (dv, NULL);
}


void
dirview_change_root_to_parent (DirView *dv)
{
   gchar *end;
   gchar *root;

   g_return_if_fail (dv);

   root = g_strdup (dv->root_dir);
   end = strrchr (root, '/');
   if (end && end != root) *end = '\0';
   end = strrchr (root, '/');
   if (end) *(end + 1) = '\0';

   dirview_change_dir (dv, root);
   g_free (root);
}


void
dirview_change_dir (DirView *dv, const gchar *str)
{
   GimvRow *row;
   gchar *destpath;

   g_return_if_fail (dv);

   destpath = add_slash (str);
   if (!destpath) return;

   if (!isdir (destpath)) {
      g_free (destpath);
      return;
   }

   /* FIXME */
   /* if selected path in directory view is same as str, adjust to it */
   /* END FIXME */

   row = get_iter_from_path (dv, destpath);

   if (row) {
      adjust_tree_idle (dv, row);
   } else {
      dirview_change_root (dv, str);
   }

   g_free (destpath);
}


void
dirview_go_home (DirView *dv)
{
   const gchar *home = g_getenv ("HOME");

   dirview_change_root (dv, home);
   dirview_change_dir (dv, home);
}


void
dirview_refresh_list (DirView *dv)
{
   GimvRow *root = dirview_get_root_row (dv);

   if (!root) return;

   refresh_dir_tree (dv, root);
}


gchar *
dirview_get_selected_path (DirView *dv)
{
   GimvRow *row;

   g_return_val_if_fail (dv, NULL);
   g_return_val_if_fail (dv->dirtree, NULL);

   row = dirview_get_selected_row (dv);
   if (!row) return NULL;

   return g_strdup (gimv_row_peek_string (row, COLUMN_FULLPATH));
}


/* gtk_tree_view_expand_row (open_all) */
static void
dirview_expand_row_all (DirView *dv, GimvRow *row, gboolean open_all)
{
   GListModel *children;
   guint i;

   dirview_expand_row (dv, row);
   if (!open_all) return;

   children = G_LIST_MODEL (gimv_row_get_children (row, FALSE));
   for (i = 0; children && i < g_list_model_get_n_items (children); i++) {
      GimvRow *child = gimv_row_store_peek (children, i);
      const gchar *label = gimv_row_peek_string (child, COLUMN_LABEL);

      if (!strcmp (label, ".") || !strcmp (label, "..")) continue;
      if (!gimv_row_get_children (child, FALSE)) continue;
      dirview_expand_row_all (dv, child, TRUE);
   }
}


void
dirview_expand_dir (DirView *dv, const gchar *dir, gboolean open_all)
{
   GimvRow *row;

   row = get_iter_from_path (dv, dir);
   if (!row) return;

   dirview_expand_row_all (dv, row, open_all);
}


gboolean
dirview_set_opened_mark (DirView *dv, const gchar *path)
{
   /* note implemented yet */
   return TRUE;
}


gboolean
dirview_unset_opened_mark (DirView *dv, const gchar *path)
{
   /* note implemented yet */
   return TRUE;
}


void
dirview_show_toolbar (DirView *dv)
{
   g_return_if_fail (dv);

   dv->show_toolbar = TRUE;
   gtk_widget_set_visible (dv->toolbar_eventbox, TRUE);
}


void
dirview_hide_toolbar (DirView *dv)
{
   g_return_if_fail (dv);

   dv->show_toolbar = FALSE;
   gtk_widget_set_visible (dv->toolbar_eventbox, FALSE);
}


DirView *
dirview_create (const gchar *root_dir,
                GtkWidget *parent_win,
                GimvThumbWin *tw)
{
   DirView *dv;
   GtkWidget *eventbox;
   const gchar *home = g_getenv ("HOME");

   dv       = g_new0 (DirView, 1);
   dv->priv = g_new0 (DirViewPrivate,1);

   if (root_dir)
      dv->root_dir = add_slash (root_dir);
   else
      dv->root_dir = add_slash (home);

   dv->dirtree           = NULL;
   dv->popup_menu        = NULL;
   dv->tw                = tw;
   dv->show_toolbar      = conf.dirview_show_toolbar;
   dv->show_dotfile      = conf.dirview_show_dotfile;
   dv->priv->hilit_dir         = -1;
   dv->priv->scroll_timer_id   = 0;
   dv->priv->button_action_id  = 0;
   dv->priv->swap_com_id       = 0;
   dv->priv->adjust_tree_id    = 0;

   /* main vbox */
   dv->container = gimv_vbox_new (FALSE, 0);
   gtk_widget_set_name (dv->container, "DirView");
   gtk_widget_set_visible (dv->container, TRUE);

   dnd_dest_set (dv->container, dnd_types_component, dnd_types_component_num);
   g_object_set_data (G_OBJECT (dv->container),
                      "gimv-component",
                      GINT_TO_POINTER (GIMV_COM_DIR_VIEW));
   g_signal_connect (G_OBJECT (dv->container), "destroy",
                     G_CALLBACK (cb_dirview_destroyed), dv);

   gimv_dnd_connect (GTK_WIDGET (dv->container), GIMV_DND_DRAG_DATA_RECEIVED, G_CALLBACK (cb_com_swap_drag_data_received), dv->tw);

   /* toolbar */
   eventbox = dv->toolbar_eventbox = gimv_event_box_new ();
   gimv_container_set_border_width (GTK_WIDGET (eventbox), 1);
   gimv_box_pack_start (GTK_BOX (dv->container), eventbox, FALSE, FALSE, 0);
   gtk_widget_set_visible (eventbox, TRUE);

   dv->toolbar = dirview_create_toolbar (dv);
   gimv_container_add (GTK_WIDGET (eventbox), dv->toolbar);

   if (!dv->show_toolbar)
      gtk_widget_set_visible (dv->toolbar_eventbox, FALSE);

   dnd_src_set  (eventbox, dnd_types_component, dnd_types_component_num);
   g_object_set_data (G_OBJECT (eventbox),
                      "gimv-component",
                      GINT_TO_POINTER (GIMV_COM_DIR_VIEW));
   gimv_dnd_connect (GTK_WIDGET (eventbox), GIMV_DND_DRAG_BEGIN, G_CALLBACK (cb_toolbar_drag_begin), tw);
   gimv_dnd_connect (GTK_WIDGET (eventbox), GIMV_DND_DRAG_DATA_GET, G_CALLBACK (cb_toolbar_drag_data_get), tw);

   /* scrolled window */
   dv->scroll_win = gimv_scrolled_window_new (NULL, NULL);
   gimv_container_set_border_width (GTK_WIDGET (dv->scroll_win), 1);
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (dv->scroll_win),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   /* was GTK_SHADOW_IN */
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (dv->scroll_win), TRUE);
   gimv_box_pack_start(GTK_BOX(dv->container), dv->scroll_win, TRUE, TRUE, 0);
   gtk_widget_set_visible (dv->scroll_win, TRUE);

   /* ctree */
   dirview_create_treeview (dv, dv->root_dir);

   return dv;
}

