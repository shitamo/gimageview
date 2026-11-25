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

/* copying and moving files (src/gfileutil.c): the times kept; renaming
   several files (src/gimv_rename_files.c): what keeps a name, the order,
   nothing overwritten, the bookmark going with the file */

#include <string.h>
#include <utime.h>
#include <glib/gstdio.h>

#include "gimv_test.h"
#include "gimv_test_core.h"
#include "gfileutil.h"
#include "gtkutils.h"
#include "gimv_bookmark.h"
#include "gimv_image_info.h"
#include "gimv_rename_files.h"
#include "gimv_thumb.h"
#include "gimv_thumb_view.h"

#define OLD_TIME 692038400       /* 1991-12-05 */

static const gchar *home;


static gchar *
make_old_file (const gchar *dir, const gchar *name)
{
   gchar *path = g_build_filename (dir, name, NULL);
   struct utimbuf ut = { OLD_TIME, OLD_TIME };

   g_assert_true (g_file_set_contents (path, "MAKI02  data", -1, NULL));
   g_assert_cmpint (utime (path, &ut), ==, 0);
   return path;
}


static void
assert_old (const gchar *path)
{
   GStatBuf st;

   g_assert_cmpint (g_stat (path, &st), ==, 0);
   g_assert_cmpint (st.st_mtime, ==, OLD_TIME);
}


static void
test_copy_keeps_time (void)
{
   gchar *src_dir = g_build_filename (home, "copy-src", NULL);
   gchar *dest_dir = g_build_filename (home, "copy-dest", G_DIR_SEPARATOR_S, NULL);
   gchar *src, *dest, *contents = NULL;
   ConfirmType action = CONFIRM_YES;

   g_mkdir_with_parents (src_dir, 0700);
   g_mkdir_with_parents (dest_dir, 0700);
   src = make_old_file (src_dir, "a.mag");
   dest = g_build_filename (dest_dir, "a.mag", NULL);

   g_assert_true (copy_file (src, dest_dir, &action, FALSE, NULL));
   assert_old (dest);
   assert_old (src);
   g_assert_true (g_file_get_contents (dest, &contents, NULL, NULL));
   g_assert_cmpstr (contents, ==, "MAKI02  data");

   g_free (contents);
   g_free (src);
   g_free (dest);
   g_free (src_dir);
   g_free (dest_dir);
}


static void
test_move_keeps_time (void)
{
   gchar *src_dir = g_build_filename (home, "move-src", NULL);
   gchar *dest_dir = g_build_filename (home, "move-dest", G_DIR_SEPARATOR_S, NULL);
   gchar *src, *dest;
   ConfirmType action = CONFIRM_YES;

   g_mkdir_with_parents (src_dir, 0700);
   g_mkdir_with_parents (dest_dir, 0700);
   src = make_old_file (src_dir, "b.mag");
   dest = g_build_filename (dest_dir, "b.mag", NULL);

   g_assert_true (move_file (src, dest_dir, &action, FALSE, NULL));
   g_assert_false (g_file_test (src, G_FILE_TEST_EXISTS));
   assert_old (dest);

   g_free (src);
   g_free (dest);
   g_free (src_dir);
   g_free (dest_dir);
}


static gchar *
dir_with (const gchar *name, const gchar * const *files)
{
   gchar *dir = g_build_filename (home, name, NULL);
   guint i;

   g_mkdir_with_parents (dir, 0700);
   for (i = 0; files[i]; i++) {
      gchar *path = g_build_filename (dir, files[i], NULL);
      /* each file its own contents: which one is where */
      g_assert_true (g_file_set_contents (path, files[i], -1, NULL));
      g_free (path);
   }
   return dir;
}


static void
assert_contents (const gchar *dir, const gchar *name, const gchar *expect)
{
   gchar *path = g_build_filename (dir, name, NULL), *contents = NULL;

   g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
   g_assert_cmpstr (contents, ==, expect);
   g_free (contents);
   g_free (path);
}


static void
test_rename_check (void)
{
   static const gchar *files[] = { "a.jpg", "b.jpg", "c.jpg", "d.jpg", "e.jpg",
                                   "f.jpg", "g.jpg", "x.jpg", NULL };
   gchar *dir = dir_with ("check", files);
   /* a -> a (same), b -> (bad), c -> x (there, not renamed), d -> y and
      e -> y (twice), f -> g and g -> h (a chain: fine), and a swap */
   const gchar *names[][2] = {
      { "a.jpg", "a.jpg" }, { "b.jpg", NULL }, { "c.jpg", "x.jpg" },
      { "d.jpg", "y.jpg" }, { "e.jpg", "y.jpg" }, { "f.jpg", "g.jpg" },
      { "g.jpg", "h.jpg" },
   };
   const GimvRenameState expect[] = {
      GIMV_RENAME_SAME, GIMV_RENAME_BAD, GIMV_RENAME_EXISTS, GIMV_RENAME_TWICE,
      GIMV_RENAME_TWICE, GIMV_RENAME_OK, GIMV_RENAME_OK,
   };
   guint n = G_N_ELEMENTS (names), i;
   gchar **paths = g_new0 (gchar *, n), **news = g_new0 (gchar *, n);
   GimvRenameState *states = g_new (GimvRenameState, n);

   for (i = 0; i < n; i++) {
      paths[i] = g_build_filename (dir, names[i][0], NULL);
      news[i] = names[i][1] ? g_build_filename (dir, names[i][1], NULL) : NULL;
   }
   gimv_rename_plan_check ((const gchar * const *) paths, (const gchar * const *) news,
                           n, states);
   for (i = 0; i < n; i++) g_assert_cmpint (states[i], ==, expect[i]);

   /* a chain ending at a file that stays: f -> g, g -> a (a stays) */
   g_free (news[6]);
   news[6] = g_build_filename (dir, "a.jpg", NULL);
   gimv_rename_plan_check ((const gchar * const *) paths, (const gchar * const *) news,
                           n, states);
   g_assert_cmpint (states[6], ==, GIMV_RENAME_EXISTS);
   g_assert_cmpint (states[5], ==, GIMV_RENAME_EXISTS);

   /* a swap: f -> g, g -> f */
   g_free (news[6]);
   news[6] = g_build_filename (dir, "f.jpg", NULL);
   gimv_rename_plan_check ((const gchar * const *) paths, (const gchar * const *) news,
                           n, states);
   g_assert_cmpint (states[5], ==, GIMV_RENAME_ROUND);
   g_assert_cmpint (states[6], ==, GIMV_RENAME_ROUND);

   for (i = 0; i < n; i++) {
      g_free (paths[i]);
      g_free (news[i]);
   }
   g_free (paths);
   g_free (news);
   g_free (states);
   g_free (dir);
}


/* 1 -> 2, 2 -> 3, 3 -> 4 given in the order of the list: renamed from the
   end, nothing lost; and back */
static void
test_rename_run (void)
{
   static const gchar *files[] = { "p1.jpg", "p2.jpg", "p3.jpg", "keep.jpg", NULL };
   gchar *dir = dir_with ("run", files);
   const gchar *olds[] = { "p1.jpg", "p2.jpg", "p3.jpg", "p4.jpg" };
   gchar *from[4], *to[4];
   gboolean wanted[4] = { TRUE, TRUE, TRUE, TRUE }, done[4];
   guint i;

   for (i = 0; i < 3; i++) {
      from[i] = g_build_filename (dir, olds[i], NULL);
      to[i] = g_build_filename (dir, olds[i + 1], NULL);
   }
   /* and one onto a file there: not renamed, nothing overwritten */
   from[3] = g_build_filename (dir, "p9.jpg", NULL);
   to[3] = g_build_filename (dir, "keep.jpg", NULL);
   g_assert_true (g_file_set_contents (from[3], "p9", -1, NULL));

   g_assert_cmpuint (gimv_rename_plan_run ((const gchar * const *) from,
                                           (const gchar * const *) to, 4,
                                           wanted, done, NULL, NULL), ==, 3);
   g_assert_true (done[0] && done[1] && done[2] && !done[3]);
   assert_contents (dir, "p2.jpg", "p1.jpg");
   assert_contents (dir, "p3.jpg", "p2.jpg");
   assert_contents (dir, "p4.jpg", "p3.jpg");
   assert_contents (dir, "keep.jpg", "keep.jpg");
   assert_contents (dir, "p9.jpg", "p9");
   g_assert_false (g_file_test (from[0], G_FILE_TEST_EXISTS));

   /* back (Undo) */
   wanted[3] = FALSE;
   g_assert_cmpuint (gimv_rename_plan_run ((const gchar * const *) to,
                                           (const gchar * const *) from, 4,
                                           wanted, done, NULL, NULL), ==, 3);
   assert_contents (dir, "p1.jpg", "p1.jpg");
   assert_contents (dir, "p2.jpg", "p2.jpg");
   assert_contents (dir, "p3.jpg", "p3.jpg");
   g_assert_false (g_file_test (to[2], G_FILE_TEST_EXISTS));

   for (i = 0; i < 4; i++) {
      g_free (from[i]);
      g_free (to[i]);
   }
   g_free (dir);
}


/* the bookmark goes with a file renamed (twice, as Undo does, before the
   thumbnail's info is read again) */
static void
test_rename_bookmark (void)
{
   static const gchar *files[] = { "m.jpg", NULL };
   gchar *dir = dir_with ("bookmark", files);
   gchar *a = g_build_filename (dir, "m.jpg", NULL), *b = g_build_filename (dir, "n.jpg", NULL);
   GimvImageInfo *info = gimv_image_info_get (a);
   GimvThumb *thumb = gimv_thumb_new (info);

   g_assert_true (gimv_bookmark_add (a));
   g_assert_cmpint (g_rename (a, b), ==, 0);
   gimv_thumb_view_rename_extras_from (thumb, a, b);
   g_assert_false (gimv_bookmark_has (a));
   g_assert_true (gimv_bookmark_has (b));

   g_assert_cmpint (g_rename (b, a), ==, 0);
   gimv_thumb_view_rename_extras_from (thumb, b, a);
   g_assert_true (gimv_bookmark_has (a));
   g_assert_false (gimv_bookmark_has (b));

   g_object_unref (thumb);
   gimv_image_info_unref (info);
   g_free (a);
   g_free (b);
   g_free (dir);
}


int
main (int argc, char *argv[])
{
   gint ret;

   g_test_init (&argc, &argv, NULL);
   home = gimv_test_core_init ();

   g_test_add_func ("/file-ops/copy-keeps-time", test_copy_keeps_time);
   g_test_add_func ("/file-ops/move-keeps-time", test_move_keeps_time);
   g_test_add_func ("/file-ops/rename-check", test_rename_check);
   g_test_add_func ("/file-ops/rename-run", test_rename_run);
   g_test_add_func ("/file-ops/rename-bookmark", test_rename_bookmark);

   ret = g_test_run ();
   gimv_test_core_finish ();
   return ret;
}
