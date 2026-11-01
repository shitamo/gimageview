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
#include <math.h>
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
   gfloat  sim;     /* sorted "least like the person first": the likeness */
   gboolean has_sim;
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


/* a person of the list on the left: the face most like the person and
 * the name */
#define GIMV_TYPE_PERSON_ITEM (gimv_person_item_get_type ())
G_DECLARE_FINAL_TYPE (GimvPersonItem, gimv_person_item, GIMV, PERSON_ITEM, GObject)

struct _GimvPersonItem {
   GObject parent;
   gchar  *key;       /* "p:ID" a person, "c:N" a group without a name */
   gint64  face;      /* shown for the person */
   guint   n_faces;
   gchar  *name;      /* the name, "Group 12" */
   gchar  *fold;      /* the name to look for (casefolded) */
};

G_DEFINE_TYPE (GimvPersonItem, gimv_person_item, G_TYPE_OBJECT)

static void
gimv_person_item_finalize (GObject *object)
{
   GimvPersonItem *item = GIMV_PERSON_ITEM (object);
   g_free (item->key);
   g_free (item->name);
   g_free (item->fold);
   G_OBJECT_CLASS (gimv_person_item_parent_class)->finalize (object);
}

static void
gimv_person_item_class_init (GimvPersonItemClass *klass)
{
   G_OBJECT_CLASS (klass)->finalize = gimv_person_item_finalize;
}

static void
gimv_person_item_init (GimvPersonItem *item)
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
   GtkWidget         *sort_drop;    /* the order of the faces */
   GtkWidget         *tags_button;  /* the tags of the person */
   GtkWidget         *tags_label;
   GtkWidget         *grid;
   GtkWidget         *buttons[8];
   GListStore        *store;
   GtkMultiSelection *selection;
   GCancellable      *cancel;
   GimvThumbWin      *tw;        /* weak */

   /* the people on the left */
   GListStore        *persons;
   GtkCustomFilter   *person_filter;
   GtkSingleSelection *person_sel;
   GtkWidget         *person_list;
   GtkWidget         *person_search;
   GCancellable      *persons_cancel;
   guint              persons_watch;
   guint              persons_timeout;
   gboolean           person_syncing;   /* the selection set to match fw.key */

   gchar             *key;
   gchar             *title;
   gboolean           changed;   /* names given: the groups made again on close */
   gboolean           busy;      /* a change being written */
   gchar             *error;     /* of the last change, shown in the heading */
} fw;

/* the order of the faces (the items of sort_drop) */
enum {
   SORT_OLD_FIRST,
   SORT_NEW_FIRST,
   SORT_UNLIKE_FIRST,   /* least like the person (or the group) first */
};
/* kept for the next faces shown (and the next window) */
static guint sort_order = SORT_OLD_FIRST;

enum {
   B_NAME, B_CONFIRM, B_REJECT, B_CLEAR, B_IGNORE, B_UNIGNORE, B_OPEN, N_BUTTONS
};


static void update_buttons (void);
static gboolean key_has_likeness (const gchar *key);
static void sync_person_selection (void);


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
   /* a named person: the tags, and the button to change them */
   gtk_widget_set_visible (fw.tags_button, fw.key && fw.key[0] == 'p');
   gtk_widget_set_sensitive (fw.tags_button, !fw.busy);
   {
      gchar *tags = NULL;
      if (fw.key && fw.key[0] == 'p') {
         GPtrArray *list = gimv_index_faces_get_person_tags (g_ascii_strtoll (fw.key + 2, NULL, 10));
         g_ptr_array_add (list, NULL);
         tags = g_strjoinv (", ", (gchar **) list->pdata);
         g_ptr_array_unref (list);
      }
      gtk_label_set_text (GTK_LABEL (fw.tags_label), tags ? tags : "");
      gtk_widget_set_visible (fw.tags_label, tags && *tags);
      g_free (tags);
   }
   if (fw.busy)
      line = g_strdup_printf ("%s — %s", count, _("writing..."));
   else if (fw.error)
      line = g_strdup_printf (_("%s — could not be written: %s"), count, fw.error);
   else if (sort_order == SORT_UNLIKE_FIRST && !key_has_likeness (fw.key))
      /* not of one person: by date */
      line = g_strdup_printf (_("%s — by date (not of one person)"), count);
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
   gfloat sim;
   gboolean has_sim;
   gfloat *feature;   /* SORT_UNLIKE_FIRST only */
} FaceRow;

typedef struct {
   gchar *key;
   guint  order;
} ListRequest;


static void
list_request_free (ListRequest *req)
{
   g_free (req->key);
   g_free (req);
}


/* "least like the person first" makes sense for the faces of one person
 * (named, or guessed) and of a group */
static gboolean
key_has_likeness (const gchar *key)
{
   return key && (key[0] == 'p' || key[0] == 'g' || key[0] == 'c') && key[1] == ':';
}

static void
face_row_clear (gpointer p)
{
   FaceRow *r = p;
   g_free (r->path);
   g_free (r->date);
   g_free (r->feature);
}


#define FACE_FEATURE_LEN 128

static void
feature_normalize (gfloat *m)
{
   gdouble len = 0;
   gint i;

   for (i = 0; i < FACE_FEATURE_LEN; i++) len += (gdouble) m[i] * m[i];
   if (len <= 0) return;
   len = sqrt (len);
   for (i = 0; i < FACE_FEATURE_LEN; i++) m[i] /= len;
}


/* the mean of the faces named as the person (for the guesses: whom they
 * are compared with), NULL when there is none */
static gfloat *
person_mean (sqlite3 *db, gint64 person)
{
   sqlite3_stmt *st;
   gfloat *m = g_new0 (gfloat, FACE_FEATURE_LEN);
   guint n = 0;
   gint i;

   if (sqlite3_prepare_v2 (db, "SELECT feature FROM faces WHERE person_id = ?1 AND ignored = 0",
                           -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64 (st, 1, person);
      while (sqlite3_step (st) == SQLITE_ROW) {
         const gfloat *f = sqlite3_column_blob (st, 0);
         if (sqlite3_column_bytes (st, 0) != FACE_FEATURE_LEN * (gint) sizeof (gfloat)) continue;
         for (i = 0; i < FACE_FEATURE_LEN; i++) m[i] += f[i];
         n++;
      }
      sqlite3_finalize (st);
   }
   if (!n) {
      g_free (m);
      return NULL;
   }
   feature_normalize (m);
   return m;
}


static gint
cmp_unlike_first (gconstpointer a, gconstpointer b)
{
   const FaceRow *x = a, *y = b;

   /* the faces without a feature at the end */
   if (x->has_sim != y->has_sim) return x->has_sim ? -1 : 1;
   if (x->sim != y->sim) return x->sim < y->sim ? -1 : 1;
   return g_strcmp0 (x->date, y->date);
}


/* the likeness of each face to the mean (of the person, else of the faces
 * listed), least alike first */
static void
sort_unlike_first (sqlite3 *db, const gchar *key, GArray *rows)
{
   gfloat *mean = NULL;
   guint i, n = 0;
   gint j;

   if (key[0] == 'g')
      mean = person_mean (db, g_ascii_strtoll (key + 2, NULL, 10));
   if (!mean) {
      mean = g_new0 (gfloat, FACE_FEATURE_LEN);
      for (i = 0; i < rows->len; i++) {
         FaceRow *r = &g_array_index (rows, FaceRow, i);
         if (!r->feature) continue;
         for (j = 0; j < FACE_FEATURE_LEN; j++) mean[j] += r->feature[j];
         n++;
      }
      feature_normalize (mean);
   }
   for (i = 0; i < rows->len; i++) {
      FaceRow *r = &g_array_index (rows, FaceRow, i);
      gfloat s = 0;
      if (!r->feature) continue;
      for (j = 0; j < FACE_FEATURE_LEN; j++) s += mean[j] * r->feature[j];
      r->sim = s;
      r->has_sim = TRUE;
   }
   g_free (mean);
   g_array_sort (rows, cmp_unlike_first);
}


static void
thread_list_faces (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   ListRequest *req = data;
   const gchar *key = req->key;
   gboolean unlike = req->order == SORT_UNLIKE_FIRST && key_has_likeness (key);
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
   sql = g_strdup_printf ("SELECT fa.id, d.path, f.name, f.date, %s FROM faces fa "
                          "JOIN files f ON f.id = fa.file_id JOIN dirs d ON d.id = f.dir_id "
                          "WHERE %s ORDER BY f.date %s, f.name %s, fa.x",
                          unlike ? "fa.feature" : "NULL", where,
                          req->order == SORT_NEW_FIRST ? "DESC" : "",
                          req->order == SORT_NEW_FIRST ? "DESC" : "");
   if (sqlite3_prepare_v2 (db, sql, -1, &st, NULL) == SQLITE_OK) {
      while (sqlite3_step (st) == SQLITE_ROW) {
         FaceRow r;
         if (g_cancellable_is_cancelled (cancel)) break;
         r.id = sqlite3_column_int64 (st, 0);
         r.path = g_build_filename ((const gchar *) sqlite3_column_text (st, 1),
                                    (const gchar *) sqlite3_column_text (st, 2), NULL);
         r.date = g_strdup ((const gchar *) sqlite3_column_text (st, 3));
         r.sim = 0;
         r.has_sim = FALSE;
         r.feature = sqlite3_column_bytes (st, 4) == FACE_FEATURE_LEN * (gint) sizeof (gfloat)
                     ? g_memdup2 (sqlite3_column_blob (st, 4), FACE_FEATURE_LEN * sizeof (gfloat))
                     : NULL;
         g_array_append_val (rows, r);
      }
      sqlite3_finalize (st);
   }
   if (unlike && !g_cancellable_is_cancelled (cancel))
      sort_unlike_first (db, key, rows);
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
      item->sim = r->sim;
      item->has_sim = r->has_sim;
      g_ptr_array_add (items, item);
   }
   g_list_store_splice (fw.store, 0, g_list_model_get_n_items (G_LIST_MODEL (fw.store)),
                        items->pdata, items->len);
   g_ptr_array_unref (items);
   g_array_unref (rows);
   update_heading ();
   update_buttons ();
}


static void reload (void);

static void
cb_sort_changed (GtkDropDown *drop, GParamSpec *pspec, gpointer data)
{
   guint order = gtk_drop_down_get_selected (drop);

   if (order == GTK_INVALID_LIST_POSITION || order == sort_order) return;
   sort_order = order;
   if (fw.key) reload ();
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
   {
      ListRequest *req = g_new0 (ListRequest, 1);
      req->key = g_strdup (fw.key);
      req->order = sort_order;
      g_task_set_task_data (task, req, (GDestroyNotify) list_request_free);
   }
   g_task_run_in_thread (task, thread_list_faces);
   g_object_unref (task);
}


/******************************************************************************
 *  the small images
 ******************************************************************************/

GdkTexture *
gimv_index_faces_get_texture (gint64 id)
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
   GdkTexture *texture = gimv_index_faces_get_texture (item->id);
   gchar *day = item->date ? g_strndup (item->date, 10) : g_strdup ("");

   gtk_picture_set_paintable (GTK_PICTURE (picture), GDK_PAINTABLE (texture));
   if (texture) g_object_unref (texture);
   if (item->has_sim) {
      /* how alike, shown with the date (the order is by it) */
      gchar *text = g_strdup_printf ("%s  %.2f", day, item->sim);
      gchar *tip = g_strdup_printf (_("%s\nLikeness: %.2f (1 the same, about 0.4 and under "
                                      "another person)"), item->path, item->sim);
      gtk_label_set_text (GTK_LABEL (label), text);
      gtk_widget_set_tooltip_text (box, tip);
      g_free (text);
      g_free (tip);
   } else {
      gtk_label_set_text (GTK_LABEL (label), day);
      gtk_widget_set_tooltip_text (box, item->path);
   }
   g_free (day);
}


static void
cb_unbind (GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
   GtkWidget *box = gtk_list_item_get_child (li);
   GtkWidget *picture = box ? gtk_widget_get_first_child (box) : NULL;
   if (GTK_IS_PICTURE (picture))
      gtk_picture_set_paintable (GTK_PICTURE (picture), NULL);
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
         sync_person_selection ();
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


static guint name_focus_id = 0;

static gboolean
idle_name_focus_out (gpointer data)
{
   GtkWidget *focus;

   name_focus_id = 0;
   if (!fw.window || !gtk_widget_get_visible (fw.name_entry)) return G_SOURCE_REMOVE;
   /* another program (to copy a name from it): the entry stays, and has
    * the focus again when the window comes back */
   if (!gtk_window_is_active (GTK_WINDOW (fw.window))) return G_SOURCE_REMOVE;
   focus = gtk_root_get_focus (GTK_ROOT (fw.window));
   if (focus && (focus == fw.name_entry || gtk_widget_is_ancestor (focus, fw.name_entry)))
      return G_SOURCE_REMOVE;
   /* clicked elsewhere in the window: as Escape */
   name_edit_end ();
   return G_SOURCE_REMOVE;
}


static void
cb_name_entry_focus_out (GtkEventControllerFocus *ctrl, gpointer data)
{
   /* looked at after the window has said whether it is still active */
   if (gtk_widget_get_visible (fw.name_entry) && !name_focus_id)
      name_focus_id = g_timeout_add (100, idle_name_focus_out, NULL);
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


static void
cb_tags_edited (GObject *source, GAsyncResult *res, gpointer data)
{
   GError *error = NULL;
   gboolean ok = gimv_index_faces_edit_finish (res, NULL, &error);

   if (fw.window) {
      fw.busy = FALSE;
      g_clear_pointer (&fw.error, g_free);
      if (!ok) fw.error = g_strdup (error ? error->message : "?");
      update_heading ();
      update_buttons ();
   }
   g_clear_error (&error);
}


static void
tags_given (const gchar *tags, gpointer data)
{
   gint64 me = fw.key && fw.key[0] == 'p' ? g_ascii_strtoll (fw.key + 2, NULL, 10) : 0;

   if (!fw.window || !me || fw.busy) return;
   fw.busy = TRUE;
   update_heading ();
   update_buttons ();
   gimv_index_faces_edit_async (GIMV_INDEX_FACES_PERSON_TAGS, NULL, tags, me,
                                cb_tags_edited, NULL);
}


static void
cb_edit_tags (GtkButton *button, gpointer data)
{
   gint64 me = fw.key && fw.key[0] == 'p' ? g_ascii_strtoll (fw.key + 2, NULL, 10) : 0;
   if (me)
      gimv_index_faces_ask_person_tags (GTK_WINDOW (fw.window), fw.title, me, tags_given, NULL);
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
reject_confirmed (gpointer data)
{
   gint64 pid = key_person ();
   if (fw.window && pid) start_edit (GIMV_INDEX_FACES_REJECT, NULL, pid, FALSE);
}


static void
cb_reject (GtkButton *button, gpointer data)
{
   GArray *ids = selected_ids ();
   gchar *name = g_strdup (fw.title ? fw.title : "");
   gsize len = strlen (name);

   /* "Eileen?" (the guesses): the name without the mark */
   if (fw.key && fw.key[0] == 'g' && len > 0) {
      const gchar *q = _("%s?");
      gchar *probe = g_strdup_printf (q, "");
      if (*probe && g_str_has_suffix (name, probe))
         name[len - strlen (probe)] = '\0';
      g_free (probe);
   }
   if (ids->len && key_person ())
      gimv_index_faces_confirm_reject (GTK_WINDOW (fw.window), name, ids->len,
                                       reject_confirmed, NULL);
   g_array_unref (ids);
   g_free (name);
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
   GimvIndexFacesNameFunc done;
   gpointer   data;
} NameDialog;


static void
name_apply (NameDialog *nd)
{
   gchar *name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (nd->entry))));

   if (*name && nd->done)
      nd->done (name, nd->data);
   g_free (name);
   gtk_window_destroy (GTK_WINDOW (nd->dialog));
}


static void
name_for_window (const gchar *name, gpointer data)
{
   /* the person shown, named so again: they stay in the list */
   gboolean same = fw.key && fw.key[0] == 'p' && !g_strcmp0 (name, fw.title);
   if (fw.window)
      start_edit (GIMV_INDEX_FACES_SET_NAME, name, 0, same);
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


/* asks for a name (a new one, or one given before); done gets it */
void
gimv_index_faces_ask_name (GtkWindow *parent, GimvIndexFacesNameFunc done, gpointer data)
{
   NameDialog *nd = g_new0 (NameDialog, 1);
   GtkWidget *vbox, *label, *scroll, *hbox, *ok, *cancel;
   GtkEventController *keys;
   GPtrArray *names = gimv_index_faces_get_names ();
   guint i;

   nd->done = done;
   nd->data = data;
   nd->dialog = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (nd->dialog), _("Name the Faces"));
   if (parent)
      gtk_window_set_transient_for (GTK_WINDOW (nd->dialog), parent);
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
cb_name (GtkButton *button, gpointer data)
{
   gimv_index_faces_ask_name (GTK_WINDOW (fw.window), name_for_window, NULL);
}


/******************************************************************************
 *  the tags of a person
 ******************************************************************************/

typedef struct {
   GtkWidget *dialog;
   GtkWidget *tags;      /* the tags given: a row each, with a remove button */
   GtkWidget *entry;
   GtkWidget *known;     /* the tags other people have, to pick */
   GimvIndexFacesNameFunc done;
   gpointer   data;
} TagsDialog;


static void tags_add (TagsDialog *td, const gchar *tag);


static gboolean
tags_has (TagsDialog *td, const gchar *tag)
{
   GtkWidget *row;
   for (row = gtk_widget_get_first_child (td->tags); row; row = gtk_widget_get_next_sibling (row))
      if (!g_strcmp0 (g_object_get_data (G_OBJECT (row), "tag"), tag)) return TRUE;
   return FALSE;
}


static void
tags_update_known (TagsDialog *td)
{
   GtkWidget *row;
   /* the ones given already are not offered */
   for (row = gtk_widget_get_first_child (td->known); row; row = gtk_widget_get_next_sibling (row))
      gtk_widget_set_visible (row, !tags_has (td, g_object_get_data (G_OBJECT (row), "tag")));
}


static void
cb_tag_remove (GtkButton *button, TagsDialog *td)
{
   GtkWidget *row = g_object_get_data (G_OBJECT (button), "row");
   gtk_list_box_remove (GTK_LIST_BOX (td->tags), row);
   tags_update_known (td);
}


static void
tags_add (TagsDialog *td, const gchar *text)
{
   gchar *tag = g_strstrip (g_strdup (text ? text : ""));
   GtkWidget *row, *box, *label, *remove;

   /* one a line in the database; a comma would look like two */
   g_strdelimit (tag, "\n\t", ' ');
   if (!*tag || tags_has (td, tag)) {
      g_free (tag);
      return;
   }
   row = gtk_list_box_row_new ();
   box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
   label = gtk_label_new (tag);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_widget_set_hexpand (label, TRUE);
   gtk_widget_set_margin_start (label, 6);
   remove = gtk_button_new_from_icon_name ("list-remove-symbolic");
   gtk_button_set_has_frame (GTK_BUTTON (remove), FALSE);
   gtk_widget_set_tooltip_text (remove, _("Remove this tag"));
   g_object_set_data (G_OBJECT (remove), "row", row);
   g_signal_connect (remove, "clicked", G_CALLBACK (cb_tag_remove), td);
   gtk_box_append (GTK_BOX (box), label);
   gtk_box_append (GTK_BOX (box), remove);
   gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
   gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
   g_object_set_data_full (G_OBJECT (row), "tag", tag, g_free);
   gtk_list_box_append (GTK_LIST_BOX (td->tags), row);
   tags_update_known (td);
}


static void
cb_tag_entry (GtkEntry *entry, TagsDialog *td)
{
   tags_add (td, gtk_editable_get_text (GTK_EDITABLE (entry)));
   gtk_editable_set_text (GTK_EDITABLE (entry), "");
}


static void
cb_tag_known (GtkListBox *list, GtkListBoxRow *row, TagsDialog *td)
{
   tags_add (td, g_object_get_data (G_OBJECT (row), "tag"));
}


static void
cb_tags_ok (GtkWidget *widget, TagsDialog *td)
{
   GString *all = g_string_new (NULL);
   GtkWidget *row;

   /* what is typed and not added yet counts too */
   tags_add (td, gtk_editable_get_text (GTK_EDITABLE (td->entry)));
   for (row = gtk_widget_get_first_child (td->tags); row; row = gtk_widget_get_next_sibling (row)) {
      if (all->len) g_string_append_c (all, '\n');
      g_string_append (all, g_object_get_data (G_OBJECT (row), "tag"));
   }
   if (td->done) td->done (all->str, td->data);
   g_string_free (all, TRUE);
   gtk_window_destroy (GTK_WINDOW (td->dialog));
}


static void
cb_tags_cancel (GtkWidget *widget, TagsDialog *td)
{
   gtk_window_destroy (GTK_WINDOW (td->dialog));
}


static gboolean
cb_tags_key (GtkEventControllerKey *ctrl, guint keyval, guint code, GdkModifierType state,
             TagsDialog *td)
{
   if (keyval == GDK_KEY_Escape) {
      gtk_window_destroy (GTK_WINDOW (td->dialog));
      return TRUE;
   }
   return FALSE;
}


static GtkWidget *
scrolled (GtkWidget *child, gint height)
{
   GtkWidget *scroll = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_widget_set_size_request (scroll, -1, height);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), child);
   gtk_widget_add_css_class (scroll, "frame");
   return scroll;
}


/* the tags of a person: done gets them, one a line ("" for none) */
void
gimv_index_faces_ask_person_tags (GtkWindow *parent, const gchar *person, gint64 person_id,
                                  GimvIndexFacesNameFunc done, gpointer data)
{
   TagsDialog *td = g_new0 (TagsDialog, 1);
   GtkWidget *vbox, *label, *hbox, *ok, *cancel;
   GtkEventController *keys;
   GPtrArray *mine = gimv_index_faces_get_person_tags (person_id);
   GPtrArray *all = gimv_index_faces_get_all_person_tags ();
   gchar *title;
   guint i;

   td->done = done;
   td->data = data;
   td->dialog = gtk_window_new ();
   title = g_strdup_printf (_("The Tags of \"%s\""), person ? person : "");
   gtk_window_set_title (GTK_WINDOW (td->dialog), title);
   g_free (title);
   if (parent) gtk_window_set_transient_for (GTK_WINDOW (td->dialog), parent);
   gtk_window_set_modal (GTK_WINDOW (td->dialog), TRUE);
   gtk_window_set_default_size (GTK_WINDOW (td->dialog), 320, 420);
   g_object_set_data_full (G_OBJECT (td->dialog), "tags-dialog", td, g_free);

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start (vbox, 10);
   gtk_widget_set_margin_end (vbox, 10);
   gtk_widget_set_margin_top (vbox, 10);
   gtk_widget_set_margin_bottom (vbox, 10);
   gtk_window_set_child (GTK_WINDOW (td->dialog), vbox);

   label = gtk_label_new (_("Tags of the person (the group, ...): they go with every "
                            "image the person is named in."));
   gtk_label_set_wrap (GTK_LABEL (label), TRUE);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_box_append (GTK_BOX (vbox), label);

   td->tags = gtk_list_box_new ();
   gtk_list_box_set_selection_mode (GTK_LIST_BOX (td->tags), GTK_SELECTION_NONE);
   gtk_box_append (GTK_BOX (vbox), scrolled (td->tags, 110));

   td->entry = gtk_entry_new ();
   gtk_entry_set_placeholder_text (GTK_ENTRY (td->entry), _("A new tag (Enter adds it)"));
   g_signal_connect (td->entry, "activate", G_CALLBACK (cb_tag_entry), td);
   gtk_box_append (GTK_BOX (vbox), td->entry);

   label = gtk_label_new (_("Tags other people have (a click adds it):"));
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_widget_add_css_class (label, "dim-label");
   gtk_box_append (GTK_BOX (vbox), label);
   td->known = gtk_list_box_new ();
   gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (td->known), TRUE);
   for (i = 0; i < all->len; i++) {
      GtkWidget *row = gtk_list_box_row_new (), *l = gtk_label_new (g_ptr_array_index (all, i));
      gtk_label_set_xalign (GTK_LABEL (l), 0.0);
      gtk_widget_set_margin_start (l, 6);
      gtk_widget_set_margin_top (l, 3);
      gtk_widget_set_margin_bottom (l, 3);
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), l);
      g_object_set_data_full (G_OBJECT (row), "tag", g_strdup (g_ptr_array_index (all, i)),
                              g_free);
      gtk_list_box_append (GTK_LIST_BOX (td->known), row);
   }
   g_signal_connect (td->known, "row-activated", G_CALLBACK (cb_tag_known), td);
   {
      GtkWidget *s = scrolled (td->known, 100);
      gtk_widget_set_vexpand (s, TRUE);
      gtk_box_append (GTK_BOX (vbox), s);
   }

   for (i = 0; i < mine->len; i++)
      tags_add (td, g_ptr_array_index (mine, i));

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_widget_set_halign (hbox, GTK_ALIGN_END);
   cancel = gtk_button_new_with_mnemonic (_("_Cancel"));
   ok = gtk_button_new_with_mnemonic (_("_OK"));
   gtk_widget_add_css_class (ok, "suggested-action");
   g_signal_connect (cancel, "clicked", G_CALLBACK (cb_tags_cancel), td);
   g_signal_connect (ok, "clicked", G_CALLBACK (cb_tags_ok), td);
   gtk_box_append (GTK_BOX (hbox), cancel);
   gtk_box_append (GTK_BOX (hbox), ok);
   gtk_box_append (GTK_BOX (vbox), hbox);

   keys = gtk_event_controller_key_new ();
   g_signal_connect (keys, "key-pressed", G_CALLBACK (cb_tags_key), td);
   gtk_widget_add_controller (td->dialog, keys);

   g_ptr_array_unref (mine);
   g_ptr_array_unref (all);
   gtk_window_present (GTK_WINDOW (td->dialog));
   gtk_widget_grab_focus (td->entry);
}


/* "not this person": asked again (it may be pressed by mistake, and the
 * faces are not guessed as that person again) */
typedef struct {
   GimvIndexFacesConfirmFunc done;
   gpointer data;
} ConfirmAsk;


static void
cb_confirm_answer (GObject *source, GAsyncResult *res, gpointer data)
{
   ConfirmAsk *ask = data;
   gint button = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL);
   if (button == 1 && ask->done) ask->done (ask->data);
   g_free (ask);
}


void
gimv_index_faces_confirm_reject (GtkWindow *parent, const gchar *name, guint n,
                                 GimvIndexFacesConfirmFunc done, gpointer data)
{
   GtkAlertDialog *dialog;
   const gchar *buttons[] = { _("_Cancel"), _("_Remove the Name"), NULL };
   ConfirmAsk *ask = g_new0 (ConfirmAsk, 1);
   gchar *detail;

   ask->done = done;
   ask->data = data;
   dialog = gtk_alert_dialog_new (n == 1 ? _("This face is not \"%s\"?")
                                         : _("These faces are not \"%s\"?"),
                                  name ? name : "");
   detail = g_strdup_printf (ngettext ("%'u face: the name goes, and it is not guessed as "
                                       "\"%s\" again.",
                                       "%'u faces: the name goes, and they are not guessed as "
                                       "\"%s\" again.", n),
                             n, name ? name : "");
   gtk_alert_dialog_set_detail (dialog, detail);
   gtk_alert_dialog_set_buttons (dialog, buttons);
   /* Enter does not remove by mistake */
   gtk_alert_dialog_set_cancel_button (dialog, 0);
   gtk_alert_dialog_set_default_button (dialog, 0);
   gtk_alert_dialog_choose (dialog, parent, NULL, cb_confirm_answer, ask);
   g_object_unref (dialog);
   g_free (detail);
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
   if (fw.persons_cancel) {
      g_cancellable_cancel (fw.persons_cancel);
      g_clear_object (&fw.persons_cancel);
   }
   if (fw.persons_watch) {
      gimv_index_remove_watch (fw.persons_watch);
      fw.persons_watch = 0;
   }
   if (fw.persons_timeout) {
      g_source_remove (fw.persons_timeout);
      fw.persons_timeout = 0;
   }
   g_clear_object (&fw.person_sel);
   g_clear_object (&fw.person_filter);
   fw.persons = NULL;   /* owned by the filter model of the selection */
   fw.person_list = NULL;
   fw.person_search = NULL;
   if (name_focus_id) {
      g_source_remove (name_focus_id);
      name_focus_id = 0;
   }
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


/******************************************************************************
 *  the people on the left: a click shows the faces of the person
 ******************************************************************************/

#define PERSON_FACE_SIZE 72

typedef struct {
   gint64 id;       /* a person: its id; a group: - its number */
   gint64 face;
   guint  n;
   gchar *name;     /* the person's name (NULL for a group) */
} PersonRow;


static void
person_row_clear (gpointer p)
{
   g_free (((PersonRow *) p)->name);
}


/* the people by name, then the groups without a name, the larger first
 * (as in the people tab) */
static gint
cmp_person_rows (gconstpointer a, gconstpointer b)
{
   const PersonRow *x = a, *y = b;
   gchar *kx, *ky;
   gint r;

   if ((x->id > 0) != (y->id > 0)) return x->id > 0 ? -1 : 1;
   if (x->id < 0) {
      if (x->n != y->n) return x->n > y->n ? -1 : 1;
      return x->id > y->id ? -1 : 1;   /* - number: the smaller number first */
   }
   kx = g_utf8_collate_key (x->name, -1);
   ky = g_utf8_collate_key (y->name, -1);
   r = strcmp (kx, ky);
   g_free (kx);
   g_free (ky);
   return r;
}


/* each person and each group without a name, and the face most like the
 * mean of its faces */
static void
thread_list_persons (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   GArray *rows = g_array_new (FALSE, TRUE, sizeof (PersonRow));
   /* id -> index + 1 (keys of their own: the array moves as it grows) */
   GHashTable *at = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
   GArray *pid = g_array_new (FALSE, FALSE, sizeof (gint64));
   GArray *fid = g_array_new (FALSE, FALSE, sizeof (gint64));
   GArray *feats = g_array_new (FALSE, FALSE, sizeof (gfloat));
   GArray *means;
   gchar *dbfile = gimv_index_db_default_path ();
   sqlite3 *db = NULL;
   sqlite3_stmt *st;
   gfloat *best;
   guint i;
   gint j;

   g_array_set_clear_func (rows, person_row_clear);
   gimv_index_db_open (dbfile, FALSE, &db, NULL);
   g_free (dbfile);
   /* the groups as "c:N" of the people tab (gimv_index_faces_key_where) */
   if (db && sqlite3_prepare_v2 (db,
          "SELECT p.id, p.name, fa.id, fa.feature FROM persons p "
          "JOIN faces fa ON fa.person_id = p.id WHERE fa.ignored = 0 "
          "AND fa.id IN (SELECT face_id FROM face_thumbs) "
          "UNION ALL SELECT - fa.cluster, NULL, fa.id, fa.feature FROM faces fa "
          "WHERE fa.cluster IS NOT NULL AND fa.person_id IS NULL AND fa.guess IS NULL "
          "AND fa.ignored = 0 AND fa.id IN (SELECT face_id FROM face_thumbs)",
          -1, &st, NULL) == SQLITE_OK)
   {
      while (sqlite3_step (st) == SQLITE_ROW && !g_cancellable_is_cancelled (cancel)) {
         gint64 p = sqlite3_column_int64 (st, 0), f = sqlite3_column_int64 (st, 2);
         gpointer idx;
         PersonRow *r;

         if (!(idx = g_hash_table_lookup (at, &p))) {
            PersonRow nr = { p, f, 0, g_strdup ((const gchar *) sqlite3_column_text (st, 1)) };
            g_array_append_val (rows, nr);
            idx = GUINT_TO_POINTER (rows->len);
            g_hash_table_insert (at, g_memdup2 (&p, sizeof (p)), idx);
         }
         r = &g_array_index (rows, PersonRow, GPOINTER_TO_UINT (idx) - 1);
         r->n++;
         if (sqlite3_column_bytes (st, 3) == FACE_FEATURE_LEN * (gint) sizeof (gfloat)) {
            g_array_append_val (pid, p);
            g_array_append_val (fid, f);
            g_array_append_vals (feats, sqlite3_column_blob (st, 3), FACE_FEATURE_LEN);
         }
      }
      sqlite3_finalize (st);
   }
   if (db) sqlite3_close (db);

   /* the mean of each person, then the face nearest to it */
   means = g_array_sized_new (FALSE, TRUE, sizeof (gfloat), rows->len * FACE_FEATURE_LEN);
   g_array_set_size (means, rows->len * FACE_FEATURE_LEN);
   best = g_new (gfloat, rows->len + 1);
   for (i = 0; i < rows->len; i++) best[i] = -2;
   for (i = 0; i < pid->len; i++) {
      guint k = GPOINTER_TO_UINT (g_hash_table_lookup (at, &g_array_index (pid, gint64, i))) - 1;
      for (j = 0; j < FACE_FEATURE_LEN; j++)
         g_array_index (means, gfloat, k * FACE_FEATURE_LEN + j)
            += g_array_index (feats, gfloat, i * FACE_FEATURE_LEN + j);
   }
   for (i = 0; i < rows->len; i++)
      feature_normalize (&g_array_index (means, gfloat, i * FACE_FEATURE_LEN));
   for (i = 0; i < pid->len; i++) {
      guint k = GPOINTER_TO_UINT (g_hash_table_lookup (at, &g_array_index (pid, gint64, i))) - 1;
      gfloat s = 0;
      for (j = 0; j < FACE_FEATURE_LEN; j++)
         s += g_array_index (means, gfloat, k * FACE_FEATURE_LEN + j)
              * g_array_index (feats, gfloat, i * FACE_FEATURE_LEN + j);
      if (s > best[k]) {
         best[k] = s;
         g_array_index (rows, PersonRow, k).face = g_array_index (fid, gint64, i);
      }
   }
   g_free (best);
   g_array_unref (means);
   g_array_unref (pid);
   g_array_unref (fid);
   g_array_unref (feats);
   g_hash_table_unref (at);

   g_array_sort (rows, cmp_person_rows);
   g_task_return_pointer (task, rows, (GDestroyNotify) g_array_unref);
}


static void
cb_persons_listed (GObject *source, GAsyncResult *res, gpointer data)
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
      PersonRow *r = &g_array_index (rows, PersonRow, i);
      GimvPersonItem *item = g_object_new (GIMV_TYPE_PERSON_ITEM, NULL);
      item->key = r->id > 0 ? g_strdup_printf ("p:%" G_GINT64_FORMAT, r->id)
                            : g_strdup_printf ("c:%" G_GINT64_FORMAT, - r->id);
      item->face = r->face;
      item->n_faces = r->n;
      item->name = r->id > 0 ? g_steal_pointer (&r->name)
                             : g_strdup_printf (_("Group %d"), (gint) - r->id);
      item->fold = g_utf8_casefold (item->name, -1);
      g_ptr_array_add (items, item);
   }
   fw.person_syncing = TRUE;
   g_list_store_splice (fw.persons, 0, g_list_model_get_n_items (G_LIST_MODEL (fw.persons)),
                        items->pdata, items->len);
   fw.person_syncing = FALSE;
   g_ptr_array_unref (items);
   g_array_unref (rows);
   sync_person_selection ();
}


static void
reload_persons (void)
{
   GTask *task;

   if (fw.persons_cancel) {
      g_cancellable_cancel (fw.persons_cancel);
      g_clear_object (&fw.persons_cancel);
   }
   fw.persons_cancel = g_cancellable_new ();
   task = g_task_new (NULL, fw.persons_cancel, cb_persons_listed, NULL);
   g_task_run_in_thread (task, thread_list_persons);
   g_object_unref (task);
}


static gboolean
cb_persons_timeout (gpointer data)
{
   fw.persons_timeout = 0;
   if (fw.window) reload_persons ();
   return G_SOURCE_REMOVE;
}


/* names given, changed, faces found: the list again, a moment later */
static void
cb_persons_changed (GimvIndexChange what, gpointer data)
{
   if (!(what & GIMV_INDEX_CHANGED_FACES) || !fw.window) return;
   if (fw.persons_timeout) g_source_remove (fw.persons_timeout);
   fw.persons_timeout = g_timeout_add (500, cb_persons_timeout, NULL);
}


/* the person shown (fw.key "p:ID") selected on the left, without showing
 * it again */
static void
sync_person_selection (void)
{
   GListModel *model;
   guint i, n, pos = GTK_INVALID_LIST_POSITION;

   if (!fw.person_sel) return;
   model = G_LIST_MODEL (fw.person_sel);
   n = g_list_model_get_n_items (model);
   for (i = 0; fw.key && i < n && pos == GTK_INVALID_LIST_POSITION; i++) {
      GimvPersonItem *item = g_list_model_get_item (model, i);
      if (!strcmp (item->key, fw.key)) pos = i;
      g_object_unref (item);
   }
   fw.person_syncing = TRUE;
   gtk_single_selection_set_selected (fw.person_sel, pos);
   fw.person_syncing = FALSE;
   if (pos != GTK_INVALID_LIST_POSITION)
      gtk_list_view_scroll_to (GTK_LIST_VIEW (fw.person_list), pos, GTK_LIST_SCROLL_NONE, NULL);
}


static void
cb_person_selected (GtkSingleSelection *sel, GParamSpec *pspec, gpointer data)
{
   GimvPersonItem *item;

   if (fw.person_syncing) return;
   item = gtk_single_selection_get_selected_item (sel);
   if (!item) return;
   if (g_strcmp0 (item->key, fw.key)) {
      gchar *key = g_strdup (item->key), *name = g_strdup (item->name);
      gimv_index_faces_win_show (fw.tw, key, name);
      g_free (key);
      g_free (name);
   }
}


static void
cb_person_setup (GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
   GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
   GtkWidget *picture = gtk_picture_new ();
   GtkWidget *label = gtk_label_new (NULL);

   gtk_widget_set_size_request (picture, PERSON_FACE_SIZE, PERSON_FACE_SIZE);
   gtk_widget_set_halign (picture, GTK_ALIGN_CENTER);
   gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_COVER);
   gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
   gtk_label_set_max_width_chars (GTK_LABEL (label), 12);
   gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
   gtk_widget_set_margin_top (box, 4);
   gtk_widget_set_margin_bottom (box, 4);
   gtk_box_append (GTK_BOX (box), picture);
   gtk_box_append (GTK_BOX (box), label);
   gtk_list_item_set_child (li, box);
}


static void
cb_person_bind (GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
   GimvPersonItem *item = gtk_list_item_get_item (li);
   GtkWidget *box = gtk_list_item_get_child (li);
   GtkWidget *picture = gtk_widget_get_first_child (box);
   GtkWidget *label = gtk_widget_get_next_sibling (picture);
   GdkTexture *texture = gimv_index_faces_get_texture (item->face);
   gchar *text = g_strdup_printf ("%s (%'u)", item->name, item->n_faces);

   gtk_picture_set_paintable (GTK_PICTURE (picture), GDK_PAINTABLE (texture));
   if (texture) g_object_unref (texture);
   gtk_label_set_text (GTK_LABEL (label), text);
   /* the groups without a name: dimmer than the people */
   if (item->key[0] == 'c')
      gtk_widget_add_css_class (label, "dim-label");
   else
      gtk_widget_remove_css_class (label, "dim-label");
   gtk_widget_set_tooltip_text (box, text);
   g_free (text);
}


static void
cb_person_unbind (GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
   GtkWidget *box = gtk_list_item_get_child (li);
   GtkWidget *picture = box ? gtk_widget_get_first_child (box) : NULL;
   if (GTK_IS_PICTURE (picture))
      gtk_picture_set_paintable (GTK_PICTURE (picture), NULL);
}


static gboolean
person_filter (gpointer object, gpointer data)
{
   GimvPersonItem *item = object;
   const gchar *text = fw.person_search
                       ? gtk_editable_get_text (GTK_EDITABLE (fw.person_search)) : "";
   gchar *fold;
   gboolean ok;

   if (!text || !*text) return TRUE;
   fold = g_utf8_casefold (text, -1);
   ok = strstr (item->fold, fold) != NULL;
   g_free (fold);
   return ok;
}


static void
cb_person_search (GtkSearchEntry *entry, gpointer data)
{
   gtk_filter_changed (GTK_FILTER (fw.person_filter), GTK_FILTER_CHANGE_DIFFERENT);
   sync_person_selection ();
}


static GtkWidget *
create_person_list (void)
{
   GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4), *scroll;
   GtkListItemFactory *factory;
   GtkFilterListModel *filtered;

   fw.person_search = gtk_search_entry_new ();
   gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (fw.person_search), _("Find a Name"));
   gtk_editable_set_width_chars (GTK_EDITABLE (fw.person_search), 10);
   g_signal_connect (fw.person_search, "search-changed", G_CALLBACK (cb_person_search), NULL);
   gtk_box_append (GTK_BOX (box), fw.person_search);

   fw.persons = g_list_store_new (GIMV_TYPE_PERSON_ITEM);
   fw.person_filter = gtk_custom_filter_new (person_filter, NULL, NULL);
   filtered = gtk_filter_list_model_new (G_LIST_MODEL (fw.persons),
                                         GTK_FILTER (g_object_ref (fw.person_filter)));
   fw.person_sel = gtk_single_selection_new (G_LIST_MODEL (filtered));
   gtk_single_selection_set_autoselect (fw.person_sel, FALSE);
   gtk_single_selection_set_can_unselect (fw.person_sel, TRUE);
   gtk_single_selection_set_selected (fw.person_sel, GTK_INVALID_LIST_POSITION);
   g_signal_connect (fw.person_sel, "notify::selected", G_CALLBACK (cb_person_selected), NULL);

   factory = gtk_signal_list_item_factory_new ();
   g_signal_connect (factory, "setup",  G_CALLBACK (cb_person_setup), NULL);
   g_signal_connect (factory, "bind",   G_CALLBACK (cb_person_bind), NULL);
   g_signal_connect (factory, "unbind", G_CALLBACK (cb_person_unbind), NULL);
   fw.person_list = gtk_list_view_new (GTK_SELECTION_MODEL (g_object_ref (fw.person_sel)),
                                       factory);
   gtk_widget_set_tooltip_text (fw.person_list, _("The people named, then the groups of faces "
                                                  "without a name: a click shows the faces"));

   scroll = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_widget_set_vexpand (scroll, TRUE);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), fw.person_list);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroll), TRUE);
   gtk_box_append (GTK_BOX (box), scroll);

   fw.persons_watch = gimv_index_add_watch (cb_persons_changed, NULL);
   reload_persons ();
   return box;
}


static void
create_window (void)
{
   GtkWidget *vbox, *scroll, *bar, *all;
   GtkListItemFactory *factory;

   fw.window = gtk_window_new ();
   gtk_window_set_default_size (GTK_WINDOW (fw.window), 1060, 660);
   g_signal_connect (fw.window, "destroy", G_CALLBACK (cb_destroy), NULL);

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start (vbox, 8);
   gtk_widget_set_margin_end (vbox, 8);
   gtk_widget_set_margin_top (vbox, 8);
   gtk_widget_set_margin_bottom (vbox, 8);
   {
      /* the people on the left, the faces of one on the right */
      GtkWidget *paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
      GtkWidget *people = create_person_list ();
      gtk_widget_set_margin_start (people, 8);
      gtk_widget_set_margin_top (people, 8);
      gtk_widget_set_margin_bottom (people, 8);
      gtk_paned_set_start_child (GTK_PANED (paned), people);
      gtk_paned_set_resize_start_child (GTK_PANED (paned), FALSE);
      gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);
      gtk_paned_set_end_child (GTK_PANED (paned), vbox);
      gtk_paned_set_shrink_end_child (GTK_PANED (paned), FALSE);
      gtk_window_set_child (GTK_WINDOW (fw.window), paned);
   }

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

   fw.tags_label = gtk_label_new (NULL);
   gtk_label_set_selectable (GTK_LABEL (fw.tags_label), TRUE);
   gtk_widget_set_focus_on_click (fw.tags_label, FALSE);
   gtk_widget_set_margin_start (fw.tags_label, 8);
   gtk_box_append (GTK_BOX (fw.heading), fw.tags_label);

   fw.tags_button = gtk_button_new_from_icon_name ("tag-symbolic");
   gtk_button_set_has_frame (GTK_BUTTON (fw.tags_button), FALSE);
   gtk_widget_set_tooltip_text (fw.tags_button, _("The tags of the person (the group, ...)"));
   gtk_widget_set_valign (fw.tags_button, GTK_ALIGN_CENTER);
   g_signal_connect (fw.tags_button, "clicked", G_CALLBACK (cb_edit_tags), NULL);
   gtk_box_append (GTK_BOX (fw.heading), fw.tags_button);

   fw.count_label = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (fw.count_label), 0.0);
   gtk_widget_add_css_class (fw.count_label, "dim-label");
   gtk_widget_set_margin_start (fw.count_label, 8);
   gtk_box_append (GTK_BOX (fw.heading), fw.count_label);
   {
      const gchar *orders[] = { _("Oldest First"), _("Newest First"),
                                _("Least Like the Person First"), NULL };
      GtkWidget *space = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_hexpand (space, TRUE);
      gtk_box_append (GTK_BOX (fw.heading), space);
      fw.sort_drop = gtk_drop_down_new_from_strings (orders);
      gtk_drop_down_set_selected (GTK_DROP_DOWN (fw.sort_drop), sort_order);
      gtk_widget_set_valign (fw.sort_drop, GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text (fw.sort_drop,
                                   _("The order of the faces. \"Least like the person first\": "
                                     "the faces most unlike the others of the person (or the "
                                     "group) come first, to find the ones named wrong."));
      g_signal_connect (fw.sort_drop, "notify::selected", G_CALLBACK (cb_sort_changed), NULL);
      gtk_box_append (GTK_BOX (fw.heading), fw.sort_drop);
   }
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
   sync_person_selection ();
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
