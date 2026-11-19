/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * The tests of src/gimv_page_layout.c: the layouts of pages side by side.
 */

#include <glib.h>

#include "gimv_page_layout.h"


static void
test_parse (void)
{
   GPtrArray *a = gimv_page_layouts_parse ("2,1,ra,見開き;2,2,r,4 ページ;4,1,,a, b, c");
   GimvPageLayout *l;
   gchar *s;

   g_assert_cmpuint (a->len, ==, 3);
   l = g_ptr_array_index (a, 0);
   g_assert_cmpstr (l->name, ==, "見開き");
   g_assert_cmpint (l->cols, ==, 2);
   g_assert_cmpint (l->rows, ==, 1);
   g_assert_true (l->rtl);
   g_assert_true (l->first_alone);
   l = g_ptr_array_index (a, 1);
   g_assert_true (l->rtl);
   g_assert_false (l->first_alone);
   l = g_ptr_array_index (a, 2);
   g_assert_false (l->rtl);
   g_assert_cmpstr (l->name, ==, "a, b, c");     /* the name may have ',' */

   s = gimv_page_layouts_to_string (a);
   g_assert_cmpstr (s, ==, "2,1,ra,見開き;2,2,r,4 ページ;4,1,,a, b, c");
   g_free (s);
   g_ptr_array_unref (a);
}


static void
test_parse_bad (void)
{
   /* out of range, not numbers, too few fields, empty */
   GPtrArray *a = gimv_page_layouts_parse ("0,1,r,x;5,1,r,x;2,4,r,x;a,1,r,x;2,1,r;;2,1,r,ok");

   g_assert_cmpuint (a->len, ==, 1);
   g_assert_cmpstr (((GimvPageLayout *) g_ptr_array_index (a, 0))->name, ==, "ok");
   g_ptr_array_unref (a);

   a = gimv_page_layouts_parse (NULL);
   g_assert_cmpuint (a->len, ==, 0);
   g_ptr_array_unref (a);
}


static void
test_name (void)
{
   /* ';' would part the layouts */
   GimvPageLayout *l = gimv_page_layout_new (" a;b ", 9, 0, 2, 3);
   GPtrArray *a = g_ptr_array_new_with_free_func ((GDestroyNotify) gimv_page_layout_free);
   gchar *s;

   g_assert_cmpstr (l->name, ==, "ab");
   g_assert_cmpint (l->cols, ==, GIMV_PAGE_LAYOUT_MAX_COLS);
   g_assert_cmpint (l->rows, ==, 1);
   g_assert_true (l->rtl == TRUE && l->first_alone == TRUE);
   g_ptr_array_add (a, l);
   g_ptr_array_add (a, gimv_page_layout_copy (l));
   s = gimv_page_layouts_to_string (a);
   g_assert_cmpstr (s, ==, "4,1,ra,ab;4,1,ra,ab");
   g_free (s);
   g_ptr_array_unref (a);
}


static void
test_place (void)
{
   gint w[5] = { 30, 20, 30, 30, 10 }, x[5], y[5], tw, th;

   /* two pages from right to left: the first on the right */
   gimv_page_layout_place (2, TRUE, 2, w, 40, x, y, &tw, &th);
   g_assert_cmpint (tw, ==, 50);
   g_assert_cmpint (th, ==, 40);
   g_assert_cmpint (x[0], ==, 20);
   g_assert_cmpint (x[1], ==, 0);

   /* left to right */
   gimv_page_layout_place (2, FALSE, 2, w, 40, x, y, &tw, &th);
   g_assert_cmpint (x[0], ==, 0);
   g_assert_cmpint (x[1], ==, 30);

   /* 2 x 3, five pages: rows as wide as the widest (60), the last one
      short, on the side the pages start from */
   gimv_page_layout_place (2, TRUE, 5, w, 40, x, y, &tw, &th);
   g_assert_cmpint (tw, ==, 60);
   g_assert_cmpint (th, ==, 120);
   g_assert_cmpint (x[0], ==, 30);   g_assert_cmpint (y[0], ==, 0);
   g_assert_cmpint (x[1], ==, 10);   g_assert_cmpint (y[1], ==, 0);
   g_assert_cmpint (x[2], ==, 30);   g_assert_cmpint (y[2], ==, 40);
   g_assert_cmpint (x[3], ==, 0);    g_assert_cmpint (y[3], ==, 40);
   g_assert_cmpint (x[4], ==, 50);   g_assert_cmpint (y[4], ==, 80);

   gimv_page_layout_place (2, FALSE, 5, w, 40, x, y, &tw, &th);
   g_assert_cmpint (x[4], ==, 0);
   g_assert_cmpint (x[3], ==, 30);

   /* one page */
   gimv_page_layout_place (4, TRUE, 1, w, 40, x, y, &tw, &th);
   g_assert_cmpint (tw, ==, 30);
   g_assert_cmpint (th, ==, 40);
   g_assert_cmpint (x[0], ==, 0);
}


int
main (int argc, char **argv)
{
   g_test_init (&argc, &argv, NULL);

   g_test_add_func ("/page-layout/parse", test_parse);
   g_test_add_func ("/page-layout/parse-bad", test_parse_bad);
   g_test_add_func ("/page-layout/name", test_name);
   g_test_add_func ("/page-layout/place", test_place);

   return g_test_run ();
}
