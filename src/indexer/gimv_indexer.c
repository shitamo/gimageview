/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView indexer
 *
 * A helper process started by gimv.  It walks the configured directories,
 * stores what it finds into an SQLite database and reports its progress on
 * stdout, one line per message.  gimv talks to it through stdin; when stdin
 * reaches EOF (gimv has quit or crashed) the indexer stops.
 *
 *   gimv-indexer --db FILE [--dotfiles] [--ext jpg,png,...] DIR...
 *
 * Messages on stdout:
 *   hello <version>
 *   busy                    another indexer already owns the database
 *   stage <n> start
 *   progress <n> <dirs> <files>
 *   stage <n> done <dirs> <files> <added> <changed> <removed>
 *   idle
 *   error <text>
 *
 * Commands on stdin:
 *   quit
 *   rescan                  look at every file again, also in directories
 *                           whose mtime has not changed (files overwritten
 *                           in place)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#include "gimv_index_db.h"

#define COMMIT_INTERVAL_USEC   (1 * G_USEC_PER_SEC)
#define PROGRESS_INTERVAL_USEC (G_USEC_PER_SEC / 2)

typedef struct Indexer_Tag
{
   sqlite3     *db;
   gchar      **roots;
   gboolean     dotfiles;
   GHashTable  *exts;          /* lower-case extension -> 1 */

   /* statistics for the running stage */
   gint         stage;
   guint        n_dirs, n_files, n_added, n_changed, n_removed;
   gint64       last_commit, last_progress;

   gboolean     quit;
   gboolean     rescan;
   gboolean     full;          /* stat every file, not only changed dirs */
   GString     *cmdbuf;

   sqlite3_stmt *st_dir_get, *st_dir_add, *st_dir_set;
   sqlite3_stmt *st_file_get, *st_file_add, *st_file_set;
   sqlite3_stmt *st_file_list, *st_file_del;
   sqlite3_stmt *st_sub_list, *st_dir_del;
} Indexer;


static const gchar *default_exts[] = {
   "jpg", "jpeg", "jpe", "jfif", "png", "gif", "bmp", "tif", "tiff",
   "webp", "avif", "heic", "heif", "svg", "xpm", "pnm", "ppm", "pgm", "pbm",
   "tga", "ico", "pcx", "mag", "mki", "pi", "pic", "zim", "jxl",
   "cr2", "nef", "arw", "orf", "rw2", "dng", "raf",
   NULL
};


static void
say (const gchar *fmt, ...)
{
   va_list ap;
   va_start (ap, fmt);
   vfprintf (stdout, fmt, ap);
   va_end (ap);
   fputc ('\n', stdout);
   fflush (stdout);
}


/******************************************************************************
 *  priority, lock
 ******************************************************************************/

#define IOPRIO_CLASS_IDLE  3
#define IOPRIO_CLASS_SHIFT 13
#define IOPRIO_WHO_PROCESS 1

static void
lower_priority (void)
{
   setpriority (PRIO_PROCESS, 0, 19);
#ifdef SYS_ioprio_set
   syscall (SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0,
            IOPRIO_CLASS_IDLE << IOPRIO_CLASS_SHIFT);
#endif
}


static gint
take_lock (const gchar *dbfile)
{
   gchar *lockfile = g_strconcat (dbfile, ".lock", NULL);
   gint fd = open (lockfile, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
   g_free (lockfile);
   if (fd < 0) return -1;
   if (flock (fd, LOCK_EX | LOCK_NB) < 0) {
      close (fd);
      return -2;
   }
   return fd;
}


/******************************************************************************
 *  commands from gimv
 ******************************************************************************/

/* non-blocking look at stdin, also used to notice that gimv has gone away */
static void
check_stdin (Indexer *ix, gint timeout_ms)
{
   struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
   gchar buf[256];

   while (!ix->quit && poll (&pfd, 1, timeout_ms) > 0) {
      gssize n = read (STDIN_FILENO, buf, sizeof (buf));
      gchar *nl;

      if (n <= 0) {
         if (n < 0 && errno == EINTR) continue;
         ix->quit = TRUE;           /* EOF: gimv is gone */
         return;
      }
      g_string_append_len (ix->cmdbuf, buf, n);
      while ((nl = strchr (ix->cmdbuf->str, '\n'))) {
         *nl = '\0';
         g_strstrip (ix->cmdbuf->str);
         if (!strcmp (ix->cmdbuf->str, "quit"))
            ix->quit = TRUE;
         else if (!strcmp (ix->cmdbuf->str, "rescan"))
            ix->rescan = TRUE;
         g_string_erase (ix->cmdbuf, 0, nl - ix->cmdbuf->str + 1);
      }
      timeout_ms = 0;
   }
}


/******************************************************************************
 *  database helpers
 ******************************************************************************/

static sqlite3_stmt *
prep (Indexer *ix, const gchar *sql)
{
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2 (ix->db, sql, -1, &st, NULL) != SQLITE_OK) {
      say ("error prepare: %s", sqlite3_errmsg (ix->db));
      return NULL;
   }
   return st;
}


static void
exec (Indexer *ix, const gchar *sql)
{
   gchar *err = NULL;
   if (sqlite3_exec (ix->db, sql, NULL, NULL, &err) != SQLITE_OK) {
      say ("error %s: %s", sql, err ? err : "?");
      sqlite3_free (err);
   }
}


static void
maybe_commit (Indexer *ix, gboolean force)
{
   gint64 now = g_get_monotonic_time ();

   if (force || now - ix->last_commit >= COMMIT_INTERVAL_USEC) {
      exec (ix, "COMMIT");
      check_stdin (ix, 0);
      exec (ix, "BEGIN");
      ix->last_commit = now;
   }
   if (force || now - ix->last_progress >= PROGRESS_INTERVAL_USEC) {
      say ("progress %d %u %u", ix->stage, ix->n_dirs, ix->n_files);
      ix->last_progress = now;
   }
}


/* local time "YYYY-MM-DD HH:MM:SS", the same form as EXIF dates */
static gchar *
time_to_date (gint64 t)
{
   GDateTime *dt = g_date_time_new_from_unix_local (t);
   gchar *s;
   if (!dt) return NULL;
   s = g_date_time_format (dt, "%Y-%m-%d %H:%M:%S");
   g_date_time_unref (dt);
   return s;
}


static gboolean
is_image_name (Indexer *ix, const gchar *name)
{
   const gchar *dot = strrchr (name, '.');
   gchar *lower;
   gboolean ret;

   if (!dot || !dot[1]) return FALSE;
   lower = g_ascii_strdown (dot + 1, -1);
   ret = g_hash_table_contains (ix->exts, lower);
   g_free (lower);
   return ret;
}


/******************************************************************************
 *  stage 1: file information
 ******************************************************************************/

static gint64
dir_lookup (Indexer *ix, const gchar *path, gint64 *mtime, gint64 *scanned)
{
   gint64 id = 0;
   sqlite3_stmt *st = ix->st_dir_get;

   sqlite3_reset (st);
   sqlite3_bind_text (st, 1, path, -1, SQLITE_STATIC);
   if (sqlite3_step (st) == SQLITE_ROW) {
      id       = sqlite3_column_int64 (st, 0);
      *mtime   = sqlite3_column_int64 (st, 1);
      *scanned = sqlite3_column_int64 (st, 2);
   }
   sqlite3_reset (st);
   return id;
}


static gint64
dir_add (Indexer *ix, const gchar *path, gint64 parent)
{
   sqlite3_stmt *st = ix->st_dir_add;

   sqlite3_reset (st);
   sqlite3_bind_text  (st, 1, path, -1, SQLITE_STATIC);
   if (parent > 0)
      sqlite3_bind_int64 (st, 2, parent);
   else
      sqlite3_bind_null (st, 2);
   sqlite3_step (st);
   sqlite3_reset (st);
   return sqlite3_last_insert_rowid (ix->db);
}


static void
dir_set_scanned (Indexer *ix, gint64 id, gint64 mtime)
{
   sqlite3_stmt *st = ix->st_dir_set;

   sqlite3_reset (st);
   sqlite3_bind_int64 (st, 1, mtime);
   sqlite3_bind_int64 (st, 2, id);
   sqlite3_step (st);
   sqlite3_reset (st);
}


/* delete a directory and everything below it from the database */
static void
dir_remove_tree (Indexer *ix, const gchar *path)
{
   sqlite3_stmt *st;
   gchar *like, *esc;
   gint n;

   /* "path/%" with LIKE wildcards escaped */
   esc = g_strdup (path);
   {
      GString *s = g_string_new (NULL);
      const gchar *p;
      for (p = esc; *p; p++) {
         if (*p == '%' || *p == '_' || *p == '\\') g_string_append_c (s, '\\');
         g_string_append_c (s, *p);
      }
      g_free (esc);
      esc = g_string_free (s, FALSE);
   }
   like = g_strconcat (esc, "/%", NULL);

   st = prep (ix, "SELECT count(*) FROM files WHERE dir_id IN "
                  "(SELECT id FROM dirs WHERE path = ?1 OR path LIKE ?2 ESCAPE '\\')");
   sqlite3_bind_text (st, 1, path, -1, SQLITE_STATIC);
   sqlite3_bind_text (st, 2, like, -1, SQLITE_STATIC);
   n = sqlite3_step (st) == SQLITE_ROW ? sqlite3_column_int (st, 0) : 0;
   sqlite3_finalize (st);
   ix->n_removed += n;

   st = prep (ix, "DELETE FROM dirs WHERE path = ?1 OR path LIKE ?2 ESCAPE '\\'");
   sqlite3_bind_text (st, 1, path, -1, SQLITE_STATIC);
   sqlite3_bind_text (st, 2, like, -1, SQLITE_STATIC);
   sqlite3_step (st);
   sqlite3_finalize (st);

   g_free (like);
   g_free (esc);
}


static void
file_update (Indexer *ix, gint64 dir_id, const gchar *dirpath,
             const gchar *name)
{
   gchar *path = g_build_filename (dirpath, name, NULL);
   struct stat sb;
   sqlite3_stmt *st;

   if (stat (path, &sb) < 0 || !S_ISREG (sb.st_mode)) {
      g_free (path);
      return;
   }
   g_free (path);

   st = ix->st_file_get;
   sqlite3_reset (st);
   sqlite3_bind_int64 (st, 1, dir_id);
   sqlite3_bind_text  (st, 2, name, -1, SQLITE_STATIC);

   if (sqlite3_step (st) == SQLITE_ROW) {
      gint64 id    = sqlite3_column_int64 (st, 0);
      gint64 size  = sqlite3_column_int64 (st, 1);
      gint64 mtime = sqlite3_column_int64 (st, 2);
      sqlite3_reset (st);

      if (size != (gint64) sb.st_size || mtime != (gint64) sb.st_mtime) {
         gchar *date = time_to_date (sb.st_mtime);
         /* the contents changed: back to stage 1, date from mtime again */
         st = ix->st_file_set;
         sqlite3_reset (st);
         sqlite3_bind_int64 (st, 1, sb.st_size);
         sqlite3_bind_int64 (st, 2, sb.st_mtime);
         sqlite3_bind_text  (st, 3, date, -1, g_free);
         sqlite3_bind_int64 (st, 4, id);
         sqlite3_step (st);
         sqlite3_reset (st);
         ix->n_changed++;
      }
   } else {
      gchar *date = time_to_date (sb.st_mtime);
      sqlite3_reset (st);
      st = ix->st_file_add;
      sqlite3_reset (st);
      sqlite3_bind_int64 (st, 1, dir_id);
      sqlite3_bind_text  (st, 2, name, -1, SQLITE_STATIC);
      sqlite3_bind_int64 (st, 3, sb.st_size);
      sqlite3_bind_int64 (st, 4, sb.st_mtime);
      sqlite3_bind_text  (st, 5, date, -1, g_free);
      sqlite3_step (st);
      sqlite3_reset (st);
      ix->n_added++;
   }
   ix->n_files++;
}


static void scan_dir (Indexer *ix, const gchar *path, gint64 parent, dev_t dev);

static void
scan_dir (Indexer *ix, const gchar *path, gint64 parent, dev_t dev)
{
   struct stat sb;
   gint64 id, old_mtime = 0, scanned = 0;
   gboolean list_files;
   GHashTable *seen_files, *seen_dirs;
   GPtrArray *subdirs;
   DIR *dp;
   struct dirent *de;
   guint i;

   if (ix->quit) return;
   if (lstat (path, &sb) < 0 || !S_ISDIR (sb.st_mode)) return;
   /* stay on one filesystem below a root */
   if (dev && sb.st_dev != dev) return;

   id = dir_lookup (ix, path, &old_mtime, &scanned);
   if (!id) id = dir_add (ix, path, parent);

   /* a directory's mtime changes when entries are added, removed or renamed
    * in it, so an unchanged directory needs no look at its files */
   list_files = ix->full || !scanned || old_mtime != (gint64) sb.st_mtime;

   dp = opendir (path);
   if (!dp) return;

   seen_files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
   seen_dirs  = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
   subdirs    = g_ptr_array_new_with_free_func (g_free);

   while ((de = readdir (dp))) {
      const gchar *name = de->d_name;
      gboolean is_dir = FALSE, is_reg = FALSE;

      if (!strcmp (name, ".") || !strcmp (name, "..")) continue;
      if (name[0] == '.' && !ix->dotfiles) continue;

      if (de->d_type == DT_DIR) {
         is_dir = TRUE;
      } else if (de->d_type == DT_REG || de->d_type == DT_LNK) {
         is_reg = TRUE;     /* symlinks to files are followed by stat() */
      } else if (de->d_type == DT_UNKNOWN) {
         gchar *p = g_build_filename (path, name, NULL);
         struct stat s2;
         if (lstat (p, &s2) == 0) {
            is_dir = S_ISDIR (s2.st_mode);
            is_reg = S_ISREG (s2.st_mode) || S_ISLNK (s2.st_mode);
         }
         g_free (p);
      }

      if (is_dir) {
         g_ptr_array_add (subdirs, g_build_filename (path, name, NULL));
         g_hash_table_add (seen_dirs, g_build_filename (path, name, NULL));
      } else if (is_reg && list_files && is_image_name (ix, name)) {
         file_update (ix, id, path, name);
         g_hash_table_add (seen_files, g_strdup (name));
         maybe_commit (ix, FALSE);
      }
   }
   closedir (dp);

   if (list_files) {
      /* files that are no longer there */
      sqlite3_stmt *st = ix->st_file_list;
      GPtrArray *gone = g_ptr_array_new_with_free_func (g_free);

      sqlite3_reset (st);
      sqlite3_bind_int64 (st, 1, id);
      while (sqlite3_step (st) == SQLITE_ROW) {
         const gchar *name = (const gchar *) sqlite3_column_text (st, 0);
         if (!g_hash_table_contains (seen_files, name))
            g_ptr_array_add (gone, g_strdup (name));
      }
      sqlite3_reset (st);

      for (i = 0; i < gone->len; i++) {
         st = ix->st_file_del;
         sqlite3_reset (st);
         sqlite3_bind_int64 (st, 1, id);
         sqlite3_bind_text  (st, 2, g_ptr_array_index (gone, i), -1, SQLITE_STATIC);
         sqlite3_step (st);
         sqlite3_reset (st);
         ix->n_removed++;
      }
      g_ptr_array_unref (gone);
   } else {
      sqlite3_stmt *st = prep (ix, "SELECT count(*) FROM files WHERE dir_id = ?");
      sqlite3_bind_int64 (st, 1, id);
      if (sqlite3_step (st) == SQLITE_ROW)
         ix->n_files += sqlite3_column_int (st, 0);
      sqlite3_finalize (st);
   }

   /* sub directories that are no longer there */
   {
      sqlite3_stmt *st = ix->st_sub_list;
      GPtrArray *gone = g_ptr_array_new_with_free_func (g_free);

      sqlite3_reset (st);
      sqlite3_bind_int64 (st, 1, id);
      while (sqlite3_step (st) == SQLITE_ROW) {
         const gchar *p = (const gchar *) sqlite3_column_text (st, 0);
         if (!g_hash_table_contains (seen_dirs, p))
            g_ptr_array_add (gone, g_strdup (p));
      }
      sqlite3_reset (st);
      for (i = 0; i < gone->len; i++)
         dir_remove_tree (ix, g_ptr_array_index (gone, i));
      g_ptr_array_unref (gone);
   }

   if (list_files)
      dir_set_scanned (ix, id, sb.st_mtime);

   ix->n_dirs++;
   maybe_commit (ix, FALSE);

   for (i = 0; i < subdirs->len && !ix->quit; i++)
      scan_dir (ix, g_ptr_array_index (subdirs, i), id, sb.st_dev);

   g_ptr_array_unref (subdirs);
   g_hash_table_unref (seen_files);
   g_hash_table_unref (seen_dirs);
}


/* forget roots that are not configured any more (comment files are kept) */
static void
drop_old_roots (Indexer *ix)
{
   sqlite3_stmt *st = prep (ix, "SELECT path FROM dirs WHERE parent IS NULL");
   GPtrArray *gone = g_ptr_array_new_with_free_func (g_free);
   guint i;

   while (sqlite3_step (st) == SQLITE_ROW) {
      const gchar *p = (const gchar *) sqlite3_column_text (st, 0);
      if (!g_strv_contains ((const gchar * const *) ix->roots, p))
         g_ptr_array_add (gone, g_strdup (p));
   }
   sqlite3_finalize (st);

   for (i = 0; i < gone->len; i++)
      dir_remove_tree (ix, g_ptr_array_index (gone, i));
   g_ptr_array_unref (gone);
}


static void
run_stage1 (Indexer *ix)
{
   gint i;

   ix->stage = 1;
   ix->n_dirs = ix->n_files = ix->n_added = ix->n_changed = ix->n_removed = 0;
   say ("stage 1 start");

   exec (ix, "BEGIN");
   ix->last_commit = ix->last_progress = g_get_monotonic_time ();

   drop_old_roots (ix);
   for (i = 0; ix->roots[i] && !ix->quit; i++)
      scan_dir (ix, ix->roots[i], 0, 0);

   exec (ix, "COMMIT");
   if (!ix->quit)
      say ("stage 1 done %u %u %u %u %u", ix->n_dirs, ix->n_files,
           ix->n_added, ix->n_changed, ix->n_removed);
}


/******************************************************************************
 *  main
 ******************************************************************************/

static gboolean
open_db (Indexer *ix, const gchar *dbfile)
{
   gchar *err = NULL;

   if (gimv_index_db_open (dbfile, TRUE, &ix->db, &err) != GIMV_INDEX_DB_OK) {
      say ("error %s", err ? err : "cannot open the database");
      g_free (err);
      return FALSE;
   }

   ix->st_dir_get   = prep (ix, "SELECT id, mtime, scanned FROM dirs WHERE path = ?");
   ix->st_dir_add   = prep (ix, "INSERT INTO dirs (path, parent, mtime, scanned) "
                                "VALUES (?, ?, 0, 0)");
   ix->st_dir_set   = prep (ix, "UPDATE dirs SET mtime = ?, scanned = 1 WHERE id = ?");
   ix->st_file_get  = prep (ix, "SELECT id, size, mtime FROM files "
                                "WHERE dir_id = ? AND name = ?");
   ix->st_file_add  = prep (ix, "INSERT INTO files (dir_id, name, size, mtime, "
                                "date, date_src, stage) VALUES (?, ?, ?, ?, ?, 0, 1)");
   ix->st_file_set  = prep (ix, "UPDATE files SET size = ?, mtime = ?, date = ?, "
                                "date_src = 0, stage = 1 WHERE id = ?");
   ix->st_file_list = prep (ix, "SELECT name FROM files WHERE dir_id = ?");
   ix->st_file_del  = prep (ix, "DELETE FROM files WHERE dir_id = ? AND name = ?");
   ix->st_sub_list  = prep (ix, "SELECT path FROM dirs WHERE parent = ?");

   return TRUE;
}


static void
close_db (Indexer *ix)
{
   sqlite3_stmt *sts[] = {
      ix->st_dir_get, ix->st_dir_add, ix->st_dir_set, ix->st_file_get,
      ix->st_file_add, ix->st_file_set, ix->st_file_list, ix->st_file_del,
      ix->st_sub_list,
   };
   guint i;

   for (i = 0; i < G_N_ELEMENTS (sts); i++)
      sqlite3_finalize (sts[i]);
   sqlite3_close (ix->db);
}


int
main (int argc, char *argv[])
{
   Indexer ix;
   gchar *dbfile = NULL, *exts = NULL;
   gboolean dotfiles = FALSE;
   gchar **dirs = NULL;
   GOptionEntry entries[] = {
      { "db",       0, 0, G_OPTION_ARG_FILENAME, &dbfile, "database file", "FILE" },
      { "dotfiles", 0, 0, G_OPTION_ARG_NONE, &dotfiles, "include dot files", NULL },
      { "ext",      0, 0, G_OPTION_ARG_STRING, &exts, "image extensions", "LIST" },
      { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &dirs, NULL, "DIR..." },
      { NULL }
   };
   GOptionContext *ctx;
   GError *error = NULL;
   gint lockfd, i;

   ctx = g_option_context_new (NULL);
   g_option_context_add_main_entries (ctx, entries, NULL);
   if (!g_option_context_parse (ctx, &argc, &argv, &error)) {
      say ("error %s", error->message);
      return 2;
   }
   g_option_context_free (ctx);

   if (!dbfile) {
      gchar *dir = g_build_filename (g_get_user_data_dir (), "gimageview", NULL);
      g_mkdir_with_parents (dir, 0700);
      dbfile = g_build_filename (dir, "index.db", NULL);
      g_free (dir);
   }

   memset (&ix, 0, sizeof (ix));
   ix.dotfiles = dotfiles;
   ix.cmdbuf = g_string_new (NULL);

   /* roots: absolute, without trailing slashes, no duplicates */
   {
      GPtrArray *a = g_ptr_array_new ();
      for (i = 0; dirs && dirs[i]; i++) {
         gchar *p = g_canonicalize_filename (dirs[i], NULL);
         if (!g_ptr_array_find_with_equal_func (a, p, g_str_equal, NULL))
            g_ptr_array_add (a, p);
         else
            g_free (p);
      }
      g_ptr_array_add (a, NULL);
      ix.roots = (gchar **) g_ptr_array_free (a, FALSE);
   }

   ix.exts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
   if (exts && *exts) {
      gchar **v = g_strsplit (exts, ",", -1);
      for (i = 0; v[i]; i++)
         if (*v[i]) g_hash_table_add (ix.exts, g_ascii_strdown (v[i], -1));
      g_strfreev (v);
   } else {
      for (i = 0; default_exts[i]; i++)
         g_hash_table_add (ix.exts, g_strdup (default_exts[i]));
   }

   say ("hello %s", GIMV_INDEX_DB_VERSION_STR);

   lockfd = take_lock (dbfile);
   if (lockfd == -2) {
      say ("busy");
      return 0;
   }

   lower_priority ();

   if (!open_db (&ix, dbfile))
      return 1;

   do {
      ix.full = ix.rescan;
      ix.rescan = FALSE;
      run_stage1 (&ix);
      if (ix.quit) break;
      say ("idle");
      /* nothing more to do for now: wait for a command or for gimv to quit */
      while (!ix.quit && !ix.rescan)
         check_stdin (&ix, -1);
   } while (!ix.quit);

   close_db (&ix);
   if (lockfd >= 0) close (lockfd);
   return 0;
}
