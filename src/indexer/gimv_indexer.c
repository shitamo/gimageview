/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView indexer
 *
 * A helper process started by gimv.  It walks the configured directories,
 * stores what it finds into an SQLite database and reports its progress on
 * stdout, one line per message.  gimv talks to it through stdin; when stdin
 * reaches EOF (gimv has quit or crashed) the indexer stops.
 *
 *   gimv-indexer --db FILE [--dotfiles] [--ext jpg,png,...]
 *                [--auto=folder,camera,shape] DIR...
 *
 * Stage 1 records the files (date from mtime), stage 3 reads the tags from
 * the gimv comment files (~/.gimv/comment/<image path>.txt, the line
 * "X-IMG-Tags: a, b"), stage 2 reads the date each image was taken: EXIF
 * DateTimeOriginal, or else a date in the file name, and puts the tags of
 * the rules (folder name, camera, portrait or landscape; see AUTO_RULE).
 * Stage 3 runs before stage 2, which may take hours the first time.
 *
 * Messages on stdout:
 *   hello <version>
 *   busy                    another indexer already owns the database
 *   stage 1 start <images last time>
 *   stage 2 start <to do> <images> <why>   why: "new" (new and changed
 *                           images), "auto-tags" or "date-rule" (every image
 *                           read again for new rules)
 *   stage 3 start
 *   progress <n> <dirs> <files>[\t<directory stage 1 is in>]
 *   stage <n> done <dirs> <files> <added> <changed> <removed>
 *   tags-updated            tags changed after a "tags" command
 *   changed                 files changed while watching (inotify)
 *   new\t<path>             an image that came while watching
 *   added-since-last <n>    images that came while gimv was not running
 *                           (found by the first stage 1, not on a new
 *                           database)
 *   moved\t<old>\t<new>     an image with a comment file was moved or
 *                           renamed by another program (the comment file
 *                           stays at the old place until gimv moves it)
 *   moved-dir\t<old>\t<new> the same for a directory with comment files
 *   missing-log\t<file>\t<n> images with comment files are missing: the
 *                           log of this run holds <n> of them now
 *   log-pruned <n>          <n> old logs were deleted (index_log_keep)
 *   watch-limit <n>         no more directories can be watched (the kernel
 *                           limit, fs.inotify.max_user_watches); changes
 *                           below the others are found at the next start
 *   idle [<files>]          nothing to do; the number of files indexed
 *   skipped\t<path>         the last run died reading this image: it is
 *                           left with the date from its mtime and no tags
 *                           from its contents
 *   error <text>
 *
 * Commands on stdin:
 *   unanswered\t<old>\t<new> an image moved by another program whose comment
 *                           file the user did not move: logged as missing
 *                           (sent when gimv quits)
 *   tags <path>             the comment file of the image changed: read its
 *                           tags again now (answered with "tags-updated")
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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#ifdef ENABLE_EXIF
#  include <libexif/exif-data.h>
#  include <libexif/exif-utils.h>
#endif

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
   GPtrArray   *tag_queue;     /* images whose comment file gimv changed */

   /* watching the directories while gimv runs (inotify) */
   gint         inotify_fd;
   GHashTable  *wd_paths;      /* watch descriptor -> directory path */
   gboolean     watch_full;    /* the kernel limit was reached */
   guint        n_watches;
   gboolean     report_new;    /* say "new" for each file added */
   gboolean     need_stage3;   /* files came while a stage ran */
   gboolean     first_run;     /* the first stage 1 of this start */

   /* missing files: images with a comment file (comments, tags) that are
    * gone from the indexed folders; written to a log per run */
   gboolean     log_missing;   /* FALSE while a folder is taken out of the
                                * configuration on purpose */
   gboolean     watching;      /* the missing ones were seen just now */
   GPtrArray   *missing;       /* "path\tlast seen\ttags" not written yet */
   gchar       *prev_scan;     /* when the files were last confirmed */
   gchar       *log_dir;
   gchar       *log_path;      /* the log of this run, once there is one */
   guint        log_count;
   gint         log_keep;
   gboolean     full;          /* stat every file, not only changed dirs */
   guint        auto_kinds;    /* AUTO_FOLDER | AUTO_CAMERA | AUTO_SHAPE */
   const gchar *cur_dir;       /* the directory stage 1 is in */
   gint         guard_fd;      /* the image stage 2 is reading (INDEX.db.reading) */
   gchar       *guard_path;
   GString     *cmdbuf;

   sqlite3_stmt *st_dir_get, *st_dir_add, *st_dir_set;
   sqlite3_stmt *st_file_get, *st_file_add, *st_file_set;
   sqlite3_stmt *st_file_list, *st_file_del;
   sqlite3_stmt *st_sub_list, *st_dir_del;
   sqlite3_stmt *st_file_by_path;
} Indexer;


static const gchar *default_exts[] = {
   "jpg", "jpeg", "jpe", "jfif", "png", "gif", "bmp", "tif", "tiff",
   "webp", "avif", "heic", "heif", "svg", "xpm", "pnm", "ppm", "pgm", "pbm",
   "tga", "ico", "pcx", "mag", "mki", "pi", "pic", "zim", "jxl",
   "cr2", "nef", "arw", "orf", "rw2", "dng", "raf",
   NULL
};


/* gimv is gone (SIGPIPE is ignored: the indexer must not die in the middle
 * of an image, which would look like a broken image to the next start) */
static gboolean output_gone = FALSE;

static void
say (const gchar *fmt, ...)
{
   va_list ap;

   if (output_gone) return;
   va_start (ap, fmt);
   vfprintf (stdout, fmt, ap);
   va_end (ap);
   fputc ('\n', stdout);
   if (fflush (stdout) != 0 || ferror (stdout))
      output_gone = TRUE;
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

static void unanswered_move (Indexer *ix, const gchar *old, const gchar *new);

/* non-blocking look at stdin, also used to notice that gimv has gone away */
static void
check_stdin (Indexer *ix, gint timeout_ms)
{
   if (output_gone) ix->quit = TRUE;
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
         if (g_str_has_prefix (ix->cmdbuf->str, "unanswered\t")) {
            gchar **v = g_strsplit (ix->cmdbuf->str, "\t", 3);
            if (g_strv_length (v) == 3)
               unanswered_move (ix, v[1], v[2]);
            g_strfreev (v);
         } else if (g_str_has_prefix (ix->cmdbuf->str, "tags /")) {
            /* the path as it is, spaces and all */
            g_ptr_array_add (ix->tag_queue, g_strdup (ix->cmdbuf->str + 5));
         } else {
            g_strstrip (ix->cmdbuf->str);
            if (!strcmp (ix->cmdbuf->str, "quit"))
               ix->quit = TRUE;
            else if (!strcmp (ix->cmdbuf->str, "rescan"))
               ix->rescan = TRUE;
         }
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


static void process_tag_queue (Indexer *ix, gboolean in_transaction);
static gboolean watch_during_stage (Indexer *ix);
static void missing_flush (Indexer *ix);

static void
maybe_commit (Indexer *ix, gboolean force)
{
   gint64 now = g_get_monotonic_time ();

   if (force || now - ix->last_commit >= COMMIT_INTERVAL_USEC) {
      gboolean changed;

      process_tag_queue (ix, TRUE);
      changed = watch_during_stage (ix);
      exec (ix, "COMMIT");
      if (changed) {
         missing_flush (ix);
         say ("changed");
      }
      check_stdin (ix, 0);
      exec (ix, "BEGIN");
      ix->last_commit = now;
   }
   if (force || now - ix->last_progress >= PROGRESS_INTERVAL_USEC) {
      if (ix->stage == 1 && ix->cur_dir)
         say ("progress %d %u %u\t%s", ix->stage, ix->n_dirs, ix->n_files, ix->cur_dir);
      else
         say ("progress %d %u %u", ix->stage, ix->n_dirs, ix->n_files);
      ix->last_progress = now;
   }
}


/*
 * Dates in the future are not used (a camera with its clock set wrong, a
 * number in a file name that only looks like a date).  The future starts
 * one day from now, for the time zones.  Old dates are fine: an old photo
 * scanned may well carry the date it was taken.
 */
static gchar date_max[20] = "9999-12-31 23:59:59";

static void
update_date_max (void)
{
   GDateTime *now = g_date_time_new_now_local ();
   GDateTime *max = g_date_time_add_days (now, 1);
   gchar *s = g_date_time_format (max, "%Y-%m-%d %H:%M:%S");

   g_strlcpy (date_max, s, sizeof (date_max));
   g_free (s);
   g_date_time_unref (max);
   g_date_time_unref (now);
}


static gboolean
date_in_range (const gchar *date)
{
   return date && strcmp (date, date_max) <= 0;
}


/* local time "YYYY-MM-DD HH:MM:SS", the same form as EXIF dates;
 * NULL when out of range */
static gchar *
time_to_date (gint64 t)
{
   GDateTime *dt = g_date_time_new_from_unix_local (t);
   gchar *s;
   if (!dt) return NULL;
   s = g_date_time_format (dt, "%Y-%m-%d %H:%M:%S");
   g_date_time_unref (dt);
   if (!date_in_range (s)) {
      g_free (s);
      return NULL;
   }
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
 *  missing files and their log
 ******************************************************************************/

static gchar *
now_string (void)
{
   GDateTime *now = g_date_time_new_now_local ();
   gchar *s = g_date_time_format (now, "%Y-%m-%d %H:%M:%S");
   g_date_time_unref (now);
   return s;
}


static gchar *
manual_tags (Indexer *ix, gint64 file_id)
{
   sqlite3_stmt *st = prep (ix, "SELECT group_concat(t.name, ', ') FROM file_tags ft "
                                "JOIN tags t ON t.id = ft.tag_id "
                                "WHERE ft.file_id = ? AND ft.auto = 0");
   gchar *tags = NULL;

   sqlite3_bind_int64 (st, 1, file_id);
   if (sqlite3_step (st) == SQLITE_ROW && sqlite3_column_text (st, 0))
      tags = g_strdup ((const gchar *) sqlite3_column_text (st, 0));
   sqlite3_finalize (st);
   return tags ? tags : g_strdup ("");
}


static void
missing_add (Indexer *ix, const gchar *path, gint64 file_id)
{
   gchar *tags = manual_tags (ix, file_id), *now = NULL;
   const gchar *last;

   if (strpbrk (path, "\t\n")) {
      g_free (tags);
      return;
   }
   if (ix->watching || !ix->prev_scan || !*ix->prev_scan)
      last = now = now_string ();
   else
      last = ix->prev_scan;
   g_ptr_array_add (ix->missing, g_strdup_printf ("%s\t%s\t%s", path, last, tags));
   g_free (tags);
   g_free (now);
}


/* the file is about to be dropped from the database */
static void
missing_file (Indexer *ix, gint64 dir_id, const gchar *dirpath, const gchar *name)
{
   sqlite3_stmt *st;

   if (!ix->log_missing) return;
   st = prep (ix, "SELECT f.id FROM files f JOIN comments c ON c.file_id = f.id "
                  "WHERE f.dir_id = ? AND f.name = ?");
   sqlite3_bind_int64 (st, 1, dir_id);
   sqlite3_bind_text  (st, 2, name, -1, SQLITE_STATIC);
   if (sqlite3_step (st) == SQLITE_ROW) {
      gchar *path = g_build_filename (dirpath, name, NULL);
      missing_add (ix, path, sqlite3_column_int64 (st, 0));
      g_free (path);
   }
   sqlite3_finalize (st);
}


/* the directory tree is about to be dropped (like: "path/%" escaped) */
static void
missing_tree (Indexer *ix, const gchar *path, const gchar *like)
{
   sqlite3_stmt *st;
   GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);
   GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
   guint i;

   if (!ix->log_missing) goto out;
   st = prep (ix, "SELECT d.path, f.name, f.id FROM files f "
                  "JOIN comments c ON c.file_id = f.id JOIN dirs d ON d.id = f.dir_id "
                  "WHERE d.path = ?1 OR d.path LIKE ?2 ESCAPE '\\'");
   sqlite3_bind_text (st, 1, path, -1, SQLITE_STATIC);
   sqlite3_bind_text (st, 2, like, -1, SQLITE_STATIC);
   while (sqlite3_step (st) == SQLITE_ROW) {
      gint64 id = sqlite3_column_int64 (st, 2);
      g_ptr_array_add (paths, g_build_filename ((const gchar *) sqlite3_column_text (st, 0),
                                               (const gchar *) sqlite3_column_text (st, 1),
                                               NULL));
      g_array_append_val (ids, id);
   }
   sqlite3_finalize (st);
   for (i = 0; i < paths->len; i++)
      missing_add (ix, g_ptr_array_index (paths, i), g_array_index (ids, gint64, i));
out:
   g_ptr_array_unref (paths);
   g_array_unref (ids);
}


static gint
compare_names (gconstpointer a, gconstpointer b)
{
   return strcmp (*(const gchar **) a, *(const gchar **) b);
}


/* keep the newest log_keep logs */
static void
prune_logs (Indexer *ix)
{
   GDir *d = g_dir_open (ix->log_dir, 0, NULL);
   GPtrArray *logs = g_ptr_array_new_with_free_func (g_free);
   const gchar *name;
   guint i, pruned = 0;

   if (!d) return;
   while ((name = g_dir_read_name (d)))
      if (g_str_has_prefix (name, "missing-") && g_str_has_suffix (name, ".log"))
         g_ptr_array_add (logs, g_strdup (name));
   g_dir_close (d);

   /* the names hold the time: sorted, the oldest come first */
   g_ptr_array_sort (logs, compare_names);
   for (i = 0; logs->len > (guint) ix->log_keep && i < logs->len - ix->log_keep; i++) {
      gchar *path = g_build_filename (ix->log_dir, g_ptr_array_index (logs, i), NULL);
      if (g_unlink (path) == 0) pruned++;
      g_free (path);
   }
   g_ptr_array_unref (logs);
   if (pruned) say ("log-pruned %u", pruned);
}


static void
missing_flush (Indexer *ix)
{
   FILE *fp;
   guint i;

   if (!ix->missing->len) return;
   if (ix->log_keep <= 0) {
      g_ptr_array_set_size (ix->missing, 0);
      return;
   }

   if (!ix->log_path) {
      GDateTime *now = g_date_time_new_now_local ();
      gchar *name = g_date_time_format (now, "missing-%Y%m%d-%H%M%S.log");
      gchar *when = g_date_time_format (now, "%Y-%m-%d %H:%M:%S");

      g_mkdir_with_parents (ix->log_dir, 0700);
      ix->log_path = g_build_filename (ix->log_dir, name, NULL);
      fp = g_fopen (ix->log_path, "w");
      if (fp) {
         fprintf (fp, "# GImageView: images with comment files (tags) that were missing\n"
                      "# found %s\n"
                      "# path\tlast seen\ttags[\tmoved to <path> (the comment file was left)]\n",
                  when);
         fclose (fp);
      }
      g_free (name);
      g_free (when);
      g_date_time_unref (now);
      prune_logs (ix);
   }

   fp = g_fopen (ix->log_path, "a");
   if (fp) {
      for (i = 0; i < ix->missing->len; i++)
         fprintf (fp, "%s\n", (gchar *) g_ptr_array_index (ix->missing, i));
      fclose (fp);
      ix->log_count += ix->missing->len;
      if (!strpbrk (ix->log_path, "\t\n"))
         say ("missing-log\t%s\t%u", ix->log_path, ix->log_count);
   }
   g_ptr_array_set_size (ix->missing, 0);
}


static GPtrArray *read_comment_tags (const gchar *file);
static gchar *comment_root (void);

/* the comment file stays at the old place: missing, with where the image
 * went */
static void
unanswered_move (Indexer *ix, const gchar *old, const gchar *new)
{
   gchar *croot = comment_root ();
   gchar *cfile = g_strconcat (croot, old, ".txt", NULL);
   GPtrArray *tags = read_comment_tags (cfile);
   gchar *now = now_string (), *joined = NULL;

   if (tags) {
      g_ptr_array_add (tags, NULL);
      joined = g_strjoinv (", ", (gchar **) tags->pdata);
      g_ptr_array_unref (tags);
   }
   if (!strpbrk (old, "\t\n") && !strpbrk (new, "\t\n"))
      g_ptr_array_add (ix->missing, g_strdup_printf ("%s\t%s\t%s\tmoved to %s", old, now,
                                                     joined ? joined : "", new));
   g_free (joined);
   g_free (now);
   g_free (cfile);
   g_free (croot);
}


/* the time of this scan, for the next run */
static void
remember_scan (Indexer *ix)
{
   gchar *now = now_string ();
   sqlite3_stmt *st = prep (ix, "INSERT OR REPLACE INTO meta VALUES ('last_scan', ?)");
   sqlite3_bind_text (st, 1, now, -1, g_free);
   sqlite3_step (st);
   sqlite3_finalize (st);
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

   missing_tree (ix, path, like);

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
      if (ix->report_new) {
         gchar *full = g_build_filename (dirpath, name, NULL);
         if (!strpbrk (full, "\t\n"))
            say ("new\t%s", full);
         g_free (full);
      }
   }
   ix->n_files++;
}


static void scan_dir (Indexer *ix, const gchar *path, gint64 parent, dev_t dev);
static void watch_add (Indexer *ix, const gchar *path);

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
   ix->cur_dir = path;
   /* stay on one filesystem below a root */
   if (dev && sb.st_dev != dev) return;

   id = dir_lookup (ix, path, &old_mtime, &scanned);
   if (!id) id = dir_add (ix, path, parent);
   watch_add (ix, path);

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
         missing_file (ix, id, path, g_ptr_array_index (gone, i));
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

   /* taken out of the configuration on purpose: not missing */
   ix->log_missing = FALSE;
   for (i = 0; i < gone->len; i++)
      dir_remove_tree (ix, g_ptr_array_index (gone, i));
   ix->log_missing = TRUE;
   g_ptr_array_unref (gone);
}


static void
run_stage1 (Indexer *ix)
{
   gint i;
   gint64 before = 0;

   ix->stage = 1;
   ix->n_dirs = ix->n_files = ix->n_added = ix->n_changed = ix->n_removed = 0;
   ix->cur_dir = NULL;
   update_date_max ();
   {
      /* the images found last time: about how many this pass will see */
      sqlite3_stmt *st = prep (ix, "SELECT count(*) FROM files");
      before = sqlite3_step (st) == SQLITE_ROW ? sqlite3_column_int64 (st, 0) : 0;
      sqlite3_finalize (st);
   }
   say ("stage 1 start %" G_GINT64_FORMAT, before);

   exec (ix, "BEGIN");
   ix->last_commit = ix->last_progress = g_get_monotonic_time ();

   drop_old_roots (ix);
   for (i = 0; ix->roots[i] && !ix->quit; i++)
      scan_dir (ix, ix->roots[i], 0, 0);
   ix->cur_dir = NULL;

   exec (ix, "COMMIT");
   if (!ix->quit)
      say ("stage 1 done %u %u %u %u %u", ix->n_dirs, ix->n_files,
           ix->n_added, ix->n_changed, ix->n_removed);
   if (!ix->quit) {
      missing_flush (ix);
      remember_scan (ix);
   }
   /* not on a new database: everything would be new */
   if (!ix->quit && ix->first_run && before > 0 && ix->n_added > 0)
      say ("added-since-last %u", ix->n_added);
   ix->first_run = FALSE;
}


/******************************************************************************
 *  stage 2: the date the image was taken
 ******************************************************************************/

#define STAGE2_BATCH 200

static gboolean
valid_date (gint y, gint mo, gint d, gint h, gint mi, gint sec)
{
   return y >= 1 && y <= 9999
      && g_date_valid_dmy (d, mo, y)
      && h >= 0 && h < 24 && mi >= 0 && mi < 60 && sec >= 0 && sec < 61;
}


/* what stage 2 reads from an image */
typedef struct {
   gchar *date;       /* "YYYY-MM-DD HH:MM:SS" (EXIF), or NULL */
   gchar *camera;     /* "Canon EOS 5D", or NULL */
   gint   orient;     /* EXIF Orientation, 0 when there is none */
} ImageInfo;


static void
image_info_clear (ImageInfo *ii)
{
   g_free (ii->date);
   g_free (ii->camera);
   memset (ii, 0, sizeof (*ii));
}


#ifdef ENABLE_EXIF
static gchar *
exif_string (ExifData *ed, ExifTag tag)
{
   ExifEntry *e = exif_data_get_entry (ed, tag);
   gchar buf[128];

   if (!e) return NULL;
   exif_entry_get_value (e, buf, sizeof (buf));
   g_strstrip (buf);
   return *buf ? g_strdup (buf) : NULL;
}


/* "Canon" + "Canon EOS 5D" -> "Canon EOS 5D", "Apple" + "iPhone 12" ->
 * "Apple iPhone 12", "NIKON CORPORATION" + "NIKON D90" -> "NIKON D90" */
static gchar *
camera_name (gchar *make, gchar *model)
{
   gchar *ret, *first;

   if (!model) return make;
   if (!make) return model;
   first = g_strndup (make, strcspn (make, " "));
   if (g_ascii_strncasecmp (model, first, strlen (first)) == 0)
      ret = g_strdup (model);
   else
      ret = g_strconcat (make, " ", model, NULL);
   g_free (first);
   g_free (make);
   g_free (model);
   return ret;
}


static void
exif_read (const gchar *path, ImageInfo *ii)
{
   static const ExifTag tags[] = {
      EXIF_TAG_DATE_TIME_ORIGINAL, EXIF_TAG_DATE_TIME_DIGITIZED, EXIF_TAG_DATE_TIME,
   };
   ExifData *ed;
   ExifEntry *e;
   guint i;

   /* libexif reads JPEG (and EXIF files); it stops after the EXIF block */
   ed = exif_data_new_from_file (path);
   if (!ed) return;

   for (i = 0; i < G_N_ELEMENTS (tags) && !ii->date; i++) {
      gchar *v = exif_string (ed, tags[i]);
      gint y, mo, d, h, mi, sec;

      /* "YYYY:MM:DD HH:MM:SS" */
      if (v && sscanf (v, "%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) == 6
          && valid_date (y, mo, d, h, mi, sec))
      {
         ii->date = g_strdup_printf ("%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, sec);
         if (!date_in_range (ii->date)) {
            g_free (ii->date);
            ii->date = NULL;
         }
      }
      g_free (v);
   }

   ii->camera = camera_name (exif_string (ed, EXIF_TAG_MAKE), exif_string (ed, EXIF_TAG_MODEL));

   e = exif_data_get_entry (ed, EXIF_TAG_ORIENTATION);
   if (e && e->format == EXIF_FORMAT_SHORT && e->components >= 1)
      ii->orient = exif_get_short (e->data, exif_data_get_byte_order (ed));

   exif_data_unref (ed);
}
#endif


/* IMG_20190503_101500.jpg, 2019-05-03 10.15.00.png, DSC20190503.jpg, ... */
static gchar *
filename_date (const gchar *name)
{
   static GRegex *re = NULL;
   GMatchInfo *mi = NULL;
   gchar *ret = NULL;

   if (!re)
      re = g_regex_new ("(?<![0-9])((?:19|20)[0-9]{2})[-_.]?([0-9]{2})[-_.]?([0-9]{2})"
                        "(?:(?:[-_ T.]| at )?([0-9]{2})[-_.:h]?([0-9]{2})[-_.:m]?([0-9]{2}))?"
                        "(?![0-9])", G_REGEX_OPTIMIZE, 0, NULL);

   g_regex_match (re, name, 0, &mi);
   while (g_match_info_matches (mi) && !ret) {
      gchar *v[7];
      gint n[7], i;

      for (i = 1; i <= 6; i++) {
         v[i] = g_match_info_fetch (mi, i);
         n[i] = v[i] && *v[i] ? atoi (v[i]) : 0;
         g_free (v[i]);
      }
      if (valid_date (n[1], n[2], n[3], n[4], n[5], n[6])) {
         ret = g_strdup_printf ("%04d-%02d-%02d %02d:%02d:%02d",
                                n[1], n[2], n[3], n[4], n[5], n[6]);
         if (!date_in_range (ret)) {
            g_free (ret);
            ret = NULL;
         }
      }
      g_match_info_next (mi, NULL);
   }
   g_match_info_free (mi);
   return ret;
}


/******************************************************************************
 *  a file that takes the indexer down
 *
 *  Stage 2 writes the id of the image it reads into INDEX.db.reading (in the
 *  page cache: no sync, it only has to outlive this process).  If the
 *  indexer dies on an image, the next start finds the id there, gives that
 *  image its stage 2 without reading it, and says "skipped\t<path>".
 ******************************************************************************/

static gint64
rows_left (Indexer *ix)
{
   sqlite3_stmt *st = prep (ix, "SELECT count(*) FROM files WHERE stage < 2");
   gint64 n = sqlite3_step (st) == SQLITE_ROW ? sqlite3_column_int64 (st, 0) : 0;
   sqlite3_finalize (st);
   return n;
}


static void
guard_set (Indexer *ix, gint64 id)
{
   gchar buf[24];

   if (ix->guard_fd < 0) return;
   if (id) {
      gint len = g_snprintf (buf, sizeof buf, "%-20" G_GINT64_FORMAT "\n", id);
      if (pwrite (ix->guard_fd, buf, len, 0) < 0) { /* nothing to do */ }
   } else {
      if (ftruncate (ix->guard_fd, 0) < 0) { /* nothing to do */ }
   }
}


/* at the start: the image the last run died on */
static void
guard_check (Indexer *ix)
{
   gchar *text = NULL;
   gint64 id;

   if (!g_file_get_contents (ix->guard_path, &text, NULL, NULL)) return;
   id = g_ascii_strtoll (text, NULL, 10);
   g_free (text);
   if (id > 0) {
      sqlite3_stmt *st = prep (ix, "SELECT d.path, f.name, f.mtime, f.date_src FROM files f "
                                   "JOIN dirs d ON d.id = f.dir_id WHERE f.id = ?");
      sqlite3_bind_int64 (st, 1, id);
      if (sqlite3_step (st) == SQLITE_ROW) {
         gchar *path = g_build_filename ((const gchar *) sqlite3_column_text (st, 0),
                                         (const gchar *) sqlite3_column_text (st, 1), NULL);
         gchar *date = time_to_date (sqlite3_column_int64 (st, 2));
         sqlite3_stmt *upd = prep (ix, "UPDATE files SET date = ?, date_src = ?, stage = 2 "
                                       "WHERE id = ?");
         /* skipped once: tried again at the next start; twice: left */
         gint src = sqlite3_column_int (st, 3) == GIMV_INDEX_DATE_SKIPPED
                    ? GIMV_INDEX_DATE_SKIPPED_TWICE : GIMV_INDEX_DATE_SKIPPED;
         if (date) sqlite3_bind_text (upd, 1, date, -1, g_free);
         else      sqlite3_bind_null (upd, 1);
         sqlite3_bind_int   (upd, 2, src);
         sqlite3_bind_int64 (upd, 3, id);
         sqlite3_step (upd);
         sqlite3_finalize (upd);
         say ("skipped\t%s", path);
         g_free (path);
      }
      sqlite3_finalize (st);
   }
   guard_set (ix, 0);
}


/******************************************************************************
 *  tags put by rules (auto = 1 in file_tags)
 *
 *  folder/<name>   the folder of the image (not a root, not a folder named
 *                  only by numbers or a date, not a camera folder such as
 *                  DCIM or 100CANON)
 *  camera/<name>   EXIF Make and Model
 *  shape/portrait, shape/landscape, shape/square
 *
 *  They are kept only in the database and made again when AUTO_RULE
 *  changes.  Stage 2 puts them with the date (one read of each image).
 ******************************************************************************/

#define AUTO_RULE "1"

/* the kinds put (--auto=folder,camera,shape; the preferences) */
enum {
   AUTO_FOLDER = 1 << 0,
   AUTO_CAMERA = 1 << 1,
   AUTO_SHAPE  = 1 << 2,
   AUTO_ALL    = AUTO_FOLDER | AUTO_CAMERA | AUTO_SHAPE,
};

static guint
auto_kinds_parse (const gchar *list)
{
   gchar **v = g_strsplit (list, ",", -1);
   guint kinds = 0, i;

   for (i = 0; v[i]; i++) {
      gchar *k = g_strstrip (v[i]);
      if (!strcmp (k, "folder"))      kinds |= AUTO_FOLDER;
      else if (!strcmp (k, "camera")) kinds |= AUTO_CAMERA;
      else if (!strcmp (k, "shape"))  kinds |= AUTO_SHAPE;
   }
   g_strfreev (v);
   return kinds;
}

static gchar *
folder_tag (const gchar *dirpath)
{
   static GRegex *skip = NULL;
   gchar *name = g_path_get_basename (dirpath), *ret = NULL;

   if (!skip)
      skip = g_regex_new ("^(?:[0-9 ._-]+|[0-9]{3}[A-Za-z0-9_]{5}|DCIM)$",
                          G_REGEX_OPTIMIZE | G_REGEX_CASELESS, 0, NULL);
   if (*name && strcmp (name, "/") && strcmp (name, ".")
       && !g_regex_match (skip, name, 0, NULL))
      ret = g_strconcat ("folder/", name, NULL);
   g_free (name);
   return ret;
}


/* the size of an image from its header, read here: the image libraries
 * are not trusted with millions of files of any kind (a broken file must
 * not take the indexer down; gdk-pixbuf 2.44 hands files to glycin, whose
 * EXIF reader aborted the process on one).  JPEG, PNG, GIF, WebP, BMP;
 * FALSE for the others. */
static guint32 be16 (const guchar *p) { return (p[0] << 8) | p[1]; }
static guint32 be32 (const guchar *p) { return ((guint32) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static guint32 le16 (const guchar *p) { return p[0] | (p[1] << 8); }
static guint32 le24 (const guchar *p) { return p[0] | (p[1] << 8) | (p[2] << 16); }
static guint32 le32 (const guchar *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((guint32) p[3] << 24); }

static gboolean
jpeg_size (FILE *f, gint *w, gint *h)
{
   guchar b[8];
   gint n;

   /* after SOI: segments; the frame header (SOFn) has the size */
   for (n = 0; n < 1000; n++) {
      gint c;
      guint len;

      do { c = fgetc (f); } while (c != EOF && c != 0xff);
      do { c = fgetc (f); } while (c == 0xff);
      if (c == EOF) return FALSE;
      if (c == 0xd8 || (c >= 0xd0 && c <= 0xd7) || c == 0x01) continue;  /* no length */
      if (c == 0xd9 || c == 0xda) return FALSE;                          /* EOI, SOS */
      if (fread (b, 1, 2, f) != 2) return FALSE;
      len = be16 (b);
      if (len < 2) return FALSE;
      if (c >= 0xc0 && c <= 0xcf && c != 0xc4 && c != 0xc8 && c != 0xcc) {
         if (fread (b, 1, 5, f) != 5) return FALSE;
         *h = be16 (b + 1);
         *w = be16 (b + 3);
         return *w > 0 && *h > 0;
      }
      if (fseek (f, len - 2, SEEK_CUR) != 0) return FALSE;
   }
   return FALSE;
}

static gboolean
image_size (const gchar *path, gint *w, gint *h)
{
   guchar b[32];
   FILE *f;
   gboolean ok = FALSE;
   gsize n;

   *w = *h = 0;
   f = fopen (path, "rb");
   if (!f) return FALSE;
   n = fread (b, 1, sizeof b, f);

   if (n >= 4 && b[0] == 0xff && b[1] == 0xd8) {
      fseek (f, 2, SEEK_SET);
      ok = jpeg_size (f, w, h);
   } else if (n >= 24 && !memcmp (b, "\x89PNG\r\n\x1a\n", 8) && !memcmp (b + 12, "IHDR", 4)) {
      *w = be32 (b + 16); *h = be32 (b + 20); ok = TRUE;
   } else if (n >= 10 && (!memcmp (b, "GIF87a", 6) || !memcmp (b, "GIF89a", 6))) {
      *w = le16 (b + 6); *h = le16 (b + 8); ok = TRUE;
   } else if (n >= 30 && !memcmp (b, "RIFF", 4) && !memcmp (b + 8, "WEBP", 4)) {
      if (!memcmp (b + 12, "VP8 ", 4) && n >= 30) {
         *w = le16 (b + 26) & 0x3fff; *h = le16 (b + 28) & 0x3fff; ok = TRUE;
      } else if (!memcmp (b + 12, "VP8L", 4) && n >= 25) {
         guint32 v = le32 (b + 21);
         *w = (v & 0x3fff) + 1; *h = ((v >> 14) & 0x3fff) + 1; ok = TRUE;
      } else if (!memcmp (b + 12, "VP8X", 4) && n >= 30) {
         *w = le24 (b + 24) + 1; *h = le24 (b + 27) + 1; ok = TRUE;
      }
   } else if (n >= 26 && b[0] == 'B' && b[1] == 'M') {
      *w = (gint32) le32 (b + 18); *h = ABS ((gint32) le32 (b + 22)); ok = TRUE;
   }
   fclose (f);
   return ok && *w > 0 && *h > 0;
}


/* the shape as shown: EXIF Orientation 5 to 8 turn the image by 90 degrees */
static const gchar *
shape_tag (gint w, gint h, gint orient)
{
   if (w <= 0 || h <= 0) return NULL;
   if (orient >= 5 && orient <= 8) {
      gint t = w; w = h; h = t;
   }
   if (ABS (w - h) * 50 <= MAX (w, h)) return "shape/square";   /* within 2 % */
   return h > w ? "shape/portrait" : "shape/landscape";
}


typedef struct {
   sqlite3_stmt *del, *tag_add, *ft_add;
} AutoTags;

static void
auto_tags_init (Indexer *ix, AutoTags *at)
{
   at->del     = prep (ix, "DELETE FROM file_tags WHERE file_id = ? AND auto = 1");
   at->tag_add = prep (ix, "INSERT OR IGNORE INTO tags (name) VALUES (?)");
   at->ft_add  = prep (ix, "INSERT OR IGNORE INTO file_tags (file_id, tag_id, auto) "
                           "SELECT ?, id, 1 FROM tags WHERE name = ?");
}


static void
auto_tags_finish (AutoTags *at)
{
   sqlite3_finalize (at->del);
   sqlite3_finalize (at->tag_add);
   sqlite3_finalize (at->ft_add);
}


static void
auto_tag_add (AutoTags *at, gint64 file_id, const gchar *name)
{
   if (!name) return;
   sqlite3_reset (at->tag_add);
   sqlite3_bind_text (at->tag_add, 1, name, -1, SQLITE_STATIC);
   sqlite3_step (at->tag_add);
   sqlite3_reset (at->tag_add);

   sqlite3_reset (at->ft_add);
   sqlite3_bind_int64 (at->ft_add, 1, file_id);
   sqlite3_bind_text  (at->ft_add, 2, name, -1, SQLITE_STATIC);
   sqlite3_step (at->ft_add);
   sqlite3_reset (at->ft_add);
}


static void
auto_tags_clear (AutoTags *at, gint64 file_id)
{
   sqlite3_reset (at->del);
   sqlite3_bind_int64 (at->del, 1, file_id);
   sqlite3_step (at->del);
   sqlite3_reset (at->del);
}


/* the folder tags of every image, from the database alone */
static void
auto_folder_all (Indexer *ix)
{
   sqlite3_stmt *st, *ins;
   AutoTags at;

   auto_tags_init (ix, &at);
   ins = prep (ix, "INSERT OR IGNORE INTO file_tags (file_id, tag_id, auto) "
                   "SELECT f.id, t.id, 1 FROM files f, tags t "
                   "WHERE f.dir_id = ?1 AND t.name = ?2");
   st = prep (ix, "SELECT id, path FROM dirs WHERE parent IS NOT NULL");
   while (sqlite3_step (st) == SQLITE_ROW) {
      gchar *tag = folder_tag ((const gchar *) sqlite3_column_text (st, 1));
      if (!tag) continue;
      sqlite3_reset (at.tag_add);
      sqlite3_bind_text (at.tag_add, 1, tag, -1, SQLITE_STATIC);
      sqlite3_step (at.tag_add);
      sqlite3_reset (at.tag_add);
      sqlite3_reset (ins);
      sqlite3_bind_int64 (ins, 1, sqlite3_column_int64 (st, 0));
      sqlite3_bind_text  (ins, 2, tag, -1, SQLITE_STATIC);
      sqlite3_step (ins);
      sqlite3_reset (ins);
      g_free (tag);
   }
   sqlite3_finalize (st);
   sqlite3_finalize (ins);
   auto_tags_finish (&at);
}


static void
auto_kind_remove (Indexer *ix, const gchar *prefix)
{
   sqlite3_stmt *st = prep (ix, "DELETE FROM file_tags WHERE auto = 1 AND tag_id IN "
                                "(SELECT id FROM tags WHERE substr(name, 1, length(?1)) = ?1)");
   sqlite3_bind_text (st, 1, prefix, -1, SQLITE_STATIC);
   sqlite3_step (st);
   sqlite3_finalize (st);
}


/* the rules or the kinds wanted changed (or a database made before them):
 * a new rule makes all the tags of the rules again; a kind turned off goes
 * away; a kind turned on comes: the folder tags at once from the database,
 * the camera and the shape by reading every image again in stage 2 */
static void
auto_tags_update (Indexer *ix)
{
   sqlite3_stmt *st;
   gchar *rule = NULL, kinds_str[16];
   guint had, added;

   st = prep (ix, "SELECT value FROM meta WHERE key = 'auto_rule'");
   if (sqlite3_step (st) == SQLITE_ROW && sqlite3_column_text (st, 0))
      rule = g_strdup ((const gchar *) sqlite3_column_text (st, 0));
   sqlite3_finalize (st);
   st = prep (ix, "SELECT value FROM meta WHERE key = 'auto_kinds'");
   /* before this key there were all kinds */
   had = sqlite3_step (st) == SQLITE_ROW ? (guint) sqlite3_column_int (st, 0) : AUTO_ALL;
   sqlite3_finalize (st);

   if (!g_strcmp0 (rule, AUTO_RULE) && had == ix->auto_kinds) {
      g_free (rule);
      return;
   }

   exec (ix, "BEGIN");
   if (g_strcmp0 (rule, AUTO_RULE)) {
      exec (ix, "DELETE FROM file_tags WHERE auto = 1");
      had = 0;
   } else {
      if ((had & AUTO_FOLDER) && !(ix->auto_kinds & AUTO_FOLDER)) auto_kind_remove (ix, "folder/");
      if ((had & AUTO_CAMERA) && !(ix->auto_kinds & AUTO_CAMERA)) auto_kind_remove (ix, "camera/");
      if ((had & AUTO_SHAPE)  && !(ix->auto_kinds & AUTO_SHAPE))  auto_kind_remove (ix, "shape/");
   }
   added = ix->auto_kinds & ~had;
   if (added & AUTO_FOLDER)
      auto_folder_all (ix);
   if (added & (AUTO_CAMERA | AUTO_SHAPE)) {
      exec (ix, "UPDATE files SET stage = 1");
      exec (ix, "INSERT OR REPLACE INTO meta VALUES ('reread', 'auto-tags')");
   }

   exec (ix, "DELETE FROM tags WHERE id NOT IN (SELECT tag_id FROM file_tags)");
   exec (ix, "INSERT OR REPLACE INTO meta VALUES ('auto_rule', '" AUTO_RULE "')");
   g_snprintf (kinds_str, sizeof kinds_str, "%u", ix->auto_kinds);
   st = prep (ix, "INSERT OR REPLACE INTO meta VALUES ('auto_kinds', ?)");
   sqlite3_bind_text (st, 1, kinds_str, -1, SQLITE_STATIC);
   sqlite3_step (st);
   sqlite3_finalize (st);
   exec (ix, "COMMIT");
   g_free (rule);
}


static void
run_stage2 (Indexer *ix)
{
   sqlite3_stmt *sel, *upd;
   gboolean more = TRUE;
   AutoTags at;

   ix->stage = 2;
   ix->n_dirs = ix->n_files = ix->n_added = ix->n_changed = ix->n_removed = 0;
   update_date_max ();
   {
      /* future dates written before (or by an older indexer): look at
       * them again */
      sqlite3_stmt *st = prep (ix, "UPDATE files SET stage = 1 WHERE date > ?1");
      sqlite3_bind_text (st, 1, date_max, -1, SQLITE_STATIC);
      sqlite3_step (st);
      sqlite3_finalize (st);
   }
   {
      /* the total is known here: the files stage 1 has found */
      sqlite3_stmt *st = prep (ix, "SELECT count(*) FROM files WHERE stage < 2");
      guint todo = sqlite3_step (st) == SQLITE_ROW ? sqlite3_column_int (st, 0) : 0;
      guint total = 0;
      gchar *why = NULL;
      sqlite3_finalize (st);
      st = prep (ix, "SELECT count(*) FROM files");
      if (sqlite3_step (st) == SQLITE_ROW) total = sqlite3_column_int (st, 0);
      sqlite3_finalize (st);
      /* every image read again, and why ("new": the new and changed ones) */
      st = prep (ix, "SELECT value FROM meta WHERE key = 'reread'");
      if (sqlite3_step (st) == SQLITE_ROW && sqlite3_column_text (st, 0))
         why = g_strdup ((const gchar *) sqlite3_column_text (st, 0));
      sqlite3_finalize (st);
      say ("stage 2 start %u %u %s", todo, total, why ? why : "new");
      g_free (why);
   }

   /* in directory order, so that the disk is read in one sweep */
   sel  = prep (ix, "SELECT f.id, d.path, f.name, f.mtime, d.parent IS NULL FROM files f "
                    "JOIN dirs d ON d.id = f.dir_id WHERE f.stage < 2 "
                    "ORDER BY f.dir_id, f.name LIMIT " G_STRINGIFY (STAGE2_BATCH));
   upd  = prep (ix, "UPDATE files SET date = ?, date_src = ?, stage = 2 WHERE id = ?");
   auto_tags_init (ix, &at);

   exec (ix, "BEGIN");
   ix->last_commit = ix->last_progress = g_get_monotonic_time ();

   while (more && !ix->quit) {
      GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
      GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);
      GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
      GArray *mtimes = g_array_new (FALSE, FALSE, sizeof (gint64));
      GPtrArray *folders = g_ptr_array_new_with_free_func (g_free);
      guint i;

      sqlite3_reset (sel);
      while (sqlite3_step (sel) == SQLITE_ROW) {
         gint64 id = sqlite3_column_int64 (sel, 0);
         g_array_append_val (ids, id);
         g_ptr_array_add (paths, g_build_filename ((const gchar *) sqlite3_column_text (sel, 1),
                                                  (const gchar *) sqlite3_column_text (sel, 2),
                                                  NULL));
         g_ptr_array_add (names, g_strdup ((const gchar *) sqlite3_column_text (sel, 2)));
         {
            gint64 mt = sqlite3_column_int64 (sel, 3);
            g_array_append_val (mtimes, mt);
         }
         /* the files right in a root have no folder tag */
         g_ptr_array_add (folders, sqlite3_column_int (sel, 4) || !(ix->auto_kinds & AUTO_FOLDER)
                          ? NULL : folder_tag ((const gchar *) sqlite3_column_text (sel, 1)));
      }
      sqlite3_reset (sel);
      more = ids->len == STAGE2_BATCH;

      for (i = 0; i < ids->len && !ix->quit; i++) {
         gint64 id = g_array_index (ids, gint64, i);
         const gchar *path = g_ptr_array_index (paths, i);
         gchar *date = NULL;
         gint src = GIMV_INDEX_DATE_MTIME, w = 0, h = 0;
         ImageInfo ii = { 0 };

         guard_set (ix, id);
#ifdef ENABLE_EXIF
         exif_read (path, &ii);
         date = g_steal_pointer (&ii.date);
         if (date) src = GIMV_INDEX_DATE_EXIF;
#endif
         /* the size from the header only */
         if (ix->auto_kinds & AUTO_SHAPE)
            image_size (path, &w, &h);
         auto_tags_clear (&at, id);
         auto_tag_add (&at, id, g_ptr_array_index (folders, i));
         if (ii.camera && (ix->auto_kinds & AUTO_CAMERA)) {
            gchar *tag = g_strconcat ("camera/", ii.camera, NULL);
            auto_tag_add (&at, id, tag);
            g_free (tag);
         }
         auto_tag_add (&at, id, shape_tag (w, h, ii.orient));
         image_info_clear (&ii);
         if (!date) {
            date = filename_date (g_ptr_array_index (names, i));
            if (date) src = GIMV_INDEX_DATE_FILENAME;
         }

         if (date) {
            ix->n_changed++;
         } else {
            /* the date from mtime, or none when that is out of range too */
            date = time_to_date (g_array_index (mtimes, gint64, i));
            src = GIMV_INDEX_DATE_MTIME;
         }
         sqlite3_reset (upd);
         if (date)
            sqlite3_bind_text (upd, 1, date, -1, g_free);
         else
            sqlite3_bind_null (upd, 1);
         sqlite3_bind_int   (upd, 2, src);
         sqlite3_bind_int64 (upd, 3, id);
         sqlite3_step (upd);
         sqlite3_reset (upd);
         ix->n_files++;
         maybe_commit (ix, FALSE);
      }

      g_array_unref (ids);
      g_array_unref (mtimes);
      g_ptr_array_unref (paths);
      g_ptr_array_unref (names);
      g_ptr_array_unref (folders);
   }

   guard_set (ix, 0);
   if (!ix->quit && more == FALSE && rows_left (ix) == 0)
      exec (ix, "DELETE FROM meta WHERE key = 'reread'");
   /* tags no file has any more (the folder of a moved image, ...) */
   if (!ix->quit)
      exec (ix, "DELETE FROM tags WHERE id NOT IN (SELECT tag_id FROM file_tags)");
   exec (ix, "COMMIT");
   sqlite3_finalize (sel);
   sqlite3_finalize (upd);
   auto_tags_finish (&at);

   if (!ix->quit)
      say ("stage 2 done %u %u %u %u %u", ix->n_dirs, ix->n_files,
           ix->n_added, ix->n_changed, ix->n_removed);
}


/******************************************************************************
 *  stage 3: tags from the comment files
 ******************************************************************************/

static gchar *
comment_root (void)
{
   /* the same place as gimv_comment_get_path () */
   const gchar *home = g_getenv ("HOME");
   return g_build_filename (home ? home : g_get_home_dir (), ".gimv", "comment", NULL);
}


/* the tags of the comment file, NULL when there is no file */
static GPtrArray *
read_comment_tags (const gchar *file)
{
   gchar *text = NULL, **lines;
   GPtrArray *tags;
   gint i;

   if (!g_file_get_contents (file, &text, NULL, NULL))
      return NULL;

   tags = g_ptr_array_new_with_free_func (g_free);
   lines = g_strsplit (text, "\n", -1);
   /* "key: value" lines, then an empty line and the note */
   for (i = 0; lines[i] && *lines[i]; i++) {
      gchar **v;
      gint j;

      if (!g_str_has_prefix (lines[i], GIMV_INDEX_TAGS_KEY ":")) continue;
      v = g_strsplit (lines[i] + strlen (GIMV_INDEX_TAGS_KEY ":"), ",", -1);
      for (j = 0; v[j]; j++) {
         gchar *t = g_strstrip (v[j]);
         if (*t && !g_ptr_array_find_with_equal_func (tags, t, g_str_equal, NULL))
            g_ptr_array_add (tags, g_strdup (t));
      }
      g_strfreev (v);
   }
   g_strfreev (lines);
   g_free (text);
   return tags;
}


static gint64
file_id_for_path (Indexer *ix, const gchar *path)
{
   gchar *dir = g_path_get_dirname (path), *name = g_path_get_basename (path);
   sqlite3_stmt *st = ix->st_file_by_path;
   gint64 id = 0;

   sqlite3_reset (st);
   sqlite3_bind_text (st, 1, dir, -1, SQLITE_STATIC);
   sqlite3_bind_text (st, 2, name, -1, SQLITE_STATIC);
   if (sqlite3_step (st) == SQLITE_ROW)
      id = sqlite3_column_int64 (st, 0);
   sqlite3_reset (st);
   g_free (dir);
   g_free (name);
   return id;
}


/* replace the tags a person put on the file */
static void
set_manual_tags (Indexer *ix, gint64 file_id, GPtrArray *tags)
{
   sqlite3_stmt *st;
   guint i;

   st = prep (ix, "DELETE FROM file_tags WHERE file_id = ? AND auto = 0");
   sqlite3_bind_int64 (st, 1, file_id);
   sqlite3_step (st);
   sqlite3_finalize (st);

   for (i = 0; tags && i < tags->len; i++) {
      const gchar *name = g_ptr_array_index (tags, i);

      st = prep (ix, "INSERT OR IGNORE INTO tags (name) VALUES (?)");
      sqlite3_bind_text (st, 1, name, -1, SQLITE_STATIC);
      sqlite3_step (st);
      sqlite3_finalize (st);

      st = prep (ix, "INSERT OR IGNORE INTO file_tags (file_id, tag_id, auto) "
                     "SELECT ?, id, 0 FROM tags WHERE name = ?");
      sqlite3_bind_int64 (st, 1, file_id);
      sqlite3_bind_text  (st, 2, name, -1, SQLITE_STATIC);
      sqlite3_step (st);
      sqlite3_finalize (st);
   }
}


/* read the comment file of one image if it changed; returns TRUE when the
 * tags in the database changed */
static gboolean
import_comment (Indexer *ix, gint64 file_id, const gchar *cfile, gboolean force)
{
   struct stat sb;
   sqlite3_stmt *st;
   gint64 old = -1, mtime;
   GPtrArray *tags;

   st = prep (ix, "SELECT mtime FROM comments WHERE file_id = ?");
   sqlite3_bind_int64 (st, 1, file_id);
   if (sqlite3_step (st) == SQLITE_ROW) old = sqlite3_column_int64 (st, 0);
   sqlite3_finalize (st);

   if (stat (cfile, &sb) < 0) {
      /* no comment file (any more) */
      if (old < 0) return FALSE;
      set_manual_tags (ix, file_id, NULL);
      st = prep (ix, "DELETE FROM comments WHERE file_id = ?");
      sqlite3_bind_int64 (st, 1, file_id);
      sqlite3_step (st);
      sqlite3_finalize (st);
      return TRUE;
   }

   /* nanoseconds: a comment file written twice in one second */
   mtime = (gint64) sb.st_mtim.tv_sec * G_GINT64_CONSTANT (1000000000) + sb.st_mtim.tv_nsec;
   if (!force && old == mtime) return FALSE;

   tags = read_comment_tags (cfile);
   set_manual_tags (ix, file_id, tags);
   if (tags) g_ptr_array_unref (tags);

   st = prep (ix, "INSERT OR REPLACE INTO comments (file_id, mtime) VALUES (?, ?)");
   sqlite3_bind_int64 (st, 1, file_id);
   sqlite3_bind_int64 (st, 2, mtime);
   sqlite3_step (st);
   sqlite3_finalize (st);
   return TRUE;
}


static void
walk_comments (Indexer *ix, const gchar *croot, const gchar *dir, GHashTable *seen)
{
   DIR *dp = opendir (dir);
   struct dirent *de;

   if (!dp) return;
   while ((de = readdir (dp)) && !ix->quit) {
      gchar *path;
      struct stat sb;

      if (!strcmp (de->d_name, ".") || !strcmp (de->d_name, "..")) continue;
      path = g_build_filename (dir, de->d_name, NULL);
      if (lstat (path, &sb) == 0) {
         if (S_ISDIR (sb.st_mode)) {
            walk_comments (ix, croot, path, seen);
         } else if (S_ISREG (sb.st_mode) && g_str_has_suffix (de->d_name, ".txt")) {
            /* ~/.gimv/comment/a/b.jpg.txt -> /a/b.jpg */
            gchar *img = g_strndup (path + strlen (croot), strlen (path) - strlen (croot) - 4);
            gint64 id = file_id_for_path (ix, img);
            if (id) {
               gint64 *key = g_new (gint64, 1);
               *key = id;
               g_hash_table_add (seen, key);
               if (import_comment (ix, id, path, FALSE))
                  ix->n_changed++;
               ix->n_files++;
               maybe_commit (ix, FALSE);
            }
            g_free (img);
         }
      }
      g_free (path);
   }
   closedir (dp);
}


static void
run_stage3 (Indexer *ix)
{
   gchar *croot = comment_root ();
   GHashTable *seen = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
   GArray *gone = g_array_new (FALSE, FALSE, sizeof (gint64));
   sqlite3_stmt *st;
   guint i;

   ix->stage = 3;
   ix->n_dirs = ix->n_files = ix->n_added = ix->n_changed = ix->n_removed = 0;
   say ("stage 3 start");

   exec (ix, "BEGIN");
   ix->last_commit = ix->last_progress = g_get_monotonic_time ();

   /* only the comment files below the indexed directories */
   for (i = 0; ix->roots[i] && !ix->quit; i++) {
      gchar *dir = g_strconcat (croot, ix->roots[i], NULL);
      walk_comments (ix, croot, dir, seen);
      g_free (dir);
   }

   if (!ix->quit) {
      /* comment files that are gone */
      st = prep (ix, "SELECT file_id FROM comments");
      while (sqlite3_step (st) == SQLITE_ROW) {
         gint64 id = sqlite3_column_int64 (st, 0);
         if (!g_hash_table_contains (seen, &id))
            g_array_append_val (gone, id);
      }
      sqlite3_finalize (st);
      for (i = 0; i < gone->len; i++) {
         gint64 id = g_array_index (gone, gint64, i);
         set_manual_tags (ix, id, NULL);
         st = prep (ix, "DELETE FROM comments WHERE file_id = ?");
         sqlite3_bind_int64 (st, 1, id);
         sqlite3_step (st);
         sqlite3_finalize (st);
         ix->n_removed++;
      }
      /* tags no file has any more */
      exec (ix, "DELETE FROM tags WHERE id NOT IN (SELECT tag_id FROM file_tags)");
   }

   exec (ix, "COMMIT");
   g_array_unref (gone);
   g_hash_table_unref (seen);
   g_free (croot);

   if (!ix->quit)
      say ("stage 3 done %u %u %u %u %u", ix->n_dirs, ix->n_files,
           ix->n_added, ix->n_changed, ix->n_removed);
}


/* "tags <path>" from gimv: its comment file was just written */
static void
process_tag_queue (Indexer *ix, gboolean in_transaction)
{
   gchar *croot;
   gboolean changed = FALSE;
   guint i;

   if (!ix->tag_queue->len) return;

   croot = comment_root ();
   if (!in_transaction) exec (ix, "BEGIN");
   for (i = 0; i < ix->tag_queue->len; i++) {
      const gchar *img = g_ptr_array_index (ix->tag_queue, i);
      gint64 id = file_id_for_path (ix, img);
      gchar *cfile;

      if (!id) continue;
      cfile = g_strconcat (croot, img, ".txt", NULL);
      changed |= import_comment (ix, id, cfile, TRUE);
      g_free (cfile);
   }
   if (changed)
      exec (ix, "DELETE FROM tags WHERE id NOT IN (SELECT tag_id FROM file_tags)");
   if (!in_transaction) exec (ix, "COMMIT");
   g_ptr_array_set_size (ix->tag_queue, 0);
   g_free (croot);

   if (changed) say ("tags-updated");
}


/******************************************************************************
 *  watching (inotify): changes while gimv runs
 ******************************************************************************/

#define WATCH_MASK (IN_CREATE | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM \
                    | IN_MOVED_TO | IN_ONLYDIR | IN_DONT_FOLLOW | IN_EXCL_UNLINK)

static void
say_idle (Indexer *ix)
{
   sqlite3_stmt *st = prep (ix, "SELECT count(*) FROM files");
   gint64 n = sqlite3_step (st) == SQLITE_ROW ? sqlite3_column_int64 (st, 0) : 0;
   sqlite3_finalize (st);
   say ("idle %" G_GINT64_FORMAT, n);
}


static void
watch_init (Indexer *ix)
{
   ix->inotify_fd = inotify_init1 (IN_NONBLOCK | IN_CLOEXEC);
   ix->wd_paths = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
}


static void
watch_add (Indexer *ix, const gchar *path)
{
   gint wd;

   if (ix->inotify_fd < 0 || ix->watch_full) return;
   wd = inotify_add_watch (ix->inotify_fd, path, WATCH_MASK);
   if (wd < 0) {
      if (errno == ENOSPC) {
         ix->watch_full = TRUE;
         say ("watch-limit %u", ix->n_watches);
      }
      return;
   }
   if (!g_hash_table_contains (ix->wd_paths, GINT_TO_POINTER (wd)))
      ix->n_watches++;
   g_hash_table_insert (ix->wd_paths, GINT_TO_POINTER (wd), g_strdup (path));
}


/* a directory moved: its watches keep their descriptors, change the paths */
static void
watch_rename_tree (Indexer *ix, const gchar *old, const gchar *new)
{
   GHashTableIter it;
   gpointer key, value;
   gsize len = strlen (old);

   g_hash_table_iter_init (&it, ix->wd_paths);
   while (g_hash_table_iter_next (&it, &key, &value)) {
      const gchar *p = value;
      if (!strncmp (p, old, len) && (p[len] == '\0' || p[len] == '/'))
         g_hash_table_iter_replace (&it, g_strconcat (new, p + len, NULL));
   }
}


static gboolean
has_comment (const gchar *croot, const gchar *path, gboolean dir)
{
   gchar *c = dir ? g_strconcat (croot, path, NULL)
                  : g_strconcat (croot, path, ".txt", NULL);
   gboolean ret = g_file_test (c, dir ? G_FILE_TEST_IS_DIR : G_FILE_TEST_IS_REGULAR);
   g_free (c);
   return ret;
}


static void
report_move (const gchar *what, const gchar *old, const gchar *new)
{
   /* tab separated: the paths may hold spaces */
   if (strpbrk (old, "\t\n") || strpbrk (new, "\t\n")) return;
   say ("%s\t%s\t%s", what, old, new);
}


static void
watch_file_added (Indexer *ix, const gchar *path)
{
   gchar *dir = g_path_get_dirname (path), *name = g_path_get_basename (path);
   gint64 mtime, scanned, id;

   if (is_image_name (ix, name) && (id = dir_lookup (ix, dir, &mtime, &scanned))) {
      file_update (ix, id, dir, name);
      ix->n_added++;
   }
   g_free (dir);
   g_free (name);
}


static void
watch_file_removed (Indexer *ix, const gchar *path)
{
   gchar *dir = g_path_get_dirname (path), *name = g_path_get_basename (path);
   gint64 mtime, scanned, id = dir_lookup (ix, dir, &mtime, &scanned);
   sqlite3_stmt *st = ix->st_file_del;

   if (id) {
      missing_file (ix, id, dir, name);
      sqlite3_reset (st);
      sqlite3_bind_int64 (st, 1, id);
      sqlite3_bind_text  (st, 2, name, -1, SQLITE_STATIC);
      sqlite3_step (st);
      if (sqlite3_changes (ix->db)) ix->n_removed++;
      sqlite3_reset (st);
   }
   g_free (dir);
   g_free (name);
}


static void
watch_dir_added (Indexer *ix, const gchar *path)
{
   gchar *parent = g_path_get_dirname (path);
   gint64 mtime, scanned, pid = dir_lookup (ix, parent, &mtime, &scanned);

   /* the whole tree: files may have been put in it already */
   if (pid) {
      scan_dir (ix, path, pid, 0);
      ix->n_added++;
   }
   g_free (parent);
}


static void
watch_file_moved (Indexer *ix, const gchar *croot, const gchar *old, const gchar *new)
{
   gchar *odir = g_path_get_dirname (old), *oname = g_path_get_basename (old);
   gchar *ndir = g_path_get_dirname (new), *nname = g_path_get_basename (new);
   gint64 mtime, scanned, nid;
   gint64 fid = file_id_for_path (ix, old);
   sqlite3_stmt *st;

   nid = dir_lookup (ix, ndir, &mtime, &scanned);

   if (!is_image_name (ix, nname)) {
      /* renamed to something that is not an image */
      watch_file_removed (ix, old);
   } else if (!fid || !nid) {
      watch_file_added (ix, new);
   } else {
      /* the same row: its tags in the database go with it */
      watch_file_removed (ix, new);          /* a file replaced there */
      /* stage 1 again: stage 2 puts the folder tag of the new place */
      st = prep (ix, "UPDATE files SET dir_id = ?, name = ?, stage = 1 WHERE id = ?");
      sqlite3_bind_int64 (st, 1, nid);
      sqlite3_bind_text  (st, 2, nname, -1, SQLITE_STATIC);
      sqlite3_bind_int64 (st, 3, fid);
      sqlite3_step (st);
      sqlite3_finalize (st);
      ix->n_changed++;

      /* the comment file is still at the old place (gimv moves it itself
       * when gimv moved the image) */
      if (has_comment (croot, old, FALSE) && !has_comment (croot, new, FALSE))
         report_move ("moved", old, new);
   }

   g_free (odir); g_free (oname); g_free (ndir); g_free (nname);
}


static void
watch_dir_moved (Indexer *ix, const gchar *croot, const gchar *old, const gchar *new)
{
   gint64 mtime, scanned, oid = dir_lookup (ix, old, &mtime, &scanned), pid;
   gchar *parent = g_path_get_dirname (new);
   sqlite3_stmt *st;

   pid = dir_lookup (ix, parent, &mtime, &scanned);
   g_free (parent);

   if (!oid || !pid) {
      watch_dir_added (ix, new);
      return;
   }

   /* "old" and "old/..." -> "new" and "new/..." */
   {
      GString *esc = g_string_new (NULL);
      const gchar *p;
      gchar *like;

      for (p = old; *p; p++) {
         if (*p == '%' || *p == '_' || *p == '\\') g_string_append_c (esc, '\\');
         g_string_append_c (esc, *p);
      }
      like = g_strconcat (esc->str, "/%", NULL);
      st = prep (ix, "UPDATE dirs SET path = ?1 || substr(path, ?2) "
                     "WHERE path = ?3 OR path LIKE ?4 ESCAPE '\\'");
      sqlite3_bind_text (st, 1, new, -1, SQLITE_STATIC);
      sqlite3_bind_int  (st, 2, strlen (old) + 1);
      sqlite3_bind_text (st, 3, old, -1, SQLITE_STATIC);
      sqlite3_bind_text (st, 4, like, -1, SQLITE_STATIC);
      sqlite3_step (st);
      sqlite3_finalize (st);
      g_free (like);
      g_string_free (esc, TRUE);
   }
   st = prep (ix, "UPDATE dirs SET parent = ? WHERE id = ?");
   sqlite3_bind_int64 (st, 1, pid);
   sqlite3_bind_int64 (st, 2, oid);
   sqlite3_step (st);
   sqlite3_finalize (st);
   /* the images right in it have a new folder name (stage 2 puts it) */
   st = prep (ix, "UPDATE files SET stage = 1 WHERE dir_id = ?");
   sqlite3_bind_int64 (st, 1, oid);
   sqlite3_step (st);
   sqlite3_finalize (st);

   watch_rename_tree (ix, old, new);
   ix->n_changed++;

   if (has_comment (croot, old, TRUE))
      report_move ("moved-dir", old, new);
}


typedef struct {
   gchar   *path;
   gboolean dir;
} MoveFrom;

static void
move_from_free (MoveFrom *m)
{
   g_free (m->path);
   g_free (m);
}


/* read what the kernel has queued and bring the database up to date (in
 * a transaction); TRUE when something changed */
static gboolean
watch_apply (Indexer *ix, gboolean *new_files_ret)
{
   gchar buf[16384] __attribute__ ((aligned (__alignof__ (struct inotify_event))));
   GHashTable *froms = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                              (GDestroyNotify) move_from_free);
   gchar *croot = comment_root ();
   gboolean any = FALSE, new_files = FALSE;
   gssize n;

   ix->report_new = TRUE;
   ix->watching = TRUE;

   while ((n = read (ix->inotify_fd, buf, sizeof (buf))) > 0) {
      gchar *p;

      for (p = buf; p < buf + n; ) {
         struct inotify_event *ev = (struct inotify_event *) p;
         const gchar *dir;
         gchar *path;
         gboolean is_dir = (ev->mask & IN_ISDIR) != 0;

         p += sizeof (struct inotify_event) + ev->len;

         if (ev->mask & IN_Q_OVERFLOW) {
            /* events were lost: look at everything again */
            ix->rescan = TRUE;
            continue;
         }
         if (ev->mask & IN_IGNORED) {
            if (g_hash_table_remove (ix->wd_paths, GINT_TO_POINTER (ev->wd)))
               ix->n_watches--;
            continue;
         }
         dir = g_hash_table_lookup (ix->wd_paths, GINT_TO_POINTER (ev->wd));
         if (!dir || !ev->len || !ev->name[0]) continue;
         if (ev->name[0] == '.' && !ix->dotfiles) continue;

         path = g_build_filename (dir, ev->name, NULL);
         any = TRUE;

         if (ev->mask & IN_MOVED_FROM) {
            MoveFrom *m = g_new0 (MoveFrom, 1);
            m->path = path;
            m->dir = is_dir;
            g_hash_table_insert (froms, GUINT_TO_POINTER (ev->cookie), m);
            continue;                       /* path is kept */
         } else if (ev->mask & IN_MOVED_TO) {
            MoveFrom *m = g_hash_table_lookup (froms, GUINT_TO_POINTER (ev->cookie));
            if (m) {
               if (is_dir)
                  watch_dir_moved (ix, croot, m->path, path);
               else
                  watch_file_moved (ix, croot, m->path, path);
               g_hash_table_remove (froms, GUINT_TO_POINTER (ev->cookie));
            } else if (is_dir) {
               watch_dir_added (ix, path);     /* from outside */
               new_files = TRUE;
            } else {
               watch_file_added (ix, path);
               new_files = TRUE;
            }
         } else if (ev->mask & IN_CREATE) {
            /* files: wait for IN_CLOSE_WRITE, they are still written */
            if (is_dir) {
               watch_dir_added (ix, path);
               new_files = TRUE;
            }
         } else if (ev->mask & IN_CLOSE_WRITE) {
            watch_file_added (ix, path);
            new_files = TRUE;
         } else if (ev->mask & IN_DELETE) {
            if (is_dir)
               dir_remove_tree (ix, path);
            else
               watch_file_removed (ix, path);
         }
         g_free (path);
      }
   }

   /* moved out of the watched directories: gone */
   {
      GHashTableIter it;
      gpointer value;

      g_hash_table_iter_init (&it, froms);
      while (g_hash_table_iter_next (&it, NULL, &value)) {
         MoveFrom *m = value;
         if (m->dir)
            dir_remove_tree (ix, m->path);
         else
            watch_file_removed (ix, m->path);
      }
   }
   g_hash_table_unref (froms);
   ix->report_new = FALSE;
   ix->watching = FALSE;
   g_free (croot);

   *new_files_ret = new_files;
   return any;
}


/* stages 2 and 3 may take hours (EXIF of millions of files): changes are
 * taken in at their commits too, so that new images are notified at once
 * and the kernel queue does not overflow.  Not in stage 1, which lists the
 * same directories. */
static gboolean
watch_during_stage (Indexer *ix)
{
   struct pollfd pfd = { ix->inotify_fd, POLLIN, 0 };
   guint saved[5];
   gboolean any, new_files = FALSE;

   if (ix->inotify_fd < 0 || ix->stage < 2) return FALSE;
   if (poll (&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) return FALSE;

   /* the counters belong to the stage */
   saved[0] = ix->n_dirs; saved[1] = ix->n_files; saved[2] = ix->n_added;
   saved[3] = ix->n_changed; saved[4] = ix->n_removed;
   ix->n_changed = 0;
   any = watch_apply (ix, &new_files);
   /* their tags (comment files moved or copied with them) after the stage;
    * their dates come from stage 2, which picks up every new row */
   if (new_files || ix->n_changed) ix->need_stage3 = TRUE;
   ix->n_dirs = saved[0]; ix->n_files = saved[1]; ix->n_added = saved[2];
   ix->n_changed = saved[3]; ix->n_removed = saved[4];
   return any;
}


static gint64
rows_to_date (Indexer *ix)
{
   sqlite3_stmt *st = prep (ix, "SELECT count(*) FROM files WHERE stage < 2");
   gint64 n = sqlite3_step (st) == SQLITE_ROW ? sqlite3_column_int64 (st, 0) : 0;
   sqlite3_finalize (st);
   return n;
}


/* in the idle loop */
static void
watch_process (Indexer *ix)
{
   gboolean any, new_files = FALSE;

   ix->n_dirs = ix->n_files = ix->n_added = ix->n_changed = ix->n_removed = 0;
   exec (ix, "BEGIN");
   any = watch_apply (ix, &new_files);
   exec (ix, "COMMIT");
   missing_flush (ix);

   if (!any) return;

   /* the new files get their tags (a comment file moved or copied with them
    * by gimv) and their dates */
   if (new_files || ix->n_changed) {
      run_stage3 (ix);
      /* new files, and moved ones (their folder tag) */
      if (new_files || rows_to_date (ix) > 0)
         run_stage2 (ix);
   }
   say ("changed");
   say_idle (ix);
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
   ix->st_file_by_path = prep (ix, "SELECT f.id FROM files f JOIN dirs d ON d.id = f.dir_id "
                                   "WHERE d.path = ? AND f.name = ?");

   /* the rules for dates changed once (9802e3e dropped dates before 1990):
    * read the files left without a date again, one time */
   exec (ix, "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)");
   {
      sqlite3_stmt *st = prep (ix, "SELECT value FROM meta WHERE key = 'last_scan'");
      if (sqlite3_step (st) == SQLITE_ROW && sqlite3_column_text (st, 0))
         ix->prev_scan = g_strdup ((const gchar *) sqlite3_column_text (st, 0));
      sqlite3_finalize (st);
   }
   {
      sqlite3_stmt *st = prep (ix, "SELECT value FROM meta WHERE key = 'date_rule'");
      gboolean current = sqlite3_step (st) == SQLITE_ROW
         && !g_strcmp0 ((const gchar *) sqlite3_column_text (st, 0), "2");
      sqlite3_finalize (st);
      if (!current) {
         exec (ix, "UPDATE files SET stage = 1 WHERE date IS NULL");
         exec (ix, "INSERT OR REPLACE INTO meta VALUES ('date_rule', '2')");
         exec (ix, "INSERT OR REPLACE INTO meta VALUES ('reread', 'date-rule')");
      }
   }
   {
      /* the tags put by rules */
      auto_tags_update (ix);
   }
   {
      /* once: a database in the middle of reading every image again for the
       * automatic tags (made before 'reread' was kept) says so */
      sqlite3_stmt *st = prep (ix, "SELECT 1 FROM meta WHERE key = 'reread_checked'");
      gboolean checked = sqlite3_step (st) == SQLITE_ROW;
      sqlite3_finalize (st);
      if (!checked) {
         gint64 left = rows_left (ix), total = 0;
         st = prep (ix, "SELECT count(*) FROM files");
         if (sqlite3_step (st) == SQLITE_ROW) total = sqlite3_column_int64 (st, 0);
         sqlite3_finalize (st);
         if (total > 0 && left * 4 > total)
            exec (ix, "INSERT OR IGNORE INTO meta VALUES ('reread', 'auto-tags')");
         exec (ix, "INSERT OR REPLACE INTO meta VALUES ('reread_checked', '1')");
      }
   }

   return TRUE;
}


static void
close_db (Indexer *ix)
{
   sqlite3_stmt *sts[] = {
      ix->st_dir_get, ix->st_dir_add, ix->st_dir_set, ix->st_file_get,
      ix->st_file_add, ix->st_file_set, ix->st_file_list, ix->st_file_del,
      ix->st_sub_list, ix->st_file_by_path,
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
   gint log_keep = 10;
   gchar *auto_list = NULL;
   gchar **dirs = NULL;
   GOptionEntry entries[] = {
      { "db",       0, 0, G_OPTION_ARG_FILENAME, &dbfile, "database file", "FILE" },
      { "dotfiles", 0, 0, G_OPTION_ARG_NONE, &dotfiles, "include dot files", NULL },
      { "ext",      0, 0, G_OPTION_ARG_STRING, &exts, "image extensions", "LIST" },
      { "log-keep", 0, 0, G_OPTION_ARG_INT, &log_keep, "logs of missing files kept (0: none)", "N" },
      { "auto",     0, 0, G_OPTION_ARG_STRING, &auto_list, "tags put by rules (default: all)", "folder,camera,shape" },
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
   ix.guard_fd = -1;
   ix.dotfiles = dotfiles;
   ix.cmdbuf = g_string_new (NULL);
   ix.tag_queue = g_ptr_array_new_with_free_func (g_free);
   watch_init (&ix);
   ix.first_run = TRUE;
   ix.log_missing = TRUE;
   ix.log_keep = log_keep;
   ix.auto_kinds = auto_list ? auto_kinds_parse (auto_list) : AUTO_ALL;
   ix.missing = g_ptr_array_new_with_free_func (g_free);
   ix.log_dir = g_path_get_dirname (dbfile);

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
   signal (SIGPIPE, SIG_IGN);

   if (!open_db (&ix, dbfile))
      return 1;

   /* the images skipped last time are read once more (the last run may have
      died for another reason) */
   exec (&ix, "UPDATE files SET stage = 1 WHERE date_src = 3");
   ix.guard_path = g_strconcat (dbfile, ".reading", NULL);
   guard_check (&ix);
   ix.guard_fd = open (ix.guard_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

   do {
      ix.full = ix.rescan;
      ix.rescan = FALSE;
      run_stage1 (&ix);
      if (ix.quit) break;
      run_stage3 (&ix);
      if (ix.quit) break;
      /* again while files come in during it */
      do {
         run_stage2 (&ix);
      } while (!ix.quit && rows_to_date (&ix) > 0);
      if (ix.quit) break;
      if (ix.need_stage3) {
         ix.need_stage3 = FALSE;
         run_stage3 (&ix);
         if (ix.quit) break;
      }
      process_tag_queue (&ix, FALSE);
      say_idle (&ix);
      /* nothing more to do for now: wait for a command or for gimv to quit */
      while (!ix.quit && !ix.rescan) {
         struct pollfd pfd[2] = {
            { STDIN_FILENO,   POLLIN, 0 },
            { ix.inotify_fd,  POLLIN, 0 },
         };
         gint np = ix.inotify_fd >= 0 ? 2 : 1;

         if (poll (pfd, np, -1) < 0 && errno != EINTR) break;
         if (pfd[0].revents)
            check_stdin (&ix, 0);
         if (ix.quit) break;
         process_tag_queue (&ix, FALSE);
         if (np == 2 && (pfd[1].revents & POLLIN)) {
            /* let a burst (a folder copied) gather first */
            g_usleep (200 * 1000);
            watch_process (&ix);
         }
      }
   } while (!ix.quit);

   /* unanswered moves sent just before quitting */
   missing_flush (&ix);
   close_db (&ix);
   if (lockfd >= 0) close (lockfd);
   return 0;
}
