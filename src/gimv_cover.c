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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gimv_cover.h"

#include <string.h>

#include "fileutil.h"

#ifdef HAVE_LIBARCHIVE
#  include <archive.h>
#  include <archive_entry.h>
#endif


gchar *
gimv_cover_of_dir (const gchar *dir, GimvCoverIsImage is_image)
{
   GDir *d;
   const gchar *name;
   gchar *best = NULL, *path;

   g_return_val_if_fail (dir && is_image, NULL);

   d = g_dir_open (dir, 0, NULL);
   if (!d) return NULL;
   while ((name = g_dir_read_name (d))) {
      if (name[0] == '.' || !is_image (name)) continue;
      if (!best || fileutil_name_compare (name, best, TRUE, TRUE) < 0) {
         path = g_build_filename (dir, name, NULL);
         /* a file (not a folder named as an image) */
         if (g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
            g_free (best);
            best = g_strdup (name);
         }
         g_free (path);
      }
   }
   g_dir_close (d);
   if (!best) return NULL;
   path = g_build_filename (dir, best, NULL);
   g_free (best);
   return path;
}


gboolean
gimv_cover_archive_ext (const gchar *path)
{
   static const gchar *exts[] = {
      ".zip", ".cbz", ".rar", ".cbr", ".7z", ".cb7", ".lzh", ".lha", ".lzs",
      ".tar", ".cbt", ".tgz", ".tar.gz", ".tbz", ".tbz2", ".tar.bz2", ".txz",
      ".tar.xz", ".tar.zst", NULL
   };
   gchar *lower;
   gboolean ok = FALSE;
   guint i;

   if (!path) return FALSE;
   lower = g_ascii_strdown (path, -1);
   for (i = 0; exts[i] && !ok; i++)
      ok = g_str_has_suffix (lower, exts[i]);
   g_free (lower);
   return ok;
}


#ifdef HAVE_LIBARCHIVE

static struct archive *
cover_open (const gchar *path)
{
   struct archive *a = archive_read_new ();

   archive_read_support_filter_all (a);
   archive_read_support_format_all (a);
   /* the names of old LHA archives (Shift_JIS ...): only their order
      matters here, any byte will do */
   archive_read_set_format_option (a, "lha", "hdrcharset", "ISO-8859-1");
   if (archive_read_open_filename (a, path, 64 * 1024) != ARCHIVE_OK) {
      archive_read_free (a);
      return NULL;
   }
   return a;
}


/* the entries can be gone over without unpacking them */
static gboolean
cheap_listing (struct archive *a)
{
   int fmt = archive_format (a) & ARCHIVE_FORMAT_BASE_MASK;

   if (fmt == ARCHIVE_FORMAT_ZIP || fmt == ARCHIVE_FORMAT_LHA) return TRUE;
   if (fmt == ARCHIVE_FORMAT_TAR && archive_filter_count (a) <= 1) return TRUE;
   return FALSE;
}


static gboolean
over_limits (gint64 start, gint64 skipped)
{
   return g_get_monotonic_time () - start > (gint64) GIMV_COVER_MAX_SECONDS * G_USEC_PER_SEC
      || skipped > GIMV_COVER_MAX_BYTES;
}


/* the data of the entry read now */
static guchar *
read_entry (struct archive *a, struct archive_entry *entry, gsize *len)
{
   la_int64_t size = archive_entry_size_is_set (entry) ? archive_entry_size (entry) : -1;
   GByteArray *buf;
   guchar chunk[65536];
   la_ssize_t n;

   if (size > GIMV_COVER_MAX_IMAGE) return NULL;
   buf = g_byte_array_sized_new (size > 0 ? (guint) size : 65536);
   while ((n = archive_read_data (a, chunk, sizeof (chunk))) > 0) {
      g_byte_array_append (buf, chunk, n);
      if (buf->len > GIMV_COVER_MAX_IMAGE) {
         g_byte_array_free (buf, TRUE);
         return NULL;
      }
   }
   if (n < 0 || buf->len == 0) {
      g_byte_array_free (buf, TRUE);
      return NULL;
   }
   *len = buf->len;
   return g_byte_array_free (buf, FALSE);
}


gboolean
gimv_cover_of_archive (const gchar *path, GimvCoverIsImage is_image,
                       guchar **data, gsize *len, gchar **name)
{
   struct archive *a;
   struct archive_entry *entry;
   gint64 start = g_get_monotonic_time (), skipped = 0;
   gint index = 0, best_index = -1;
   gchar *best = NULL;
   gboolean by_name = FALSE, ok = FALSE;
   int ret;

   g_return_val_if_fail (path && is_image && data && len && name, FALSE);
   *data = NULL; *len = 0; *name = NULL;

   a = cover_open (path);
   if (!a) return FALSE;

   while ((ret = archive_read_next_header (a, &entry)) == ARCHIVE_OK || ret == ARCHIVE_WARN) {
      const gchar *n = archive_entry_pathname (entry);
      gboolean image = n && archive_entry_filetype (entry) == AE_IFREG && is_image (n);

      if (index == 0) by_name = cheap_listing (a);
      if (image && !by_name
          && (!*name || fileutil_name_compare (n, *name, TRUE, TRUE) < 0))
      {
         /* not listed without unpacking: the first by name of those gone
            over within the limits, read as it is found */
         gsize l = 0;
         guchar *d = read_entry (a, entry, &l);
         if (d) {
            g_free (*data);
            g_free (*name);
            *data = d;
            *len = l;
            *name = g_strdup (n);
            ok = TRUE;
         }
      }
      if (image && by_name && (!best || fileutil_name_compare (n, best, TRUE, TRUE) < 0)) {
         g_free (best);
         best = g_strdup (n);
         best_index = index;
      }
      if (archive_entry_size_is_set (entry)) skipped += archive_entry_size (entry);
      if (!by_name && over_limits (start, skipped)) break;
      if (by_name && g_get_monotonic_time () - start
                     > (gint64) GIMV_COVER_MAX_SECONDS * G_USEC_PER_SEC)
         break;
      index++;
   }
   archive_read_free (a);

   if (ok || !by_name || best_index < 0) {
      g_free (best);
      return ok;
   }

   /* by name: again, to that one (its place is known: no names compared,
      whatever their charset) */
   a = cover_open (path);
   if (!a) {
      g_free (best);
      return FALSE;
   }
   index = 0;
   while ((ret = archive_read_next_header (a, &entry)) == ARCHIVE_OK || ret == ARCHIVE_WARN) {
      if (index == best_index) {
         *data = read_entry (a, entry, len);
         if (*data) {
            *name = best;
            best = NULL;
            ok = TRUE;
         }
         break;
      }
      if (g_get_monotonic_time () - start > (gint64) GIMV_COVER_MAX_SECONDS * 2 * G_USEC_PER_SEC)
         break;
      index++;
   }
   archive_read_free (a);
   g_free (best);
   return ok;
}

#else /* HAVE_LIBARCHIVE */

gboolean
gimv_cover_of_archive (const gchar *path, GimvCoverIsImage is_image,
                       guchar **data, gsize *len, gchar **name)
{
   *data = NULL; *len = 0; *name = NULL;
   return FALSE;
}

#endif /* HAVE_LIBARCHIVE */
