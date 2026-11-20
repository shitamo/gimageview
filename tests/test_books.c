/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * The tests of src/gimv_books.c: where the books were left.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "gimv_books.h"


static void
test_books (void)
{
   gchar *dir = g_dir_make_tmp ("gimv-books-XXXXXX", NULL);
   gchar *file = g_build_filename (dir, "books", NULL);
   gchar *page, *layout;

   gimv_books_set_file (file);
   g_assert_false (gimv_books_get ("/a/book", &page, &layout));
   g_assert_null (page);

   /* a folder ('/' at the end or not), an archive; names with [ ] */
   gimv_books_set ("/a/book/", "p10.png", "Two Pages");
   gimv_books_set ("/a/[x] vol 1.zip", "vol1/[x] 003.jpg", "Four Pages");
   g_assert_true (gimv_books_get ("/a/book", &page, &layout));
   g_assert_cmpstr (page, ==, "p10.png");
   g_assert_cmpstr (layout, ==, "Two Pages");
   g_free (page); g_free (layout);

   /* written, read again */
   gimv_books_save ();
   gimv_books_set_file (file);
   g_assert_true (gimv_books_get ("/a/[x] vol 1.zip", &page, &layout));
   g_assert_cmpstr (page, ==, "vol1/[x] 003.jpg");
   g_assert_cmpstr (layout, ==, "Four Pages");
   g_free (page); g_free (layout);

   /* on, then forgotten */
   gimv_books_set ("/a/book", "p12.png", NULL);
   g_assert_true (gimv_books_get ("/a/book/", &page, &layout));
   g_assert_cmpstr (page, ==, "p12.png");
   g_assert_cmpstr (layout, ==, "Two Pages");
   g_free (page); g_free (layout);
   gimv_books_forget ("/a/book");
   g_assert_false (gimv_books_get ("/a/book", NULL, NULL));
   gimv_books_save ();
   gimv_books_set_file (file);
   g_assert_false (gimv_books_get ("/a/book", NULL, NULL));
   g_assert_true (gimv_books_get ("/a/[x] vol 1.zip", NULL, NULL));

   {
      gchar *b, *p;
      gimv_books_split ("/a/b/c.jpg", &b, &p);
      g_assert_cmpstr (b, ==, "/a/b");
      g_assert_cmpstr (p, ==, "c.jpg");
      g_free (b); g_free (p);
   }

   gimv_books_set_file (NULL);
   g_unlink (file);
   g_rmdir (dir);
   g_free (file);
   g_free (dir);
}


static void
test_max (void)
{
   gchar *dir = g_dir_make_tmp ("gimv-books-XXXXXX", NULL);
   gchar *file = g_build_filename (dir, "books", NULL);
   gint i;

   gimv_books_set_file (file);
   for (i = 0; i < GIMV_BOOKS_MAX + 5; i++) {
      gchar *b = g_strdup_printf ("/books/%d", i);
      gimv_books_set (b, "1.jpg", NULL);
      g_free (b);
   }
   gimv_books_save ();
   gimv_books_set_file (file);
   /* the most recent kept (all of the same second: some of them) */
   {
      gint n = 0;
      for (i = 0; i < GIMV_BOOKS_MAX + 5; i++) {
         gchar *b = g_strdup_printf ("/books/%d", i);
         if (gimv_books_get (b, NULL, NULL)) n++;
         g_free (b);
      }
      g_assert_cmpint (n, ==, GIMV_BOOKS_MAX);
   }
   gimv_books_set_file (NULL);
   g_unlink (file);
   g_rmdir (dir);
   g_free (file);
   g_free (dir);
}


int
main (int argc, char **argv)
{
   g_test_init (&argc, &argv, NULL);

   g_test_add_func ("/books/set-get", test_books);
   g_test_add_func ("/books/max", test_max);

   return g_test_run ();
}
