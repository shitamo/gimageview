/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: the EXIF data of an image file of any format.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimv_exif.h"

#ifdef ENABLE_EXIF

#include <string.h>
#include <libexif/exif-utils.h>

#ifdef ENABLE_GEXIV2
#  include <gexiv2/gexiv2.h>

/* the EXIF data Exiv2 finds in the file (a TIFF structure) given to libexif
 * as the APP1 segment of a JPEG has it ("Exif\0\0" first) */
static ExifData *
exiv2_exif_data (const gchar *path)
{
   static gsize init = 0;
   static gboolean ok = FALSE;
   GExiv2Metadata *m;
   GBytes *bytes = NULL;
   ExifData *ed = NULL;

   if (g_once_init_enter (&init)) {
      ok = gexiv2_initialize ();
      g_once_init_leave (&init, 1);
   }
   if (!ok) return NULL;

   m = gexiv2_metadata_new ();
   if (gexiv2_metadata_open_path (m, path, NULL) && gexiv2_metadata_has_exif (m))
      bytes = gexiv2_metadata_get_exif_data (m, GEXIV2_BYTE_ORDER_LITTLE, NULL);
   g_object_unref (m);

   if (bytes) {
      gsize len;
      const guint8 *data = g_bytes_get_data (bytes, &len);

      if (data && len >= 8 && len < 0xfffff0) {
         guint8 *buf = g_malloc (len + 6);
         memcpy (buf, "Exif\0\0", 6);
         memcpy (buf + 6, data, len);
         /* as it is: not "fixed" with the tags the specification asks
          * for (a made up resolution of 72 dpi, ...) */
         ed = exif_data_new ();
         exif_data_unset_option (ed, EXIF_DATA_OPTION_FOLLOW_SPECIFICATION);
         exif_data_load_data (ed, buf, len + 6);
         g_free (buf);
      }
      g_bytes_unref (bytes);
   }
   return ed;
}
#endif /* ENABLE_GEXIV2 */


/* anything in it */
static gboolean
exif_data_has_some (ExifData *ed)
{
   gint i;

   if (ed->data && ed->size > 0) return TRUE;
   for (i = 0; i < EXIF_IFD_COUNT; i++)
      if (ed->ifd[i] && ed->ifd[i]->count > 0) return TRUE;
   return FALSE;
}


ExifData *
gimv_exif_data_new_from_file (const gchar *path)
{
   ExifData *ed;

   if (!path || !*path) return NULL;
   ed = exif_data_new_from_file (path);
   if (ed && exif_data_has_some (ed)) return ed;
   if (ed) exif_data_unref (ed);
#ifdef ENABLE_GEXIV2
   ed = exiv2_exif_data (path);
   if (ed && exif_data_has_some (ed)) return ed;
   if (ed) exif_data_unref (ed);
#endif
   return NULL;
}


/* the formats whose decoders turn the image as their EXIF data says */
static gboolean
decoder_turns (const gchar *path)
{
   static const gchar *const exts[] = {
      ".heic", ".heif", ".hif", ".avif", ".jxl",
   };
   gsize n = strlen (path);
   guint i;

   for (i = 0; i < G_N_ELEMENTS (exts); i++) {
      gsize l = strlen (exts[i]);
      if (n > l && !g_ascii_strcasecmp (path + n - l, exts[i])) return TRUE;
   }
   return FALSE;
}


gint
gimv_exif_orientation (const gchar *path)
{
   ExifData *ed;
   ExifEntry *entry;
   gint o = 1;

   if (!path || !*path || decoder_turns (path)) return 1;
   ed = gimv_exif_data_new_from_file (path);
   if (!ed) return 1;
   entry = exif_content_get_entry (ed->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
   if (entry && entry->format == EXIF_FORMAT_SHORT && entry->size >= 2)
      o = exif_get_short (entry->data, exif_data_get_byte_order (ed));
   exif_data_unref (ed);
   return o >= 1 && o <= 8 ? o : 1;
}

#endif /* ENABLE_EXIF */
