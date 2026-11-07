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

#include <string.h>
#include <time.h>

#include "gimv_comment.h"
#include "gimv_dupl_scan.h"
#include "gimv_icon_stock.h"
#include "gimv_image_info.h"
#include "gimv_thumb.h"
#include "prefs.h"

#ifdef ENABLE_INDEX
#  include "gimv_index.h"
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
   GPtrArray     *groups;      /* GimvDuplGroup *: shown */
   gboolean       groups_owned;/* ... freed here (the first ones) */

   /* the marks: path -> GINT_TO_POINTER (1 keep, 2 delete) set by hand;
    * otherwise the suggestion of the group */
   GHashTable    *marks;
   GHashTable    *removed;     /* path: moved to the trash */
   gint           skipped;     /* in archives: not looked at */

   /* the pictures: "size:path" -> GdkTexture (or a GdkPaintable) */
   GHashTable    *textures;
   GHashTable    *waiting;     /* "size:path" -> GSList of GWeakRef * */
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
   GdkPixbuf *pixbuf;   /* the result */
} PicJob;

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

   if (job->shared->sw) {
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


/* the picture of path in the size (a box), when it is read */
static void
set_picture (GimvDuplWin *sw, GtkWidget *picture, const gchar *path, gint size)
{
   gchar *key = g_strdup_printf ("%d:%s", size, path);
   GdkPaintable *paintable = g_hash_table_lookup (sw->priv->textures, key);
   GSList *list;
   GWeakRef *ref;

   g_object_set_data_full (G_OBJECT (picture), "gimv-dupl-want", g_strdup (key), g_free);
   if (paintable) {
      gtk_picture_set_paintable (GTK_PICTURE (picture), paintable);
      g_free (key);
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

   ref = g_new0 (GWeakRef, 1);
   g_weak_ref_init (ref, picture);
   if (g_hash_table_lookup_extended (sw->priv->waiting, key, NULL, (gpointer *) &list)) {
      list = g_slist_prepend (list, ref);
      g_hash_table_steal (sw->priv->waiting, key);
      g_hash_table_insert (sw->priv->waiting, key, list);
      return;   /* asked already */
   }
   g_hash_table_insert (sw->priv->waiting, g_strdup (key), g_slist_prepend (NULL, ref));
   {
      PicJob *job = g_new0 (PicJob, 1);
      job->shared = shared_ref (sw->priv->shared);
      job->key    = key;
      job->path   = g_strdup (path);
      job->size   = size;
      g_thread_pool_push (sw->priv->pool, job, NULL);
   }
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

   if (!thumb) return;
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
   gtk_widget_set_tooltip_text (picture, _("Click to show it in the thumbnail window"));
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
      text = g_strdup ("<b>—</b>");
   label = card_label (box, NULL, FALSE);
   gtk_label_set_markup (GTK_LABEL (label), text);
   g_free (text);

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
} ScanJob;


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
   gboolean ok = gimv_dupl_scan_run (job->scan, cancel, scan_progress, job);
   g_task_return_boolean (task, ok);
}


static void
scan_job_free (gpointer data)
{
   ScanJob *job = data;
   /* the window gone: the scan is nobody's */
   if (!job->shared->sw) gimv_dupl_scan_free (job->scan);
   shared_unref (job->shared);
   g_free (job);
}


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
   if (priv->skipped > 0) {
      gchar *t = g_strdup_printf (_("%s (%d in archives not compared)"), text, priv->skipped);
      g_free (text);
      text = t;
   }
   gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (priv->progress), ok ? 1.0 : 0.0);
   gtk_progress_bar_set_text (GTK_PROGRESS_BAR (priv->progress), text);
   g_free (text);
   gtk_widget_set_visible (priv->empty_label, n == 0);
   update_summary (sw);
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


static void
cb_stop (GtkButton *button, GimvDuplWin *sw)
{
   if (sw->priv->cancel) g_cancellable_cancel (sw->priv->cancel);
}


/******************************************************************************
 *   to the trash
 ******************************************************************************/

typedef struct {
   Shared    *shared;
   GPtrArray *paths;      /* gchar * */
   GPtrArray *done;       /* the paths moved */
   GString   *errors;
   guint      position;   /* of the group in the list */
} TrashJob;


static void
trash_job_free (gpointer data)
{
   TrashJob *job = data;
   shared_unref (job->shared);
   g_ptr_array_unref (job->paths);
   g_ptr_array_unref (job->done);
   g_string_free (job->errors, TRUE);
   g_free (job);
}


static void
trash_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   TrashJob *job = data;
   guint i;

   for (i = 0; i < job->paths->len; i++) {
      const gchar *path = g_ptr_array_index (job->paths, i);
      GFile *file = g_file_new_for_path (path);
      GError *error = NULL;

      if (g_file_trash (file, NULL, &error)) {
         g_ptr_array_add (job->done, g_strdup (path));
      } else {
         gchar *name = g_filename_display_name (path);
         g_string_append_printf (job->errors, "%s: %s\n", name,
                                 error ? error->message : "");
         g_free (name);
         g_clear_error (&error);
      }
      g_object_unref (file);
   }
   g_task_return_boolean (task, TRUE);
}


static void
trash_done (GObject *source, GAsyncResult *res, gpointer data)
{
   TrashJob *job = g_task_get_task_data (G_TASK (res));
   GimvDuplWin *sw = job->shared->sw;
   guint i;

   if (!sw) return;
   sw->priv->trashing = FALSE;
   for (i = 0; i < job->done->len; i++)
      g_hash_table_add (sw->priv->removed, g_strdup (g_ptr_array_index (job->done, i)));

   gtk_widget_set_sensitive (sw->priv->stop_button, FALSE);
   {
      gchar *text = g_strdup_printf (_("Moved to the trash: %u images"), job->done->len);
      gtk_progress_bar_set_text (GTK_PROGRESS_BAR (sw->priv->progress), text);
      g_free (text);
   }
   fill_groups (sw);
   /* the group done is gone (one left): on to the next, which is now where
    * it was */
   {
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
}


/* the paths to delete of the group shown */
static GPtrArray *
paths_to_delete (GimvDuplWin *sw, gboolean *all, guint *with_comment, goffset *bytes)
{
   GimvDuplWinPriv *priv = sw->priv;
   GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);
   GimvDuplGroup *group;
   guint k, n_del, n_keep;

   *all = FALSE;
   *with_comment = 0;
   *bytes = 0;
   if (!priv->groups || priv->current < 0 || priv->current >= (gint) priv->groups->len)
      return paths;
   group = g_ptr_array_index (priv->groups, priv->current);
   group_counts (sw, group, &n_del, bytes, &n_keep);
   *all = n_del > 0 && n_keep == 0;
   for (k = 0; k < group->members->len; k++) {
      GimvDuplItem *item = g_ptr_array_index (group->members, k);
      if (is_removed (sw, item) || !marked_delete (sw, group, k)) continue;
      g_ptr_array_add (paths, g_strdup (item->path));
      if (has_comment (item->path)) (*with_comment)++;
   }
   return paths;
}


static void
cb_trash_answer (GObject *source, GAsyncResult *res, gpointer data)
{
   Shared *shared = data;
   gint button = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL);
   GimvDuplWin *sw = shared->sw;
   gboolean all;
   guint with_comment;
   goffset bytes;
   TrashJob *job;
   GTask *task;

   if (button != 1 || !sw) { shared_unref (shared); return; }

   job = g_new0 (TrashJob, 1);
   job->shared = shared;   /* the reference passes on */
   job->paths = paths_to_delete (sw, &all, &with_comment, &bytes);
   job->position = gtk_single_selection_get_selected
      (GTK_SINGLE_SELECTION (gtk_list_view_get_model (GTK_LIST_VIEW (sw->priv->group_view))));
   sw->priv->trashing = TRUE;
   job->done = g_ptr_array_new_with_free_func (g_free);
   job->errors = g_string_new (NULL);

   gtk_widget_set_sensitive (sw->priv->trash_button, FALSE);
   gtk_progress_bar_set_text (GTK_PROGRESS_BAR (sw->priv->progress),
                              _("Moving to the trash..."));
   task = g_task_new (NULL, NULL, trash_done, NULL);
   g_task_set_task_data (task, job, trash_job_free);
   g_task_run_in_thread (task, trash_thread);
   g_object_unref (task);
}


static void
cb_trash (GtkButton *button, GimvDuplWin *sw)
{
   gboolean all;
   guint with_comment;
   goffset bytes;
   GPtrArray *paths = paths_to_delete (sw, &all, &with_comment, &bytes);
   GtkAlertDialog *dialog;
   GString *detail = g_string_new (NULL);
   gchar *size;
   const gchar *buttons[3];

   if (paths->len == 0) { g_ptr_array_unref (paths); g_string_free (detail, TRUE); return; }

   size = g_format_size (bytes);
   dialog = gtk_alert_dialog_new (_("Move %u images (%s) of this group to the trash?"),
                                  paths->len, size);
   g_free (size);
   /* the names, the ones not shown too (a group of many) */
   {
      guint k;
      for (k = 0; k < paths->len && k < 10; k++) {
         gchar *name = g_filename_display_basename (g_ptr_array_index (paths, k));
         g_string_append_printf (detail, "%s\n", name);
         g_free (name);
      }
      if (paths->len > 10) {
         g_string_append_printf (detail, _("and %u more"), paths->len - 10);
         g_string_append (detail, "\n");
      }
      g_string_append (detail, "\n");
   }
   if (all) {
      g_string_append (detail, _("Every image of this group is marked to delete."));
      g_string_append (detail, "\n");
   }
   if (with_comment > 0) {
      g_string_append_printf (detail,
                              _("%u of them have comments; the comments stay."),
                              with_comment);
      g_string_append (detail, "\n");
   }
   g_string_append (detail, _("They can be put back from the trash."));
   gtk_alert_dialog_set_detail (dialog, detail->str);
   buttons[0] = _("_Cancel");
   buttons[1] = _("_Move to Trash");
   buttons[2] = NULL;
   gtk_alert_dialog_set_buttons (dialog, buttons);
   gtk_alert_dialog_set_cancel_button (dialog, 0);
   gtk_alert_dialog_set_default_button (dialog, all ? 0 : 1);
   gtk_alert_dialog_choose (dialog, GTK_WINDOW (sw), NULL, cb_trash_answer,
                            shared_ref (sw->priv->shared));
   g_object_unref (dialog);
   g_string_free (detail, TRUE);
   g_ptr_array_unref (paths);
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
      g_clear_pointer (&priv->marks, g_hash_table_destroy);
      g_clear_pointer (&priv->removed, g_hash_table_destroy);
      g_clear_pointer (&priv->textures, g_hash_table_destroy);
      g_clear_pointer (&priv->waiting, g_hash_table_destroy);
      if (priv->pool) g_thread_pool_free (priv->pool, TRUE, FALSE);
      priv->pool = NULL;
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
   priv->pool     = g_thread_pool_new_full (pic_thread, NULL,
                                            (GDestroyNotify) pic_job_free,
                                            2, FALSE, NULL);

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
