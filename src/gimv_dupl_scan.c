/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: finding the duplicate images of a list, in one pass sorted
 * into kinds.
 *
 * The signature of a picture (32 x 32 means of its colors) and its
 * comparison come from GQview (Copyright (C) 2001 John Ellis), through
 * the old compare_similar.c of GImageView.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimv_dupl_scan.h"

#include <stdlib.h>
#include <string.h>

/* the pictures are read this small for their signatures */
#define READ_SIZE 128

struct GimvDuplScan_Tag
{
   gfloat      accuracy;
   GPtrArray  *items;         /* GimvDuplItem * */
   gint       *parent;        /* union-find over the items */
   gint       *same_parent;   /* the same, after the same files only */
   GPtrArray  *groups;        /* GimvDuplGroup * */
};


/******************************************************************************
 *   signatures
 ******************************************************************************/

void
gimv_dupl_sig_fill (guint8 sig[GIMV_DUPL_SIG_SIZE], GdkPixbuf *pixbuf)
{
   gint w, h, rs, p_step, x_inc, y_inc, xs, ys, i, j;
   gboolean x_small = FALSE, y_small = FALSE;
   const guchar *pix;

   memset (sig, 0, GIMV_DUPL_SIG_SIZE);
   if (!pixbuf) return;

   w      = gdk_pixbuf_get_width (pixbuf);
   h      = gdk_pixbuf_get_height (pixbuf);
   rs     = gdk_pixbuf_get_rowstride (pixbuf);
   pix    = gdk_pixbuf_read_pixels (pixbuf);
   p_step = gdk_pixbuf_get_n_channels (pixbuf);
   if (w < 1 || h < 1 || p_step < 3) return;

   x_inc = w / 32;
   y_inc = h / 32;
   if (x_inc < 1) { x_inc = 1; x_small = TRUE; }
   if (y_inc < 1) { y_inc = 1; y_small = TRUE; }

   j = 0;
   for (ys = 0; ys < 32; ys++) {
      if (y_small) j = (gfloat) h / 32 * ys;
      i = 0;
      for (xs = 0; xs < 32; xs++) {
         gint x, y, r = 0, g = 0, b = 0;

         if (x_small) i = (gfloat) w / 32 * xs;
         for (y = j; y < j + y_inc && y < h; y++) {
            const guchar *p = pix + y * rs + i * p_step;
            for (x = i; x < i + x_inc && x < w; x++) {
               r += p[0]; g += p[1]; b += p[2];
               p += p_step;
            }
         }
         r /= x_inc * y_inc;
         g /= x_inc * y_inc;
         b /= x_inc * y_inc;
         sig[(ys * 32 + xs) * 3]     = r;
         sig[(ys * 32 + xs) * 3 + 1] = g;
         sig[(ys * 32 + xs) * 3 + 2] = b;
         i += x_inc;
      }
      j += y_inc;
   }
}


/* the sum of the differences, up to limit (more: limit + 1) */
static gint64
sig_diff (const guint8 *a, const guint8 *b, gint64 limit)
{
   gint64 d = 0;
   gint i;

   for (i = 0; i < GIMV_DUPL_SIG_SIZE; i++) {
      d += abs ((gint) a[i] - (gint) b[i]);
      if ((i & 255) == 255 && d > limit) return limit + 1;
   }
   return d;
}


/* 1.0: the same; 0.0: the opposite (black and white) */
gfloat
gimv_dupl_sig_compare (const guint8 *a, const guint8 *b)
{
   if (!a || !b) return 0.0;
   return 1.0 - (gdouble) sig_diff (a, b, G_MAXINT64 - 1)
                / (GIMV_DUPL_SIG_SIZE * 255.0);
}


/******************************************************************************
 *   the scan
 ******************************************************************************/

GimvDuplScan *
gimv_dupl_scan_new (gfloat accuracy)
{
   GimvDuplScan *scan = g_new0 (GimvDuplScan, 1);

   scan->accuracy = CLAMP (accuracy, 0.5, 1.0);
   scan->items    = g_ptr_array_new ();
   scan->groups   = g_ptr_array_new_with_free_func ((GDestroyNotify) gimv_dupl_group_free);
   return scan;
}


static void
item_free (GimvDuplItem *item)
{
   g_free (item->path);
   g_free (item->sig);
   g_free (item);
}


void
gimv_dupl_scan_free (GimvDuplScan *scan)
{
   if (!scan) return;
   g_ptr_array_unref (scan->groups);
   g_ptr_array_foreach (scan->items, (GFunc) item_free, NULL);
   g_ptr_array_unref (scan->items);
   g_free (scan->parent);
   g_free (scan->same_parent);
   g_free (scan);
}


GimvDuplItem *
gimv_dupl_scan_add (GimvDuplScan *scan, const gchar *path,
                    goffset size, gint64 mtime, gpointer user)
{
   GimvDuplItem *item;

   g_return_val_if_fail (scan && path, NULL);
   item = g_new0 (GimvDuplItem, 1);
   item->path  = g_strdup (path);
   item->size  = size;
   item->mtime = mtime;
   item->user  = user;
   item->index = scan->items->len;
   g_ptr_array_add (scan->items, item);
   return item;
}


void
gimv_dupl_group_free (GimvDuplGroup *group)
{
   if (!group) return;
   g_ptr_array_unref (group->members);
   g_array_unref (group->sim);
   g_free (group);
}


static gint
uf_find (gint *parent, gint i)
{
   while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
   }
   return i;
}


static void
uf_union (gint *parent, gint a, gint b)
{
   a = uf_find (parent, a);
   b = uf_find (parent, b);
   if (a == b) return;
   /* the smaller index is the root: the first of its kind in the list */
   if (a < b) parent[b] = a; else parent[a] = b;
}


#define ITEM(scan, i) ((GimvDuplItem *) g_ptr_array_index ((scan)->items, (i)))


/* progress not more often than every 100 ms (and at the ends) */
typedef struct {
   GimvDuplProgressFunc func;
   gpointer             data;
   gint64               last;
} Progress;

static void
progress (Progress *p, GimvDuplPhase phase, guint done, guint total, gboolean force)
{
   gint64 now;

   if (!p->func) return;
   now = g_get_monotonic_time ();
   if (!force && now - p->last < 100 * 1000) return;
   p->last = now;
   p->func (phase, done, total, p->data);
}


static gboolean
file_md5 (const gchar *path, gchar out[33], GCancellable *cancel)
{
   GChecksum *sum;
   FILE *fp;
   guchar buf[65536];
   gsize n;
   gboolean ok;

   out[0] = '\0';
   fp = fopen (path, "rb");
   if (!fp) return FALSE;
   sum = g_checksum_new (G_CHECKSUM_MD5);
   while ((n = fread (buf, 1, sizeof buf, fp)) > 0) {
      g_checksum_update (sum, buf, n);
      if (g_cancellable_is_cancelled (cancel)) break;
   }
   ok = !ferror (fp) && !g_cancellable_is_cancelled (cancel);
   if (ok) g_strlcpy (out, g_checksum_get_string (sum), 33);
   g_checksum_free (sum);
   fclose (fp);
   return ok;
}


/* the files of the same size and the same bytes are one */
static void
phase_same (GimvDuplScan *scan, GCancellable *cancel, Progress *p)
{
   GHashTable *by_size = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                                NULL, (GDestroyNotify) g_array_unref);
   GHashTableIter it;
   gpointer value;
   GPtrArray *todo = g_ptr_array_new ();
   guint i, done = 0, total = 0;

   for (i = 0; i < scan->items->len; i++) {
      GimvDuplItem *item = ITEM (scan, i);
      GArray *a;

      if (item->size <= 0) continue;
      a = g_hash_table_lookup (by_size, &item->size);
      if (!a) {
         a = g_array_new (FALSE, FALSE, sizeof (gint));
         g_hash_table_insert (by_size, &item->size, a);
      }
      g_array_append_val (a, i);
   }

   g_hash_table_iter_init (&it, by_size);
   while (g_hash_table_iter_next (&it, NULL, &value)) {
      GArray *a = value;
      if (a->len >= 2) {
         g_ptr_array_add (todo, a);
         total += a->len;
      }
   }

   progress (p, GIMV_DUPL_PHASE_SAME, 0, total, TRUE);
   for (i = 0; i < todo->len && !g_cancellable_is_cancelled (cancel); i++) {
      GArray *a = g_ptr_array_index (todo, i);
      GHashTable *by_md5 = g_hash_table_new (g_str_hash, g_str_equal);
      guint k;

      for (k = 0; k < a->len; k++) {
         gint idx = g_array_index (a, gint, k);
         GimvDuplItem *item = ITEM (scan, idx);
         gpointer first;

         if (file_md5 (item->path, item->md5, cancel)) {
            if (g_hash_table_lookup_extended (by_md5, item->md5, NULL, &first))
               uf_union (scan->parent, GPOINTER_TO_INT (first), idx);
            else
               g_hash_table_insert (by_md5, item->md5, GINT_TO_POINTER (idx));
         }
         progress (p, GIMV_DUPL_PHASE_SAME, ++done, total, FALSE);
         if (g_cancellable_is_cancelled (cancel)) break;
      }
      g_hash_table_destroy (by_md5);
   }

   g_ptr_array_unref (todo);
   g_hash_table_destroy (by_size);
}


/* the picture of the item, small, as shown; its size as shown */
static void
read_item (GimvDuplItem *item)
{
   GdkPixbuf *px, *oriented;
   gint fw = 0, fh = 0;

   px = gdk_pixbuf_new_from_file_at_scale (item->path, READ_SIZE, READ_SIZE,
                                           TRUE, NULL);
   if (!px) return;
   oriented = gdk_pixbuf_apply_embedded_orientation (px);
   g_object_unref (px);
   if (!oriented) return;

   item->sig = g_malloc (GIMV_DUPL_SIG_SIZE);
   gimv_dupl_sig_fill (item->sig, oriented);
   {
      gint i;
      item->sig_sum = 0;
      for (i = 0; i < GIMV_DUPL_SIG_SIZE; i++) item->sig_sum += item->sig[i];
   }

   /* the size of the file, turned as the picture is (the loaders may or
    * may not have turned it already) */
   if (gdk_pixbuf_get_file_info (item->path, &fw, &fh) && fw > 0 && fh > 0) {
      gint pw = gdk_pixbuf_get_width (oriented), ph = gdk_pixbuf_get_height (oriented);
      if (fw != fh && pw != ph && ((fw > fh) != (pw > ph))) {
         gint t = fw; fw = fh; fh = t;
      }
      item->width  = fw;
      item->height = fh;
   }
   g_object_unref (oriented);
}


/* one item of each set of the same files is read; the others share it */
static GArray *
phase_read (GimvDuplScan *scan, GCancellable *cancel, Progress *p)
{
   GArray *reps = g_array_new (FALSE, FALSE, sizeof (gint));
   guint i;

   for (i = 0; i < scan->items->len; i++)
      if (uf_find (scan->parent, i) == (gint) i)
         g_array_append_val (reps, i);

   progress (p, GIMV_DUPL_PHASE_READ, 0, reps->len, TRUE);
   for (i = 0; i < reps->len && !g_cancellable_is_cancelled (cancel); i++) {
      read_item (ITEM (scan, g_array_index (reps, gint, i)));
      progress (p, GIMV_DUPL_PHASE_READ, i + 1, reps->len, FALSE);
   }

   /* the size of the copies */
   for (i = 0; i < scan->items->len; i++) {
      GimvDuplItem *item = ITEM (scan, i), *rep;
      rep = ITEM (scan, uf_find (scan->parent, i));
      if (rep != item) {
         item->width  = rep->width;
         item->height = rep->height;
      }
   }
   return reps;
}


static gint
cmp_sig_sum (gconstpointer a, gconstpointer b, gpointer data)
{
   GimvDuplScan *scan = data;
   gint64 sa = ITEM (scan, *(const gint *) a)->sig_sum;
   gint64 sb = ITEM (scan, *(const gint *) b)->sig_sum;

   return sa < sb ? -1 : sa > sb ? 1 : 0;
}


/*
 * The pictures alike are one.  The difference of two signatures is at
 * least the difference of their sums: sorted by the sums, a picture is
 * compared only with those whose sums are near enough.
 */
static void
phase_compare (GimvDuplScan *scan, GArray *reps, GCancellable *cancel, Progress *p)
{
   GArray *sigs = g_array_new (FALSE, FALSE, sizeof (gint));
   gint64 limit = (gint64) ((1.0 - scan->accuracy) * GIMV_DUPL_SIG_SIZE * 255.0);
   guint i, j;

   for (i = 0; i < reps->len; i++) {
      gint idx = g_array_index (reps, gint, i);
      if (ITEM (scan, idx)->sig) g_array_append_val (sigs, idx);
   }
   g_array_sort_with_data (sigs, cmp_sig_sum, scan);

   progress (p, GIMV_DUPL_PHASE_COMPARE, 0, sigs->len, TRUE);
   for (i = 0; i < sigs->len && !g_cancellable_is_cancelled (cancel); i++) {
      GimvDuplItem *a = ITEM (scan, g_array_index (sigs, gint, i));

      for (j = i + 1; j < sigs->len; j++) {
         GimvDuplItem *b = ITEM (scan, g_array_index (sigs, gint, j));

         if (b->sig_sum - a->sig_sum > limit) break;
         if (sig_diff (a->sig, b->sig, limit) <= limit)
            uf_union (scan->parent, g_array_index (sigs, gint, i),
                      g_array_index (sigs, gint, j));
      }
      progress (p, GIMV_DUPL_PHASE_COMPARE, i + 1, sigs->len, FALSE);
   }
   g_array_unref (sigs);
}


/* the one to keep first: the most pixels, the largest file, the oldest,
 * the shortest path */
static gint
cmp_keep (gconstpointer pa, gconstpointer pb)
{
   const GimvDuplItem *a = *(GimvDuplItem * const *) pa;
   const GimvDuplItem *b = *(GimvDuplItem * const *) pb;
   gint64 na = (gint64) a->width * a->height, nb = (gint64) b->width * b->height;
   gsize la, lb;

   if (na != nb) return na > nb ? -1 : 1;
   if (a->size != b->size) return a->size > b->size ? -1 : 1;
   if (a->mtime != b->mtime) return a->mtime < b->mtime ? -1 : 1;
   la = strlen (a->path); lb = strlen (b->path);
   if (la != lb) return la < lb ? -1 : 1;
   return strcmp (a->path, b->path);
}


/* the same picture in other sizes: all of the same shape, no two of the
 * same size (two of the same size and not the same bytes were edited or
 * saved again: similar) */
static gboolean
all_resized (GimvDuplScan *scan, GArray *units)
{
   GimvDuplItem *a0;
   guint i, j;

   if (units->len < 2) return FALSE;
   a0 = ITEM (scan, g_array_index (units, gint, 0));
   for (i = 0; i < units->len; i++) {
      GimvDuplItem *a = ITEM (scan, g_array_index (units, gint, i));
      gdouble cross;

      if (a->width <= 0 || a->height <= 0) return FALSE;
      /* the shape within 1 % */
      cross = (gdouble) a->width * a0->height - (gdouble) a0->width * a->height;
      if (ABS (cross) > 0.01 * (gdouble) a0->width * a->height) return FALSE;
      for (j = i + 1; j < units->len; j++) {
         GimvDuplItem *b = ITEM (scan, g_array_index (units, gint, j));
         if (a->width == b->width && a->height == b->height) return FALSE;
      }
   }
   return TRUE;
}


static gint
cmp_group (gconstpointer pa, gconstpointer pb)
{
   const GimvDuplGroup *a = *(GimvDuplGroup * const *) pa;
   const GimvDuplGroup *b = *(GimvDuplGroup * const *) pb;

   gchar *ka, *kb;
   gint r;

   if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
   /* as the file names are sorted ("v2" before "v10") */
   ka = g_utf8_collate_key_for_filename (((GimvDuplItem *) g_ptr_array_index (a->members, 0))->path, -1);
   kb = g_utf8_collate_key_for_filename (((GimvDuplItem *) g_ptr_array_index (b->members, 0))->path, -1);
   r = strcmp (ka, kb);
   g_free (ka);
   g_free (kb);
   return r;
}


/* the groups of the union-find parent (same_parent: the sets of the same
 * files; the copies of a file share the signature of the first) */
static GPtrArray *
build_groups (GimvDuplScan *scan, gint *parent)
{
   GPtrArray *groups = g_ptr_array_new_with_free_func ((GDestroyNotify) gimv_dupl_group_free);
   GHashTable *by_root = g_hash_table_new (g_direct_hash, g_direct_equal);
   GHashTableIter it;
   gpointer key, value;
   guint i;

   for (i = 0; i < scan->items->len; i++) {
      gint root = uf_find (parent, i);
      GPtrArray *m = g_hash_table_lookup (by_root, GINT_TO_POINTER (root));
      if (!m) {
         m = g_ptr_array_new ();
         g_hash_table_insert (by_root, GINT_TO_POINTER (root), m);
      }
      g_ptr_array_add (m, GINT_TO_POINTER (i));
   }

   g_hash_table_iter_init (&it, by_root);
   while (g_hash_table_iter_next (&it, &key, &value)) {
      GPtrArray *m = value;
      GimvDuplGroup *group;
      GArray *units;
      GimvDuplItem *keep;
      const guint8 *keep_sig;
      gint keep_unit;

      if (m->len < 2) { g_ptr_array_unref (m); continue; }

      /* the sets of the same files in it */
      units = g_array_new (FALSE, FALSE, sizeof (gint));
      for (i = 0; i < m->len; i++) {
         gint idx = GPOINTER_TO_INT (g_ptr_array_index (m, i));
         gint unit = uf_find (scan->same_parent, idx);
         guint k;
         for (k = 0; k < units->len; k++)
            if (g_array_index (units, gint, k) == unit) break;
         if (k == units->len) g_array_append_val (units, unit);
      }

      group = g_new0 (GimvDuplGroup, 1);
      group->members = g_ptr_array_new ();
      group->sim = g_array_new (FALSE, FALSE, sizeof (gfloat));
      for (i = 0; i < m->len; i++)
         g_ptr_array_add (group->members,
                          ITEM (scan, GPOINTER_TO_INT (g_ptr_array_index (m, i))));
      g_ptr_array_sort (group->members, cmp_keep);

      if (units->len == 1)
         group->kind = GIMV_DUPL_SAME;
      else if (all_resized (scan, units))
         group->kind = GIMV_DUPL_RESIZED;
      else
         group->kind = GIMV_DUPL_SIMILAR;
      group->suggested = group->kind != GIMV_DUPL_SIMILAR;

      keep = g_ptr_array_index (group->members, 0);
      keep_unit = uf_find (scan->same_parent, keep->index);
      keep_sig = ITEM (scan, keep_unit)->sig;
      for (i = 0; i < group->members->len; i++) {
         GimvDuplItem *item = g_ptr_array_index (group->members, i);
         gint unit = uf_find (scan->same_parent, item->index);
         gfloat sim = unit == keep_unit ? 1.0
            : gimv_dupl_sig_compare (ITEM (scan, unit)->sig, keep_sig);
         g_array_append_val (group->sim, sim);
      }

      g_ptr_array_add (groups, group);
      g_array_unref (units);
      g_ptr_array_unref (m);
   }
   g_hash_table_destroy (by_root);

   g_ptr_array_sort (groups, cmp_group);
   return groups;
}


GPtrArray *
gimv_dupl_scan_same_groups (GimvDuplScan *scan)
{
   g_return_val_if_fail (scan && scan->same_parent, NULL);
   return build_groups (scan, scan->same_parent);
}


gboolean
gimv_dupl_scan_run (GimvDuplScan *scan, GCancellable *cancel,
                    GimvDuplProgressFunc func, gpointer data)
{
   Progress p = { func, data, 0 };
   guint i, n;
   GArray *reps;

   g_return_val_if_fail (scan, FALSE);

   n = scan->items->len;
   g_free (scan->parent);
   g_free (scan->same_parent);
   scan->parent = g_new (gint, MAX (n, 1));
   for (i = 0; i < n; i++) scan->parent[i] = i;
   g_ptr_array_set_size (scan->groups, 0);

   phase_same (scan, cancel, &p);
   scan->same_parent = g_memdup2 (scan->parent, sizeof (gint) * MAX (n, 1));
   for (i = 0; i < n; i++) uf_find (scan->same_parent, i);

   reps = phase_read (scan, cancel, &p);
   phase_compare (scan, reps, cancel, &p);
   g_array_unref (reps);

   g_ptr_array_unref (scan->groups);
   scan->groups = build_groups (scan, scan->parent);
   progress (&p, GIMV_DUPL_PHASE_DONE, n, n, TRUE);

   return !g_cancellable_is_cancelled (cancel);
}


GPtrArray *
gimv_dupl_scan_get_groups (GimvDuplScan *scan)
{
   g_return_val_if_fail (scan, NULL);
   return scan->groups;
}
