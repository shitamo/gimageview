/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2026 shitamo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * Finding the duplicate images (src/gimv_dupl_scan.c): the same files,
 * the same picture in other sizes, similar pictures, the one to keep,
 * and the pictures alone.
 */

#include <string.h>
#include <glib/gstdio.h>

#include "gimv_dupl_scan.h"

static gchar *dir;


typedef enum { PAT_HGRAD, PAT_VGRAD, PAT_CHECK, PAT_RINGS, PAT_DIAG } Pattern;

/* a w x h picture of the pattern; mark: a small white square at the top
 * left (an edit) */
static GdkPixbuf *
picture (Pattern pat, gint w, gint h, gboolean mark)
{
   GdkPixbuf *px = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, w, h);
   guchar *pix = gdk_pixbuf_get_pixels (px);
   gint rs = gdk_pixbuf_get_rowstride (px), x, y;

   for (y = 0; y < h; y++) {
      for (x = 0; x < w; x++) {
         guchar *p = pix + y * rs + x * 3;
         gdouble fx = (gdouble) x / w, fy = (gdouble) y / h;
         switch (pat) {
         case PAT_HGRAD: p[0] = 255 * fx; p[1] = 80;  p[2] = 255 * (1 - fx); break;
         case PAT_VGRAD: p[0] = 40;  p[1] = 255 * fy; p[2] = 200; break;
         case PAT_CHECK: {
            gboolean on = ((gint) (fx * 8) + (gint) (fy * 8)) % 2;
            p[0] = p[1] = p[2] = on ? 230 : 20; break;
         }
         case PAT_RINGS: {
            gdouble d = (fx - 0.5) * (fx - 0.5) + (fy - 0.5) * (fy - 0.5);
            p[0] = ((gint) (d * 60) % 2) ? 250 : 0; p[1] = 120; p[2] = 60; break;
         }
         case PAT_DIAG: p[0] = 255 * (fx + fy) / 2; p[1] = 255 * fy; p[2] = 30; break;
         }
         if (mark && fx < 0.2 && fy < 0.2) p[0] = p[1] = p[2] = 255;
      }
   }
   return px;
}


static gchar *
save (const gchar *name, GdkPixbuf *px, gint64 mtime)
{
   gchar *path = g_build_filename (dir, name, NULL);
   GError *error = NULL;

   g_assert_true (gdk_pixbuf_save (px, path, g_str_has_suffix (name, ".jpg") ? "jpeg" : "png",
                                   &error, NULL));
   g_assert_no_error (error);
   g_object_unref (px);
   if (mtime) {
      GFile *f = g_file_new_for_path (path);
      g_assert_true (g_file_set_attribute_uint64 (f, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                                  mtime, 0, NULL, NULL));
      g_object_unref (f);
   }
   return path;
}


static gchar *
copy (const gchar *from, const gchar *name, gint64 mtime)
{
   gchar *path = g_build_filename (dir, name, NULL), *data;
   gsize len;

   g_assert_true (g_file_get_contents (from, &data, &len, NULL));
   g_assert_true (g_file_set_contents (path, data, len, NULL));
   g_free (data);
   if (mtime) {
      GFile *f = g_file_new_for_path (path);
      g_assert_true (g_file_set_attribute_uint64 (f, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                                  mtime, 0, NULL, NULL));
      g_object_unref (f);
   }
   return path;
}


static void
add (GimvDuplScan *scan, const gchar *path)
{
   GStatBuf st;

   g_assert_cmpint (g_stat (path, &st), ==, 0);
   gimv_dupl_scan_add (scan, path, st.st_size, st.st_mtime, NULL);
}


static const gchar *
base (GimvDuplGroup *g, guint i)
{
   GimvDuplItem *item = g_ptr_array_index (g->members, i);
   return strrchr (item->path, '/') + 1;
}


/* the group whose first is name */
static GimvDuplGroup *
group_of (GPtrArray *groups, const gchar *first)
{
   guint i;

   for (i = 0; i < groups->len; i++) {
      GimvDuplGroup *g = g_ptr_array_index (groups, i);
      if (!strcmp (base (g, 0), first)) return g;
   }
   return NULL;
}


static guint progress_calls;
static guint same_groups_seen;
static GimvDuplScan *the_scan;

static void
on_progress (GimvDuplPhase phase, guint done, guint total, gpointer data)
{
   progress_calls++;
   g_assert_cmpuint (done, <=, total);
   if (phase == GIMV_DUPL_PHASE_READ && done == 0) {
      /* the same files are known by now */
      GPtrArray *same = gimv_dupl_scan_same_groups (the_scan);
      same_groups_seen = same->len;
      g_ptr_array_unref (same);
   }
}


static void
test_kinds (void)
{
   GimvDuplScan *scan = gimv_dupl_scan_new (0.95);
   gchar *p[16];
   GPtrArray *groups;
   GimvDuplGroup *g;
   gint n = 0, i;

   /* resized: a (200x100, the oldest of its copies), its copy, a smaller one */
   p[n++] = save ("a.png", picture (PAT_HGRAD, 200, 100, FALSE), 1000000);
   p[n++] = copy (p[0], "a_copy.png", 2000000);
   p[n++] = save ("a_small.png", picture (PAT_HGRAD, 100, 50, FALSE), 3000000);
   /* the same: three copies */
   p[n++] = save ("c.png", picture (PAT_CHECK, 120, 120, FALSE), 5000000);
   p[n++] = copy (p[3], "c2.png", 4000000);
   p[n++] = copy (p[3], "sub_c3.png", 6000000);
   /* similar: an edit of the same size */
   p[n++] = save ("v.png", picture (PAT_VGRAD, 160, 120, FALSE), 0);
   p[n++] = save ("v_edit.png", picture (PAT_VGRAD, 160, 120, TRUE), 0);
   /* alone */
   p[n++] = save ("rings.png", picture (PAT_RINGS, 150, 150, FALSE), 0);
   p[n++] = save ("diag.jpg", picture (PAT_DIAG, 90, 140, FALSE), 0);
   /* not an image, of the size of an image: neither same nor read */
   {
      gchar *t = g_build_filename (dir, "text.png", NULL);
      g_assert_true (g_file_set_contents (t, "not an image", -1, NULL));
      p[n++] = t;
   }

   for (i = 0; i < n; i++) add (scan, p[i]);
   the_scan = scan;
   progress_calls = same_groups_seen = 0;
   g_assert_true (gimv_dupl_scan_run (scan, NULL, on_progress, NULL));
   g_assert_cmpuint (progress_calls, >=, 4);
   /* a + a_copy, c + c2 + sub_c3 */
   g_assert_cmpuint (same_groups_seen, ==, 2);

   groups = gimv_dupl_scan_get_groups (scan);
   g_assert_cmpuint (groups->len, ==, 3);

   /* in the order of the kinds */
   g = g_ptr_array_index (groups, 0);
   g_assert_cmpint (g->kind, ==, GIMV_DUPL_SAME);
   g = group_of (groups, "c2.png");   /* the oldest of the copies */
   g_assert_nonnull (g);
   g_assert_cmpint (g->kind, ==, GIMV_DUPL_SAME);
   g_assert_true (g->suggested);
   g_assert_cmpuint (g->members->len, ==, 3);
   g_assert_cmpfloat (g_array_index (g->sim, gfloat, 2), ==, 1.0);

   g = group_of (groups, "a.png");
   g_assert_nonnull (g);
   g_assert_cmpint (g->kind, ==, GIMV_DUPL_RESIZED);
   g_assert_true (g->suggested);
   g_assert_cmpuint (g->members->len, ==, 3);
   g_assert_cmpstr (base (g, 1), ==, "a_copy.png");
   g_assert_cmpstr (base (g, 2), ==, "a_small.png");
   {
      GimvDuplItem *a = g_ptr_array_index (g->members, 0);
      GimvDuplItem *s = g_ptr_array_index (g->members, 2);
      g_assert_cmpint (a->width, ==, 200);
      g_assert_cmpint (a->height, ==, 100);
      g_assert_cmpint (s->width, ==, 100);
      g_assert_cmpint (s->height, ==, 50);
      g_assert_cmpstr (((GimvDuplItem *) g_ptr_array_index (g->members, 1))->md5, ==, a->md5);
   }
   g_assert_cmpfloat (g_array_index (g->sim, gfloat, 1), ==, 1.0);
   g_assert_cmpfloat (g_array_index (g->sim, gfloat, 2), >, 0.95);
   g_assert_cmpfloat (g_array_index (g->sim, gfloat, 2), <, 1.0);

   g = g_ptr_array_index (groups, 2);
   g_assert_cmpint (g->kind, ==, GIMV_DUPL_SIMILAR);
   g_assert_false (g->suggested);
   g_assert_cmpuint (g->members->len, ==, 2);

   g_assert_null (group_of (groups, "rings.png"));
   g_assert_null (group_of (groups, "diag.jpg"));

   gimv_dupl_scan_free (scan);
   for (i = 0; i < n; i++) { g_unlink (p[i]); g_free (p[i]); }
}


/* a picture saved again in the same size (re-compressed): similar, not
 * the same picture resized */
static void
test_same_size_not_resized (void)
{
   GimvDuplScan *scan = gimv_dupl_scan_new (0.95);
   gchar *a = save ("r.png", picture (PAT_DIAG, 200, 150, FALSE), 0);
   gchar *b = save ("r.jpg", picture (PAT_DIAG, 200, 150, FALSE), 0);
   gchar *c = save ("r_half.png", picture (PAT_DIAG, 100, 75, FALSE), 0);
   GPtrArray *groups;
   GimvDuplGroup *g;

   add (scan, a); add (scan, b); add (scan, c);
   g_assert_true (gimv_dupl_scan_run (scan, NULL, NULL, NULL));
   groups = gimv_dupl_scan_get_groups (scan);
   g_assert_cmpuint (groups->len, ==, 1);
   g = g_ptr_array_index (groups, 0);
   g_assert_cmpuint (g->members->len, ==, 3);
   g_assert_cmpint (g->kind, ==, GIMV_DUPL_SIMILAR);
   g_assert_false (g->suggested);
   gimv_dupl_scan_free (scan);
   g_unlink (a); g_unlink (b); g_unlink (c);
   g_free (a); g_free (b); g_free (c);
}


/* a different shape is not the same picture resized */
static void
test_other_shape (void)
{
   GimvDuplScan *scan = gimv_dupl_scan_new (0.90);
   gchar *a = save ("s.png", picture (PAT_HGRAD, 200, 100, FALSE), 0);
   gchar *b = save ("s_square.png", picture (PAT_HGRAD, 100, 100, FALSE), 0);
   GPtrArray *groups;

   add (scan, a); add (scan, b);
   g_assert_true (gimv_dupl_scan_run (scan, NULL, NULL, NULL));
   groups = gimv_dupl_scan_get_groups (scan);
   g_assert_cmpuint (groups->len, ==, 1);
   g_assert_cmpint (((GimvDuplGroup *) g_ptr_array_index (groups, 0))->kind,
                    ==, GIMV_DUPL_SIMILAR);
   gimv_dupl_scan_free (scan);
   g_unlink (a); g_unlink (b); g_free (a); g_free (b);
}


/* cancelled: FALSE, and what was found so far */
static void
test_cancel (void)
{
   GimvDuplScan *scan = gimv_dupl_scan_new (0.95);
   GCancellable *cancel = g_cancellable_new ();
   gchar *a = save ("k.png", picture (PAT_CHECK, 64, 64, FALSE), 0);
   gchar *b = copy (a, "k2.png", 0);

   add (scan, a); add (scan, b);
   g_cancellable_cancel (cancel);
   g_assert_false (gimv_dupl_scan_run (scan, cancel, NULL, NULL));
   g_assert_nonnull (gimv_dupl_scan_get_groups (scan));
   g_object_unref (cancel);
   gimv_dupl_scan_free (scan);
   g_unlink (a); g_unlink (b); g_free (a); g_free (b);
}


/* nothing, one: no groups */
static void
test_empty (void)
{
   GimvDuplScan *scan = gimv_dupl_scan_new (0.95);
   gchar *a;

   g_assert_true (gimv_dupl_scan_run (scan, NULL, NULL, NULL));
   g_assert_cmpuint (gimv_dupl_scan_get_groups (scan)->len, ==, 0);
   gimv_dupl_scan_free (scan);

   scan = gimv_dupl_scan_new (0.95);
   a = save ("one.png", picture (PAT_CHECK, 64, 64, FALSE), 0);
   add (scan, a);
   gimv_dupl_scan_add (scan, "/nonexistent/x.png", 100, 0, NULL);
   g_assert_true (gimv_dupl_scan_run (scan, NULL, NULL, NULL));
   g_assert_cmpuint (gimv_dupl_scan_get_groups (scan)->len, ==, 0);
   gimv_dupl_scan_free (scan);
   g_unlink (a); g_free (a);
}


/* the signature: the same picture 1.0, black and white 0.0 */
static void
test_sig (void)
{
   guint8 a[GIMV_DUPL_SIG_SIZE], b[GIMV_DUPL_SIG_SIZE];
   GdkPixbuf *black = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 40, 20);
   GdkPixbuf *white = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 10, 70);

   gdk_pixbuf_fill (black, 0x000000ff);
   gdk_pixbuf_fill (white, 0xffffffff);
   gimv_dupl_sig_fill (a, black);
   gimv_dupl_sig_fill (b, white);
   g_assert_cmpfloat (gimv_dupl_sig_compare (a, a), ==, 1.0);
   g_assert_cmpfloat (gimv_dupl_sig_compare (a, b), ==, 0.0);
   g_assert_cmpfloat (gimv_dupl_sig_compare (a, NULL), ==, 0.0);
   g_object_unref (black);
   g_object_unref (white);
}


int
main (int argc, char *argv[])
{
   gint r;

   g_test_init (&argc, &argv, NULL);
   dir = g_dir_make_tmp ("gimv-test-dupl-XXXXXX", NULL);
   g_assert_nonnull (dir);
   g_test_add_func ("/dupl/kinds", test_kinds);
   g_test_add_func ("/dupl/same-size-not-resized", test_same_size_not_resized);
   g_test_add_func ("/dupl/other-shape", test_other_shape);
   g_test_add_func ("/dupl/cancel", test_cancel);
   g_test_add_func ("/dupl/empty", test_empty);
   g_test_add_func ("/dupl/sig", test_sig);
   r = g_test_run ();
   g_rmdir (dir);
   g_free (dir);
   return r;
}
