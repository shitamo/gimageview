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
 * The EXIF orientations of the thumbnails (src/gimv_orient.c): every
 * orientation, every pair of them, and every turning the user can give an
 * image in the image window after each of them, checked on the pixels of
 * a small image against turnings computed here from the EXIF definition.
 */

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "gimv_test.h"
#include "gimv_orient.h"

#define W 3
#define H 2


/* a W x H image whose pixels are all different */
static GdkPixbuf *
make_image (void)
{
   GdkPixbuf *p = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, W, H);
   guchar *px = gdk_pixbuf_get_pixels (p);
   gint rs = gdk_pixbuf_get_rowstride (p), x, y;

   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++) {
         guchar *d = px + y * rs + x * 3;
         d[0] = 10 + x;
         d[1] = 20 + y;
         d[2] = 30 + x * H + y;
      }
   return p;
}


static gboolean
same_pixels (GdkPixbuf *a, GdkPixbuf *b)
{
   gint w = gdk_pixbuf_get_width (a), h = gdk_pixbuf_get_height (a), y;
   gint n = gdk_pixbuf_get_n_channels (a);

   if (w != gdk_pixbuf_get_width (b) || h != gdk_pixbuf_get_height (b)
       || n != gdk_pixbuf_get_n_channels (b))
      return FALSE;
   for (y = 0; y < h; y++)
      if (memcmp (gdk_pixbuf_get_pixels (a) + y * gdk_pixbuf_get_rowstride (a),
                  gdk_pixbuf_get_pixels (b) + y * gdk_pixbuf_get_rowstride (b),
                  (gsize) w * n))
         return FALSE;
   return TRUE;
}


/*
 * The image as seen for the EXIF orientation o of the image as stored, by
 * the definition of the tag (where row 0 and column 0 of the stored image
 * are shown): 1 top left, 2 top right, 3 bottom right, 4 bottom left,
 * 5 left top, 6 right top, 7 right bottom, 8 left bottom.
 */
static GdkPixbuf *
exif_seen (GdkPixbuf *src, gint o)
{
   gint w = gdk_pixbuf_get_width (src), h = gdk_pixbuf_get_height (src);
   gboolean swap = o >= 5;
   GdkPixbuf *dst = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, swap ? h : w, swap ? w : h);
   gint x, y;

   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++) {
         gint X = 0, Y = 0;
         switch (o) {
         case 1: X = x;         Y = y;         break;
         case 2: X = w - 1 - x; Y = y;         break;
         case 3: X = w - 1 - x; Y = h - 1 - y; break;
         case 4: X = x;         Y = h - 1 - y; break;
         case 5: X = y;         Y = x;         break;
         case 6: X = h - 1 - y; Y = x;         break;
         case 7: X = h - 1 - y; Y = w - 1 - x; break;
         case 8: X = y;         Y = w - 1 - x; break;
         default: g_assert_not_reached ();
         }
         memcpy (gdk_pixbuf_get_pixels (dst) + Y * gdk_pixbuf_get_rowstride (dst) + X * 3,
                 gdk_pixbuf_get_pixels (src) + y * gdk_pixbuf_get_rowstride (src) + x * 3, 3);
      }
   return dst;
}


/* each orientation turns the image as EXIF says */
static void
test_pixbuf_turn (void)
{
   GdkPixbuf *img = make_image ();
   gint o;

   for (o = 1; o <= 8; o++) {
      GdkPixbuf *got = gimv_orient_pixbuf_turn (img, o);
      GdkPixbuf *want = exif_seen (img, o);
      g_assert_nonnull (got);
      if (!same_pixels (got, want))
         g_error ("orientation %d: the image is not turned as EXIF says", o);
      g_object_unref (got);
      g_object_unref (want);
   }

   /* anything else: as it is (a new reference to the same pixbuf) */
   {
      GdkPixbuf *got = gimv_orient_pixbuf_turn (img, 0);
      g_assert_true (got == img);
      g_object_unref (got);
      got = gimv_orient_pixbuf_turn (img, 9);
      g_assert_true (got == img);
      g_object_unref (got);
   }
   g_object_unref (img);
}


/* compose (a, b): a, then b; all 64 pairs */
static void
test_compose (void)
{
   GdkPixbuf *img = make_image ();
   gint a, b;

   for (a = 1; a <= 8; a++) {
      GdkPixbuf *pa = exif_seen (img, a);
      for (b = 1; b <= 8; b++) {
         GdkPixbuf *pab = exif_seen (pa, b);
         gint c = gimv_orient_compose (a, b);
         GdkPixbuf *pc = exif_seen (img, c);

         g_assert_cmpint (c, >=, 1);
         g_assert_cmpint (c, <=, 8);
         if (!same_pixels (pab, pc))
            g_error ("compose (%d, %d) = %d: not %d then %d", a, b, c, a, b);
         g_object_unref (pab);
         g_object_unref (pc);
      }
      g_object_unref (pa);
   }
   g_object_unref (img);

   /* 1 is no turning */
   for (a = 1; a <= 8; a++) {
      g_assert_cmpint (gimv_orient_compose (1, a), ==, a);
      g_assert_cmpint (gimv_orient_compose (a, 1), ==, a);
   }
   /* a few by name: 90 degrees clockwise twice is 180 degrees, four times
    * nothing; flipped twice is nothing */
   g_assert_cmpint (gimv_orient_compose (6, 6), ==, 3);
   g_assert_cmpint (gimv_orient_compose (3, 3), ==, 1);
   g_assert_cmpint (gimv_orient_compose (6, 8), ==, 1);
   g_assert_cmpint (gimv_orient_compose (2, 2), ==, 1);
   g_assert_cmpint (gimv_orient_compose (2, 4), ==, 3);
   g_assert_cmpint (gimv_orient_compose (2, 6), ==, 7);   /* flipped, then 90 cw */
   g_assert_cmpint (gimv_orient_compose (2, 8), ==, 5);
}


static void
test_inverse (void)
{
   static const gint expected[9] = { 0, 1, 2, 3, 4, 5, 8, 7, 6 };
   GdkPixbuf *img = make_image ();
   gint o;

   for (o = 1; o <= 8; o++) {
      gint inv = gimv_orient_inverse (o);
      GdkPixbuf *seen = exif_seen (img, o), *back;

      g_assert_cmpint (inv, ==, expected[o]);
      g_assert_cmpint (gimv_orient_compose (o, inv), ==, 1);
      g_assert_cmpint (gimv_orient_compose (inv, o), ==, 1);
      back = gimv_orient_pixbuf_turn (seen, inv);
      g_assert_true (same_pixels (back, img));
      g_object_unref (back);
      g_object_unref (seen);
   }
   g_object_unref (img);
}


/* the image window: turned over left and right (flip), then turned
 * (rotate 1: 90 degrees counterclockwise, 2: 180, 3: 90 clockwise), from
 * the image as stored */
static GdkPixbuf *
window_turn (GdkPixbuf *src, gint rotate, gboolean flip)
{
   GdkPixbuf *a = flip ? gdk_pixbuf_flip (src, TRUE) : g_object_ref (src), *b;
   static const GdkPixbufRotation rot[4] = {
      GDK_PIXBUF_ROTATE_NONE, GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE,
      GDK_PIXBUF_ROTATE_UPSIDEDOWN, GDK_PIXBUF_ROTATE_CLOCKWISE,
   };

   b = gdk_pixbuf_rotate_simple (a, rot[rotate]);
   g_object_unref (a);
   return b;
}


/* all 8 EXIF orientations x the 8 turnings of the image window: the
 * thumbnail (turned by EXIF) turned by user_turn shows what the image
 * window shows */
static void
test_user_turn (void)
{
   GdkPixbuf *img = make_image ();
   gint exif, rotate, flip;

   for (exif = 1; exif <= 8; exif++) {
      GdkPixbuf *thumb = gimv_orient_pixbuf_turn (img, exif);
      for (rotate = 0; rotate < 4; rotate++)
         for (flip = 0; flip < 2; flip++) {
            gint o = gimv_orient_user_turn (exif, rotate, flip);
            GdkPixbuf *shown = gimv_orient_pixbuf_turn (thumb, o);
            GdkPixbuf *want = window_turn (img, rotate, flip);
            if (!same_pixels (shown, want))
               g_error ("EXIF %d, rotate %d, flip %d: turned by %d, not as the "
                        "image window shows it", exif, rotate, flip, o);
            g_object_unref (shown);
            g_object_unref (want);
         }
      g_object_unref (thumb);
   }
   g_object_unref (img);

   /* nothing given: nothing to turn whatever the EXIF orientation */
   g_assert_cmpint (gimv_orient_user_turn (1, 0, FALSE), ==, 1);
   /* given nothing but the image has an orientation: it is undone (the
    * image window showed it as stored) */
   g_assert_cmpint (gimv_orient_user_turn (6, 0, FALSE), ==, 8);
}


/* the image window turns an image by its EXIF orientation with this table
 * (get_exif_rotation () in src/gimv_image_view.c: rotate, flip): turned
 * so, a thumbnail needs no more turning */
static void
test_image_view_table (void)
{
   static const struct { gint rotate; gboolean flip; } table[9] = {
      { 0, FALSE },
      { 0, FALSE }, { 0, TRUE }, { 2, FALSE }, { 2, TRUE },
      { 1, TRUE }, { 3, FALSE }, { 3, TRUE }, { 1, FALSE },
   };
   GdkPixbuf *img = make_image ();
   gint o;

   for (o = 1; o <= 8; o++) {
      GdkPixbuf *w = window_turn (img, table[o].rotate, table[o].flip);
      GdkPixbuf *e = exif_seen (img, o);
      g_assert_true (same_pixels (w, e));
      g_assert_cmpint (gimv_orient_user_turn (o, table[o].rotate, table[o].flip), ==, 1);
      g_object_unref (w);
      g_object_unref (e);
   }
   g_object_unref (img);
}


int
main (int argc, char *argv[])
{
   g_test_init (&argc, &argv, NULL);

   g_test_add_func ("/orient/pixbuf-turn", test_pixbuf_turn);
   g_test_add_func ("/orient/compose", test_compose);
   g_test_add_func ("/orient/inverse", test_inverse);
   g_test_add_func ("/orient/user-turn", test_user_turn);
   g_test_add_func ("/orient/image-view-table", test_image_view_table);

   return g_test_run ();
}
