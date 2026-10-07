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

#include "fileutil.h"
#include "fr-archive.h"
#include "fr-command.h"
#include "gimv_image_info.h"
#include "gimv_plugin.h"


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
static struct archive *
open_archive (const char *filename)
{
   struct archive *a = archive_read_new ();

   archive_read_support_filter_all (a);
   archive_read_support_format_all (a);
   if (archive_read_open_filename (a, filename, 64 * 1024) != ARCHIVE_OK) {
      g_warning ("libarchive: %s: %s", filename, archive_error_string (a));
      archive_read_free (a);
      return NULL;
   }

   return a;
}


/* the name of an entry as gimv shows and extracts it (UTF-8, relative),
   or NULL for entries to skip (directories, links, unsafe paths) */
static gchar *
entry_name (struct archive *a, struct archive_entry *entry)
{
   const char *raw;
   gchar *name = NULL, **parts, *p;
   gboolean upper = FALSE, lower = FALSE;
   guint i;

   if (archive_entry_filetype (entry) != AE_IFREG) return NULL;

   raw = archive_entry_pathname_utf8 (entry);
   if (raw && g_utf8_validate (raw, -1, NULL)) {
      name = g_strdup (raw);
   } else {
      raw = archive_entry_pathname (entry);
      if (!raw) return NULL;
      /* DOS era Japanese archives (lha, zip) have Shift_JIS names */
      if (g_utf8_validate (raw, -1, NULL))
         name = g_strdup (raw);
      else
         name = g_convert (raw, -1, "UTF-8", "CP932", NULL, NULL, NULL);
      if (!name)
         name = g_utf8_make_valid (raw, -1);
   }

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
   if (archive_format (a) == ARCHIVE_FORMAT_LHA) {
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


/* let the FRProcess finish (and emit "done") after the work was done */
static void
queue_result (FRCommand *comm, gboolean success)
{
   fr_process_begin_command (comm->process, success ? "true" : "false");
   fr_process_end_command (comm->process);
}


static void
fr_command_libarchive_list (FRCommand *comm)
{
   struct archive *a;
   struct archive_entry *entry;
   gboolean success = TRUE;
   int ret;

   fr_process_clear (comm->process);

   a = open_archive (comm->filename);
   if (!a) {
      queue_result (comm, FALSE);
      fr_process_start (comm->process, FALSE);
      return;
   }

   while ((ret = archive_read_next_header (a, &entry)) == ARCHIVE_OK
          || ret == ARCHIVE_WARN)
   {
      gchar *name = entry_name (a, entry);
      GimvImageInfo *info;
      struct stat st;

      if (!name) continue;

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
      g_warning ("libarchive: %s: %s", comm->filename, archive_error_string (a));
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
      g_warning ("libarchive: %s", archive_error_string (a));
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
   GList *node;
   gboolean success = TRUE;
   int ret;

   a = open_archive (comm->filename);
   if (!a) {
      queue_result (comm, FALSE);
      return;
   }

   if (file_list) {
      wanted = g_hash_table_new (g_str_hash, g_str_equal);
      for (node = file_list; node; node = g_list_next (node))
         if (node->data) g_hash_table_add (wanted, node->data);
   }

   while ((ret = archive_read_next_header (a, &entry)) == ARCHIVE_OK
          || ret == ARCHIVE_WARN)
   {
      gchar *name = entry_name (a, entry), *path;
      struct stat st;

      if (!name) continue;
      if (wanted && !g_hash_table_contains (wanted, name)) {
         g_free (name);
         continue;
      }

      if (junk_paths) {
         gchar *base = g_path_get_basename (name);
         path = g_build_filename (dest_dir ? dest_dir : ".", base, NULL);
         g_free (base);
      } else {
         path = g_build_filename (dest_dir ? dest_dir : ".", name, NULL);
      }

      if (g_stat (path, &st) == 0
          && (!overwrite
              || (skip_older && st.st_mtime >= archive_entry_mtime (entry))))
      {
         /* keep the existing file */
      } else if (!extract_entry (a, path)) {
         success = FALSE;
      }

      if (wanted) g_hash_table_remove (wanted, name);
      g_free (path);
      g_free (name);

      if (wanted && g_hash_table_size (wanted) == 0) break;
   }
   if (ret != ARCHIVE_EOF && ret != ARCHIVE_OK && ret != ARCHIVE_WARN) {
      g_warning ("libarchive: %s: %s", comm->filename, archive_error_string (a));
      success = FALSE;
   }

   if (wanted) g_hash_table_destroy (wanted);
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
