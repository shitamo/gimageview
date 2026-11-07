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
 * src/gimv_exif.c: the EXIF data and the orientation of a JPEG (libexif)
 * and of the other formats (Exiv2).
 */

#include <string.h>
#include <glib/gstdio.h>
#include <gexiv2/gexiv2.h>

#include "gimv_test_exif.h"
#include "gimv_exif.h"


static gchar *root = NULL;


static gchar *
in_root (const gchar *name)
{
   return g_build_filename (root, name, NULL);
}


/* a 6 x 4 image of the type ("png", "tiff") at path, with the orientation
 * and the camera maker in its EXIF data (written by Exiv2) when o > 0 */
static void
write_image (const gchar *path, const gchar *type, gint o)
{
   GdkPixbuf *px = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 6, 4);
   GError *error = NULL;

   gdk_pixbuf_fill (px, 0x336699ff);
   g_assert_true (gdk_pixbuf_save (px, path, type, &error, NULL));
   g_assert_no_error (error);
   g_object_unref (px);
   if (o > 0) {
      GExiv2Metadata *m = gexiv2_metadata_new ();
      gchar v[4];

      g_snprintf (v, sizeof v, "%d", o);
      g_assert_true (gexiv2_metadata_open_path (m, path, &error));
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      gexiv2_metadata_set_tag_string (m, "Exif.Image.Orientation", v);
      gexiv2_metadata_set_tag_string (m, "Exif.Image.Make", "FUJIFILM");
      G_GNUC_END_IGNORE_DEPRECATIONS
      g_assert_true (gexiv2_metadata_save_file (m, path, &error));
      g_assert_no_error (error);
      g_object_unref (m);
   }
}


static gchar *
make_of (ExifData *ed)
{
   ExifEntry *e = exif_content_get_entry (ed->ifd[EXIF_IFD_0], EXIF_TAG_MAKE);
   gchar buf[64];

   if (!e) return NULL;
   return g_strdup (exif_entry_get_value (e, buf, sizeof buf));
}


/* a JPEG: libexif as before */
static void
test_jpeg (void)
{
   gchar *path = in_root ("o6.jpg");
   GByteArray *jpeg = gimv_test_jpeg (6, 4, 6, NULL, "Canon", NULL);
   ExifData *ed;
   gchar *make;

   g_assert_true (g_file_set_contents (path, (const gchar *) jpeg->data, jpeg->len, NULL));
   g_byte_array_unref (jpeg);
   ed = gimv_exif_data_new_from_file (path);
   g_assert_nonnull (ed);
   make = make_of (ed);
   g_assert_cmpstr (make, ==, "Canon");
   g_free (make);
   exif_data_unref (ed);
   g_assert_cmpint (gimv_exif_orientation (path), ==, 6);
   g_free (path);
}


/* PNG and TIFF: the EXIF data Exiv2 reads */
static void
test_other_formats (void)
{
   static const struct { const gchar *name, *type; gint o; } t[] = {
      { "o6.png",  "png",  6 },
      { "o8.tiff", "tiff", 8 },
      { "o3.png",  "png",  3 },
   };
   guint i;

   for (i = 0; i < G_N_ELEMENTS (t); i++) {
      gchar *path = in_root (t[i].name), *make;
      ExifData *ed;

      write_image (path, t[i].type, t[i].o);
      ed = gimv_exif_data_new_from_file (path);
      g_assert_nonnull (ed);
      make = make_of (ed);
      g_assert_cmpstr (make, ==, "FUJIFILM");
      g_free (make);
      exif_data_unref (ed);
      g_assert_cmpint (gimv_exif_orientation (path), ==, t[i].o);
      g_free (path);
   }
}


/* none: NULL, orientation 1 */
static void
test_none (void)
{
   gchar *png = in_root ("plain.png"), *missing = in_root ("missing.png");
   gchar *text = in_root ("text.png");

   write_image (png, "png", 0);
   g_assert_null (gimv_exif_data_new_from_file (png));
   g_assert_cmpint (gimv_exif_orientation (png), ==, 1);
   g_assert_null (gimv_exif_data_new_from_file (missing));
   g_assert_cmpint (gimv_exif_orientation (missing), ==, 1);
   g_assert_true (g_file_set_contents (text, "not an image", -1, NULL));
   g_assert_null (gimv_exif_data_new_from_file (text));
   g_assert_null (gimv_exif_data_new_from_file (NULL));
   g_assert_null (gimv_exif_data_new_from_file (""));
   g_free (png); g_free (missing); g_free (text);
}


/* HEIF, AVIF, JPEG XL: their decoders turn the image, the orientation in
 * the EXIF data is not to be done again (the EXIF data is still read) */
static void
test_decoder_turns (void)
{
   static const gchar *const names[] = { "x.heic", "x.HEIF", "x.avif", "x.jxl" };
   gchar *src = in_root ("turned.png");
   guint i;

   write_image (src, "png", 6);
   for (i = 0; i < G_N_ELEMENTS (names); i++) {
      gchar *path = in_root (names[i]), *data;
      gsize len;
      ExifData *ed;

      g_assert_true (g_file_get_contents (src, &data, &len, NULL));
      g_assert_true (g_file_set_contents (path, data, len, NULL));
      g_free (data);
      ed = gimv_exif_data_new_from_file (path);
      g_assert_nonnull (ed);
      exif_data_unref (ed);
      g_assert_cmpint (gimv_exif_orientation (path), ==, 1);
      g_free (path);
   }
   g_free (src);
}


int
main (int argc, char *argv[])
{
   gint r;

   g_test_init (&argc, &argv, NULL);
   g_assert_true (gexiv2_initialize ());
   root = g_dir_make_tmp ("gimv-test-exif-XXXXXX", NULL);
   g_assert_nonnull (root);
   g_test_add_func ("/exif/jpeg", test_jpeg);
   g_test_add_func ("/exif/other-formats", test_other_formats);
   g_test_add_func ("/exif/none", test_none);
   g_test_add_func ("/exif/decoder-turns", test_decoder_turns);
   r = g_test_run ();
   {
      gchar *cmd = g_strdup_printf ("rm -rf '%s'", root);
      if (system (cmd)) {}
      g_free (cmd);
   }
   g_free (root);
   return r;
}
