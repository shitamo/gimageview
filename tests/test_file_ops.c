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

/* copying and moving files (src/gfileutil.c): the times kept */

#include <string.h>
#include <utime.h>
#include <glib/gstdio.h>

#include "gimv_test.h"
#include "gimv_test_core.h"
#include "gfileutil.h"
#include "gtkutils.h"

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


int
main (int argc, char *argv[])
{
   gint ret;

   g_test_init (&argc, &argv, NULL);
   home = gimv_test_core_init ();

   g_test_add_func ("/file-ops/copy-keeps-time", test_copy_keeps_time);
   g_test_add_func ("/file-ops/move-keeps-time", test_move_keeps_time);

   ret = g_test_run ();
   gimv_test_core_finish ();
   return ret;
}
