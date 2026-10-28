/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: the faces window.
 *
 * Shows the faces of a row of the "People" tab (a person, the faces that
 * look like one, a group of unnamed faces that look alike, ...) as the
 * small images gimv-faces made, and lets them be named, taken out, or
 * ignored.  One window, shown again for another row.
 *
 * The list of faces is read in a worker thread; the small images (a few
 * kB of JPEG each) are read from the database as they come into view.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimageview.h"

#include <string.h>
#include <gio/gio.h>

#include "gimv_image_info.h"
#include "gimv_image_win.h"
#include "gimv_index.h"
#include "gimv_index_db.h"
#include "gimv_index_faces.h"
#include "gimv_index_faces_win.h"
#include "gimv_thumb_view.h"

#define FACE_SIZE 128


/******************************************************************************
 *  a face (the items of the list)
 ******************************************************************************/

#define GIMV_TYPE_FACE_ITEM (gimv_face_item_get_type ())
G_DECLARE_FINAL_TYPE (GimvFaceItem, gimv_face_item, GIMV, FACE_ITEM, GObject)

struct _GimvFaceItem {
   GObject parent;
   gint64  id;
   gchar  *path;
   gchar  *date;
};

G_DEFINE_TYPE (GimvFaceItem, gimv_face_item, G_TYPE_OBJECT)

static void
gimv_face_item_finalize (GObject *object)
{
   GimvFaceItem *item = GIMV_FACE_ITEM (object);
   g_free (item->path);
   g_free (item->date);
   G_OBJECT_CLASS (gimv_face_item_parent_class)->finalize (object);
}

static void
gimv_face_item_class_init (GimvFaceItemClass *klass)
{
   G_OBJECT_CLASS (klass)->finalize = gimv_face_item_finalize;
}

static void
gimv_face_item_init (GimvFaceItem *item)
{
}


/******************************************************************************
 *  the window
 ******************************************************************************/

static struct {
   GtkWidget         *window;
   GtkWidget         *heading;   /* the box of these: */
   GtkWidget         *name_label;   /* the title, selectable (to copy) */
   GtkWidget         *name_entry;   /* the name being changed */
   GtkWidget         *edit_button;  /* the pencil: change the name */
   GtkWidget         *count_label;
   GtkWidget         *grid;
   GtkWidget         *buttons[8];
   GListStore        *store;
   GtkMultiSelection *selection;
   GCancellable      *cancel;
   GimvThumbWin      *tw;        /* weak */

   gchar             *key;
   gchar             *title;
   gboolean           changed;   /* names given: the groups made again on close */
   gboolean           busy;      /* a change being written */
   gchar             *error;     /* of the last change, shown in the heading */
} fw;

enum {
   B_NAME, B_CONFIRM, B_REJECT, B_CLEAR, B_IGNORE, B_UNIGNORE, B_OPEN, N_BUTTONS
};


static void update_buttons (void);


static void
update_heading (void)
{
   guint n = fw.store ? g_list_model_get_n_items (G_LIST_MODEL (fw.store)) : 0;
   gchar *count = g_strdup_printf (ngettext ("%'u face", "%'u faces", n), n);
   gchar *title = g_strdup_printf ("%s: %s", fw.title ? fw.title : "", count);
   gchar *line;

   gtk_window_set_title (GTK_WINDOW (fw.window), title);
   if (!gtk_widget_get_visible (fw.name_entry))
      gtk_label_set_text (GTK_LABEL (fw.name_label), fw.title ? fw.title : "");
   /* a named person: the name can be changed */
   gtk_widget_set_visible (fw.edit_button, fw.key && fw.key[0] == 'p'
                                           && !gtk_widget_get_visible (fw.name_entry));
   gtk_widget_set_sensitive (fw.edit_button, !fw.busy);
   if (fw.busy)
      line = g_strdup_printf ("%s — %s", count, _("writing..."));
   else if (fw.error)
      line = g_strdup_printf (_("%s — could not be written: %s"), count, fw.error);
   else
      line = g_strdup (count);
   gtk_label_set_text (GTK_LABEL (fw.count_label), line);
   g_free (line);
   g_free (title);
   g_free (count);
}


/* the ids of the faces selected */
static GArray *
selected_ids (void)
{
   GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
   /* a copy: the selection's own may change while it is gone through */
   GtkBitset *live = gtk_selection_model_get_selection (GTK_SELECTION_MODEL (fw.selection));
   GtkBitset *set = gtk_bitset_copy (live);
   GtkBitsetIter iter;
   guint pos;

   gtk_bitset_unref (live);
   if (gtk_bitset_iter_init_first (&iter, set, &pos)) {
      do {
         GimvFaceItem *item = g_list_model_get_item (G_LIST_MODEL (fw.store), pos);
         if (item) {
            g_array_append_val (ids, item->id);
            g_object_unref (item);
         }
      } while (gtk_bitset_iter_next (&iter, &pos));
   }
   gtk_bitset_unref (set);
   return ids;
}


/* these faces are no longer of this row: out of the list (the rest
 * stays where it was, not read again).  By id: the selection may have
 * changed while they were written (and a GtkMultiSelection's own bitset
 * changes under an iterator as items go). */
static void
remove_ids (GArray *ids)
{
   GHashTable *gone = g_hash_table_new (g_int64_hash, g_int64_equal);
   guint i, n;

   for (i = 0; i < ids->len; i++)
      g_hash_table_add (gone, &g_array_index (ids, gint64, i));
   n = g_list_model_get_n_items (G_LIST_MODEL (fw.store));
   /* from the end, the positions before stay right */
   for (i = n; i > 0; i--) {
      GimvFaceItem *item = g_list_model_get_item (G_LIST_MODEL (fw.store), i - 1);
      gboolean drop = item && g_hash_table_contains (gone, &item->id);
      if (item) g_object_unref (item);
      if (drop) g_list_store_remove (fw.store, i - 1);
   }
   g_hash_table_unref (gone);
   update_heading ();
   update_buttons ();
}


static gint64
key_person (void)
{
   if (fw.key && (fw.key[0] == 'p' || fw.key[0] == 'g') && fw.key[1] == ':')
      return g_ascii_strtoll (fw.key + 2, NULL, 10);
   return 0;
}


static void
update_buttons (void)
{
   GtkBitset *set;
   guint64 n;
   gchar kind = fw.key ? fw.key[0] : '\0';
   guint i;

   if (!fw.window) return;
   set = gtk_selection_model_get_selection (GTK_SELECTION_MODEL (fw.selection));
   n = gtk_bitset_get_size (set);
   gtk_bitset_unref (set);

   gtk_widget_set_visible (fw.buttons[B_CONFIRM],  kind == 'g');
   gtk_widget_set_visible (fw.buttons[B_REJECT],   kind == 'g' || kind == 'p');
   gtk_widget_set_visible (fw.buttons[B_CLEAR],    kind == 'p');
   gtk_widget_set_visible (fw.buttons[B_IGNORE],   kind != 'i');
   gtk_widget_set_visible (fw.buttons[B_UNIGNORE], kind == 'i');
   /* one change at a time */
   for (i = 0; i < N_BUTTONS; i++)
      gtk_widget_set_sensitive (fw.buttons[i], i == B_OPEN ? n == 1 : n > 0 && !fw.busy);
}


/******************************************************************************
 *  reading the list
 ******************************************************************************/

typedef struct {
   gint64 id;
   gchar *path;
   gchar *date;
} FaceRow;

static void
face_row_clear (gpointer p)
{
   FaceRow *r = p;
   g_free (r->path);
   g_free (r->date);
}


static void
thread_list_faces (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   const gchar *key = data;
   gchar *where = gimv_index_faces_key_where (key), *sql, *dbfile;
   GArray *rows = g_array_new (FALSE, TRUE, sizeof (FaceRow));
   sqlite3 *db = NULL;
   sqlite3_stmt *st;

   g_array_set_clear_func (rows, face_row_clear);
   dbfile = gimv_index_db_default_path ();
   gimv_index_db_open (dbfile, FALSE, &db, NULL);
   g_free (dbfile);
   if (!db || !where) {
      if (db) sqlite3_close (db);
      g_free (where);
      g_task_return_pointer (task, rows, (GDestroyNotify) g_array_unref);
      return;
   }

   /* by date: the faces of one day (one event) come together */
   sql = g_strdup_printf ("SELECT fa.id, d.path, f.name, f.date FROM faces fa "
                          "JOIN files f ON f.id = fa.file_id JOIN dirs d ON d.id = f.dir_id "
                          "WHERE %s ORDER BY f.date, f.name, fa.x", where);
   if (sqlite3_prepare_v2 (db, sql, -1, &st, NULL) == SQLITE_OK) {
      while (sqlite3_step (st) == SQLITE_ROW) {
         FaceRow r;
         if (g_cancellable_is_cancelled (cancel)) break;
         r.id = sqlite3_column_int64 (st, 0);
         r.path = g_build_filename ((const gchar *) sqlite3_column_text (st, 1),
                                    (const gchar *) sqlite3_column_text (st, 2), NULL);
         r.date = g_strdup ((const gchar *) sqlite3_column_text (st, 3));
         g_array_append_val (rows, r);
      }
      sqlite3_finalize (st);
   }
   g_free (sql);
   g_free (where);
   sqlite3_close (db);
   g_task_return_pointer (task, rows, (GDestroyNotify) g_array_unref);
}


static void
cb_listed (GObject *source, GAsyncResult *res, gpointer data)
{
   GTask *task = G_TASK (res);
   GArray *rows;
   GPtrArray *items;
   guint i;

   if (g_cancellable_is_cancelled (g_task_get_cancellable (task))) return;
   rows = g_task_propagate_pointer (task, NULL);
   if (!rows || !fw.window) {
      if (rows) g_array_unref (rows);
      return;
   }

   items = g_ptr_array_new_with_free_func (g_object_unref);
   for (i = 0; i < rows->len; i++) {
      FaceRow *r = &g_array_index (rows, FaceRow, i);
      GimvFaceItem *item = g_object_new (GIMV_TYPE_FACE_ITEM, NULL);
      item->id = r->id;
      item->path = g_steal_pointer (&r->path);
      item->date = g_steal_pointer (&r->date);
      g_ptr_array_add (items, item);
   }
   g_list_store_splice (fw.store, 0, g_list_model_get_n_items (G_LIST_MODEL (fw.store)),
                        items->pdata, items->len);
   g_ptr_array_unref (items);
   g_array_unref (rows);
   update_heading ();
   update_buttons ();
}


static void
reload (void)
{
   GTask *task;

   if (fw.cancel) {
      g_cancellable_cancel (fw.cancel);
      g_clear_object (&fw.cancel);
   }
   g_list_store_remove_all (fw.store);
   update_heading ();
   update_buttons ();

   fw.cancel = g_cancellable_new ();
   task = g_task_new (NULL, fw.cancel, cb_listed, NULL);
   g_task_set_task_data (task, g_strdup (fw.key), g_free);
   g_task_run_in_thread (task, thread_list_faces);
   g_object_unref (task);
}


/******************************************************************************
 *  the small images
 ******************************************************************************/

static GdkTexture *
face_texture (gint64 id)
{
   sqlite3 *db = gimv_index_get_db ();
   sqlite3_stmt *st;
   GdkTexture *texture = NULL;

   if (!db) return NULL;
   if (sqlite3_prepare_v2 (db, "SELECT jpeg FROM face_thumbs WHERE face_id = ?1",
                           -1, &st, NULL) != SQLITE_OK)
      return NULL;
   sqlite3_bind_int64 (st, 1, id);
   if (sqlite3_step (st) == SQLITE_ROW && sqlite3_column_bytes (st, 0) > 1) {
      GBytes *bytes = g_bytes_new (sqlite3_column_blob (st, 0), sqlite3_column_bytes (st, 0));
      texture = gdk_texture_new_from_bytes (bytes, NULL);
      g_bytes_unref (bytes);
   }
   sqlite3_finalize (st);
   return texture;
}


static void
cb_setup (GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
   GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
   GtkWidget *picture = gtk_picture_new ();
   GtkWidget *label = gtk_label_new (NULL);

   gtk_widget_set_size_request (picture, FACE_SIZE, FACE_SIZE);
   gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_COVER);
   gtk_widget_add_css_class (label, "dim-label");
   gtk_widget_add_css_class (label, "caption");
   gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
   gtk_label_set_max_width_chars (GTK_LABEL (label), 14);
   gtk_box_append (GTK_BOX (box), picture);
   gtk_box_append (GTK_BOX (box), label);
   gtk_list_item_set_child (li, box);
}


static void
cb_bind (GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
   GimvFaceItem *item = gtk_list_item_get_item (li);
   GtkWidget *box = gtk_list_item_get_child (li);
   GtkWidget *picture = gtk_widget_get_first_child (box);
   GtkWidget *label = gtk_widget_get_next_sibling (picture);
   GdkTexture *texture = face_texture (item->id);
   gchar *day = item->date ? g_strndup (item->date, 10) : g_strdup ("");

   gtk_picture_set_paintable (GTK_PICTURE (picture), GDK_PAINTABLE (texture));
   if (texture) g_object_unref (texture);
   gtk_label_set_text (GTK_LABEL (label), day);
   gtk_widget_set_tooltip_text (box, item->path);
   g_free (day);
}


static void
cb_unbind (GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
   GtkWidget *box = gtk_list_item_get_child (li);
   gtk_picture_set_paintable (GTK_PICTURE (gtk_widget_get_first_child (box)), NULL);
}


/******************************************************************************
 *  what to do with the faces
 ******************************************************************************/

static void
open_image (const gchar *path)
{
   GimvImageInfo *info = gimv_image_info_get (path);
   if (info)
      gimv_image_win_open_window_auto (info);
}


static void
cb_activate (GtkGridView *grid, guint pos, gpointer data)
{
   GimvFaceItem *item = g_list_model_get_item (G_LIST_MODEL (fw.store), pos);
   if (!item) return;
   open_image (item->path);
   g_object_unref (item);
}


static void
cb_open (GtkButton *button, gpointer data)
{
   GtkBitset *set = gtk_selection_model_get_selection (GTK_SELECTION_MODEL (fw.selection));
   if (!gtk_bitset_is_empty (set))
      cb_activate (NULL, gtk_bitset_get_minimum (set), NULL);
   gtk_bitset_unref (set);
}


typedef struct {
   GArray  *ids;
   gboolean stays;   /* in this list after the change */
} EditCtx;


static void
cb_edited (GObject *source, GAsyncResult *res, gpointer data)
{
   EditCtx *ctx = data;
   GError *error = NULL;
   gboolean ok = gimv_index_faces_edit_finish (res, NULL, &error);

   /* the window may have gone meanwhile */
   if (fw.window) {
      fw.busy = FALSE;
      g_clear_pointer (&fw.error, g_free);
      if (ok) {
         fw.changed = TRUE;
         if (ctx->stays)
            gtk_selection_model_unselect_all (GTK_SELECTION_MODEL (fw.selection));
         else
            remove_ids (ctx->ids);
      } else {
         fw.error = g_strdup (error ? error->message : "?");
      }
      update_heading ();
      update_buttons ();
   } else if (ok) {
      gimv_index_faces_flush ();
   }
   g_clear_error (&error);
   g_array_unref (ctx->ids);
   g_free (ctx);
}


static void
start_edit (GimvIndexFacesEdit edit, const gchar *name, gint64 person, gboolean stays)
{
   EditCtx *ctx;

   if (fw.busy) return;
   ctx = g_new0 (EditCtx, 1);
   ctx->ids = selected_ids ();
   ctx->stays = stays;
   if (!ctx->ids->len) {
      g_array_unref (ctx->ids);
      g_free (ctx);
      return;
   }
   fw.busy = TRUE;
   g_clear_pointer (&fw.error, g_free);
   update_heading ();
   update_buttons ();
   gimv_index_faces_edit_async (edit, ctx->ids, name, person, cb_edited, ctx);
}


/******************************************************************************
 *  changing the name of the person shown (the pencil)
 ******************************************************************************/

static void
name_edit_end (void)
{
   gtk_widget_set_visible (fw.name_entry, FALSE);
   gtk_widget_set_visible (fw.name_label, TRUE);
   update_heading ();
}


static void
cb_renamed (GObject *source, GAsyncResult *res, gpointer data)
{
   gchar *name = data;
   GError *error = NULL;
   gint64 person = 0;
   gboolean ok = gimv_index_faces_edit_finish (res, &person, &error);

   if (fw.window) {
      fw.busy = FALSE;
      g_clear_pointer (&fw.error, g_free);
      if (ok) {
         gchar *key = g_strdup_printf ("p:%" G_GINT64_FORMAT, person);
         gboolean merged = g_strcmp0 (key, fw.key) != 0;
         fw.changed = TRUE;
         g_free (fw.key);
         fw.key = key;
         g_free (fw.title);
         fw.title = g_strdup (name);
         /* two made one: the faces of both */
         if (merged) reload ();
      } else {
         fw.error = g_strdup (error ? error->message : "?");
      }
      update_heading ();
      update_buttons ();
   } else if (ok) {
      gimv_index_faces_flush ();
   }
   g_clear_error (&error);
   g_free (name);
}


static void
rename_to (const gchar *name)
{
   gint64 me = fw.key && fw.key[0] == 'p' ? g_ascii_strtoll (fw.key + 2, NULL, 10) : 0;

   if (!me || fw.busy) return;
   fw.busy = TRUE;
   g_clear_pointer (&fw.error, g_free);
   update_heading ();
   update_buttons ();
   gimv_index_faces_edit_async (GIMV_INDEX_FACES_RENAME, NULL, name, me,
                                cb_renamed, g_strdup (name));
}


static void
cb_merge_answer (GObject *source, GAsyncResult *res, gpointer data)
{
   gchar *name = data;
   gint button = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL);

   if (button == 1 && fw.window) rename_to (name);
   g_free (name);
}


static void
cb_name_entry_activate (GtkEntry *entry, gpointer data)
{
   gchar *name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry))));
   gint64 me = fw.key && fw.key[0] == 'p' ? g_ascii_strtoll (fw.key + 2, NULL, 10) : 0;
   gint64 other;

   name_edit_end ();
   if (!*name || !g_strcmp0 (name, fw.title) || !me) {
      g_free (name);
      return;
   }

   other = gimv_index_faces_find_person (name);
   if (other && other != me) {
      /* the same person named twice: one person, after asking */
      GtkAlertDialog *dialog = gtk_alert_dialog_new (_("\"%s\" is there already."), name);
      const gchar *buttons[] = { _("_Cancel"), _("_Make Them One"), NULL };
      gchar *detail = g_strdup_printf (_("The faces of \"%s\" become the faces of \"%s\", "
                                         "and \"%s\" goes."), fw.title, name, fw.title);
      gtk_alert_dialog_set_detail (dialog, detail);
      gtk_alert_dialog_set_buttons (dialog, buttons);
      gtk_alert_dialog_set_cancel_button (dialog, 0);
      gtk_alert_dialog_set_default_button (dialog, 1);
      gtk_alert_dialog_choose (dialog, GTK_WINDOW (fw.window), NULL, cb_merge_answer, name);
      g_object_unref (dialog);
      g_free (detail);
      return;
   }
   rename_to (name);
   g_free (name);
}


static gboolean
cb_name_entry_key (GtkEventControllerKey *ctrl, guint keyval, guint code,
                   GdkModifierType state, gpointer data)
{
   if (keyval == GDK_KEY_Escape) {
      name_edit_end ();
      return TRUE;
   }
   return FALSE;
}


static void
cb_name_entry_focus_out (GtkEventControllerFocus *ctrl, gpointer data)
{
   /* clicked elsewhere: as Escape */
   if (gtk_widget_get_visible (fw.name_entry))
      name_edit_end ();
}


static void
cb_edit_name (GtkButton *button, gpointer data)
{
   gtk_editable_set_text (GTK_EDITABLE (fw.name_entry), fw.title ? fw.title : "");
   gtk_widget_set_visible (fw.name_label, FALSE);
   gtk_widget_set_visible (fw.edit_button, FALSE);
   gtk_widget_set_visible (fw.name_entry, TRUE);
   gtk_widget_grab_focus (fw.name_entry);
   gtk_editable_select_region (GTK_EDITABLE (fw.name_entry), 0, -1);
}


/******************************************************************************
 *  the buttons
 ******************************************************************************/

static void
cb_confirm (GtkButton *button, gpointer data)
{
   start_edit (GIMV_INDEX_FACES_CONFIRM, NULL, 0, FALSE);
}


static void
cb_reject (GtkButton *button, gpointer data)
{
   gint64 pid = key_person ();
   if (pid) start_edit (GIMV_INDEX_FACES_REJECT, NULL, pid, FALSE);
}


static void
cb_clear (GtkButton *button, gpointer data)
{
   start_edit (GIMV_INDEX_FACES_CLEAR_NAME, NULL, 0, FALSE);
}


static void
cb_ignore (GtkButton *button, gpointer data)
{
   start_edit (GPOINTER_TO_INT (data) ? GIMV_INDEX_FACES_IGNORE : GIMV_INDEX_FACES_UNIGNORE,
               NULL, 0, FALSE);
}


/* the name: typed, or one given before picked from the list */
typedef struct {
   GtkWidget *dialog;
   GtkWidget *entry;
   GtkWidget *list;
} NameDialog;


static void
name_apply (NameDialog *nd)
{
   gchar *name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (nd->entry))));

   if (*name) {
      /* the person shown, named so again: they stay in the list */
      gboolean same = fw.key && fw.key[0] == 'p' && !g_strcmp0 (name, fw.title);
      start_edit (GIMV_INDEX_FACES_SET_NAME, name, 0, same);
   }
   g_free (name);
   gtk_window_destroy (GTK_WINDOW (nd->dialog));
}


static gboolean
name_filter (GtkListBoxRow *row, gpointer data)
{
   NameDialog *nd = data;
   const gchar *text = gtk_editable_get_text (GTK_EDITABLE (nd->entry));
   const gchar *name = g_object_get_data (G_OBJECT (row), "name");
   gchar *a, *b;
   gboolean found;

   if (!text || !*text) return TRUE;
   a = g_utf8_casefold (name, -1);
   b = g_utf8_casefold (text, -1);
   found = strstr (a, b) != NULL;
   g_free (a);
   g_free (b);
   return found;
}


static void
cb_name_changed (GtkEditable *entry, NameDialog *nd)
{
   gtk_list_box_invalidate_filter (GTK_LIST_BOX (nd->list));
}


static void
cb_name_row (GtkListBox *list, GtkListBoxRow *row, NameDialog *nd)
{
   gtk_editable_set_text (GTK_EDITABLE (nd->entry), g_object_get_data (G_OBJECT (row), "name"));
   name_apply (nd);
}


static void
cb_name_ok (GtkWidget *widget, NameDialog *nd)
{
   name_apply (nd);
}


static void
cb_name_cancel (GtkWidget *widget, NameDialog *nd)
{
   gtk_window_destroy (GTK_WINDOW (nd->dialog));
}


static gboolean
cb_name_key (GtkEventControllerKey *ctrl, guint keyval, guint code, GdkModifierType state,
             NameDialog *nd)
{
   if (keyval == GDK_KEY_Escape) {
      gtk_window_destroy (GTK_WINDOW (nd->dialog));
      return TRUE;
   }
   return FALSE;
}


static void
cb_name (GtkButton *button, gpointer data)
{
   NameDialog *nd = g_new0 (NameDialog, 1);
   GtkWidget *vbox, *label, *scroll, *hbox, *ok, *cancel;
   GtkEventController *keys;
   GPtrArray *names = gimv_index_faces_get_names ();
   guint i;

   nd->dialog = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (nd->dialog), _("Name the Faces"));
   gtk_window_set_transient_for (GTK_WINDOW (nd->dialog), GTK_WINDOW (fw.window));
   gtk_window_set_modal (GTK_WINDOW (nd->dialog), TRUE);
   gtk_window_set_default_size (GTK_WINDOW (nd->dialog), 320, 360);
   g_object_set_data_full (G_OBJECT (nd->dialog), "name-dialog", nd, g_free);

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start (vbox, 10);
   gtk_widget_set_margin_end (vbox, 10);
   gtk_widget_set_margin_top (vbox, 10);
   gtk_widget_set_margin_bottom (vbox, 10);
   gtk_window_set_child (GTK_WINDOW (nd->dialog), vbox);

   label = gtk_label_new (_("The name of the person (a new one, or one of these):"));
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_label_set_wrap (GTK_LABEL (label), TRUE);
   gtk_box_append (GTK_BOX (vbox), label);

   nd->entry = gtk_entry_new ();
   gtk_box_append (GTK_BOX (vbox), nd->entry);
   g_signal_connect (nd->entry, "activate", G_CALLBACK (cb_name_ok), nd);
   g_signal_connect (nd->entry, "changed", G_CALLBACK (cb_name_changed), nd);

   nd->list = gtk_list_box_new ();
   gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (nd->list), TRUE);
   for (i = 0; i < names->len; i++) {
      const gchar *name = g_ptr_array_index (names, i);
      GtkWidget *row = gtk_list_box_row_new (), *l = gtk_label_new (name);
      gtk_label_set_xalign (GTK_LABEL (l), 0.0);
      gtk_widget_set_margin_start (l, 6);
      gtk_widget_set_margin_top (l, 3);
      gtk_widget_set_margin_bottom (l, 3);
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), l);
      g_object_set_data_full (G_OBJECT (row), "name", g_strdup (name), g_free);
      gtk_list_box_append (GTK_LIST_BOX (nd->list), row);
   }
   g_ptr_array_unref (names);
   gtk_list_box_set_filter_func (GTK_LIST_BOX (nd->list), name_filter, nd, NULL);
   g_signal_connect (nd->list, "row-activated", G_CALLBACK (cb_name_row), nd);
   scroll = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_widget_set_vexpand (scroll, TRUE);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), nd->list);
   gtk_box_append (GTK_BOX (vbox), scroll);

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_widget_set_halign (hbox, GTK_ALIGN_END);
   cancel = gtk_button_new_with_mnemonic (_("_Cancel"));
   ok = gtk_button_new_with_mnemonic (_("_OK"));
   gtk_widget_add_css_class (ok, "suggested-action");
   g_signal_connect (cancel, "clicked", G_CALLBACK (cb_name_cancel), nd);
   g_signal_connect (ok, "clicked", G_CALLBACK (cb_name_ok), nd);
   gtk_box_append (GTK_BOX (hbox), cancel);
   gtk_box_append (GTK_BOX (hbox), ok);
   gtk_box_append (GTK_BOX (vbox), hbox);

   keys = gtk_event_controller_key_new ();
   g_signal_connect (keys, "key-pressed", G_CALLBACK (cb_name_key), nd);
   gtk_widget_add_controller (nd->dialog, keys);

   gtk_window_present (GTK_WINDOW (nd->dialog));
   gtk_widget_grab_focus (nd->entry);
}


static void
cb_select_all (GtkButton *button, gpointer data)
{
   gtk_selection_model_select_all (GTK_SELECTION_MODEL (fw.selection));
}


static void
cb_selection_changed (GtkSelectionModel *model, guint pos, guint n, gpointer data)
{
   update_buttons ();
}


static void
cb_tw_gone (gpointer data, GObject *where)
{
   fw.tw = NULL;
}


static void
cb_destroy (GtkWidget *widget, gpointer data)
{
   if (fw.cancel) {
      g_cancellable_cancel (fw.cancel);
      g_clear_object (&fw.cancel);
   }
   if (fw.tw) g_object_weak_unref (G_OBJECT (fw.tw), cb_tw_gone, NULL);
   fw.tw = NULL;
   fw.window = NULL;
   g_clear_object (&fw.selection);
   fw.store = NULL;   /* owned by the selection */
   g_clear_pointer (&fw.key, g_free);
   g_clear_pointer (&fw.title, g_free);
   g_clear_pointer (&fw.error, g_free);
   fw.busy = FALSE;
   /* the groups and guesses with the names given, now */
   if (fw.changed) gimv_index_faces_flush ();
   fw.changed = FALSE;
}


static GtkWidget *
add_button (GtkWidget *box, gint which, const gchar *label, const gchar *tip,
            GCallback cb, gpointer data)
{
   GtkWidget *b = gtk_button_new_with_mnemonic (label);
   if (tip) gtk_widget_set_tooltip_text (b, tip);
   g_signal_connect (b, "clicked", cb, data);
   gtk_box_append (GTK_BOX (box), b);
   if (which >= 0) fw.buttons[which] = b;
   return b;
}


static void
create_window (void)
{
   GtkWidget *vbox, *scroll, *bar, *all;
   GtkListItemFactory *factory;

   fw.window = gtk_window_new ();
   gtk_window_set_default_size (GTK_WINDOW (fw.window), 900, 640);
   g_signal_connect (fw.window, "destroy", G_CALLBACK (cb_destroy), NULL);

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start (vbox, 8);
   gtk_widget_set_margin_end (vbox, 8);
   gtk_widget_set_margin_top (vbox, 8);
   gtk_widget_set_margin_bottom (vbox, 8);
   gtk_window_set_child (GTK_WINDOW (fw.window), vbox);

   /* the name (selectable, to copy), the pencil to change it, the count */
   fw.heading = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
   fw.name_label = gtk_label_new (NULL);
   gtk_label_set_selectable (GTK_LABEL (fw.name_label), TRUE);
   gtk_label_set_xalign (GTK_LABEL (fw.name_label), 0.0);
   gtk_widget_add_css_class (fw.name_label, "heading");
   /* selectable labels take the focus first: not wanted at the start */
   gtk_widget_set_focus_on_click (fw.name_label, FALSE);
   gtk_box_append (GTK_BOX (fw.heading), fw.name_label);

   fw.name_entry = gtk_entry_new ();
   gtk_editable_set_width_chars (GTK_EDITABLE (fw.name_entry), 24);
   gtk_widget_set_visible (fw.name_entry, FALSE);
   g_signal_connect (fw.name_entry, "activate", G_CALLBACK (cb_name_entry_activate), NULL);
   {
      GtkEventController *keys = gtk_event_controller_key_new ();
      GtkEventController *focus = gtk_event_controller_focus_new ();
      g_signal_connect (keys, "key-pressed", G_CALLBACK (cb_name_entry_key), NULL);
      g_signal_connect (focus, "leave", G_CALLBACK (cb_name_entry_focus_out), NULL);
      gtk_widget_add_controller (fw.name_entry, keys);
      gtk_widget_add_controller (fw.name_entry, focus);
   }
   gtk_box_append (GTK_BOX (fw.heading), fw.name_entry);

   fw.edit_button = gtk_button_new_from_icon_name ("document-edit-symbolic");
   gtk_button_set_has_frame (GTK_BUTTON (fw.edit_button), FALSE);
   gtk_widget_set_tooltip_text (fw.edit_button, _("Change the name"));
   gtk_widget_set_valign (fw.edit_button, GTK_ALIGN_CENTER);
   g_signal_connect (fw.edit_button, "clicked", G_CALLBACK (cb_edit_name), NULL);
   gtk_box_append (GTK_BOX (fw.heading), fw.edit_button);

   fw.count_label = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (fw.count_label), 0.0);
   gtk_widget_add_css_class (fw.count_label, "dim-label");
   gtk_widget_set_margin_start (fw.count_label, 8);
   gtk_box_append (GTK_BOX (fw.heading), fw.count_label);
   gtk_box_append (GTK_BOX (vbox), fw.heading);

   fw.store = g_list_store_new (GIMV_TYPE_FACE_ITEM);
   fw.selection = gtk_multi_selection_new (G_LIST_MODEL (fw.store));
   g_signal_connect (fw.selection, "selection-changed", G_CALLBACK (cb_selection_changed), NULL);

   factory = gtk_signal_list_item_factory_new ();
   g_signal_connect (factory, "setup",  G_CALLBACK (cb_setup), NULL);
   g_signal_connect (factory, "bind",   G_CALLBACK (cb_bind), NULL);
   g_signal_connect (factory, "unbind", G_CALLBACK (cb_unbind), NULL);
   /* the grid holds a reference of its own */
   fw.grid = gtk_grid_view_new (GTK_SELECTION_MODEL (g_object_ref (fw.selection)), factory);
   gtk_grid_view_set_max_columns (GTK_GRID_VIEW (fw.grid), 30);
   gtk_grid_view_set_enable_rubberband (GTK_GRID_VIEW (fw.grid), TRUE);
   g_signal_connect (fw.grid, "activate", G_CALLBACK (cb_activate), NULL);

   scroll = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_widget_set_vexpand (scroll, TRUE);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), fw.grid);
   gtk_box_append (GTK_BOX (vbox), scroll);

   bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_box_append (GTK_BOX (vbox), bar);
   all = add_button (bar, -1, _("Select _All"), _("Select all the faces (Ctrl+A)"),
                     G_CALLBACK (cb_select_all), NULL);
   (void) all;
   add_button (bar, B_NAME, _("_Name..."), _("Give a name to the faces selected"),
               G_CALLBACK (cb_name), NULL);
   add_button (bar, B_CONFIRM, _("_Right"), _("These faces are of this person"),
               G_CALLBACK (cb_confirm), NULL);
   add_button (bar, B_REJECT, _("Not This _Person"),
               _("These faces are not of this person, and are not guessed so again"),
               G_CALLBACK (cb_reject), NULL);
   add_button (bar, B_CLEAR, _("_Remove the Name"), _("The faces selected have no name again"),
               G_CALLBACK (cb_clear), NULL);
   add_button (bar, B_IGNORE, _("_Ignore"),
               _("Not to be named (not a face, a stranger): left out of the groups"),
               G_CALLBACK (cb_ignore), GINT_TO_POINTER (TRUE));
   add_button (bar, B_UNIGNORE, _("Do Not _Ignore"), NULL,
               G_CALLBACK (cb_ignore), GINT_TO_POINTER (FALSE));
   add_button (bar, B_OPEN, _("_Open the Image"), _("Open the image of the face (double click)"),
               G_CALLBACK (cb_open), NULL);
}


void
gimv_index_faces_win_show (GimvThumbWin *tw, const gchar *key, const gchar *title)
{
   g_return_if_fail (key);

   if (!fw.window) create_window ();
   if (fw.tw != tw) {
      if (fw.tw) g_object_weak_unref (G_OBJECT (fw.tw), cb_tw_gone, NULL);
      fw.tw = tw;
      if (tw) g_object_weak_ref (G_OBJECT (tw), cb_tw_gone, NULL);
   }
   g_free (fw.key);
   fw.key = g_strdup (key);
   g_free (fw.title);
   fw.title = g_strdup (title ? title : key);
   if (gtk_widget_get_visible (fw.name_entry)) name_edit_end ();
   reload ();
   /* the faces have the keys (Ctrl+A), not the name (a selectable label
    * would take the focus with its text all selected) */
   gtk_widget_grab_focus (fw.grid);
   gtk_label_select_region (GTK_LABEL (fw.name_label), 0, 0);
   gtk_window_present (GTK_WINDOW (fw.window));
}


gboolean
gimv_index_faces_win_is_open (void)
{
   return fw.window != NULL;
}
