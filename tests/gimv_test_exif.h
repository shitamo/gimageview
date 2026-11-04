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
 * Test images: a JPEG (gdk-pixbuf) with an EXIF block (libexif) holding
 * the orientation, the date taken and the camera, as cameras write them.
 */

#ifndef __GIMV_TEST_EXIF_H__
#define __GIMV_TEST_EXIF_H__

#include <stdlib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <libexif/exif-data.h>
#include <libexif/exif-utils.h>

#include "gimv_test.h"


/* an entry of the type of the tag, in its IFD */
static inline ExifEntry *
gimv_test_exif_entry (ExifData *ed, ExifIfd ifd, ExifTag tag)
{
   ExifEntry *e = exif_entry_new ();

   exif_content_add_entry (ed->ifd[ifd], e);
   exif_entry_initialize (e, tag);
   exif_entry_unref (e);
   return e;
}


static inline void
gimv_test_exif_ascii (ExifData *ed, ExifIfd ifd, ExifTag tag, const gchar *value)
{
   ExifEntry *e = exif_entry_new ();
   gsize len = strlen (value) + 1;

   exif_content_add_entry (ed->ifd[ifd], e);
   e->tag = tag;
   e->format = EXIF_FORMAT_ASCII;
   e->components = len;
   e->size = len;
   e->data = malloc (len);              /* freed by libexif (free ()) */
   memcpy (e->data, value, len);
   exif_entry_unref (e);
}


/*
 * A w x h JPEG.  orientation: the EXIF Orientation, 0 for none; date:
 * DateTimeOriginal ("YYYY:MM:DD HH:MM:SS") or NULL; make, model: or NULL.
 * No EXIF block at all when all of them are none.  The pixels: red to the
 * right, green down.
 */
static inline GByteArray *
gimv_test_jpeg (gint w, gint h, gint orientation, const gchar *date,
                const gchar *make, const gchar *model)
{
   GdkPixbuf *pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, w, h);
   GByteArray *out = g_byte_array_new ();
   gchar *jpeg = NULL;
   gsize jpeg_len = 0;
   GError *error = NULL;
   gint x, y;

   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++) {
         guchar *p = gdk_pixbuf_get_pixels (pixbuf)
            + y * gdk_pixbuf_get_rowstride (pixbuf) + x * 3;
         p[0] = w > 1 ? x * 255 / (w - 1) : 0;
         p[1] = h > 1 ? y * 255 / (h - 1) : 0;
         p[2] = 128;
      }
   gdk_pixbuf_save_to_buffer (pixbuf, &jpeg, &jpeg_len, "jpeg", &error, "quality", "95", NULL);
   g_assert_no_error (error);
   g_object_unref (pixbuf);
   g_assert_cmpuint (jpeg_len, >, 4);
   g_assert_cmpint ((guchar) jpeg[0], ==, 0xff);
   g_assert_cmpint ((guchar) jpeg[1], ==, 0xd8);

   g_byte_array_append (out, (const guint8 *) jpeg, 2);          /* SOI */

   if (orientation || date || make || model) {
      ExifData *ed = exif_data_new ();
      guchar *d = NULL;
      guint ds = 0;

      exif_data_set_byte_order (ed, EXIF_BYTE_ORDER_INTEL);
      if (orientation) {
         ExifEntry *e = gimv_test_exif_entry (ed, EXIF_IFD_0, EXIF_TAG_ORIENTATION);
         exif_set_short (e->data, EXIF_BYTE_ORDER_INTEL, orientation);
      }
      if (make)  gimv_test_exif_ascii (ed, EXIF_IFD_0, EXIF_TAG_MAKE, make);
      if (model) gimv_test_exif_ascii (ed, EXIF_IFD_0, EXIF_TAG_MODEL, model);
      if (date)  gimv_test_exif_ascii (ed, EXIF_IFD_EXIF, EXIF_TAG_DATE_TIME_ORIGINAL, date);
      exif_data_save_data (ed, &d, &ds);
      g_assert_nonnull (d);
      g_assert_cmpuint (ds + 2, <, 65536);

      gimv_test_put8 (out, 0xff);                                   /* APP1 */
      gimv_test_put8 (out, 0xe1);
      gimv_test_put8 (out, (ds + 2) >> 8);
      gimv_test_put8 (out, (ds + 2) & 0xff);
      g_byte_array_append (out, d, ds);
      free (d);
      exif_data_unref (ed);
   }

   g_byte_array_append (out, (const guint8 *) jpeg + 2, jpeg_len - 2);
   g_free (jpeg);
   return out;
}


static inline void
gimv_test_write_jpeg (const gchar *path, gint w, gint h, gint orientation,
                      const gchar *date, const gchar *make, const gchar *model)
{
   GByteArray *a = gimv_test_jpeg (w, h, orientation, date, make, model);
   gimv_test_write (path, a->data, a->len);
   g_byte_array_unref (a);
}

#endif /* __GIMV_TEST_EXIF_H__ */
