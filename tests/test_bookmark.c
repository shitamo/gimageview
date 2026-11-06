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
 * The bookmarks of images (src/gimv_bookmark.c): the file they are kept
 * in, the notes, the order, and the bookmarks following a file or a
 * folder that is moved.
 */

#include <string.h>
#include <glib/gstdio.h>

#include "gimv_test.h"
#include "gimv_bookmark.h"

static gchar *dir;
static gchar *file;


static void
setup (void)
{
   dir = g_dir_make_tmp ("gimv-bookmark-XXXXXX", NULL);
   g_assert_nonnull (dir);
   file = g_build_filename (dir, "bookmarks", NULL);
   gimv_bookmark_set_file (file);
}


static void
teardown (void)
{
   g_remove (file);
   g_rmdir (dir);
   g_clear_pointer (&file, g_free);
   g_clear_pointer (&dir, g_free);
   gimv_bookmark_set_file (NULL);
}


static void
test_escape (void)
{
   static const gchar *texts[] = {
      "", "plain", "tab\there", "two\nlines\r\n", "back\\slash", "\\t not a tab",
      "日本語のメモ", "end\\",
   };
   guint i;

   for (i = 0; i < G_N_ELEMENTS (texts); i++) {
      gchar *e = gimv_bookmark_escape (texts[i]);
      gchar *u = gimv_bookmark_unescape (e);
      g_assert_null (strchr (e, '\t'));
      g_assert_null (strchr (e, '\n'));
      g_assert_cmpstr (u, ==, texts[i]);
      g_free (e);
      g_free (u);
   }
}


static void
test_add_note_remove (void)
{
   GimvBookmark *bm;

   setup ();
   g_assert_false (gimv_bookmark_has ("/pics/a.jpg"));
   g_assert_true (gimv_bookmark_add ("/pics/a.jpg"));
   /* a second time: already there */
   g_assert_false (gimv_bookmark_add ("/pics/a.jpg"));
   g_assert_true (gimv_bookmark_has ("/pics/a.jpg"));

   bm = gimv_bookmark_get ("/pics/a.jpg");
   g_assert_nonnull (bm);
   g_assert_null (bm->note);
   g_assert_cmpint (bm->added, >, 0);
   gimv_bookmark_free (bm);

   gimv_bookmark_set_note ("/pics/a.jpg", "  the red one\twith\na tab  ");
   bm = gimv_bookmark_get ("/pics/a.jpg");
   g_assert_cmpstr (bm->note, ==, "the red one\twith\na tab");
   gimv_bookmark_free (bm);

   /* empty: no note */
   gimv_bookmark_set_note ("/pics/a.jpg", "   ");
   bm = gimv_bookmark_get ("/pics/a.jpg");
   g_assert_null (bm->note);
   gimv_bookmark_free (bm);

   /* no note for a path without a bookmark */
   gimv_bookmark_set_note ("/pics/none.jpg", "x");
   g_assert_false (gimv_bookmark_has ("/pics/none.jpg"));

   gimv_bookmark_remove ("/pics/a.jpg");
   g_assert_false (gimv_bookmark_has ("/pics/a.jpg"));
   g_assert_cmpuint (gimv_bookmark_count (), ==, 0);
   teardown ();
}


/* written at each change, read back as they were */
static void
test_file (void)
{
   gchar *contents = NULL;
   GimvBookmark *bm;

   setup ();
   gimv_bookmark_add ("/pics/with\ttab.jpg");
   gimv_bookmark_set_note ("/pics/with\ttab.jpg", "line 1\nline 2 \\ end");
   gimv_bookmark_add ("/写真/京都.jpg");

   g_assert_true (g_file_get_contents (file, &contents, NULL, NULL));
   g_assert_true (g_str_has_prefix (contents, "# GImageView bookmarks\n"));
   /* one line each (and the header) */
   {
      gchar **lines = g_strsplit (contents, "\n", -1);
      g_assert_cmpuint (g_strv_length (lines), ==, 4);   /* the last is "" */
      g_strfreev (lines);
   }
   g_free (contents);

   /* as another gimv starting */
   gimv_bookmark_set_file (file);
   g_assert_cmpuint (gimv_bookmark_count (), ==, 2);
   bm = gimv_bookmark_get ("/pics/with\ttab.jpg");
   g_assert_nonnull (bm);
   g_assert_cmpstr (bm->note, ==, "line 1\nline 2 \\ end");
   gimv_bookmark_free (bm);
   g_assert_true (gimv_bookmark_has ("/写真/京都.jpg"));
   teardown ();
}


/* another gimv wrote the file: read again */
static void
test_changed_outside (void)
{
   setup ();
   gimv_bookmark_add ("/pics/a.jpg");
   /* a later time, as the clock would give it */
   g_usleep (20000);
   g_assert_true (g_file_set_contents (file,
                                       "# GImageView bookmarks\n"
                                       "1700000000\t/pics/b.jpg\tfrom another\n", -1, NULL));
   g_assert_false (gimv_bookmark_has ("/pics/a.jpg"));
   g_assert_true (gimv_bookmark_has ("/pics/b.jpg"));
   teardown ();
}


static void
test_order (void)
{
   GPtrArray *list;

   setup ();
   g_assert_true (g_file_set_contents (file,
                                       "# GImageView bookmarks\n"
                                       "100\t/old.jpg\t\n"
                                       "300\t/new.jpg\tn\n"
                                       "200\t/mid.jpg\t\n"
                                       "broken line\n"
                                       "\n", -1, NULL));
   gimv_bookmark_set_file (file);
   list = gimv_bookmark_list ();
   g_assert_cmpuint (list->len, ==, 3);
   g_assert_cmpstr (((GimvBookmark *) g_ptr_array_index (list, 0))->path, ==, "/new.jpg");
   g_assert_cmpstr (((GimvBookmark *) g_ptr_array_index (list, 1))->path, ==, "/mid.jpg");
   g_assert_cmpstr (((GimvBookmark *) g_ptr_array_index (list, 2))->path, ==, "/old.jpg");
   g_assert_cmpstr (((GimvBookmark *) g_ptr_array_index (list, 0))->note, ==, "n");
   g_assert_null (((GimvBookmark *) g_ptr_array_index (list, 1))->note);
   g_ptr_array_unref (list);
   teardown ();
}


static void
test_move (void)
{
   GimvBookmark *bm;

   setup ();
   gimv_bookmark_add ("/pics/a.jpg");
   gimv_bookmark_set_note ("/pics/a.jpg", "kept");
   gimv_bookmark_add ("/pics/trip/b.jpg");
   gimv_bookmark_add ("/pics/trip/sub/c.jpg");
   gimv_bookmark_add ("/pics/trip2/d.jpg");   /* not below "trip" */
   gimv_bookmark_add ("/pics/Trip/e.jpg");    /* another case */

   /* a file */
   gimv_bookmark_move ("/pics/a.jpg", "/pics/renamed.jpg");
   g_assert_false (gimv_bookmark_has ("/pics/a.jpg"));
   bm = gimv_bookmark_get ("/pics/renamed.jpg");
   g_assert_nonnull (bm);
   g_assert_cmpstr (bm->note, ==, "kept");
   gimv_bookmark_free (bm);

   /* a folder */
   gimv_bookmark_move ("/pics/trip", "/archive/2025 trip");
   g_assert_true (gimv_bookmark_has ("/archive/2025 trip/b.jpg"));
   g_assert_true (gimv_bookmark_has ("/archive/2025 trip/sub/c.jpg"));
   g_assert_true (gimv_bookmark_has ("/pics/trip2/d.jpg"));
   g_assert_true (gimv_bookmark_has ("/pics/Trip/e.jpg"));
   g_assert_cmpuint (gimv_bookmark_count (), ==, 5);

   /* onto a bookmarked path: that one is kept */
   gimv_bookmark_set_note ("/pics/trip2/d.jpg", "d");
   gimv_bookmark_move ("/pics/renamed.jpg", "/pics/trip2/d.jpg");
   bm = gimv_bookmark_get ("/pics/trip2/d.jpg");
   g_assert_cmpstr (bm->note, ==, "d");
   gimv_bookmark_free (bm);
   g_assert_cmpuint (gimv_bookmark_count (), ==, 4);

   /* it was written */
   gimv_bookmark_set_file (file);
   g_assert_true (gimv_bookmark_has ("/archive/2025 trip/sub/c.jpg"));
   teardown ();
}


static void
cb_changed (gpointer data)
{
   (*(gint *) data)++;
}


static void
test_watch (void)
{
   gint n = 0;
   guint id;

   setup ();
   id = gimv_bookmark_add_watch (cb_changed, &n);
   gimv_bookmark_add ("/a.jpg");
   gimv_bookmark_add ("/a.jpg");            /* nothing changed */
   gimv_bookmark_set_note ("/a.jpg", "x");
   gimv_bookmark_set_note ("/a.jpg", "x");  /* nothing changed */
   gimv_bookmark_move ("/b", "/c");         /* nothing there */
   gimv_bookmark_remove ("/a.jpg");
   g_assert_cmpint (n, ==, 3);
   gimv_bookmark_remove_watch (id);
   gimv_bookmark_add ("/a.jpg");
   g_assert_cmpint (n, ==, 3);
   teardown ();
}


static void
test_tooltip (void)
{
   gchar *t;

   setup ();
   g_assert_null (gimv_bookmark_tooltip ("/a.jpg"));
   gimv_bookmark_add ("/a.jpg");
   t = gimv_bookmark_tooltip ("/a.jpg");
   g_assert_true (g_str_has_prefix (t, "★ "));
   g_free (t);
   gimv_bookmark_set_note ("/a.jpg", "the note");
   t = gimv_bookmark_tooltip ("/a.jpg");
   g_assert_cmpstr (t, ==, "★ the note");
   g_free (t);
   teardown ();
}


int
main (int argc, char *argv[])
{
   g_test_init (&argc, &argv, NULL);

   g_test_add_func ("/bookmark/escape", test_escape);
   g_test_add_func ("/bookmark/add-note-remove", test_add_note_remove);
   g_test_add_func ("/bookmark/file", test_file);
   g_test_add_func ("/bookmark/changed-outside", test_changed_outside);
   g_test_add_func ("/bookmark/order", test_order);
   g_test_add_func ("/bookmark/move", test_move);
   g_test_add_func ("/bookmark/watch", test_watch);
   g_test_add_func ("/bookmark/tooltip", test_tooltip);

   return g_test_run ();
}
