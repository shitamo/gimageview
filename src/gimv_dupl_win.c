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
 */

/*
 * The window of the duplicate images (2026: rewritten for comparing and
 * deleting).  The scan (gimv_dupl_scan.c) runs in a thread; the groups of
 * the same files come first, all of them at the end.  The pictures of the
 * window are read here, in threads too, only for what is shown: they do not
 * depend on the thumbnails the thumbnail view has loaded.
 */

#include "gimv_dupl_win.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "gimv_bookmark.h"
#include "gimv_comment.h"
#include "gimv_dupl_scan.h"
#include "gimv_enlarged.h"
#include "gimv_icon_stock.h"
#include "gimv_image_info.h"
#include "gimv_image_win.h"
#include "gimv_thumb.h"
#include "prefs.h"

#ifdef ENABLE_INDEX
#  include <sqlite3.h>
#  include "gimv_index.h"
#  include "gimv_index_db.h"
#  include "gimv_index_tags.h"
#endif

#define ROW_ICON_SIZE   48
#define CARD_PIC_SIZE   170
/* the cards shown of a group (a group of thousands of copies is not
 * looked at one by one) */
#define MAX_CARDS       60


/* what lives on in the threads after the window is gone */
typedef struct {
   gint          ref;
   GimvDuplWin  *sw;          /* NULL: the window is gone */
} Shared;

static Shared *
shared_ref (Shared *s)
{
   g_atomic_int_inc (&s->ref);
   return s;
}

static void
shared_unref (Shared *s)
{
   if (g_atomic_int_dec_and_test (&s->ref)) g_free (s);
}


struct GimvDuplWinPriv_Tag
{
   Shared        *shared;

   /* the scan */
   GimvDuplScan  *scan;        /* while it runs, the thread's */
   GCancellable  *cancel;
   gboolean       running;
   gboolean       final;       /* the groups are all of them (not the
                                  same files only, first) */
   gboolean       from_index;  /* the same files the index found (all the
                                  indexed images, or a folder) */
   GPtrArray     *groups;      /* GimvDuplGroup *: shown */
   gboolean       groups_owned;/* ... freed here (the first ones) */
   gboolean       checking;    /* looking for enlarged copies (priv->cancel
                                  stops it) */
   gchar         *done_text;   /* of the scan, shown again after it */

   /* the marks: path -> GINT_TO_POINTER (1 keep, 2 delete) set by hand;
    * otherwise the suggestion of the group */
   GHashTable    *marks;
   GHashTable    *removed;     /* path: moved to the trash */
   gint           skipped;     /* in archives: not looked at */

   /* the pictures: "size:path" -> GdkTexture (or a GdkPaintable) */
   GHashTable    *textures;
   GHashTable    *waiting;     /* "size:path" -> GSList of GWeakRef * */
   GHashTable    *pending;     /* "size:path" -> PicJob * queued or being read */
   guint          pic_serial;  /* the order of the requests */
   GThreadPool   *pool;

   /* widgets */
   GListStore    *group_store; /* GimvDuplEntry */
   GtkWidget     *group_view;
   GtkWidget     *head_label;
   GtkWidget     *kind_label;
   GtkWidget     *cards;       /* GtkFlowBox */
   GtkWidget     *empty_label;
   GtkWidget     *more_label;  /* the members not shown */
   GtkWidget     *progress;
   GtkWidget     *stop_button;
   GtkWidget     *summary_label;  /* of the group shown */
   GtkWidget     *trash_button;   /* ... its images to delete */
   GtkWidget     *trash_all_button; /* those of every group */
   gboolean       trashing;

   gint           current;     /* the group shown, -1 */
};


static GtkWindowClass *parent_class = NULL;

G_DEFINE_TYPE (GimvDuplWin, gimv_dupl_win, GTK_TYPE_WINDOW)


/******************************************************************************
 *   a row of the group list: the index of the group
 ******************************************************************************/

#define GIMV_TYPE_DUPL_ENTRY (gimv_dupl_entry_get_type ())
G_DECLARE_FINAL_TYPE (GimvDuplEntry, gimv_dupl_entry, GIMV, DUPL_ENTRY, GObject)

struct _GimvDuplEntry
{
   GObject parent;
   gint    index;
};

G_DEFINE_TYPE (GimvDuplEntry, gimv_dupl_entry, G_TYPE_OBJECT)

static void gimv_dupl_entry_class_init (GimvDuplEntryClass *klass) { }
static void gimv_dupl_entry_init (GimvDuplEntry *entry) { }

static GimvDuplEntry *
entry_new (gint index)
{
   GimvDuplEntry *entry = g_object_new (GIMV_TYPE_DUPL_ENTRY, NULL);
   entry->index = index;
   return entry;
}


/******************************************************************************
 *   kinds and marks
 ******************************************************************************/

static const gchar *
kind_label (GimvDuplKind kind)
{
   switch (kind) {
   case GIMV_DUPL_SAME:    return _("Identical");
   case GIMV_DUPL_RESIZED: return _("Other Sizes");
   default:                return _("Similar");
   }
}


static const gchar *
kind_text (GimvDuplKind kind)
{
   switch (kind) {
   case GIMV_DUPL_SAME:
      return _("The same files (the same bytes).");
   case GIMV_DUPL_RESIZED:
      return _("The same picture in other sizes (and its copies).");
   default:
      return _("Similar pictures: edited, saved again, or other shots.  "
               "None is chosen to keep.");
   }
}


#define MARK_KEEP   1
#define MARK_DELETE 2

static gboolean
is_removed (GimvDuplWin *sw, GimvDuplItem *item)
{
   return g_hash_table_contains (sw->priv->removed, item->path);
}


/* to delete: by hand, or the suggestion (all but the first) */
static gboolean
marked_delete (GimvDuplWin *sw, GimvDuplGroup *group, guint i)
{
   GimvDuplItem *item = g_ptr_array_index (group->members, i);
   gint mark = GPOINTER_TO_INT (g_hash_table_lookup (sw->priv->marks, item->path));

   if (mark) return mark == MARK_DELETE;
   return group->suggested && i > 0;
}


/* the members still there */
static guint
n_left (GimvDuplWin *sw, GimvDuplGroup *group)
{
   guint i, n = 0;

   for (i = 0; i < group->members->len; i++)
      if (!is_removed (sw, g_ptr_array_index (group->members, i))) n++;
   return n;
}


static void
group_counts (GimvDuplWin *sw, GimvDuplGroup *group,
              guint *n_del, goffset *bytes, guint *n_keep)
{
   guint i;

   *n_del = 0; *bytes = 0; if (n_keep) *n_keep = 0;
   for (i = 0; i < group->members->len; i++) {
      GimvDuplItem *item = g_ptr_array_index (group->members, i);
      if (is_removed (sw, item)) continue;
      if (marked_delete (sw, group, i)) {
         (*n_del)++;
         *bytes += item->size;
      } else if (n_keep) {
         (*n_keep)++;
      }
   }
}


/******************************************************************************
 *   the pictures, read in threads
 ******************************************************************************/

typedef struct {
   Shared    *shared;
   gchar     *key;
   gchar     *path;
   gint       size;
   guint      serial;   /* the last asked first */
   gint       cancelled;/* atomic: no picture wants it any more */
   gboolean   skipped;  /* not read, being cancelled */
   GdkPixbuf *pixbuf;   /* the result */
} PicJob;


/* Only the pictures on the screen are read (asked when mapped, cancelled
 * when unmapped: the list binds far more rows than it shows), the cards
 * before the rows, in the order asked. */
static gint
pic_job_cmp (gconstpointer a, gconstpointer b, gpointer data)
{
   const PicJob *ja = a, *jb = b;

   if (ja->size != jb->size) return ja->size > jb->size ? -1 : 1;
   return ja->serial < jb->serial ? -1 : ja->serial > jb->serial ? 1 : 0;
}

static void
pic_job_free (PicJob *job)
{
   shared_unref (job->shared);
   g_free (job->key);
   g_free (job->path);
   g_clear_object (&job->pixbuf);
   g_free (job);
}


static void
weak_ref_free (gpointer p)
{
   g_weak_ref_clear (p);
   g_free (p);
}


static gboolean
idle_pic_done (gpointer data)
{
   PicJob *job = data;
   GimvDuplWin *sw = job->shared->sw;
   GdkPaintable *paintable;
   GSList *list, *node;

   if (!sw) goto out;

   if (g_hash_table_lookup (sw->priv->pending, job->key) == job)
      g_hash_table_remove (sw->priv->pending, job->key);
   if (job->skipped) {
      /* cancelled, but wanted again meanwhile: asked again */
      GSList *again = g_hash_table_lookup (sw->priv->waiting, job->key);
      if (again && !g_hash_table_contains (sw->priv->pending, job->key)) {
         PicJob *j = g_new0 (PicJob, 1);
         j->shared = shared_ref (sw->priv->shared);
         j->key    = g_strdup (job->key);
         j->path   = g_strdup (job->path);
         j->size   = job->size;
         j->serial = ++sw->priv->pic_serial;
         g_hash_table_insert (sw->priv->pending, g_strdup (j->key), j);
         g_thread_pool_push (sw->priv->pool, j, NULL);
      }
      goto out;
   }

   if (job->pixbuf)
      paintable = GDK_PAINTABLE (gdk_texture_new_for_pixbuf (job->pixbuf));
   else
      paintable = NULL;   /* not readable: the icon stays */
   if (paintable)
      g_hash_table_insert (sw->priv->textures, g_strdup (job->key), paintable);

   list = g_hash_table_lookup (sw->priv->waiting, job->key);
   g_hash_table_steal (sw->priv->waiting, job->key);
   for (node = list; node; node = g_slist_next (node)) {
      GtkWidget *picture = g_weak_ref_get (node->data);
      if (picture) {
         const gchar *want = g_object_get_data (G_OBJECT (picture), "gimv-dupl-want");
         if (paintable && !g_strcmp0 (want, job->key))
            gtk_picture_set_paintable (GTK_PICTURE (picture), paintable);
         g_object_unref (picture);
      }
   }
   g_slist_free_full (list, weak_ref_free);

out:
   pic_job_free (job);
   return G_SOURCE_REMOVE;
}


static void
pic_thread (gpointer data, gpointer user_data)
{
   PicJob *job = data;

   if (g_atomic_int_get (&job->cancelled)) {
      job->skipped = TRUE;
   } else if (job->shared->sw) {
      gint w = 0, h = 0;
      GdkPixbuf *px;

      /* a small picture as it is, not blown up */
      if (gdk_pixbuf_get_file_info (job->path, &w, &h) && w > 0 && h > 0
          && w <= job->size && h <= job->size)
         px = gdk_pixbuf_new_from_file (job->path, NULL);
      else
         px = gdk_pixbuf_new_from_file_at_scale (job->path, job->size,
                                                 job->size, TRUE, NULL);
      if (px) {
         job->pixbuf = gdk_pixbuf_apply_embedded_orientation (px);
         g_object_unref (px);
      }
   }
   g_idle_add (idle_pic_done, job);
}


/* picture (NULL: the dead ones only) wants key no more; nobody left: its
 * job is cancelled */
static void
drop_waiter (GimvDuplWin *sw, const gchar *key, GtkWidget *picture)
{
   GSList *list, *node, *keep = NULL;
   gpointer old_key;
   PicJob *job;

   /* out of the table (its list is freed here) */
   if (!g_hash_table_steal_extended (sw->priv->waiting, key, &old_key, (gpointer *) &list))
      return;
   for (node = list; node; node = g_slist_next (node)) {
      GtkWidget *w = g_weak_ref_get (node->data);
      if (w && w != picture)
         keep = g_slist_prepend (keep, node->data);
      else
         weak_ref_free (node->data);
      if (w) g_object_unref (w);
   }
   g_slist_free (list);
   if (keep) {
      g_hash_table_insert (sw->priv->waiting, old_key, keep);
      return;
   }
   job = g_hash_table_lookup (sw->priv->pending, old_key);
   if (job) g_atomic_int_set (&job->cancelled, 1);
   g_free (old_key);
}


/* the cards of a group shown before are gone: their pictures not read */
static void
drop_dead_waiters (GimvDuplWin *sw)
{
   GPtrArray *keys = g_ptr_array_new_with_free_func (g_free);
   GHashTableIter it;
   gpointer key;
   guint i;

   g_hash_table_iter_init (&it, sw->priv->waiting);
   while (g_hash_table_iter_next (&it, &key, NULL))
      g_ptr_array_add (keys, g_strdup (key));
   for (i = 0; i < keys->len; i++)
      drop_waiter (sw, g_ptr_array_index (keys, i), NULL);
   g_ptr_array_unref (keys);
}


/* the picture wanted by picture read (in the queue) */
static void
request_picture (GimvDuplWin *sw, GtkWidget *picture)
{
   const gchar *want = g_object_get_data (G_OBJECT (picture), "gimv-dupl-want");
   const gchar *path = g_object_get_data (G_OBJECT (picture), "gimv-dupl-path");
   gint size = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (picture), "gimv-dupl-size"));
   gchar *key;
   GSList *list, *node;
   GWeakRef *ref;
   PicJob *job;

   if (!want || !path || g_hash_table_contains (sw->priv->textures, want)) return;
   key = g_strdup (want);

   /* asked by this picture already */
   list = g_hash_table_lookup (sw->priv->waiting, key);
   for (node = list; node; node = g_slist_next (node)) {
      GtkWidget *w = g_weak_ref_get (node->data);
      if (w) g_object_unref (w);
      if (w == picture) { g_free (key); return; }
   }

   ref = g_new0 (GWeakRef, 1);
   g_weak_ref_init (ref, picture);
   if (g_hash_table_lookup_extended (sw->priv->waiting, key, NULL, (gpointer *) &list)) {
      list = g_slist_prepend (list, ref);
      g_hash_table_steal (sw->priv->waiting, key);
      g_hash_table_insert (sw->priv->waiting, key, list);
      return;   /* asked already */
   }
   g_hash_table_insert (sw->priv->waiting, g_strdup (key), g_slist_prepend (NULL, ref));

   job = g_hash_table_lookup (sw->priv->pending, key);
   if (job && !g_atomic_int_get (&job->cancelled)) {
      g_free (key);
      return;   /* being read */
   }
   if (job) {
      /* cancelled but maybe not taken yet: wanted again (else read again
       * when it comes back skipped, see idle_pic_done) */
      g_atomic_int_set (&job->cancelled, 0);
      g_free (key);
      return;
   }
   job = g_new0 (PicJob, 1);
   job->shared = shared_ref (sw->priv->shared);
   job->key    = key;
   job->path   = g_strdup (path);
   job->size   = size;
   job->serial = ++sw->priv->pic_serial;
   g_hash_table_insert (sw->priv->pending, g_strdup (key), job);
   g_thread_pool_push (sw->priv->pool, job, NULL);
}


static void
cb_picture_map (GtkWidget *picture, GimvDuplWin *sw)
{
   if (sw->priv) request_picture (sw, picture);
}


static void
cb_picture_unmap (GtkWidget *picture, GimvDuplWin *sw)
{
   const gchar *want = g_object_get_data (G_OBJECT (picture), "gimv-dupl-want");

   /* scrolled away (or the window closing): not read if not begun */
   if (sw->priv && want) drop_waiter (sw, want, picture);
}


/* the picture of path in the size (a box), when it is read */
static void
set_picture (GimvDuplWin *sw, GtkWidget *picture, const gchar *path, gint size)
{
   gchar *key = g_strdup_printf ("%d:%s", size, path);
   GdkPaintable *paintable = g_hash_table_lookup (sw->priv->textures, key);
   const gchar *old = g_object_get_data (G_OBJECT (picture), "gimv-dupl-want");

   /* a row of the list used again for another group */
   if (old && strcmp (old, key))
      drop_waiter (sw, old, picture);
   g_object_set_data_full (G_OBJECT (picture), "gimv-dupl-want", key, g_free);
   g_object_set_data_full (G_OBJECT (picture), "gimv-dupl-path", g_strdup (path), g_free);
   g_object_set_data (G_OBJECT (picture), "gimv-dupl-size", GINT_TO_POINTER (size));
   if (!g_object_get_data (G_OBJECT (picture), "gimv-dupl-hooked")) {
      g_object_set_data (G_OBJECT (picture), "gimv-dupl-hooked", GINT_TO_POINTER (1));
      g_signal_connect (picture, "map", G_CALLBACK (cb_picture_map), sw);
      g_signal_connect (picture, "unmap", G_CALLBACK (cb_picture_unmap), sw);
   }
   if (paintable) {
      gtk_picture_set_paintable (GTK_PICTURE (picture), paintable);
      return;
   }

   /* meanwhile an icon */
   {
      GtkIconTheme *theme = gtk_icon_theme_get_for_display (gtk_widget_get_display (picture));
      GtkIconPaintable *icon = gtk_icon_theme_lookup_icon (theme, "image-x-generic",
                                                           NULL, MIN (size, 64), 1,
                                                           GTK_TEXT_DIR_NONE, 0);
      gtk_picture_set_paintable (GTK_PICTURE (picture), GDK_PAINTABLE (icon));
      g_object_unref (icon);
   }
   if (gtk_widget_get_mapped (picture))
      request_picture (sw, picture);
}


/******************************************************************************
 *   opening an image of the thumbnail view
 ******************************************************************************/

static GimvThumb *
thumb_of_path (GimvDuplWin *sw, const gchar *path)
{
   GList *node;

   if (!sw->tv) return NULL;
   for (node = sw->tv->thumblist; node; node = g_list_next (node)) {
      GimvThumb *thumb = node->data;
      if (thumb && thumb->info
          && !g_strcmp0 (gimv_image_info_get_path (thumb->info), path))
         return thumb;
   }
   return NULL;
}


static void
open_path (GimvDuplWin *sw, const gchar *path)
{
   GimvThumb *thumb = thumb_of_path (sw, path);

   if (!thumb) {
      /* from the index (or the tab is gone): in an image window */
      GimvImageInfo *info = gimv_image_info_get (path);
      if (info) {
         gimv_image_win_open_window_auto (info);
         gimv_image_info_unref (info);
      }
      return;
   }
   if (conf.simwin_sel_thumbview) {
      gimv_thumb_view_set_selection_all (sw->tv, FALSE);
      gimv_thumb_view_set_selection (thumb, TRUE);
      gimv_thumb_view_adjust (sw->tv, thumb);
   }
   if (conf.simwin_sel_preview)
      gimv_thumb_view_open_image (sw->tv, thumb, GIMV_THUMB_VIEW_OPEN_IMAGE_PREVIEW);
   if (conf.simwin_sel_new_win)
      gimv_thumb_view_open_image (sw->tv, thumb, GIMV_THUMB_VIEW_OPEN_IMAGE_NEW_WIN);
   if (conf.simwin_sel_shared_win)
      gimv_thumb_view_open_image (sw->tv, thumb, GIMV_THUMB_VIEW_OPEN_IMAGE_SHARED_WIN);
}


/******************************************************************************
 *   the summary, the group list
 ******************************************************************************/

static void
update_summary (GimvDuplWin *sw)
{
   GimvDuplWinPriv *priv = sw->priv;
   guint n = 0, n_keep = 0;
   goffset bytes = 0;
   gchar *size, *text;
   gboolean shown = priv->groups && priv->current >= 0
                    && priv->current < (gint) priv->groups->len;

   /* the group shown only: it is moved to the trash a group at a time */
   if (shown)
      group_counts (sw, g_ptr_array_index (priv->groups, priv->current),
                    &n, &bytes, &n_keep);

   size = g_format_size (bytes);
   text = g_strdup_printf (_("To delete: %u images (%s)"), n, size);
   gtk_label_set_text (GTK_LABEL (priv->summary_label), text);
   gtk_widget_set_visible (priv->summary_label, shown);
   gtk_widget_set_visible (priv->trash_button, shown);
   gtk_widget_set_sensitive (priv->trash_button, n > 0 && !priv->trashing);
   if (n > 0 && n_keep == 0) {
      gtk_widget_add_css_class (priv->summary_label, "error");
      gtk_widget_set_tooltip_text (priv->summary_label,
                                   _("Every image of this group is marked to delete."));
   } else {
      gtk_widget_remove_css_class (priv->summary_label, "error");
      gtk_widget_set_tooltip_text (priv->summary_label, NULL);
   }
   g_free (text);
   g_free (size);

   /* every group at once (many groups: from the index) */
   {
      guint i, all_del = 0, groups = 0;
      goffset all_bytes = 0;

      for (i = 0; priv->groups && i < priv->groups->len; i++) {
         GimvDuplGroup *g = g_ptr_array_index (priv->groups, i);
         guint d, k;
         goffset b;
         if (n_left (sw, g) < 2) continue;
         groups++;
         group_counts (sw, g, &d, &b, &k);
         /* a group with nothing kept is left to be done by itself */
         if (d > 0 && k > 0) { all_del += d; all_bytes += b; }
      }
      gtk_widget_set_visible (priv->trash_all_button, groups > 1);
      gtk_widget_set_sensitive (priv->trash_all_button,
                                all_del > 0 && !priv->trashing && !priv->running);
      size = g_format_size (all_bytes);
      text = g_strdup_printf (_("Moves the images marked Delete of all the %u groups "
                                "to the trash: %u images (%s)"), groups, all_del, size);
      gtk_widget_set_tooltip_text (priv->trash_all_button, text);
      g_free (text);
      g_free (size);
   }
}


static GtkWidget *
group_row_setup (void)
{
   GtkWidget *box, *picture, *vbox, *label;

   box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
   gtk_widget_set_margin_top (box, 3);
   gtk_widget_set_margin_bottom (box, 3);
   gtk_widget_set_margin_start (box, 4);
   gtk_widget_set_margin_end (box, 4);

   picture = gtk_picture_new ();
   gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_SCALE_DOWN);
   gtk_widget_set_size_request (picture, ROW_ICON_SIZE, ROW_ICON_SIZE);
   gtk_box_append (GTK_BOX (box), picture);

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
   gtk_widget_set_valign (vbox, GTK_ALIGN_CENTER);
   gtk_widget_set_hexpand (vbox, TRUE);
   gtk_box_append (GTK_BOX (box), vbox);

   label = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_box_append (GTK_BOX (vbox), label);

   label = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
   gtk_label_set_width_chars (GTK_LABEL (label), 18);
   gtk_widget_add_css_class (label, "dim-label");
   gtk_box_append (GTK_BOX (vbox), label);

   label = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_widget_add_css_class (label, "dim-label");
   gtk_box_append (GTK_BOX (vbox), label);

   return box;
}


static void
cb_row_setup (GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
   gtk_list_item_set_child (item, group_row_setup ());
}


static void
group_row_fill (GimvDuplWin *sw, GtkWidget *box, gint index)
{
   GimvDuplGroup *group = g_ptr_array_index (sw->priv->groups, index);
   GtkWidget *picture = gtk_widget_get_first_child (box);
   GtkWidget *vbox = gtk_widget_get_next_sibling (picture);
   GtkWidget *l1 = gtk_widget_get_first_child (vbox);
   GtkWidget *l2 = gtk_widget_get_next_sibling (l1);
   GtkWidget *l3 = gtk_widget_get_next_sibling (l2);
   GimvDuplItem *first = NULL;
   guint i, n_del, n_keep;
   goffset bytes;
   gchar *markup, *name, *text;

   for (i = 0; i < group->members->len && !first; i++)
      if (!is_removed (sw, g_ptr_array_index (group->members, i)))
         first = g_ptr_array_index (group->members, i);
   if (!first) first = g_ptr_array_index (group->members, 0);

   set_picture (sw, picture, first->path, ROW_ICON_SIZE);

   markup = g_markup_printf_escaped (_("<b>%s</b>  %u images"),
                                     kind_label (group->kind), n_left (sw, group));
   gtk_label_set_markup (GTK_LABEL (l1), markup);
   g_free (markup);

   name = g_filename_display_basename (first->path);
   gtk_label_set_text (GTK_LABEL (l2), name);
   g_free (name);

   group_counts (sw, group, &n_del, &bytes, &n_keep);
   if (n_del > 0 && n_keep == 0) {
      text = g_strdup (_("All to delete!"));
   } else if (n_del > 0) {
      gchar *size = g_format_size (bytes);
      text = g_strdup_printf (_("Delete %u (%s)"), n_del, size);
      g_free (size);
   } else {
      text = g_strdup (_("Keep all"));
   }
   gtk_label_set_text (GTK_LABEL (l3), text);
   g_free (text);
}


static void
cb_row_bind (GtkSignalListItemFactory *f, GtkListItem *item, GimvDuplWin *sw)
{
   GimvDuplEntry *entry = gtk_list_item_get_item (item);
   GtkWidget *box = gtk_list_item_get_child (item);

   g_object_set_data (G_OBJECT (entry), "gimv-dupl-row", box);
   group_row_fill (sw, box, entry->index);
}


static void
cb_row_unbind (GtkSignalListItemFactory *f, GtkListItem *item, GimvDuplWin *sw)
{
   GimvDuplEntry *entry = gtk_list_item_get_item (item);

   if (entry && g_object_get_data (G_OBJECT (entry), "gimv-dupl-row")
                == gtk_list_item_get_child (item))
      g_object_set_data (G_OBJECT (entry), "gimv-dupl-row", NULL);
}


/* the rows shown show the counts again (the selection stays) */
static void
refresh_rows (GimvDuplWin *sw)
{
   GListModel *model = G_LIST_MODEL (sw->priv->group_store);
   guint i, n = g_list_model_get_n_items (model);

   for (i = 0; i < n; i++) {
      GimvDuplEntry *entry = g_list_model_get_item (model, i);
      GtkWidget *box = g_object_get_data (G_OBJECT (entry), "gimv-dupl-row");
      if (box) group_row_fill (sw, box, entry->index);
      g_object_unref (entry);
   }
}


/******************************************************************************
 *   the cards of the group shown
 ******************************************************************************/

static void show_group (GimvDuplWin *sw, gint index);


typedef struct {
   GimvDuplWin *sw;
   gchar       *path;
} CardData;

static void
card_data_free (gpointer p)
{
   CardData *cd = p;
   g_free (cd->path);
   g_free (cd);
}


static void
set_mark (GimvDuplWin *sw, const gchar *path, gint mark)
{
   g_hash_table_insert (sw->priv->marks, g_strdup (path), GINT_TO_POINTER (mark));
}


static void
card_update (GtkWidget *card, gboolean del)
{
   GtkWidget *picture = g_object_get_data (G_OBJECT (card), "gimv-dupl-picture");
   GtkWidget *keep = g_object_get_data (G_OBJECT (card), "gimv-dupl-keep");
   GtkWidget *delete = g_object_get_data (G_OBJECT (card), "gimv-dupl-delete");

   gtk_widget_set_opacity (picture, del ? 0.4 : 1.0);
   if (del) {
      gtk_widget_add_css_class (card, "gimv-dupl-delete");
      gtk_widget_add_css_class (delete, "destructive-action");
      gtk_widget_remove_css_class (keep, "suggested-action");
   } else {
      gtk_widget_remove_css_class (card, "gimv-dupl-delete");
      gtk_widget_remove_css_class (delete, "destructive-action");
      gtk_widget_add_css_class (keep, "suggested-action");
   }
}


static void
cb_mark_toggled (GtkToggleButton *button, gpointer data)
{
   CardData *cd = data;
   GtkWidget *card = g_object_get_data (G_OBJECT (button), "gimv-dupl-card");
   gboolean del = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "gimv-dupl-del"));

   if (!gtk_toggle_button_get_active (button)) return;
   set_mark (cd->sw, cd->path, del ? MARK_DELETE : MARK_KEEP);
   card_update (card, del);
   refresh_rows (cd->sw);
   update_summary (cd->sw);
}


static void
cb_picture_pressed (GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                    gpointer data)
{
   CardData *cd = data;
   open_path (cd->sw, cd->path);
}


static gboolean
has_comment (const gchar *path)
{
   gchar *cpath = gimv_comment_get_path (path);
   gboolean ret = cpath && g_file_test (cpath, G_FILE_TEST_EXISTS);
   g_free (cpath);
   return ret;
}


static gchar *
date_text (GimvDuplItem *item)
{
   const gchar *d = NULL;
   gchar buf[32];

#ifdef ENABLE_INDEX
   d = gimv_index_date_of (item->path);
#endif
   if (!d) {
      time_t t = item->mtime;
      struct tm tm;
      if (localtime_r (&t, &tm) && strftime (buf, sizeof buf, "%Y-%m-%d %H:%M", &tm) > 0)
         d = buf;
   }
   if (!d) return g_strdup ("");
   /* "YYYY-MM-DD HH:MM[:SS]" -> "YYYY/MM/DD HH:MM" */
   {
      gchar *s = g_strndup (d, 16), *p;
      for (p = s; *p && p < s + 10; p++) if (*p == '-') *p = '/';
      return s;
   }
}


static GtkWidget *
card_label (GtkWidget *box, const gchar *text, gboolean dim)
{
   GtkWidget *label = gtk_label_new (text);

   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
   gtk_label_set_max_width_chars (GTK_LABEL (label), 26);
   if (dim) gtk_widget_add_css_class (label, "dim-label");
   gtk_box_append (GTK_BOX (box), label);
   return label;
}


static GtkWidget *
card_new (GimvDuplWin *sw, GimvDuplGroup *group, guint i)
{
   GimvDuplItem *item = g_ptr_array_index (group->members, i);
   gfloat sim = g_array_index (group->sim, gfloat, i);
   GtkWidget *frame, *box, *picture, *label, *hbox, *keep, *delete;
   GtkGesture *click;
   CardData *cd;
   gchar *text, *name, *dir, *size;
   gboolean del = marked_delete (sw, group, i);

   frame = gtk_frame_new (NULL);
   box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);
   gtk_widget_set_margin_top (box, 6);
   gtk_widget_set_margin_bottom (box, 6);
   gtk_widget_set_margin_start (box, 6);
   gtk_widget_set_margin_end (box, 6);
   gtk_frame_set_child (GTK_FRAME (frame), box);

   cd = g_new0 (CardData, 1);
   cd->sw = sw;
   cd->path = g_strdup (item->path);
   g_object_set_data_full (G_OBJECT (frame), "gimv-dupl-card-data", cd, card_data_free);

   picture = gtk_picture_new ();
   gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_SCALE_DOWN);
   gtk_widget_set_size_request (picture, CARD_PIC_SIZE, CARD_PIC_SIZE);
   gtk_widget_set_tooltip_text (picture, sw->tv ? _("Click to show it in the thumbnail window")
                                                : _("Click to open it"));
   click = gtk_gesture_click_new ();
   g_signal_connect (click, "pressed", G_CALLBACK (cb_picture_pressed), cd);
   gtk_widget_add_controller (picture, GTK_EVENT_CONTROLLER (click));
   gtk_box_append (GTK_BOX (box), picture);
   set_picture (sw, picture, item->path, CARD_PIC_SIZE);
   g_object_set_data (G_OBJECT (frame), "gimv-dupl-picture", picture);

   /* the size of the picture (known at the end) and of the file */
   if (sw->priv->final && item->width > 0)
      text = g_strdup_printf ("<b>%d × %d</b>", item->width, item->height);
   else
      text = sw->priv->from_index ? NULL : g_strdup ("<b>—</b>");
   if (text) {
      label = card_label (box, NULL, FALSE);
      gtk_label_set_markup (GTK_LABEL (label), text);
      g_free (text);
   }

   size = g_format_size (item->size);
   {
      const gchar *ext = strrchr (item->path, '.');
      gchar *up = ext && !strchr (ext, '/') ? g_ascii_strup (ext + 1, -1) : g_strdup ("");
      text = *up ? g_strdup_printf ("%s · %s", size, up) : g_strdup (size);
      g_free (up);
   }
   card_label (box, text, FALSE);
   g_free (text);
   g_free (size);

   text = date_text (item);
   card_label (box, text, FALSE);
   g_free (text);

   if (item->enlarged) {
      label = card_label (box, _("Looks like an enlarged copy"), FALSE);
      gtk_widget_add_css_class (label, "accent");
      gtk_widget_set_tooltip_text (label, _("It has no more detail than a smaller one "
                                            "of the group: only made larger"));
   }

   if (i == 0)
      text = g_strdup (group->suggested ? _("Suggested to keep") : "");
   else if (sim >= 1.0)
      text = g_strdup (_("The same file as the first"));
   else
      text = g_strdup_printf (_("%.1f%% alike the first"), sim * 100.0);
   if (*text) card_label (box, text, TRUE);
   g_free (text);

   name = g_filename_display_basename (item->path);
   label = card_label (box, name, FALSE);
   gtk_widget_set_tooltip_text (label, item->path);
   g_free (name);
   {
      gchar *d = g_path_get_dirname (item->path);
      dir = g_filename_display_name (d);
      g_free (d);
   }
   label = card_label (box, dir, TRUE);
   gtk_widget_set_tooltip_text (label, item->path);
   g_free (dir);

   if (has_comment (item->path)) {
      label = card_label (box, _("Has a comment"), FALSE);
      gtk_widget_add_css_class (label, "accent");
   }

   /* keep or delete */
   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
   gtk_widget_add_css_class (hbox, "linked");
   gtk_widget_set_margin_top (hbox, 4);
   gtk_widget_set_halign (hbox, GTK_ALIGN_CENTER);
   keep = gtk_toggle_button_new_with_mnemonic (_("Keep"));
   delete = gtk_toggle_button_new_with_mnemonic (_("Delete"));
   gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (delete), GTK_TOGGLE_BUTTON (keep));
   gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (del ? delete : keep), TRUE);
   g_object_set_data (G_OBJECT (keep), "gimv-dupl-card", frame);
   g_object_set_data (G_OBJECT (delete), "gimv-dupl-card", frame);
   g_object_set_data (G_OBJECT (delete), "gimv-dupl-del", GINT_TO_POINTER (1));
   g_signal_connect (keep, "toggled", G_CALLBACK (cb_mark_toggled), cd);
   g_signal_connect (delete, "toggled", G_CALLBACK (cb_mark_toggled), cd);
   gtk_box_append (GTK_BOX (hbox), keep);
   gtk_box_append (GTK_BOX (hbox), delete);
   gtk_box_append (GTK_BOX (box), hbox);
   g_object_set_data (G_OBJECT (frame), "gimv-dupl-keep", keep);
   g_object_set_data (G_OBJECT (frame), "gimv-dupl-delete", delete);

   card_update (frame, del);
   return frame;
}


static void
show_group (GimvDuplWin *sw, gint index)
{
   GimvDuplWinPriv *priv = sw->priv;
   GtkWidget *child;
   GimvDuplGroup *group;
   guint i;

   while ((child = gtk_widget_get_first_child (priv->cards)))
      gtk_flow_box_remove (GTK_FLOW_BOX (priv->cards), child);
   drop_dead_waiters (sw);

   priv->current = index;
   gtk_widget_set_visible (priv->more_label, FALSE);
   if (!priv->groups || index < 0 || index >= (gint) priv->groups->len) {
      gtk_label_set_text (GTK_LABEL (priv->head_label), "");
      gtk_label_set_text (GTK_LABEL (priv->kind_label), "");
      update_summary (sw);
      return;
   }

   group = g_ptr_array_index (priv->groups, index);
   {
      gchar *markup = g_markup_printf_escaped (_("<b>%s</b>  %u images"),
                                               kind_label (group->kind),
                                               n_left (sw, group));
      gtk_label_set_markup (GTK_LABEL (priv->head_label), markup);
      g_free (markup);
   }
   gtk_label_set_text (GTK_LABEL (priv->kind_label), kind_text (group->kind));

   {
      guint shown = 0, left = n_left (sw, group);
      for (i = 0; i < group->members->len && shown < MAX_CARDS; i++) {
         GtkWidget *card;
         if (is_removed (sw, g_ptr_array_index (group->members, i))) continue;
         card = card_new (sw, group, i);
         gtk_flow_box_append (GTK_FLOW_BOX (priv->cards), card);
         shown++;
      }
      if (left > shown) {
         gchar *text = group->suggested
            ? g_strdup_printf (_("and %u more, not shown: to delete"), left - shown)
            : g_strdup_printf (_("and %u more, not shown: to keep"), left - shown);
         gtk_label_set_text (GTK_LABEL (priv->more_label), text);
         g_free (text);
      }
      gtk_widget_set_visible (priv->more_label, left > shown);
   }
   update_summary (sw);
}


static void
cb_group_selected (GtkSelectionModel *model, guint position, guint n_items,
                   GimvDuplWin *sw)
{
   GimvDuplEntry *entry
      = gtk_single_selection_get_selected_item (GTK_SINGLE_SELECTION (model));

   show_group (sw, entry ? entry->index : -1);
}


/* the groups of priv->groups into the list (the ones with two left);
 * the selection stays on the group of the path shown before */
static void
fill_groups (GimvDuplWin *sw)
{
   GimvDuplWinPriv *priv = sw->priv;
   GtkSelectionModel *sel = gtk_list_view_get_model (GTK_LIST_VIEW (priv->group_view));
   gchar *was = NULL;
   guint i, select = GTK_INVALID_LIST_POSITION, n = 0;

   if (priv->current >= 0 && priv->groups && priv->current < (gint) priv->groups->len) {
      GimvDuplGroup *g = g_ptr_array_index (priv->groups, priv->current);
      was = g_strdup (((GimvDuplItem *) g_ptr_array_index (g->members, 0))->path);
   }

   g_signal_handlers_block_by_func (sel, cb_group_selected, sw);
   g_list_store_remove_all (priv->group_store);
   for (i = 0; priv->groups && i < priv->groups->len; i++) {
      GimvDuplGroup *group = g_ptr_array_index (priv->groups, i);
      GimvDuplEntry *entry;
      guint k;

      if (n_left (sw, group) < 2) continue;
      entry = entry_new (i);
      g_list_store_append (priv->group_store, entry);
      g_object_unref (entry);
      if (was && select == GTK_INVALID_LIST_POSITION)
         for (k = 0; k < group->members->len; k++)
            if (!strcmp (((GimvDuplItem *) g_ptr_array_index (group->members, k))->path, was))
               select = n;
      n++;
   }
   g_free (was);
   if (n > 0 && select == GTK_INVALID_LIST_POSITION) select = 0;
   gtk_single_selection_set_selected (GTK_SINGLE_SELECTION (sel), select);
   g_signal_handlers_unblock_by_func (sel, cb_group_selected, sw);

   gtk_label_set_text (GTK_LABEL (priv->empty_label),
                       g_hash_table_size (priv->removed) > 0
                       ? _("No groups are left.") : _("No duplicates were found."));
   gtk_widget_set_visible (priv->empty_label, n == 0 && !priv->running);
   cb_group_selected (sel, 0, 0, sw);
   update_summary (sw);
}


static void
set_groups (GimvDuplWin *sw, GPtrArray *groups, gboolean owned)
{
   GimvDuplWinPriv *priv = sw->priv;

   if (priv->groups && priv->groups_owned) g_ptr_array_unref (priv->groups);
   priv->groups = groups;
   priv->groups_owned = owned;
   fill_groups (sw);
}


/******************************************************************************
 *   the scan
 ******************************************************************************/

typedef struct {
   Shared        *shared;
   GimvDuplPhase  phase;
   guint          done, total;
   GPtrArray     *same;      /* the groups of the same files, or NULL */
} ProgressMsg;


static gboolean
idle_progress (gpointer data)
{
   ProgressMsg *msg = data;
   GimvDuplWin *sw = msg->shared->sw;

   if (sw && sw->priv->running) {
      GimvDuplWinPriv *priv = sw->priv;
      gchar *text = NULL;

      switch (msg->phase) {
      case GIMV_DUPL_PHASE_SAME:
         text = g_strdup_printf (_("Comparing the files of the same size (%u/%u)"),
                                 msg->done, msg->total);
         break;
      case GIMV_DUPL_PHASE_READ:
         text = g_strdup_printf (_("Reading the images (%u/%u)"), msg->done, msg->total);
         break;
      case GIMV_DUPL_PHASE_COMPARE:
         text = g_strdup_printf (_("Comparing the images (%u/%u)"), msg->done, msg->total);
         break;
      default:
         break;
      }
      if (text) {
         gtk_progress_bar_set_text (GTK_PROGRESS_BAR (priv->progress), text);
         gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (priv->progress),
                                        msg->total ? (gdouble) msg->done / msg->total : 0.0);
         g_free (text);
      }
      if (msg->same && !priv->final) {
         set_groups (sw, msg->same, TRUE);
         msg->same = NULL;
      }
   }
   if (msg->same) g_ptr_array_unref (msg->same);
   shared_unref (msg->shared);
   g_free (msg);
   return G_SOURCE_REMOVE;
}


typedef struct {
   Shared        *shared;
   GimvDuplScan  *scan;

   /* from the index */
   gboolean       from_index;
   gchar         *dir;        /* the groups with an image below it ("/.../"),
                                 NULL: all */
   GHashTable    *only;       /* the groups of these paths, NULL: all */
   gchar         *error;
   gint64         n_hashes;   /* files hashed by the index */
} ScanJob;


#ifdef ENABLE_INDEX

typedef struct {
   gchar  *path;
   gint64  size, mtime;
} IndexMember;

static void
index_member_clear (gpointer p)
{
   g_free (((IndexMember *) p)->path);
}


/* a group of the index: its files still there, each file once (a hard
 * link is the same file), into the scan */
static void
index_group_add (ScanJob *job, GArray *members, const gchar *key)
{
   GArray *ok;
   GHashTable *inodes;
   gboolean wanted = !job->dir && !job->only;
   guint i;

   if (members->len < 2) return;
   for (i = 0; i < members->len && !wanted; i++) {
      IndexMember *m = &g_array_index (members, IndexMember, i);
      if (job->dir && g_str_has_prefix (m->path, job->dir)) wanted = TRUE;
      if (job->only && g_hash_table_contains (job->only, m->path)) wanted = TRUE;
   }
   if (!wanted) return;

   ok = g_array_new (FALSE, FALSE, sizeof (guint));
   inodes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
   for (i = 0; i < members->len; i++) {
      IndexMember *m = &g_array_index (members, IndexMember, i);
      GStatBuf st;
      gchar *ino;

      /* gone, or changed since the index read it */
      if (g_stat (m->path, &st) != 0 || !S_ISREG (st.st_mode) || st.st_size != m->size)
         continue;
      ino = g_strdup_printf ("%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT,
                             (guint64) st.st_dev, (guint64) st.st_ino);
      if (g_hash_table_contains (inodes, ino)) { g_free (ino); continue; }
      g_hash_table_add (inodes, ino);
      m->mtime = st.st_mtime;
      g_array_append_val (ok, i);
   }
   if (ok->len >= 2) {
      for (i = 0; i < ok->len; i++) {
         IndexMember *m = &g_array_index (members, IndexMember, g_array_index (ok, guint, i));
         GimvDuplItem *item = gimv_dupl_scan_add (job->scan, m->path, m->size, m->mtime, NULL);
         item->key = g_strdup (key);
      }
   }
   g_hash_table_destroy (inodes);
   g_array_unref (ok);
}


/* the groups of the same files the index has found */
static gboolean
index_fill (ScanJob *job, GCancellable *cancel)
{
   gchar *dbfile = gimv_index_db_default_path ();
   sqlite3 *db = NULL;
   sqlite3_stmt *st = NULL;
   GArray *members;
   gchar *key = NULL;

   if (gimv_index_db_open (dbfile, FALSE, &db, NULL) != GIMV_INDEX_DB_OK || !db) {
      g_free (dbfile);
      job->error = g_strdup (_("The index cannot be read (it is not made yet?)."));
      return FALSE;
   }
   g_free (dbfile);

   if (sqlite3_prepare_v2 (db, "SELECT count(*) FROM hashes", -1, &st, NULL) == SQLITE_OK
       && sqlite3_step (st) == SQLITE_ROW)
      job->n_hashes = sqlite3_column_int64 (st, 0);
   sqlite3_finalize (st);
   st = NULL;

   if (sqlite3_prepare_v2 (db,
          "SELECT hex (h.whole), d.path, f.name, f.size FROM hashes h "
          "JOIN files f ON f.id = h.file_id JOIN dirs d ON d.id = f.dir_id "
          "WHERE length (h.whole) = 20 AND h.size = f.size AND h.mtime = f.mtime "
          "AND h.whole IN (SELECT whole FROM hashes WHERE length (whole) = 20 "
          "                GROUP BY whole HAVING count (*) > 1) "
          "ORDER BY h.whole", -1, &st, NULL) != SQLITE_OK) {
      job->error = g_strdup (sqlite3_errmsg (db));
      sqlite3_close (db);
      return FALSE;
   }

   members = g_array_new (FALSE, TRUE, sizeof (IndexMember));
   g_array_set_clear_func (members, index_member_clear);
   while (sqlite3_step (st) == SQLITE_ROW && !g_cancellable_is_cancelled (cancel)) {
      const gchar *k = (const gchar *) sqlite3_column_text (st, 0);
      IndexMember m;

      if (key && strcmp (key, k)) {
         index_group_add (job, members, key);
         g_array_set_size (members, 0);
      }
      if (!key || strcmp (key, k)) {
         g_free (key);
         key = g_strdup (k);
      }
      m.path  = g_build_filename ((const gchar *) sqlite3_column_text (st, 1),
                                  (const gchar *) sqlite3_column_text (st, 2), NULL);
      m.size  = sqlite3_column_int64 (st, 3);
      m.mtime = 0;
      g_array_append_val (members, m);
   }
   if (key) index_group_add (job, members, key);
   g_array_unref (members);
   g_free (key);
   sqlite3_finalize (st);
   sqlite3_close (db);
   return !g_cancellable_is_cancelled (cancel);
}

#endif /* ENABLE_INDEX */


static void
scan_progress (GimvDuplPhase phase, guint done, guint total, gpointer data)
{
   ScanJob *job = data;
   ProgressMsg *msg = g_new0 (ProgressMsg, 1);

   msg->shared = shared_ref (job->shared);
   msg->phase = phase;
   msg->done = done;
   msg->total = total;
   if (phase == GIMV_DUPL_PHASE_READ && done == 0)
      msg->same = gimv_dupl_scan_same_groups (job->scan);
   g_idle_add (idle_progress, msg);
}


static void
scan_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   ScanJob *job = data;
   gboolean ok = TRUE;

#ifdef ENABLE_INDEX
   if (job->from_index)
      ok = index_fill (job, cancel);
#endif
   if (ok)
      ok = gimv_dupl_scan_run (job->scan, cancel, scan_progress, job);
   g_task_return_boolean (task, ok);
}


static void
scan_job_free (gpointer data)
{
   ScanJob *job = data;
   /* the window gone: the scan is nobody's */
   if (!job->shared->sw) gimv_dupl_scan_free (job->scan);
   shared_unref (job->shared);
   g_free (job->dir);
   if (job->only) g_hash_table_destroy (job->only);
   g_free (job->error);
   g_free (job);
}


static void enlarged_start (GimvDuplWin *sw);

static void
scan_done (GObject *source, GAsyncResult *res, gpointer data)
{
   ScanJob *job = g_task_get_task_data (G_TASK (res));
   GimvDuplWin *sw = job->shared->sw;
   gboolean ok = g_task_propagate_boolean (G_TASK (res), NULL);
   GimvDuplWinPriv *priv;
   gchar *text;
   guint n;

   if (!sw) return;
   priv = sw->priv;
   priv->running = FALSE;
   priv->final = TRUE;
   gtk_widget_set_sensitive (priv->stop_button, FALSE);
   set_groups (sw, gimv_dupl_scan_get_groups (priv->scan), FALSE);

   n = g_list_model_get_n_items (G_LIST_MODEL (priv->group_store));
   text = ok ? g_strdup_printf (_("Done: %u groups"), n)
             : g_strdup_printf (_("Stopped: %u groups"), n);
#ifdef ENABLE_INDEX
   if (job->from_index) {
      guint done, todo;
      gchar *t = NULL;

      if (job->error) {
         t = g_strdup (job->error);
      } else if (gimv_index_hash_progress (&done, &todo)) {
         t = g_strdup_printf (_("%s: the index is still looking for the same files "
                                "(%d%%); open this again later for the rest"),
                              text, todo ? (gint) ((guint64) done * 100 / todo) : 0);
      } else if (job->n_hashes == 0) {
         t = g_strdup (conf.index_duplicates
                       ? _("The index has not looked for the same files yet.")
                       : _("The index does not look for the same files: turn on "
                           "\"Look for the same files\" in [Preferences | Index]."));
      }
      if (t) {
         g_free (text);
         text = t;
         if (n == 0)
            gtk_label_set_text (GTK_LABEL (priv->empty_label), t);
      }
   }
#endif
   if (priv->skipped > 0) {
      gchar *t = g_strdup_printf (_("%s (%d in archives not compared)"), text, priv->skipped);
      g_free (text);
      text = t;
   }
   gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (priv->progress), ok ? 1.0 : 0.0);
   gtk_progress_bar_set_text (GTK_PROGRESS_BAR (priv->progress), text);
   g_free (priv->done_text);
   priv->done_text = text;
   gtk_widget_set_visible (priv->empty_label, n == 0);
   update_summary (sw);
   if (ok) enlarged_start (sw);
}


/******************************************************************************
 *   enlarged copies: in the groups of other sizes, a larger image that is
 *   only a smaller one made larger is not the one to keep (gimv_enlarged.c).
 *   After the groups are shown, in a thread; each group is ordered again
 *   when it is done (the marks set by hand stay).
 ******************************************************************************/

/* the larger images compared with at most so many smaller ones */
#define ENLARGED_TRIES 3

typedef struct {
   gint       index;           /* in priv->groups */
   gchar    **paths;           /* the members, the largest first */
   gint      *w, *h;
   goffset   *size;
   guint      n;
} CheckGroup;

typedef struct {
   Shared      *shared;
   GPtrArray   *groups;        /* CheckGroup * */
} CheckJob;

typedef struct {
   Shared      *shared;
   gint         index;         /* -1: the end */
   GPtrArray   *enlarged;      /* gchar *: the paths found */
   guint        done, total;
   gboolean     ok;
} CheckMsg;


static void
check_group_free (gpointer data)
{
   CheckGroup *g = data;
   g_strfreev (g->paths);
   g_free (g->w);
   g_free (g->h);
   g_free (g->size);
   g_free (g);
}


static void
check_job_free (gpointer data)
{
   CheckJob *job = data;
   shared_unref (job->shared);
   g_ptr_array_unref (job->groups);
   g_free (job);
}


static gboolean
idle_check (gpointer data)
{
   CheckMsg *msg = data;
   GimvDuplWin *sw = msg->shared->sw;

   if (sw && msg->index >= 0 && sw->priv->groups
       && msg->index < (gint) sw->priv->groups->len)
   {
      GimvDuplWinPriv *priv = sw->priv;
      GimvDuplGroup *group = g_ptr_array_index (priv->groups, msg->index);
      gboolean any = FALSE;
      guint i, k;

      for (i = 0; i < group->members->len; i++) {
         GimvDuplItem *item = g_ptr_array_index (group->members, i);
         for (k = 0; k < msg->enlarged->len; k++)
            if (!strcmp (item->path, g_ptr_array_index (msg->enlarged, k))) {
               item->enlarged = any = TRUE;
               break;
            }
      }
      if (any && priv->scan) {
         gimv_dupl_scan_order_group (priv->scan, group);
         refresh_rows (sw);
         if (priv->current == msg->index) show_group (sw, msg->index);
      }
   }
   if (sw) {
      GimvDuplWinPriv *priv = sw->priv;
      if (msg->index < 0) {
         priv->checking = FALSE;
         gtk_widget_set_sensitive (priv->stop_button, FALSE);
         gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (priv->progress), 1.0);
         gtk_progress_bar_set_text (GTK_PROGRESS_BAR (priv->progress), priv->done_text);
      } else if (msg->total > 0) {
         gchar *text = g_strdup_printf (_("Looking for enlarged copies: %u of %u groups"),
                                        msg->done, msg->total);
         gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (priv->progress),
                                        (gdouble) msg->done / msg->total);
         gtk_progress_bar_set_text (GTK_PROGRESS_BAR (priv->progress), text);
         g_free (text);
      }
   }
   shared_unref (msg->shared);
   if (msg->enlarged) g_ptr_array_unref (msg->enlarged);
   g_free (msg);
   return G_SOURCE_REMOVE;
}


static void
check_post (CheckJob *job, gint index, GPtrArray *enlarged, guint done, guint total)
{
   CheckMsg *msg = g_new0 (CheckMsg, 1);
   msg->shared = shared_ref (job->shared);
   msg->index = index;
   msg->enlarged = enlarged;
   msg->done = done;
   msg->total = total;
   g_idle_add (idle_check, msg);
}


static void
check_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   CheckJob *job = data;
   guint gi;

   for (gi = 0; gi < job->groups->len && !g_cancellable_is_cancelled (cancel); gi++) {
      CheckGroup *g = g_ptr_array_index (job->groups, gi);
      GPtrArray *found = g_ptr_array_new_with_free_func (g_free);
      /* the copies of one file (same size in pixels and bytes) once */
      GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      guint j, i;

      for (j = 0; j < g->n && !g_cancellable_is_cancelled (cancel); j++) {
         gchar *key = g_strdup_printf ("%dx%d:%" G_GINT64_FORMAT, g->w[j], g->h[j],
                                       (gint64) g->size[j]);
         gpointer was;
         gboolean enlarged = FALSE;
         gint tries = 0;

         if (g_hash_table_lookup_extended (seen, key, NULL, &was)) {
            enlarged = GPOINTER_TO_INT (was);
            g_free (key);
         } else {
            /* the smaller ones from the largest down (an enlarged copy of
             * a middle one has more than the smallest) */
            for (i = j + 1; i < g->n && tries < ENLARGED_TRIES && !enlarged; i++) {
               gdouble f = MIN ((gdouble) g->w[j] / MAX (g->w[i], 1),
                                (gdouble) g->h[j] / MAX (g->h[i], 1));
               gdouble sc;
               if (f < GIMV_ENLARGED_MIN_FACTOR) continue;
               tries++;
               sc = gimv_enlarged_score_files (g->paths[i], g->paths[j], cancel);
               enlarged = sc >= 0 && sc < GIMV_ENLARGED_LIMIT;
            }
            g_hash_table_insert (seen, key, GINT_TO_POINTER (enlarged));
         }
         if (enlarged) g_ptr_array_add (found, g_strdup (g->paths[j]));
      }
      g_hash_table_destroy (seen);
      check_post (job, g->index, found, gi + 1, job->groups->len);
   }
   check_post (job, -1, NULL, 0, 0);
   g_task_return_boolean (task, TRUE);
}


static gint
cmp_pixels_desc (gconstpointer pa, gconstpointer pb)
{
   const GimvDuplItem *a = *(GimvDuplItem * const *) pa;
   const GimvDuplItem *b = *(GimvDuplItem * const *) pb;
   gint64 na = (gint64) a->width * a->height, nb = (gint64) b->width * b->height;
   return na == nb ? 0 : (na > nb ? -1 : 1);
}


static void
enlarged_start (GimvDuplWin *sw)
{
   GimvDuplWinPriv *priv = sw->priv;
   CheckJob *job;
   GTask *task;
   guint i, k;

   if (!priv->groups || !priv->scan) return;

   job = g_new0 (CheckJob, 1);
   job->groups = g_ptr_array_new_with_free_func (check_group_free);
   for (i = 0; i < priv->groups->len; i++) {
      GimvDuplGroup *group = g_ptr_array_index (priv->groups, i);
      GPtrArray *m;
      CheckGroup *g;
      GimvDuplItem *big, *small;

      if (group->kind != GIMV_DUPL_RESIZED || !group->suggested) continue;
      m = g_ptr_array_copy (group->members, NULL, NULL);
      g_ptr_array_sort (m, cmp_pixels_desc);
      big = g_ptr_array_index (m, 0);
      small = g_ptr_array_index (m, m->len - 1);
      if (small->width <= 0 || small->height <= 0
          || MIN ((gdouble) big->width / small->width, (gdouble) big->height / small->height)
             < GIMV_ENLARGED_MIN_FACTOR)
      {
         g_ptr_array_unref (m);
         continue;
      }
      g = g_new0 (CheckGroup, 1);
      g->index = i;
      g->n = m->len;
      g->paths = g_new0 (gchar *, m->len + 1);
      g->w = g_new (gint, m->len);
      g->h = g_new (gint, m->len);
      g->size = g_new (goffset, m->len);
      for (k = 0; k < m->len; k++) {
         GimvDuplItem *item = g_ptr_array_index (m, k);
         g->paths[k] = g_strdup (item->path);
         g->w[k] = item->width;
         g->h[k] = item->height;
         g->size[k] = item->size;
      }
      g_ptr_array_add (job->groups, g);
      g_ptr_array_unref (m);
   }
   if (job->groups->len == 0) {
      g_ptr_array_unref (job->groups);
      g_free (job);
      return;
   }

   job->shared = shared_ref (priv->shared);
   g_clear_object (&priv->cancel);
   priv->cancel = g_cancellable_new ();
   priv->checking = TRUE;
   gtk_widget_set_sensitive (priv->stop_button, TRUE);
   task = g_task_new (NULL, priv->cancel, NULL, NULL);
   g_task_set_task_data (task, job, check_job_free);
   g_task_run_in_thread (task, check_thread);
   g_object_unref (task);
}



void
gimv_dupl_win_start (GimvDuplWin *sw, GimvThumbView *tv)
{
   GimvDuplWinPriv *priv;
   GList *node;
   GTask *task;
   ScanJob *job;

   g_return_if_fail (GIMV_IS_DUPL_WIN (sw));
   g_return_if_fail (GIMV_IS_THUMB_VIEW (tv));
   priv = sw->priv;
   g_return_if_fail (!priv->running && !priv->scan);

   priv->scan = gimv_dupl_scan_new (conf.search_similar_accuracy);
   for (node = tv->thumblist; node; node = g_list_next (node)) {
      GimvThumb *thumb = node->data;
      const gchar *path;

      if (!thumb || !thumb->info) continue;
      if (gimv_image_info_is_dir (thumb->info)) continue;
      /* in an archive: no file to compare nor to delete */
      if (gimv_image_info_need_temp_file (thumb->info)) { priv->skipped++; continue; }
      path = gimv_image_info_get_path (thumb->info);
      if (!path || !*path) continue;
      gimv_dupl_scan_add (priv->scan, path, thumb->info->st.st_size,
                          thumb->info->st.st_mtime, NULL);
   }

   priv->running = TRUE;
   priv->cancel = g_cancellable_new ();
   gtk_widget_set_sensitive (priv->stop_button, TRUE);
   gtk_widget_set_visible (priv->empty_label, FALSE);

   job = g_new0 (ScanJob, 1);
   job->shared = shared_ref (priv->shared);
   job->scan = priv->scan;
   task = g_task_new (NULL, priv->cancel, scan_done, NULL);
   g_task_set_task_data (task, job, scan_job_free);
   g_task_run_in_thread (task, scan_thread);
   g_object_unref (task);
}


#ifdef ENABLE_INDEX
/*
 * The same files the index found (stage 4 of the indexer): of all the
 * indexed images (dir NULL), of those below a folder, or of some images
 * (paths: the groups with one of them).  Nothing is read but the index.
 */
void
gimv_dupl_win_start_index (GimvDuplWin *sw, const gchar *dir, GList *paths)
{
   GimvDuplWinPriv *priv;
   GTask *task;
   ScanJob *job;
   GList *node;

   g_return_if_fail (GIMV_IS_DUPL_WIN (sw));
   priv = sw->priv;
   g_return_if_fail (!priv->running && !priv->scan);

   priv->from_index = TRUE;
   priv->scan = gimv_dupl_scan_new (conf.search_similar_accuracy);
   gimv_dupl_scan_set_same_only (priv->scan, TRUE);

   job = g_new0 (ScanJob, 1);
   job->shared = shared_ref (priv->shared);
   job->scan = priv->scan;
   job->from_index = TRUE;
   if (dir) {
      gsize len = strlen (dir);
      job->dir = len > 1 && dir[len - 1] == '/' ? g_strdup (dir)
                                                : g_strconcat (dir, "/", NULL);
   }
   for (node = paths; node; node = g_list_next (node)) {
      if (!job->only)
         job->only = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      g_hash_table_add (job->only, g_strdup (node->data));
   }

   priv->running = TRUE;
   priv->cancel = g_cancellable_new ();
   gtk_widget_set_sensitive (priv->stop_button, TRUE);
   gtk_widget_set_visible (priv->empty_label, FALSE);
   gtk_progress_bar_set_text (GTK_PROGRESS_BAR (priv->progress), _("Reading the index..."));
   gtk_progress_bar_pulse (GTK_PROGRESS_BAR (priv->progress));

   task = g_task_new (NULL, priv->cancel, scan_done, NULL);
   g_task_set_task_data (task, job, scan_job_free);
   g_task_run_in_thread (task, scan_thread);
   g_object_unref (task);
}
#endif


#ifdef ENABLE_INDEX
/* a window of the same files the index found (see above) */
GimvDuplWin *
gimv_dupl_win_open_index (GtkWindow *parent, const gchar *dir, GList *paths)
{
   GimvDuplWin *sw = gimv_dupl_win_new ();
   gchar *title;

   if (dir) {
      gchar *d = g_strdup (dir), *name;
      gsize len = strlen (d);
      if (len > 1 && d[len - 1] == '/') d[len - 1] = '\0';
      name = g_filename_display_name (d);
      g_free (d);
      title = g_strdup_printf (_("Find Duplicates - \"%s\" and the folders in it"), name);
      g_free (name);
   } else if (paths) {
      title = g_strdup (_("Find Duplicates - the same files as the images selected"));
   } else {
      title = g_strdup (_("Find Duplicates - all the indexed images"));
   }
   gtk_window_set_title (GTK_WINDOW (sw), title);
   g_free (title);
   if (parent)
      gtk_window_set_transient_for (GTK_WINDOW (sw), parent);
   gtk_window_present (GTK_WINDOW (sw));
   gimv_dupl_win_start_index (sw, dir, paths);
   return sw;
}
#endif


static void
cb_stop (GtkButton *button, GimvDuplWin *sw)
{
   if (sw->priv->cancel) g_cancellable_cancel (sw->priv->cancel);
}


/******************************************************************************
 *   to the trash
 ******************************************************************************/

typedef struct {
   gchar    *path;      /* to delete */
   gchar    *keep;      /* the image kept of its group (NULL: none) */
   gboolean  verify;    /* keep has the same bytes (checked again first) */
   gboolean  extras;    /* has a comment (tags) or a bookmark */
} TrashItem;

static void
trash_item_clear (gpointer p)
{
   TrashItem *t = p;
   g_free (t->path);
   g_free (t->keep);
}


typedef struct {
   Shared    *shared;
   GArray    *items;      /* TrashItem */
   gboolean   all_groups; /* every group, not the one shown */
   gboolean   move_extras;/* comments, tags and bookmarks to the kept image */
   gboolean   link;       /* a link to the kept image left in the place of
                             each one (the name kept) */
   gint       link_button;/* the dialog's button for it (-1: none) */
   guint      n_links;    /* links made */
   guint      n_groups;
   guint      skipped;    /* groups with nothing kept (left out) */
   gboolean   all_marked; /* the group shown: nothing kept */
   goffset    bytes;
   guint      with_extras;
   GPtrArray *done;       /* TrashItem * moved */
   GString   *errors;
   guint      position;   /* of the group in the list */
} TrashJob;


static void
trash_job_free (gpointer data)
{
   TrashJob *job = data;
   shared_unref (job->shared);
   g_array_unref (job->items);
   if (job->done) g_ptr_array_unref (job->done);
   if (job->errors) g_string_free (job->errors, TRUE);
   g_free (job);
}


/* the same bytes (a file checked again right before it is deleted: it
 * may have changed since it was compared) */
static gboolean
same_bytes (const gchar *a, const gchar *b)
{
   gint fa, fb;
   GStatBuf sa, sb;
   gboolean same = FALSE;

   if (g_stat (a, &sa) != 0 || g_stat (b, &sb) != 0) return FALSE;
   if (sa.st_size != sb.st_size) return FALSE;
   fa = g_open (a, O_RDONLY | O_CLOEXEC, 0);
   fb = g_open (b, O_RDONLY | O_CLOEXEC, 0);
   if (fa >= 0 && fb >= 0) {
      guchar ba[65536], bb[65536];
      same = TRUE;
      for (;;) {
         gssize na = read (fa, ba, sizeof ba), nb;
         if (na < 0 && errno == EINTR) continue;
         if (na < 0) { same = FALSE; break; }
         nb = 0;
         while (nb < na) {
            gssize r = read (fb, bb + nb, na - nb);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            nb += r;
         }
         if (nb != na || memcmp (ba, bb, na)) { same = FALSE; break; }
         if (na == 0) break;
      }
   }
   if (fa >= 0) close (fa);
   if (fb >= 0) close (fb);
   return same;
}


/*
 * The file to the trash and, in its place, a link to the image kept: the
 * name stays (and its comment and bookmark with it).  A hard link on the
 * same file system (the same file, nothing to break), else a symbolic link
 * (an absolute path).  The link is made first under a hidden name, so that
 * nothing is moved when it cannot be made, and renamed over the place once
 * the file is in the trash.
 */
static void
link_and_trash (TrashJob *job, TrashItem *t)
{
   gchar *dir = g_path_get_dirname (t->path);
   gchar *base = g_path_get_basename (t->path);
   gchar *tmp = NULL, *name;
   GFile *file;
   GError *error = NULL;
   gint i;

   for (i = 0; i < 100 && !tmp; i++) {
      gchar *n = g_strdup_printf (".%s.gimv-link-%d", base, i);
      gchar *p = g_build_filename (dir, n, NULL);
      g_free (n);
      if (link (t->keep, p) == 0
          || ((errno == EXDEV || errno == EPERM || errno == EMLINK || errno == ENOTSUP)
              && symlink (t->keep, p) == 0))
         tmp = p;
      else if (errno == EEXIST)
         g_free (p);
      else {
         gint e = errno;
         g_free (p);
         name = g_filename_display_name (t->path);
         g_string_append_printf (job->errors, "%s: %s: %s\n", name,
                                 _("the link could not be made, left as it is"),
                                 g_strerror (e));
         g_free (name);
         break;
      }
   }
   if (tmp) {
      file = g_file_new_for_path (t->path);
      if (g_file_trash (file, NULL, &error)) {
         if (g_rename (tmp, t->path) == 0) {
            g_ptr_array_add (job->done, t);
            job->n_links++;
         } else {
            gint e = errno;
            name = g_filename_display_name (t->path);
            g_string_append_printf (job->errors, "%s: %s: %s\n", name,
                                    _("in the trash, but the link could not be put in "
                                      "its place"), g_strerror (e));
            g_free (name);
            g_unlink (tmp);
            g_ptr_array_add (job->done, t);
         }
      } else {
         name = g_filename_display_name (t->path);
         g_string_append_printf (job->errors, "%s: %s\n", name, error ? error->message : "");
         g_free (name);
         g_clear_error (&error);
         g_unlink (tmp);
      }
      g_object_unref (file);
   }
   g_free (tmp);
   g_free (dir);
   g_free (base);
}


static void
trash_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   TrashJob *job = data;
   guint i;

   for (i = 0; i < job->items->len; i++) {
      TrashItem *t = &g_array_index (job->items, TrashItem, i);
      GFile *file;
      GError *error = NULL;

      if (t->verify && !same_bytes (t->path, t->keep)) {
         gchar *name = g_filename_display_name (t->path);
         g_string_append_printf (job->errors, "%s: %s\n", name,
                                 _("not the same as the image kept any more (changed or "
                                   "gone): left as it is"));
         g_free (name);
         continue;
      }
      if (job->link) {
         link_and_trash (job, t);
         continue;
      }
      file = g_file_new_for_path (t->path);
      if (g_file_trash (file, NULL, &error)) {
         g_ptr_array_add (job->done, t);
      } else {
         gchar *name = g_filename_display_name (t->path);
         g_string_append_printf (job->errors, "%s: %s\n", name,
                                 error ? error->message : "");
         g_free (name);
         g_clear_error (&error);
      }
      g_object_unref (file);
   }
   g_task_return_boolean (task, TRUE);
}


#ifdef ENABLE_INDEX
/* a comment file with nothing but the tags (and empty lines) */
static gboolean
only_tags (const gchar *cfile)
{
   gchar *text = NULL, **lines;
   gboolean only = TRUE;
   gint i;

   if (!g_file_get_contents (cfile, &text, NULL, NULL)) return FALSE;
   lines = g_strsplit (text, "\n", -1);
   for (i = 0; lines[i] && only; i++) {
      g_strchomp (lines[i]);
      if (*lines[i] && !g_str_has_prefix (lines[i], GIMV_COMMENT_TAGS_KEY ":"))
         only = FALSE;
   }
   g_strfreev (lines);
   g_free (text);
   return only;
}
#endif


/* the comment (tags) and the bookmark of a copy deleted to the image kept:
 * the comment file moves when the kept image has none, else only the tags
 * are added to its own (the rest of the comment stays with the copy) */
static void
move_extras (const gchar *from, const gchar *to)
{
   gchar *cfrom = gimv_comment_find_file (from);

   if (cfrom) {
      gchar *cto = gimv_comment_find_file (to);
      if (!cto) {
         gchar *dest = gimv_comment_get_path (to);
         gchar *dir = dest ? g_path_get_dirname (dest) : NULL;
         if (dest && g_mkdir_with_parents (dir, 0700) == 0 && g_rename (cfrom, dest) == 0) {
#ifdef ENABLE_INDEX
            gimv_index_tags_changed (to);
            gimv_index_tags_changed (from);
#endif
         }
         g_free (dir);
         g_free (dest);
      } else {
#ifdef ENABLE_INDEX
         gimv_index_tags_merge (from, to);
         /* nothing but the tags in it: they are with the kept image now
          * (else it would be a missing image with a comment) */
         if (only_tags (cfrom) && g_unlink (cfrom) == 0)
            gimv_index_tags_changed (from);
#endif
      }
      g_free (cto);
      g_free (cfrom);
   }
   if (gimv_bookmark_has (from)) {
      if (gimv_bookmark_has (to))
         gimv_bookmark_remove (from);
      else
         gimv_bookmark_move (from, to);
   }
}


static void
trash_done (GObject *source, GAsyncResult *res, gpointer data)
{
   TrashJob *job = g_task_get_task_data (G_TASK (res));
   GimvDuplWin *sw = job->shared->sw;
   guint i, moved = 0;

   /* the comments and bookmarks follow, the window open or not */
   for (i = 0; i < job->done->len; i++) {
      TrashItem *t = g_ptr_array_index (job->done, i);
      if (job->move_extras && !job->link && t->extras && t->keep) {
         move_extras (t->path, t->keep);
         moved++;
      }
   }

   if (!sw) return;
   sw->priv->trashing = FALSE;
   for (i = 0; i < job->done->len; i++)
      g_hash_table_add (sw->priv->removed,
                        g_strdup (((TrashItem *) g_ptr_array_index (job->done, i))->path));

   gtk_widget_set_sensitive (sw->priv->stop_button, FALSE);
   {
      gchar *text = job->link
         ? g_strdup_printf (_("Moved to the trash: %u images (links to the images kept in "
                              "their places: %u)"), job->done->len, job->n_links)
         : moved > 0
         ? g_strdup_printf (_("Moved to the trash: %u images (the comments and bookmarks of "
                              "%u to the images kept)"), job->done->len, moved)
         : g_strdup_printf (_("Moved to the trash: %u images"), job->done->len);
      gtk_progress_bar_set_text (GTK_PROGRESS_BAR (sw->priv->progress), text);
      g_free (text);
   }
   fill_groups (sw);
   /* the group done is gone (one left): on to the next, which is now where
    * it was */
   if (!job->all_groups) {
      GtkSingleSelection *sel = GTK_SINGLE_SELECTION
         (gtk_list_view_get_model (GTK_LIST_VIEW (sw->priv->group_view)));
      guint n = g_list_model_get_n_items (G_LIST_MODEL (sw->priv->group_store));
      if (n > 0 && gtk_single_selection_get_selected (sel) != MIN (job->position, n - 1)
          && job->done->len > 0) {
         gtk_single_selection_set_selected (sel, MIN (job->position, n - 1));
         gtk_list_view_scroll_to (GTK_LIST_VIEW (sw->priv->group_view),
                                  MIN (job->position, n - 1), GTK_LIST_SCROLL_FOCUS, NULL);
      }
   }

   if (sw->tv && job->done->len > 0)
      gimv_thumb_view_refresh_list (sw->tv);

   if (job->errors->len > 0) {
      GtkAlertDialog *dialog = gtk_alert_dialog_new ("%s", _("Some could not be moved to the trash"));
      gtk_alert_dialog_set_detail (dialog, job->errors->str);
      gtk_alert_dialog_show (dialog, GTK_WINDOW (sw));
      g_object_unref (dialog);
   }
   update_summary (sw);
}


/* the images to delete of a group into the job */
static void
add_group (GimvDuplWin *sw, TrashJob *job, GimvDuplGroup *group)
{
   GimvDuplItem *keep = NULL;
   guint k, n_del, n_keep;
   goffset bytes;

   group_counts (sw, group, &n_del, &bytes, &n_keep);
   if (n_del == 0) return;
   if (n_keep == 0) {
      if (job->all_groups) { job->skipped++; return; }
      job->all_marked = TRUE;
   }
   for (k = 0; k < group->members->len && !keep; k++) {
      GimvDuplItem *item = g_ptr_array_index (group->members, k);
      if (!is_removed (sw, item) && !marked_delete (sw, group, k)) keep = item;
   }
   job->n_groups++;
   job->bytes += bytes;
   for (k = 0; k < group->members->len; k++) {
      GimvDuplItem *item = g_ptr_array_index (group->members, k);
      TrashItem t = { 0 };
      if (is_removed (sw, item) || !marked_delete (sw, group, k)) continue;
      t.path = g_strdup (item->path);
      t.keep = keep ? g_strdup (keep->path) : NULL;
      t.verify = keep && group->kind == GIMV_DUPL_SAME;
      g_array_append_val (job->items, t);
   }
}


static TrashJob *
trash_job_new (GimvDuplWin *sw, gboolean all_groups)
{
   GimvDuplWinPriv *priv = sw->priv;
   TrashJob *job = g_new0 (TrashJob, 1);
   guint i;

   job->shared = shared_ref (priv->shared);
   job->items = g_array_new (FALSE, TRUE, sizeof (TrashItem));
   g_array_set_clear_func (job->items, trash_item_clear);
   job->all_groups = all_groups;
   if (all_groups) {
      for (i = 0; priv->groups && i < priv->groups->len; i++) {
         GimvDuplGroup *g = g_ptr_array_index (priv->groups, i);
         if (n_left (sw, g) >= 2) add_group (sw, job, g);
      }
   } else if (priv->groups && priv->current >= 0 && priv->current < (gint) priv->groups->len) {
      add_group (sw, job, g_ptr_array_index (priv->groups, priv->current));
   }
   job->position = gtk_single_selection_get_selected
      (GTK_SINGLE_SELECTION (gtk_list_view_get_model (GTK_LIST_VIEW (priv->group_view))));
   return job;
}


static void
trash_start (GimvDuplWin *sw, TrashJob *job)
{
   GTask *task;

   sw->priv->trashing = TRUE;
   job->done = g_ptr_array_new ();
   job->errors = g_string_new (NULL);
   gtk_widget_set_sensitive (sw->priv->trash_button, FALSE);
   gtk_widget_set_sensitive (sw->priv->trash_all_button, FALSE);
   gtk_progress_bar_set_text (GTK_PROGRESS_BAR (sw->priv->progress),
                              _("Moving to the trash..."));
   task = g_task_new (NULL, NULL, trash_done, NULL);
   g_task_set_task_data (task, job, trash_job_free);
   g_task_run_in_thread (task, trash_thread);
   g_object_unref (task);
}


static void
cb_trash_answer (GObject *source, GAsyncResult *res, gpointer data)
{
   TrashJob *job = data;
   gint button = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL);
   GimvDuplWin *sw = job->shared->sw;

   /* 0: cancel; 1: to the trash; 2: the comments moved first (when they
    * have some); link_button: links left in their places */
   if (button <= 0 || !sw || sw->priv->trashing) {
      trash_job_free (job);
      return;
   }
   job->link = button == job->link_button;
   job->move_extras = !job->link && button == 2;
   trash_start (sw, job);
}


/* the comment files looked for in a thread (thousands of images) */
static void
extras_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   TrashJob *job = data;
   guint i;

   for (i = 0; i < job->items->len; i++) {
      TrashItem *t = &g_array_index (job->items, TrashItem, i);
      t->extras = has_comment (t->path);
   }
   g_task_return_boolean (task, TRUE);
}


static void
extras_done (GObject *source, GAsyncResult *res, gpointer data)
{
   TrashJob *job = g_task_get_task_data (G_TASK (res));
   GimvDuplWin *sw = job->shared->sw;
   GtkAlertDialog *dialog;
   GString *detail;
   gchar *size;
   const gchar *buttons[5];
   gboolean can_link = TRUE;
   gint nb;
   guint i;

   /* the job goes on to the dialog (the task does not free it) */
   if (!sw) { trash_job_free (job); return; }
   gtk_progress_bar_set_text (GTK_PROGRESS_BAR (sw->priv->progress), "");

   /* the bookmarks are in this thread's hands */
   job->with_extras = 0;
   for (i = 0; i < job->items->len; i++) {
      TrashItem *t = &g_array_index (job->items, TrashItem, i);
      if (!t->extras && gimv_bookmark_has (t->path)) t->extras = TRUE;
      if (t->extras && t->keep) job->with_extras++;
   }

   size = g_format_size (job->bytes);
   if (job->all_groups)
      dialog = gtk_alert_dialog_new (_("Move %u images (%s) of %u groups to the trash?"),
                                     job->items->len, size, job->n_groups);
   else
      dialog = gtk_alert_dialog_new (_("Move %u images (%s) of this group to the trash?"),
                                     job->items->len, size);
   g_free (size);

   detail = g_string_new (NULL);
   /* the names, the ones not shown too (a group of many) */
   for (i = 0; i < job->items->len && i < 10; i++) {
      gchar *name = g_filename_display_basename (g_array_index (job->items, TrashItem, i).path);
      g_string_append_printf (detail, "%s\n", name);
      g_free (name);
   }
   if (job->items->len > 10) {
      g_string_append_printf (detail, _("and %u more"), job->items->len - 10);
      g_string_append (detail, "\n");
   }
   g_string_append (detail, "\n");
   if (job->all_marked) {
      g_string_append (detail, _("Every image of this group is marked to delete."));
      g_string_append (detail, "\n");
   }
   if (job->skipped > 0) {
      g_string_append_printf (detail, _("%u groups with every image marked to delete are "
                                        "left out: move those one group at a time."),
                              job->skipped);
      g_string_append (detail, "\n");
   }
   if (job->with_extras > 0) {
      g_string_append_printf (detail,
                              _("%u of them have comments (tags) or bookmarks. They can go "
                                "to the image kept of the group: a comment moves when that "
                                "one has none, else its tags are added to those of that one; "
                                "a bookmark moves when that one has none."),
                              job->with_extras);
      g_string_append (detail, "\n");
   }
   /* links: of the same files only (not another size of the picture) */
   for (i = 0; i < job->items->len && can_link; i++)
      if (!g_array_index (job->items, TrashItem, i).verify) can_link = FALSE;
   if (can_link) {
      g_string_append (detail, _("\"Trash and Leave Links\": each name stays, as a link to "
                                 "the image kept (a hard link on the same file system, "
                                 "else a symbolic link); its comment and bookmark stay "
                                 "with it."));
      g_string_append (detail, "\n");
   }
   g_string_append (detail, _("Each one is compared with the image kept again first. "
                              "They can be put back from the trash."));
   gtk_alert_dialog_set_detail (dialog, detail->str);
   g_string_free (detail, TRUE);

   buttons[0] = _("_Cancel");
   if (job->with_extras > 0) {
      buttons[1] = _("Move to Trash _Only");
      buttons[2] = _("_Move Them to the Kept Ones and Trash");
      nb = 3;
   } else {
      buttons[1] = _("_Move to Trash");
      nb = 2;
   }
   job->link_button = -1;
   if (can_link) {
      job->link_button = nb;
      buttons[nb++] = _("Trash and Leave _Links");
   }
   buttons[nb] = NULL;
   gtk_alert_dialog_set_buttons (dialog, buttons);
   gtk_alert_dialog_set_cancel_button (dialog, 0);
   gtk_alert_dialog_set_default_button (dialog, job->all_marked ? 0
                                                : job->with_extras > 0 ? 2 : 1);
   gtk_alert_dialog_choose (dialog, GTK_WINDOW (sw), NULL, cb_trash_answer, job);
   g_object_unref (dialog);
}


static void
trash_ask (GimvDuplWin *sw, gboolean all_groups)
{
   TrashJob *job;
   GTask *task;

   if (sw->priv->trashing) return;
   job = trash_job_new (sw, all_groups);
   if (job->items->len == 0) { trash_job_free (job); return; }
   gtk_progress_bar_set_text (GTK_PROGRESS_BAR (sw->priv->progress),
                              _("Looking at the images to delete..."));
   task = g_task_new (NULL, NULL, extras_done, NULL);
   g_task_set_task_data (task, job, NULL);
   g_task_run_in_thread (task, extras_thread);
   g_object_unref (task);
}


static void
cb_trash (GtkButton *button, GimvDuplWin *sw)
{
   trash_ask (sw, FALSE);
}


static void
cb_trash_all (GtkButton *button, GimvDuplWin *sw)
{
   trash_ask (sw, TRUE);
}


/******************************************************************************
 *   the window
 ******************************************************************************/

static void
gimv_dupl_win_dispose (GObject *object)
{
   GimvDuplWin *sw = GIMV_DUPL_WIN (object);
   GimvDuplWinPriv *priv = sw->priv;

   if (priv) {
      if (priv->cancel) g_cancellable_cancel (priv->cancel);
      if (priv->shared) {
         priv->shared->sw = NULL;
         shared_unref (priv->shared);
         priv->shared = NULL;
      }
      /* a scan still running is freed when its thread ends */
      if (priv->scan && !priv->running) gimv_dupl_scan_free (priv->scan);
      priv->scan = NULL;
      if (priv->groups && priv->groups_owned) g_ptr_array_unref (priv->groups);
      priv->groups = NULL;
      g_clear_object (&priv->cancel);
      g_clear_pointer (&priv->done_text, g_free);
      g_clear_pointer (&priv->marks, g_hash_table_destroy);
      g_clear_pointer (&priv->removed, g_hash_table_destroy);
      g_clear_pointer (&priv->textures, g_hash_table_destroy);
      g_clear_pointer (&priv->waiting, g_hash_table_destroy);
      /* The jobs left are not read, but run (and freed by idle_pic_done,
       * the window gone): g_thread_pool_free (immediate) would give its
       * item free function the pool's own wake-up markers too (a crash
       * closing the window while the pictures were being read). */
      if (priv->pending) {
         GHashTableIter it;
         gpointer job;
         g_hash_table_iter_init (&it, priv->pending);
         while (g_hash_table_iter_next (&it, NULL, &job))
            g_atomic_int_set (&((PicJob *) job)->cancelled, 1);
      }
      if (priv->pool) g_thread_pool_free (priv->pool, FALSE, FALSE);
      priv->pool = NULL;
      g_clear_pointer (&priv->pending, g_hash_table_destroy);
      g_clear_object (&priv->group_store);
      g_free (priv);
      sw->priv = NULL;
   }

   G_OBJECT_CLASS (parent_class)->dispose (object);
}


static void
gimv_dupl_win_class_init (GimvDuplWinClass *klass)
{
   parent_class = g_type_class_peek_parent (klass);
   G_OBJECT_CLASS (klass)->dispose = gimv_dupl_win_dispose;
}


static void
waiting_free (gpointer p)
{
   g_slist_free_full (p, weak_ref_free);
}


static void
gimv_dupl_win_init (GimvDuplWin *sw)
{
   GimvDuplWinPriv *priv;
   GtkWidget *vbox, *paned, *scrolled, *right, *hbox;
   GtkListItemFactory *factory;
   GtkSingleSelection *sel;

   sw->tv = NULL;
   priv = sw->priv = g_new0 (GimvDuplWinPriv, 1);
   priv->shared   = g_new0 (Shared, 1);
   priv->shared->ref = 1;
   priv->shared->sw  = sw;
   priv->current  = -1;
   priv->marks    = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
   priv->removed  = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
   priv->textures = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
   priv->waiting  = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, waiting_free);
   priv->pending  = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
   priv->pool     = g_thread_pool_new (pic_thread, NULL, 2, FALSE, NULL);
   g_thread_pool_set_sort_function (priv->pool, pic_job_cmp, NULL);

   gtk_window_set_title (GTK_WINDOW (sw), _("Find Duplicates - Results"));
   {
      /* Escape closes it */
      GtkEventController *keys = gtk_shortcut_controller_new ();
      gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
         gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                           gtk_named_action_new ("window.close")));
      gtk_widget_add_controller (GTK_WIDGET (sw), keys);
   }
   gtk_window_set_default_size (GTK_WINDOW (sw), 960, 600);

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
   gtk_window_set_child (GTK_WINDOW (sw), vbox);

   paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
   gtk_widget_set_vexpand (paned, TRUE);
   gtk_paned_set_position (GTK_PANED (paned), 280);
   gtk_box_append (GTK_BOX (vbox), paned);

   /* the groups */
   priv->group_store = g_list_store_new (GIMV_TYPE_DUPL_ENTRY);
   sel = gtk_single_selection_new (G_LIST_MODEL (g_object_ref (priv->group_store)));
   gtk_single_selection_set_autoselect (sel, FALSE);
   gtk_single_selection_set_can_unselect (sel, FALSE);
   g_signal_connect (sel, "selection-changed", G_CALLBACK (cb_group_selected), sw);
   factory = gtk_signal_list_item_factory_new ();
   g_signal_connect (factory, "setup", G_CALLBACK (cb_row_setup), sw);
   g_signal_connect (factory, "bind", G_CALLBACK (cb_row_bind), sw);
   g_signal_connect (factory, "unbind", G_CALLBACK (cb_row_unbind), sw);
   priv->group_view = gtk_list_view_new (GTK_SELECTION_MODEL (sel), factory);
   scrolled = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), priv->group_view);
   gtk_widget_set_size_request (scrolled, 200, -1);
   gtk_paned_set_start_child (GTK_PANED (paned), scrolled);
   gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);

   /* the group shown */
   right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
   gtk_widget_set_margin_start (right, 8);
   gtk_widget_set_margin_end (right, 8);
   gtk_widget_set_margin_top (right, 6);
   /* the group: its kind and number, what is to delete, to the trash */
   {
      GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      priv->head_label = gtk_label_new (NULL);
      gtk_label_set_xalign (GTK_LABEL (priv->head_label), 0.0);
      gtk_widget_set_hexpand (priv->head_label, TRUE);
      gtk_box_append (GTK_BOX (head), priv->head_label);

      priv->summary_label = gtk_label_new (NULL);
      gtk_box_append (GTK_BOX (head), priv->summary_label);

      priv->trash_button = gtk_button_new_with_mnemonic (_("Move to _Trash..."));
      gtk_widget_set_tooltip_text (priv->trash_button,
                                   _("Moves the images of this group marked Delete to the trash"));
      gtk_widget_add_css_class (priv->trash_button, "destructive-action");
      g_signal_connect (priv->trash_button, "clicked", G_CALLBACK (cb_trash), sw);
      gtk_box_append (GTK_BOX (head), priv->trash_button);
      gtk_box_append (GTK_BOX (right), head);
   }
   priv->kind_label = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (priv->kind_label), 0.0);
   gtk_label_set_wrap (GTK_LABEL (priv->kind_label), TRUE);
   gtk_widget_add_css_class (priv->kind_label, "dim-label");
   gtk_box_append (GTK_BOX (right), priv->kind_label);
   priv->empty_label = gtk_label_new (_("No duplicates were found."));
   gtk_widget_set_vexpand (priv->empty_label, TRUE);
   gtk_widget_set_visible (priv->empty_label, FALSE);
   gtk_box_append (GTK_BOX (right), priv->empty_label);

   priv->cards = gtk_flow_box_new ();
   gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (priv->cards), GTK_SELECTION_NONE);
   gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (priv->cards), TRUE);
   gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (priv->cards), 8);
   gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (priv->cards), 6);
   gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (priv->cards), 6);
   gtk_widget_set_valign (priv->cards, GTK_ALIGN_START);
   priv->more_label = gtk_label_new (NULL);
   gtk_widget_add_css_class (priv->more_label, "dim-label");
   gtk_widget_set_margin_top (priv->more_label, 6);
   gtk_widget_set_visible (priv->more_label, FALSE);
   {
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      gtk_box_append (GTK_BOX (box), priv->cards);
      gtk_box_append (GTK_BOX (box), priv->more_label);
      scrolled = gtk_scrolled_window_new ();
      gtk_widget_set_vexpand (scrolled, TRUE);
      gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), box);
   }
   gtk_box_append (GTK_BOX (right), scrolled);
   gtk_paned_set_end_child (GTK_PANED (paned), right);
   gtk_paned_set_shrink_end_child (GTK_PANED (paned), FALSE);

   /* the bottom: progress, stop, what is to delete, to the trash */
   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
   gtk_widget_set_margin_start (hbox, 8);
   gtk_widget_set_margin_end (hbox, 8);
   gtk_widget_set_margin_top (hbox, 6);
   gtk_widget_set_margin_bottom (hbox, 6);
   gtk_box_append (GTK_BOX (vbox), hbox);

   priv->progress = gtk_progress_bar_new ();
   gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (priv->progress), TRUE);
   gtk_progress_bar_set_ellipsize (GTK_PROGRESS_BAR (priv->progress), PANGO_ELLIPSIZE_END);
   gtk_widget_set_valign (priv->progress, GTK_ALIGN_CENTER);
   gtk_widget_set_hexpand (priv->progress, TRUE);
   gtk_box_append (GTK_BOX (hbox), priv->progress);

   priv->trash_all_button = gtk_button_new_with_mnemonic (_("Move _All Marked to Trash..."));
   gtk_widget_add_css_class (priv->trash_all_button, "destructive-action");
   gtk_widget_set_visible (priv->trash_all_button, FALSE);
   g_signal_connect (priv->trash_all_button, "clicked", G_CALLBACK (cb_trash_all), sw);
   gtk_box_append (GTK_BOX (hbox), priv->trash_all_button);

   priv->stop_button = gtk_button_new_with_mnemonic (_("_Stop"));
   gtk_widget_set_sensitive (priv->stop_button, FALSE);
   g_signal_connect (priv->stop_button, "clicked", G_CALLBACK (cb_stop), sw);
   gtk_box_append (GTK_BOX (hbox), priv->stop_button);

   update_summary (sw);
}


GimvDuplWin *
gimv_dupl_win_new (void)
{
   GimvDuplWin *sw = GIMV_DUPL_WIN (g_object_new (GIMV_TYPE_DUPL_WIN, NULL));

   gimv_icon_stock_set_window_icon (GTK_WIDGET (sw), "gimv_icon");
   return sw;
}


void
gimv_dupl_win_set_relation (GimvDuplWin *sw, GimvThumbView *tv)
{
   g_return_if_fail (GIMV_IS_DUPL_WIN (sw));
   sw->tv = tv;
}


void
gimv_dupl_win_unset_relation (GimvDuplWin *sw)
{
   g_return_if_fail (GIMV_IS_DUPL_WIN (sw));
   sw->tv = NULL;
}
