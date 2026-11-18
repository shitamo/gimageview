/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2026 shitamo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * Archive support with libarchive.
 *
 * Replaces the File Roller style plugins of 0.2.27 (lha-ext, rar-ext,
 * tar-ext, zip-ext), which ran lha, unzip, rar, tar ... and parsed their
 * output (which broke when lhasa changed its listing).  libarchive reads
 * the archives in-process.  Archives are only read (gimv never writes
 * them).
 *
 * FRArchive expects a command to queue work on its FRProcess and reports
 * "done" when the process finishes; list and extract do their work right
 * away and queue "true", so the callers see the usual asynchronous "done"
 * (they wait for it in a nested main loop).
 *
 * libarchive does not unpack the -lh1- method of LHarc 1.x (common in old
 * LZH archives); those members are unpacked by lha-lh1.c.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <glib/gstdio.h>
#include <archive.h>
#include <archive_entry.h>

#include "gimageview.h"

#include "charset.h"
#include "fileutil.h"
#include "fr-archive.h"
#include "fr-command.h"
#include "gimv_image_info.h"
#include "gimv_plugin.h"
#include "lha-lh1.h"
#ifdef HAVE_LHASA
#  include <lhasa.h>
#endif
#include "prefs.h"


#define FR_COMMAND_LIBARCHIVE_TYPE  fr_command_libarchive_get_type ()
#define IS_FR_COMMAND_LIBARCHIVE(o) G_TYPE_CHECK_INSTANCE_TYPE (o, FR_COMMAND_LIBARCHIVE_TYPE)

typedef struct {
   FRCommand __parent;
} FRCommandLibarchive;

typedef struct {
   FRCommandClass __parent_class;
} FRCommandLibarchiveClass;

GType             fr_command_libarchive_get_type (void);
static FRCommand *fr_command_libarchive_new      (FRProcess  *process,
                                                  const char *filename,
                                                  FRArchive  *archive);


/* formats that libarchive reads (the ones that can hold images) */
#define ARCHIVER(ext, compressed) \
   {GIMV_ARCHIVER_IF_VERSION, ext, fr_command_libarchive_new, compressed}

static ExtArchiverPlugin plugin_impl[] =
{
   ARCHIVER (".zip",      TRUE),
   ARCHIVER (".cbz",      TRUE),
   ARCHIVER (".jar",      TRUE),
   ARCHIVER (".ear",      TRUE),
   ARCHIVER (".war",      TRUE),
   ARCHIVER (".rar",      TRUE),
   ARCHIVER (".cbr",      TRUE),
   ARCHIVER (".7z",       TRUE),
   ARCHIVER (".cb7",      TRUE),
   ARCHIVER (".lzh",      TRUE),
   ARCHIVER (".lha",      TRUE),
   ARCHIVER (".tar",      FALSE),
   ARCHIVER (".cbt",      FALSE),
   ARCHIVER (".tar.gz",   TRUE),
   ARCHIVER (".tgz",      TRUE),
   ARCHIVER (".tar.bz2",  TRUE),
   ARCHIVER (".tbz2",     TRUE),
   ARCHIVER (".tar.bz",   TRUE),
   ARCHIVER (".tbz",      TRUE),
   ARCHIVER (".tar.xz",   TRUE),
   ARCHIVER (".txz",      TRUE),
   ARCHIVER (".tar.zst",  TRUE),
   ARCHIVER (".tzst",     TRUE),
   ARCHIVER (".tar.lz",   TRUE),
   ARCHIVER (".tar.lzma", TRUE),
   ARCHIVER (".tlz",      TRUE),
   ARCHIVER (".tar.lzo",  TRUE),
   ARCHIVER (".tzo",      TRUE),
   ARCHIVER (".tar.Z",    TRUE),
   ARCHIVER (".taz",      TRUE),
   ARCHIVER (".cab",      TRUE),
   ARCHIVER (".iso",      FALSE),
};

GIMV_PLUGIN_GET_IMPL(plugin_impl, GIMV_PLUGIN_EXT_ARCHIVER)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("Archive support (libarchive)"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   N_("Reads zip, rar, 7z, lha, tar and other archives with libarchive"),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: NULL,
   get_prefs_ui:  NULL,
};


gchar *
g_module_check_init (GModule *module)
{
   /* the command class is registered with the GType system */
   g_module_make_resident (module);
   return NULL;
}


/******************************************************************************
 *
 *   reading
 *
 ******************************************************************************/
static const char *
la_error (struct archive *a)
{
   const char *e = archive_error_string (a);
   return e ? e : "not an archive libarchive can read";
}


static struct archive *
open_with (const char *filename, gboolean latin1)
{
   struct archive *a = archive_read_new ();

   archive_read_support_filter_all (a);
   archive_read_support_format_all (a);
   if (latin1)
      archive_read_set_format_option (a, "lha", "hdrcharset", "ISO-8859-1");
   if (archive_read_open_filename (a, filename, 64 * 1024) != ARCHIVE_OK) {
      g_warning ("libarchive: %s: %s", filename, la_error (a));
      archive_read_free (a);
      return NULL;
   }

   return a;
}


/* an LHA archive whose names are not in the charset of the system (the
   MS-DOS days: Shift_JIS, or EUC-JP of UNIX): libarchive fails on them
   (with no message) unless it is told their charset.  It is told
   Latin-1, which takes any byte, and entry_name () gets the bytes back
   and guesses their charset itself. */
static gboolean
lha_needs_raw_names (const char *filename)
{
   struct archive *a = open_with (filename, FALSE);
   struct archive_entry *entry;
   gboolean raw = FALSE;
   int ret;

   if (!a) return FALSE;
   while ((ret = archive_read_next_header (a, &entry)) == ARCHIVE_OK
          || ret == ARCHIVE_WARN)
   {
      if (archive_format (a) != ARCHIVE_FORMAT_LHA) break;
   }
   if (ret == ARCHIVE_FATAL && archive_format (a) == ARCHIVE_FORMAT_LHA)
      raw = TRUE;
   archive_read_free (a);
   return raw;
}


/* *latin1: the names are Latin-1 for their bytes (see above) */
static struct archive *
open_archive (const char *filename, gboolean *latin1)
{
   *latin1 = lha_needs_raw_names (filename);
   return open_with (filename, *latin1);
}


/* a name not in UTF-8: guessed, else as %XX (see charset_to_display ());
   *note: "charset\tthe bytes as %XX" (no charset when not readable), for
   gimv_image_info_get_name_note () */
static gchar *
name_from_bytes (const gchar *bytes, gchar **note)
{
   const gchar *cs = NULL;
   gchar *name, *escaped;

   name = charset_guess_to_utf8 (bytes, -1,
                                 charset_japanese_likely (conf.charset_auto_detect_fn), &cs);
   if (g_utf8_validate (bytes, -1, NULL)) return name;
   escaped = charset_escape_bytes (bytes, -1);
   if (!name) {
      name = g_strdup (escaped);
      cs = "";
   }
   *note = g_strdup_printf ("%s\t%s", cs ? cs : "", escaped);
   g_free (escaped);
   return name;
}


/* a name of the archive made safe (freed and NULL if it cannot be):
   "/" separators, no leading "/" or "./", nothing outside the extraction
   directory; LHA (lha): MS-DOS names in upper case shown in lower case */
static gchar *
clean_name (gchar *name, gboolean lha)
{
   gchar **parts, *p;
   gboolean upper = FALSE, lower = FALSE;
   guint i;

   /* DOS style separators, leading "./" or "/" */
   g_strdelimit (name, "\\", '/');
   p = name;
   while (*p == '/' || (p[0] == '.' && p[1] == '/'))
      p += (*p == '/') ? 1 : 2;
   if (p != name) memmove (name, p, strlen (p) + 1);

   /* nothing may be written outside the extraction directory */
   parts = g_strsplit (name, "/", -1);
   for (i = 0; parts[i]; i++) {
      if (!strcmp (parts[i], "..")) {
         g_strfreev (parts);
         g_free (name);
         return NULL;
      }
   }
   g_strfreev (parts);
   if (!*name) {
      g_free (name);
      return NULL;
   }

   /* LHA: MS-DOS names are upper case; show them in lower case like lha
      (lhasa, LHa for UNIX) does when it extracts */
   if (lha) {
      for (p = name; *p; p++) {
         if (g_ascii_isupper (*p)) upper = TRUE;
         if (g_ascii_islower (*p)) lower = TRUE;
      }
      if (upper && !lower) {
         for (p = name; *p; p++) *p = g_ascii_tolower (*p);
      }
   }

   return name;
}


/* the name of a file in the archive in UTF-8, NULL for other entries or
   names that would be written outside the extraction directory.  Names not
   in UTF-8 (Shift_JIS of DOS era archives, EUC-JP, ...) are guessed, and
   shown as %XX where they cannot be (see charset_to_display ()). */
static gchar *
entry_name (struct archive *a, struct archive_entry *entry, gboolean latin1,
            gchar **note)
{
   const char *raw;
   gchar *name = NULL;

   if (archive_entry_filetype (entry) != AE_IFREG) return NULL;

   if (latin1) {
      /* the bytes of the header back from their Latin-1 */
      gchar *bytes;
      raw = archive_entry_pathname (entry);
      if (!raw) return NULL;
      bytes = g_convert (raw, -1, "ISO-8859-1", "UTF-8", NULL, NULL, NULL);
      if (bytes) {
         name = name_from_bytes (bytes, note);
         g_free (bytes);
      } else {
         /* not from the header (a level 2 Unicode name) */
         name = g_strdup (raw);
      }
   } else {
      raw = archive_entry_pathname_utf8 (entry);
      if (raw && g_utf8_validate (raw, -1, NULL)) {
         name = g_strdup (raw);
      } else {
         raw = archive_entry_pathname (entry);
         if (!raw) return NULL;
         name = name_from_bytes (raw, note);
      }
   }
   if (!name) return NULL;

   return clean_name (name, archive_format (a) == ARCHIVE_FORMAT_LHA);
}


/* names not in UTF-8: how they were read, shown with them (note is taken) */
static void
record_note (FRCommand *comm, const gchar *name, gchar *note)
{
   GHashTable *notes;

   if (!note) return;
   notes = g_object_get_data (G_OBJECT (comm->archive), GIMV_ARCHIVE_NAME_NOTES);
   if (!notes) {
      notes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      g_object_set_data_full (G_OBJECT (comm->archive), GIMV_ARCHIVE_NAME_NOTES,
                              notes, (GDestroyNotify) g_hash_table_destroy);
   }
   g_hash_table_replace (notes, g_strdup (name), note);
}


/* where a file of the archive is extracted to */
static gchar *
output_path (const gchar *dest_dir, const gchar *name, gboolean junk_paths)
{
   gchar *base, *path;

   if (!junk_paths)
      return g_build_filename (dest_dir ? dest_dir : ".", name, NULL);
   base = g_path_get_basename (name);
   path = g_build_filename (dest_dir ? dest_dir : ".", base, NULL);
   g_free (base);
   return path;
}


/* let the FRProcess finish (and emit "done") after the work was done */
static void
queue_result (FRCommand *comm, gboolean success)
{
   fr_process_begin_command (comm->process, success ? "true" : "false");
   fr_process_end_command (comm->process);
}


#ifdef HAVE_LHASA
/******************************************************************************
 *
 *   LHA with liblhasa: all the methods of the old days (-lh1- -lh4-
 *   -lzs- -lz5- -pm1- ...; libarchive unpacks -lh5- to -lh7- only) and
 *   self-extracting archives (MS-DOS, X68000, ...)
 *
 ******************************************************************************/
static gboolean make_parent_dirs (const gchar *path);


static gboolean
lhasa_wanted (const gchar *filename)
{
   const gchar *ext = strrchr (filename, '.');

   return ext && (!g_ascii_strcasecmp (ext, ".lzh") || !g_ascii_strcasecmp (ext, ".lha")
                  || !g_ascii_strcasecmp (ext, ".lzs"));
}


/* the name of a file of the archive (NULL: a directory, an unsafe name) */
static gchar *
lhasa_name (LHAFileHeader *h, gchar **note)
{
   gchar *raw, *name;

   *note = NULL;
   if (!h->filename || !strcmp (h->compress_method, LHA_COMPRESS_TYPE_DIR)
       || h->symlink_target)
      return NULL;
   raw = g_strconcat (h->path ? h->path : "", h->filename, NULL);
   name = clean_name (name_from_bytes (raw, note), TRUE);
   g_free (raw);
   if (!name) g_clear_pointer (note, g_free);
   return name;
}


static LHAReader *
lhasa_open (const gchar *filename, LHAInputStream **stream)
{
   *stream = lha_input_stream_from ((char *) filename);
   if (!*stream) return NULL;
   return lha_reader_new (*stream);
}


static void
lhasa_close (LHAReader *reader, LHAInputStream *stream)
{
   if (reader) lha_reader_free (reader);
   if (stream) lha_input_stream_free (stream);
}


/* FALSE: not an LHA archive liblhasa reads (libarchive is tried) */
static gboolean
lhasa_list (FRCommand *comm)
{
   LHAInputStream *stream;
   LHAReader *reader = lhasa_open (comm->filename, &stream);
   LHAFileHeader *h;
   guint headers = 0;

   if (!reader) {
      lhasa_close (NULL, stream);
      return FALSE;
   }
   while ((h = lha_reader_next_file (reader))) {
      gchar *note, *name;
      GimvImageInfo *info;
      struct stat st;

      headers++;
      name = lhasa_name (h, &note);
      if (!name) continue;
      record_note (comm, name, note);

      memset (&st, 0, sizeof (st));
      st.st_mode  = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
      st.st_size  = h->length;
      st.st_mtime = h->timestamp;
      info = gimv_image_info_get_with_archive (name, FR_ARCHIVE (comm->archive), &st);
      if (info)
         comm->file_list = g_list_prepend (comm->file_list, info);
      g_free (name);
   }
   lhasa_close (reader, stream);
   return headers > 0;
}


/* the file of the archive now unpacked to path (by way of a temporary
   name), its length and CRC checked */
static gboolean
lhasa_extract_file (LHAReader *reader, LHAFileHeader *h, const gchar *path)
{
   gchar *tmp;
   guchar buf[64 * 1024];
   guint16 crc = 0;
   gsize total = 0, n;
   gboolean ok = TRUE;
   FILE *f;

   if (!make_parent_dirs (path)) return FALSE;
   tmp = g_strconcat (path, ".part", NULL);
   f = g_fopen (tmp, "wb");
   if (!f) {
      g_free (tmp);
      return FALSE;
   }
   while ((n = lha_reader_read (reader, buf, sizeof (buf))) > 0) {
      crc = gimv_lha_crc16_update (crc, buf, n);
      total += n;
      if (fwrite (buf, 1, n, f) != n) {
         ok = FALSE;
         break;
      }
   }
   if (fclose (f) != 0) ok = FALSE;
   if (ok && total != h->length) {
      g_warning ("lha: %s: the data ends early", path);
      ok = FALSE;
   } else if (ok && crc != h->crc) {
      g_warning ("lha: %s: CRC error", path);
      ok = FALSE;
   }
   if (ok && g_rename (tmp, path) != 0) ok = FALSE;
   if (!ok) g_unlink (tmp);
   g_free (tmp);
   return ok;
}


/* FALSE: not an LHA archive liblhasa reads (libarchive is tried) */
static gboolean
lhasa_extract (FRCommand *comm, GHashTable *wanted, const gchar *dest_dir,
               gboolean overwrite, gboolean skip_older, gboolean junk_paths,
               gboolean *success)
{
   LHAInputStream *stream;
   LHAReader *reader = lhasa_open (comm->filename, &stream);
   LHAFileHeader *h;
   guint headers = 0;

   if (!reader) {
      lhasa_close (NULL, stream);
      return FALSE;
   }
   *success = TRUE;
   while ((h = lha_reader_next_file (reader))) {
      gchar *note, *name, *path;
      struct stat st;

      headers++;
      name = lhasa_name (h, &note);
      g_free (note);
      if (!name) continue;
      if (wanted && !g_hash_table_contains (wanted, name)) {
         g_free (name);
         continue;
      }

      path = output_path (dest_dir, name, junk_paths);
      if (g_stat (path, &st) == 0
          && (!overwrite || (skip_older && st.st_mtime >= (time_t) h->timestamp)))
      {
         /* keep the existing file */
      } else if (!lhasa_extract_file (reader, h, path)) {
         *success = FALSE;
      }

      if (wanted) g_hash_table_remove (wanted, name);
      g_free (path);
      g_free (name);
      if (wanted && g_hash_table_size (wanted) == 0) break;
   }
   lhasa_close (reader, stream);
   if (!headers) return FALSE;
   return TRUE;
}
#endif /* HAVE_LHASA */


static void
fr_command_libarchive_list (FRCommand *comm)
{
   struct archive *a;
   struct archive_entry *entry;
   gboolean success = TRUE, latin1;
   int ret;

   fr_process_clear (comm->process);

#ifdef HAVE_LHASA
   if (lhasa_wanted (comm->filename) && lhasa_list (comm)) {
      queue_result (comm, TRUE);
      fr_process_start (comm->process, FALSE);
      return;
   }
#endif

   a = open_archive (comm->filename, &latin1);
   if (!a) {
      queue_result (comm, FALSE);
      fr_process_start (comm->process, FALSE);
      return;
   }

   while ((ret = archive_read_next_header (a, &entry)) == ARCHIVE_OK
          || ret == ARCHIVE_WARN)
   {
      gchar *note = NULL, *name = entry_name (a, entry, latin1, &note);
      GimvImageInfo *info;
      struct stat st;

      if (!name) {
         g_free (note);
         continue;
      }
      record_note (comm, name, note);

      memset (&st, 0, sizeof (st));
      st.st_mode  = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
      st.st_size  = archive_entry_size (entry);
      st.st_mtime = archive_entry_mtime (entry);

      info = gimv_image_info_get_with_archive (name, FR_ARCHIVE (comm->archive), &st);
      if (info)
         comm->file_list = g_list_prepend (comm->file_list, info);
      g_free (name);
   }
   if (ret != ARCHIVE_EOF) {
      g_warning ("libarchive: %s: %s", comm->filename, la_error (a));
      success = comm->file_list != NULL;   /* show what could be read */
   }

   archive_read_free (a);

   queue_result (comm, success);
   fr_process_start (comm->process, FALSE);
}


static gboolean
make_parent_dirs (const gchar *path)
{
   gchar *dir = g_path_get_dirname (path);
   gboolean ok = g_mkdir_with_parents (dir, 0755) == 0;

   g_free (dir);
   return ok;
}


/* data written to path (by way of a temporary name) */
static gboolean
write_file (const gchar *path, const guchar *data, gsize len)
{
   gchar *tmp;
   gboolean ok;

   if (!make_parent_dirs (path)) return FALSE;
   tmp = g_strconcat (path, ".part", NULL);
   ok = g_file_set_contents (tmp, (const gchar *) data, len, NULL)
        && g_rename (tmp, path) == 0;
   if (!ok) g_unlink (tmp);
   g_free (tmp);
   return ok;
}


/* the LHA member whose header is at offset, if libarchive cannot unpack
   it: TRUE when done here (ok: unpacked and written) */
static gboolean
extract_lha_member (GMappedFile *map, la_int64_t offset, const gchar *path,
                    gboolean *ok)
{
   const guchar *buf;
   gsize len;
   GimvLhaMember m;
   guchar *out;

   if (!map || offset < 0) return FALSE;
   buf = (const guchar *) g_mapped_file_get_contents (map);
   len = g_mapped_file_get_length (map);
   if (!gimv_lha_member (buf, len, offset, &m)) return FALSE;
   if (strcmp (m.method, "-lh1-")) return FALSE;

   *ok = FALSE;
   if (m.original > 512 * 1024 * 1024) {
      g_warning ("lha: %s: too large", path);
      return TRUE;
   }
   out = g_try_malloc (MAX (m.original, 1));
   if (!out) return TRUE;
   if (!gimv_lh1_decode (buf + m.data, m.packed, out, m.original))
      g_warning ("lha: %s: the packed data ends early", path);
   else if (m.has_crc && gimv_lha_crc16 (out, m.original) != m.crc)
      g_warning ("lha: %s: CRC error", path);
   else
      *ok = write_file (path, out, m.original);
   g_free (out);
   return TRUE;
}


static gboolean
extract_entry (struct archive *a, const gchar *path)
{
   const void *buf;
   size_t size;
   la_int64_t offset;
   gchar *tmp;
   int fd, ret;
   gboolean ok = TRUE;

   if (!make_parent_dirs (path)) return FALSE;

   /* write to a temporary name: a half written file must not look done */
   tmp = g_strconcat (path, ".part", NULL);
   fd = g_open (tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
   if (fd < 0) {
      g_free (tmp);
      return FALSE;
   }

   while ((ret = archive_read_data_block (a, &buf, &size, &offset)) == ARCHIVE_OK) {
      if (lseek (fd, offset, SEEK_SET) < 0) { ok = FALSE; break; }
      while (size > 0) {
         ssize_t n = write (fd, buf, size);
         if (n < 0) {
            if (errno == EINTR) continue;
            ok = FALSE;
            break;
         }
         buf = (const char *) buf + n;
         size -= n;
      }
      if (!ok) break;
   }
   if (ret != ARCHIVE_EOF && ret != ARCHIVE_OK) {
      g_warning ("libarchive: %s", la_error (a));
      ok = FALSE;
   }

   if (close (fd) != 0) ok = FALSE;
   if (ok && g_rename (tmp, path) != 0) ok = FALSE;
   if (!ok) g_unlink (tmp);
   g_free (tmp);

   return ok;
}


static void
fr_command_libarchive_extract (FRCommand *comm,
                               GList     *file_list,
                               char      *dest_dir,
                               gboolean   overwrite,
                               gboolean   skip_older,
                               gboolean   junk_paths)
{
   struct archive *a;
   struct archive_entry *entry;
   GHashTable *wanted = NULL;
   GMappedFile *map = NULL;
   GList *node;
   gboolean success = TRUE, latin1;
   int ret;

   if (file_list) {
      wanted = g_hash_table_new (g_str_hash, g_str_equal);
      for (node = file_list; node; node = g_list_next (node))
         if (node->data) g_hash_table_add (wanted, node->data);
   }

#ifdef HAVE_LHASA
   if (lhasa_wanted (comm->filename)
       && lhasa_extract (comm, wanted, dest_dir, overwrite, skip_older, junk_paths,
                         &success))
   {
      if (wanted) g_hash_table_destroy (wanted);
      queue_result (comm, success);
      return;
   }
#endif

   a = open_archive (comm->filename, &latin1);
   if (!a) {
      if (wanted) g_hash_table_destroy (wanted);
      queue_result (comm, FALSE);
      return;
   }

   while ((ret = archive_read_next_header (a, &entry)) == ARCHIVE_OK
          || ret == ARCHIVE_WARN)
   {
      gchar *note = NULL, *name = entry_name (a, entry, latin1, &note), *path;
      struct stat st;
      gboolean ok;

      g_free (note);

      if (!name) continue;
      if (wanted && !g_hash_table_contains (wanted, name)) {
         g_free (name);
         continue;
      }

      path = output_path (dest_dir, name, junk_paths);

      if (g_stat (path, &st) == 0
          && (!overwrite
              || (skip_older && st.st_mtime >= archive_entry_mtime (entry))))
      {
         /* keep the existing file */
      } else if (archive_format (a) == ARCHIVE_FORMAT_LHA
                 && (map || (map = g_mapped_file_new (comm->filename, FALSE, NULL)))
                 && extract_lha_member (map, archive_read_header_position (a),
                                        path, &ok))
      {
         if (!ok) success = FALSE;
      } else if (!extract_entry (a, path)) {
         success = FALSE;
      }

      if (wanted) g_hash_table_remove (wanted, name);
      g_free (path);
      g_free (name);

      if (wanted && g_hash_table_size (wanted) == 0) break;
   }
   if (ret != ARCHIVE_EOF && ret != ARCHIVE_OK && ret != ARCHIVE_WARN) {
      g_warning ("libarchive: %s: %s", comm->filename, la_error (a));
      success = FALSE;
   }

   if (wanted) g_hash_table_destroy (wanted);
   if (map) g_mapped_file_unref (map);
   archive_read_free (a);

   /* fr_archive_extract () starts the process */
   queue_result (comm, success);
}


/* gimv doesn't write archives */
static void
fr_command_libarchive_add (FRCommand *comm, GList *file_list,
                           gchar *base_dir, gboolean update)
{
   queue_result (comm, FALSE);
}


static void
fr_command_libarchive_delete (FRCommand *comm, GList *file_list)
{
   queue_result (comm, FALSE);
}


static void
fr_command_libarchive_class_init (FRCommandLibarchiveClass *class)
{
   FRCommandClass *afc = (FRCommandClass *) class;

   afc->list    = fr_command_libarchive_list;
   afc->add     = fr_command_libarchive_add;
   afc->delete  = fr_command_libarchive_delete;
   afc->extract = fr_command_libarchive_extract;
}


static void
fr_command_libarchive_init (FRCommandLibarchive *self)
{
   FRCommand *comm = FR_COMMAND (self);

   comm->propAddCanUpdate             = FALSE;
   comm->propExtractCanAvoidOverwrite = TRUE;
   comm->propExtractCanSkipOlder      = TRUE;
   comm->propExtractCanJunkPaths      = TRUE;
   comm->propHasRatio                 = FALSE;
}


G_DEFINE_TYPE (FRCommandLibarchive, fr_command_libarchive, FR_COMMAND_TYPE)


static FRCommand *
fr_command_libarchive_new (FRProcess  *process,
                           const char *filename,
                           FRArchive  *archive)
{
   FRCommand *comm;

   comm = FR_COMMAND (g_object_new (FR_COMMAND_LIBARCHIVE_TYPE, NULL));
   fr_command_construct (comm, process, filename);
   comm->archive = archive;

   return comm;
}
