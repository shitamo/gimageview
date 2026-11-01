/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: runs gimv-faces, which finds the faces in the
 * images of the folders chosen for it.
 *
 * It is started when the indexer has nothing to do (the files come from
 * the index), works through the new and changed images and exits.  Each
 * later "idle" of the indexer (images came in) starts it again.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimageview.h"

#include <string.h>
#include <gio/gio.h>

#include "gimv_index.h"
#include "gimv_index_faces.h"
#include "gimv_index_db.h"
#include "prefs.h"

#define DETECT_MODEL    "face_detection_yunet_2023mar.onnx"
#define RECOGNIZE_MODEL "face_recognition_sface_2021dec.onnx"

static struct {
   GSubprocess      *proc;
   GOutputStream    *in;
   GDataInputStream *out;
   GCancellable     *cancel;

   GimvIndexFacesState state;
   gboolean          again;     /* asked while running: once more after it */
   gboolean          regroup;
   guint             todo, done;
   gchar            *error;
} fx;


static void start_faces (void);
static guint    regroup_id;


/******************************************************************************
 *  configuration
 ******************************************************************************/

gboolean
gimv_index_faces_available (void)
{
#ifdef ENABLE_FACES
   return TRUE;
#else
   return FALSE;
#endif
}


gboolean
gimv_index_faces_is_configured (void)
{
   gchar **dirs;
   gboolean found = FALSE;
   gint i;

   if (!gimv_index_faces_available ()) return FALSE;
   if (!gimv_index_is_configured () || !conf.index_face_dirs) return FALSE;
   dirs = g_strsplit (conf.index_face_dirs, ";", -1);
   for (i = 0; dirs[i] && !found; i++)
      found = *g_strstrip (dirs[i]) != '\0';
   g_strfreev (dirs);
   return found;
}


gchar *
gimv_index_faces_user_model_dir (void)
{
   return g_build_filename (g_get_user_data_dir (), "gimageview", "models", NULL);
}


static gboolean
has_models (const gchar *dir)
{
   gchar *a = g_build_filename (dir, DETECT_MODEL, NULL);
   gchar *b = g_build_filename (dir, RECOGNIZE_MODEL, NULL);
   gboolean ok = g_file_test (a, G_FILE_TEST_IS_REGULAR)
                 && g_file_test (b, G_FILE_TEST_IS_REGULAR);
   g_free (a);
   g_free (b);
   return ok;
}


/* where the model files are: the user's folder, then the one installed
 * with GImageView (a package may put them there); NULL: nowhere */
gchar *
gimv_index_faces_model_dir (void)
{
   gchar *dir = gimv_index_faces_user_model_dir ();

   if (has_models (dir)) return dir;
   g_free (dir);
   dir = g_build_filename (DATADIR, "models", NULL);
   if (has_models (dir)) return dir;
   g_free (dir);
   return NULL;
}


/******************************************************************************
 *  status
 ******************************************************************************/

GimvIndexFacesState
gimv_index_faces_get_state (void)
{
   return fx.state;
}


static void
count_faces (guint *faces, guint *groups, guint *named)
{
   sqlite3 *db = gimv_index_get_db ();
   sqlite3_stmt *st;

   *faces = *groups = *named = 0;
   if (!db) return;
   if (sqlite3_prepare_v2 (db, "SELECT count(*), count(DISTINCT cluster), "
                               "count(DISTINCT person_id) FROM faces",
                           -1, &st, NULL) != SQLITE_OK)
      return;
   if (sqlite3_step (st) == SQLITE_ROW) {
      *faces  = sqlite3_column_int (st, 0);
      *groups = sqlite3_column_int (st, 1);
      *named  = sqlite3_column_int (st, 2);
   }
   sqlite3_finalize (st);
}


/* the short text for the status bar while working, NULL otherwise */
gchar *
gimv_index_faces_status (void)
{
   if (fx.state != GIMV_INDEX_FACES_RUNNING) return NULL;
   if (!fx.todo) return g_strdup (_("Index: faces"));
   return g_strdup_printf (_("Index: faces %d%%"),
                           (gint) ((guint64) MIN (fx.done, fx.todo) * 100 / fx.todo));
}


/* a line for the tooltip of the status, NULL when off */
gchar *
gimv_index_faces_tooltip (void)
{
   guint faces, groups, named;
   gchar *dir, *text;

   switch (fx.state) {
   case GIMV_INDEX_FACES_RUNNING:
      if (fx.todo)
         return g_strdup_printf (_("Finding faces: %'u of %'u images."), fx.done, fx.todo);
      return g_strdup (_("Finding faces: starting."));
   case GIMV_INDEX_FACES_NO_MODELS:
      dir = gimv_index_faces_user_model_dir ();
      text = g_strdup_printf (_("Faces: the model files are not found. Put them in %s "
                                "(see the Index page of the preferences)."), dir);
      g_free (dir);
      return text;
   case GIMV_INDEX_FACES_ERROR:
      return g_strdup_printf (_("Faces: stopped (%s)."), fx.error ? fx.error : "?");
   case GIMV_INDEX_FACES_DONE:
      count_faces (&faces, &groups, &named);
      return g_strdup_printf (_("Faces: %'u found, %'u groups of faces that look alike."),
                              faces, groups);
   case GIMV_INDEX_FACES_OFF:
   default:
      return NULL;
   }
}


static void
set_state (GimvIndexFacesState state)
{
   fx.state = state;
   gimv_index_status_changed ();
}


/******************************************************************************
 *  the process
 ******************************************************************************/

static gchar *
find_exe (void)
{
   gchar *self, *dir, *path;

   self = g_file_read_link ("/proc/self/exe", NULL);
   if (self) {
      dir = g_path_get_dirname (self);
      path = g_build_filename (dir, "gimv-faces", NULL);
      g_free (dir);
      g_free (self);
      if (g_file_test (path, G_FILE_TEST_IS_EXECUTABLE))
         return path;
      g_free (path);
   }
   path = g_build_filename (LIBEXECDIR, "gimv-faces", NULL);
   if (g_file_test (path, G_FILE_TEST_IS_EXECUTABLE))
      return path;
   g_free (path);
   return NULL;
}


static void
handle_line (const gchar *line)
{
   gchar **v = g_strsplit (line, " ", -1);
   guint n = g_strv_length (v);

   if (n == 0) {
      /* nothing */
   } else if (!strcmp (v[0], "start") && n >= 2) {
      fx.todo = strtoul (v[1], NULL, 10);
      fx.done = 0;
      gimv_index_status_changed ();
   } else if (!strcmp (v[0], "progress") && n >= 3) {
      fx.done = strtoul (v[1], NULL, 10);
      fx.todo = strtoul (v[2], NULL, 10);
      gimv_index_status_changed ();
   } else if (!strcmp (v[0], "done")) {
      fx.state = GIMV_INDEX_FACES_DONE;
      gimv_index_status_changed ();
      gimv_index_faces_changed ();
      /* new faces: the names in the comment files may go on them now */
      gimv_index_sync_names ();
   } else if (!strcmp (v[0], "no-models")) {
      fx.state = GIMV_INDEX_FACES_NO_MODELS;
      gimv_index_status_changed ();
   } else if (!strcmp (v[0], "busy")) {
      fx.state = GIMV_INDEX_FACES_OFF;
      gimv_index_status_changed ();
   } else if (!strcmp (v[0], "error")) {
      g_warning ("gimv-faces: %s", line + 6);
      g_free (fx.error);
      /* OpenCV's messages are long: the start of the first line */
      fx.error = g_strndup (line + 6, strcspn (line + 6, "\n"));
      if (g_utf8_strlen (fx.error, -1) > 100) {
         gchar *cut = g_utf8_substring (fx.error, 0, 100);
         g_free (fx.error);
         fx.error = g_strconcat (cut, "…", NULL);
         g_free (cut);
      }
   }
   g_strfreev (v);
}


static void read_line (void);

static void
cb_exited (GObject *source, GAsyncResult *res, gpointer data)
{
   GSubprocess *proc = G_SUBPROCESS (source);

   g_subprocess_wait_finish (proc, res, NULL);
   if (fx.state == GIMV_INDEX_FACES_RUNNING) {
      /* ended without "done" */
      g_free (fx.error);
      if (g_subprocess_get_if_signaled (proc))
         fx.error = g_strdup_printf (_("signal %d"), g_subprocess_get_term_sig (proc));
      else if (!fx.error)
         fx.error = g_strdup_printf (_("exit status %d"), g_subprocess_get_exit_status (proc));
      set_state (GIMV_INDEX_FACES_ERROR);
   }
   g_object_unref (proc);

   if (fx.again && !fx.proc) {
      /* asked again while running (also after a crash: the image it died
       * on is not read again) */
      fx.again = FALSE;
      start_faces ();
   }
}


static void
cb_read_line (GObject *source, GAsyncResult *res, gpointer data)
{
   GError *error = NULL;
   gchar *line;

   line = g_data_input_stream_read_line_finish_utf8 (G_DATA_INPUT_STREAM (source),
                                                     res, NULL, &error);
   if (error) {
      gboolean cancelled = g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_error_free (error);
      if (cancelled) return;
      line = NULL;
   }

   if (!line) {
      GSubprocess *proc = fx.proc;
      fx.proc = NULL;
      fx.in = NULL;
      g_clear_object (&fx.out);
      g_clear_object (&fx.cancel);
      if (proc)
         g_subprocess_wait_async (proc, NULL, cb_exited, NULL);
      return;
   }

   handle_line (line);
   g_free (line);
   read_line ();
}


static void
read_line (void)
{
   g_data_input_stream_read_line_async (fx.out, G_PRIORITY_DEFAULT,
                                        fx.cancel, cb_read_line, NULL);
}


static void
start_faces (void)
{
   GPtrArray *argv;
   gchar *exe, *models, *dbfile, **dirs;
   GError *error = NULL;
   guint i;

   if (fx.proc) {
      fx.again = TRUE;
      return;
   }
   if (!gimv_index_faces_is_configured ()) {
      set_state (GIMV_INDEX_FACES_OFF);
      return;
   }

   models = gimv_index_faces_model_dir ();
   if (!models) {
      set_state (GIMV_INDEX_FACES_NO_MODELS);
      return;
   }
   exe = find_exe ();
   if (!exe) {
      g_free (models);
      g_free (fx.error);
      fx.error = g_strdup (_("gimv-faces is not found"));
      set_state (GIMV_INDEX_FACES_ERROR);
      return;
   }

   dbfile = gimv_index_db_default_path ();
   argv = g_ptr_array_new_with_free_func (g_free);
   g_ptr_array_add (argv, exe);
   g_ptr_array_add (argv, g_strdup ("--db"));
   g_ptr_array_add (argv, dbfile);
   g_ptr_array_add (argv, g_strdup ("--models"));
   g_ptr_array_add (argv, models);
   if (fx.regroup)
      g_ptr_array_add (argv, g_strdup ("--regroup"));
   fx.regroup = FALSE;
   g_ptr_array_add (argv, g_strdup ("--"));
   dirs = g_strsplit (conf.index_face_dirs, ";", -1);
   for (i = 0; dirs[i]; i++) {
      gchar *d = g_strstrip (dirs[i]);
      if (*d) g_ptr_array_add (argv, g_strdup (d));
   }
   g_strfreev (dirs);
   g_ptr_array_add (argv, NULL);

   fx.proc = g_subprocess_newv ((const gchar * const *) argv->pdata,
                                G_SUBPROCESS_FLAGS_STDIN_PIPE
                                | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
                                &error);
   g_ptr_array_unref (argv);
   if (!fx.proc) {
      g_free (fx.error);
      fx.error = g_strdup (error->message);
      g_error_free (error);
      set_state (GIMV_INDEX_FACES_ERROR);
      return;
   }

   g_clear_pointer (&fx.error, g_free);
   fx.in  = g_subprocess_get_stdin_pipe (fx.proc);
   fx.out = g_data_input_stream_new (g_subprocess_get_stdout_pipe (fx.proc));
   fx.cancel = g_cancellable_new ();
   fx.todo = fx.done = 0;
   set_state (GIMV_INDEX_FACES_RUNNING);
   read_line ();
}


/* returns the process still exiting (holding its lock), if any */
static GSubprocess *
stop_faces (void)
{
   GSubprocess *old = fx.proc ? g_object_ref (fx.proc) : NULL;

   fx.again = FALSE;
   if (fx.cancel) {
      g_cancellable_cancel (fx.cancel);
      g_clear_object (&fx.cancel);
   }
   if (fx.proc) {
      /* it commits what it has and exits at EOF of stdin */
      g_output_stream_close (fx.in, NULL, NULL);
      fx.in = NULL;
      g_clear_object (&fx.out);
      g_clear_object (&fx.proc);
   }
   fx.state = GIMV_INDEX_FACES_OFF;
   return old;
}


static void
cb_old_exited (GObject *source, GAsyncResult *res, gpointer data)
{
   g_subprocess_wait_finish (G_SUBPROCESS (source), res, NULL);
   g_object_unref (source);
   if (gimv_index_get_state () == GIMV_INDEX_STATE_IDLE)
      start_faces ();
}


/* the indexer has nothing to do */
void
gimv_index_faces_kick (void)
{
   if (fx.state == GIMV_INDEX_FACES_ERROR && !fx.proc) {
      /* not again and again after it died: the next start of gimv, or the
       * preferences changed */
      return;
   }
   start_faces ();
}


/* the folders changed in the preferences */
void
gimv_index_faces_restart (void)
{
   GSubprocess *old = stop_faces ();

   gimv_index_status_changed ();
   if (old)
      g_subprocess_wait_async (old, NULL, cb_old_exited, NULL);
   else if (gimv_index_get_state () == GIMV_INDEX_STATE_IDLE)
      start_faces ();
}


/* names were given: the groups of the rest are made again */
void
gimv_index_faces_regroup (void)
{
   fx.regroup = TRUE;
   start_faces ();
}


void
gimv_index_faces_quit (void)
{
   if (regroup_id) {
      g_source_remove (regroup_id);
      regroup_id = 0;
   }

   GSubprocess *old = stop_faces ();

   if (old) g_object_unref (old);
   g_clear_pointer (&fx.error, g_free);
}


/******************************************************************************
 *  what a person says about the faces (the faces window)
 ******************************************************************************/

#define REGROUP_DELAY 8   /* seconds after the last change */

static gboolean
cb_regroup (gpointer data)
{
   regroup_id = 0;
   gimv_index_faces_regroup ();
   return G_SOURCE_REMOVE;
}


static void
changed (void)
{
   gimv_index_faces_changed ();
   /* the names to the comment files too */
   gimv_index_sync_names ();
   /* the groups and guesses again, once the changes have stopped */
   if (regroup_id) g_source_remove (regroup_id);
   regroup_id = g_timeout_add_seconds (REGROUP_DELAY, cb_regroup, NULL);
}


/*
 * The changes are written in a worker thread, all of one in a single
 * transaction.  The indexer (and gimv-faces) keep a transaction open most
 * of the time while they work, committing every second or so; a writer
 * gets in only in the short gaps.  Before, each face was written by
 * itself in the main loop, each waiting up to 3 seconds when the gap was
 * missed: hundreds of faces froze gimv for many minutes.
 */

typedef struct {
   GimvIndexFacesEdit edit;
   GArray            *ids;
   gchar             *name;
   gint64             person;
   gint64             result;     /* RENAME: the person after it */
} EditData;


static void
edit_data_free (EditData *e)
{
   g_array_unref (e->ids);
   g_free (e->name);
   g_free (e);
}


/* run sql for each face id (?1; ?2 is "arg") */
static gboolean
for_each_face (sqlite3 *db, const gchar *sql, GArray *ids, gint64 arg)
{
   sqlite3_stmt *st;
   guint i;
   gboolean ok = TRUE;

   if (sqlite3_prepare_v2 (db, sql, -1, &st, NULL) != SQLITE_OK) return FALSE;
   for (i = 0; i < ids->len && ok; i++) {
      sqlite3_bind_int64 (st, 1, g_array_index (ids, gint64, i));
      sqlite3_bind_int64 (st, 2, arg);
      ok = sqlite3_step (st) == SQLITE_DONE;
      sqlite3_reset (st);
   }
   sqlite3_finalize (st);
   return ok;
}


/* the id of the person of that name, made when there is none */
static gint64
person_id (sqlite3 *db, const gchar *name)
{
   sqlite3_stmt *st;
   gint64 id = 0;

   if (sqlite3_prepare_v2 (db, "INSERT OR IGNORE INTO persons (name) VALUES (?1)",
                           -1, &st, NULL) == SQLITE_OK)
   {
      sqlite3_bind_text (st, 1, name, -1, SQLITE_TRANSIENT);
      sqlite3_step (st);
      sqlite3_finalize (st);
   }
   if (sqlite3_prepare_v2 (db, "SELECT id FROM persons WHERE name = ?1",
                           -1, &st, NULL) == SQLITE_OK)
   {
      sqlite3_bind_text (st, 1, name, -1, SQLITE_TRANSIENT);
      if (sqlite3_step (st) == SQLITE_ROW)
         id = sqlite3_column_int64 (st, 0);
      sqlite3_finalize (st);
   }
   return id;
}


static gboolean
exec_person (sqlite3 *db, const gchar *sql, gint64 a, gint64 b)
{
   sqlite3_stmt *st;
   gboolean ok;

   if (sqlite3_prepare_v2 (db, sql, -1, &st, NULL) != SQLITE_OK) return FALSE;
   sqlite3_bind_int64 (st, 1, a);
   sqlite3_bind_int64 (st, 2, b);
   ok = sqlite3_step (st) == SQLITE_DONE;
   sqlite3_finalize (st);
   return ok;
}


/* a new name; a name another person has: that person gets the faces, the
 * guesses and the "not this person" of this one, and this one goes */
static gboolean
rename_person (sqlite3 *db, EditData *e)
{
   sqlite3_stmt *st;
   gint64 other = 0, me = e->person;
   gboolean ok;

   if (sqlite3_prepare_v2 (db, "SELECT id FROM persons WHERE name = ?1", -1, &st, NULL)
       != SQLITE_OK)
      return FALSE;
   sqlite3_bind_text (st, 1, e->name, -1, SQLITE_TRANSIENT);
   if (sqlite3_step (st) == SQLITE_ROW) other = sqlite3_column_int64 (st, 0);
   sqlite3_finalize (st);

   if (!other || other == me) {
      if (sqlite3_prepare_v2 (db, "UPDATE persons SET name = ?2 WHERE id = ?1", -1, &st, NULL)
          != SQLITE_OK)
         return FALSE;
      sqlite3_bind_int64 (st, 1, me);
      sqlite3_bind_text (st, 2, e->name, -1, SQLITE_TRANSIENT);
      ok = sqlite3_step (st) == SQLITE_DONE;
      sqlite3_finalize (st);
      e->result = me;
      return ok;
   }

   ok = exec_person (db, "UPDATE faces SET person_id = ?2 WHERE person_id = ?1", me, other)
        && exec_person (db, "UPDATE faces SET guess = ?2 WHERE guess = ?1", me, other)
        && exec_person (db, "INSERT OR IGNORE INTO face_rejects (face_id, person_id) "
                            "SELECT face_id, ?2 FROM face_rejects WHERE person_id = ?1",
                        me, other)
        /* the tags of both */
        && exec_person (db, "INSERT OR IGNORE INTO person_tags (person_id, tag) "
                            "SELECT ?2, tag FROM person_tags WHERE person_id = ?1", me, other)
        /* a face said to be the other one is not "not this person" */
        && exec_person (db, "DELETE FROM face_rejects WHERE person_id = ?2 AND face_id IN "
                            "(SELECT id FROM faces WHERE person_id = ?2)", me, other)
        && exec_person (db, "DELETE FROM persons WHERE id = ?1", me, 0);
   e->result = other;
   return ok;
}


/* the tags of a person: as given (one a line), the ones before go */
static gboolean
set_person_tags (sqlite3 *db, EditData *e)
{
   sqlite3_stmt *st;
   gchar **tags;
   gboolean ok;
   guint i;

   if (!exec_person (db, "DELETE FROM person_tags WHERE person_id = ?1", e->person, 0))
      return FALSE;
   if (sqlite3_prepare_v2 (db, "INSERT OR IGNORE INTO person_tags (person_id, tag) "
                               "VALUES (?1, ?2)", -1, &st, NULL) != SQLITE_OK)
      return FALSE;
   tags = g_strsplit (e->name ? e->name : "", "\n", -1);
   ok = TRUE;
   for (i = 0; tags[i] && ok; i++) {
      gchar *t = g_strstrip (tags[i]);
      if (!*t) continue;
      sqlite3_bind_int64 (st, 1, e->person);
      sqlite3_bind_text (st, 2, t, -1, SQLITE_TRANSIENT);
      ok = sqlite3_step (st) == SQLITE_DONE;
      sqlite3_reset (st);
   }
   sqlite3_finalize (st);
   g_strfreev (tags);
   return ok;
}


/* "old\nnew": every person with the old tag gets the new one instead */
static gboolean
rename_person_tag (sqlite3 *db, EditData *e)
{
   gchar **v = g_strsplit (e->name ? e->name : "", "\n", 2);
   sqlite3_stmt *st;
   gboolean ok = v[0] && v[1] && *v[0] && *v[1];

   if (ok && strcmp (v[0], v[1])) {
      ok = sqlite3_prepare_v2 (db, "INSERT OR IGNORE INTO person_tags (person_id, tag) "
                                   "SELECT person_id, ?2 FROM person_tags WHERE tag = ?1",
                               -1, &st, NULL) == SQLITE_OK;
      if (ok) {
         sqlite3_bind_text (st, 1, v[0], -1, SQLITE_TRANSIENT);
         sqlite3_bind_text (st, 2, v[1], -1, SQLITE_TRANSIENT);
         ok = sqlite3_step (st) == SQLITE_DONE;
         sqlite3_finalize (st);
      }
      if (ok)
         ok = sqlite3_prepare_v2 (db, "DELETE FROM person_tags WHERE tag = ?1",
                                  -1, &st, NULL) == SQLITE_OK;
      if (ok) {
         sqlite3_bind_text (st, 1, v[0], -1, SQLITE_TRANSIENT);
         ok = sqlite3_step (st) == SQLITE_DONE;
         sqlite3_finalize (st);
      }
   }
   g_strfreev (v);
   return ok;
}


static gboolean
edit_in_db (sqlite3 *db, EditData *e)
{
   gint64 pid;

   switch (e->edit) {
   case GIMV_INDEX_FACES_SET_NAME:
      pid = person_id (db, e->name);
      if (!pid) return FALSE;
      return for_each_face (db, "UPDATE faces SET person_id = ?2, guess = NULL, cluster = NULL, "
                                "ignored = 0 WHERE id = ?1", e->ids, pid)
             /* told it is that person after all */
             && for_each_face (db, "DELETE FROM face_rejects WHERE face_id = ?1 "
                                   "AND person_id = ?2", e->ids, pid);
   case GIMV_INDEX_FACES_CONFIRM:
      return for_each_face (db, "UPDATE faces SET person_id = guess, guess = NULL, cluster = NULL "
                                "WHERE id = ?1 AND guess IS NOT NULL", e->ids, 0);
   case GIMV_INDEX_FACES_CLEAR_NAME:
      return for_each_face (db, "UPDATE faces SET person_id = NULL WHERE id = ?1", e->ids, 0);
   case GIMV_INDEX_FACES_REJECT:
      /* "not this person": the name (or the guess) goes, and the face is
       * not guessed as that person again */
      return for_each_face (db, "INSERT OR IGNORE INTO face_rejects (face_id, person_id) "
                                "VALUES (?1, ?2)", e->ids, e->person)
             && for_each_face (db, "UPDATE faces SET person_id = NULL "
                                   "WHERE id = ?1 AND person_id = ?2", e->ids, e->person)
             && for_each_face (db, "UPDATE faces SET guess = NULL "
                                   "WHERE id = ?1 AND guess = ?2", e->ids, e->person);
   case GIMV_INDEX_FACES_IGNORE:
      return for_each_face (db, "UPDATE faces SET ignored = 1, person_id = NULL, guess = NULL, "
                                "cluster = NULL WHERE id = ?1", e->ids, 0);
   case GIMV_INDEX_FACES_UNIGNORE:
      return for_each_face (db, "UPDATE faces SET ignored = 0 WHERE id = ?1", e->ids, 0);
   case GIMV_INDEX_FACES_RENAME:
      return rename_person (db, e);
   case GIMV_INDEX_FACES_PERSON_TAGS:
      return set_person_tags (db, e);
   case GIMV_INDEX_FACES_RENAME_PERSON_TAG:
      return rename_person_tag (db, e);
   }
   return FALSE;
}


static void
thread_edit (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   EditData *e = data;
   gchar *path = gimv_index_db_default_path ();
   sqlite3 *db = NULL;
   gboolean ok = FALSE;
   gchar *why = NULL;

   /* the indexer made it: only an existing one */
   if (sqlite3_open_v2 (path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK) {
      gimv_index_db_wait_busy (db, 60);
      sqlite3_exec (db, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);
      if (sqlite3_exec (db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK) {
         ok = edit_in_db (db, e)
              && sqlite3_exec (db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
         if (!ok) {
            why = g_strdup (sqlite3_errmsg (db));
            sqlite3_exec (db, "ROLLBACK", NULL, NULL, NULL);
         }
      } else {
         why = g_strdup (sqlite3_errmsg (db));
      }
   } else {
      why = g_strdup (db ? sqlite3_errmsg (db) : "?");
   }
   sqlite3_close (db);
   g_free (path);

   if (ok)
      g_task_return_boolean (task, TRUE);
   else
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", why ? why : "?");
   g_free (why);
}


void
gimv_index_faces_edit_async (GimvIndexFacesEdit edit, GArray *ids, const gchar *name,
                             gint64 person, GAsyncReadyCallback callback, gpointer data)
{
   EditData *e = g_new0 (EditData, 1);
   GTask *task = g_task_new (NULL, NULL, callback, data);

   e->edit = edit;
   e->ids = ids ? g_array_ref (ids) : g_array_new (FALSE, FALSE, sizeof (gint64));
   ids = e->ids;
   e->name = name ? (edit == GIMV_INDEX_FACES_PERSON_TAGS
                     || edit == GIMV_INDEX_FACES_RENAME_PERSON_TAG
                     ? g_strdup (name) : g_strstrip (g_strdup (name)))
                  : NULL;
   e->person = person;
   g_task_set_task_data (task, e, (GDestroyNotify) edit_data_free);
   if ((edit != GIMV_INDEX_FACES_RENAME && edit != GIMV_INDEX_FACES_PERSON_TAGS
        && edit != GIMV_INDEX_FACES_RENAME_PERSON_TAG && !ids->len)
       || ((edit == GIMV_INDEX_FACES_SET_NAME || edit == GIMV_INDEX_FACES_RENAME
            || edit == GIMV_INDEX_FACES_RENAME_PERSON_TAG)
           && (!e->name || !*e->name))) {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "nothing to do");
   } else {
      g_task_run_in_thread (task, thread_edit);
   }
   g_object_unref (task);
}


void
gimv_index_faces_named_by_indexer (void)
{
   gimv_index_faces_changed ();
   /* the groups and guesses again (not the names: they came from there) */
   if (regroup_id) g_source_remove (regroup_id);
   regroup_id = g_timeout_add_seconds (REGROUP_DELAY, cb_regroup, NULL);
}


/* TRUE when written; the tabs are told, and the groups made again later */
gboolean
gimv_index_faces_edit_finish (GAsyncResult *res, gint64 *person, GError **error)
{
   gboolean ok = g_task_propagate_boolean (G_TASK (res), error);
   EditData *e = g_task_get_task_data (G_TASK (res));

   if (person) *person = ok && e ? e->result : 0;
   if (ok && e && (e->edit == GIMV_INDEX_FACES_PERSON_TAGS
                   || e->edit == GIMV_INDEX_FACES_RENAME_PERSON_TAG))
      /* the tags of people: no names or faces changed */
      gimv_index_faces_changed ();
   else if (ok)
      changed ();
   return ok;
}


static GPtrArray *
string_list (const gchar *sql, gint64 arg)
{
   GPtrArray *list = g_ptr_array_new_with_free_func (g_free);
   sqlite3 *db = gimv_index_get_db ();
   sqlite3_stmt *st;

   if (db && sqlite3_prepare_v2 (db, sql, -1, &st, NULL) == SQLITE_OK) {
      if (sqlite3_bind_parameter_count (st) > 0) sqlite3_bind_int64 (st, 1, arg);
      while (sqlite3_step (st) == SQLITE_ROW)
         g_ptr_array_add (list, g_strdup ((const gchar *) sqlite3_column_text (st, 0)));
      sqlite3_finalize (st);
   }
   return list;
}


GPtrArray *
gimv_index_faces_get_person_tags (gint64 person)
{
   return string_list ("SELECT tag FROM person_tags WHERE person_id = ?1 ORDER BY tag", person);
}


GPtrArray *
gimv_index_faces_get_all_person_tags (void)
{
   return string_list ("SELECT DISTINCT tag FROM person_tags ORDER BY tag", 0);
}


gint64
gimv_index_faces_find_person (const gchar *name)
{
   sqlite3 *db = gimv_index_get_db ();
   sqlite3_stmt *st;
   gint64 id = 0;

   if (!db || !name) return 0;
   if (sqlite3_prepare_v2 (db, "SELECT id FROM persons WHERE name = ?1", -1, &st, NULL)
       != SQLITE_OK)
      return 0;
   sqlite3_bind_text (st, 1, name, -1, SQLITE_TRANSIENT);
   if (sqlite3_step (st) == SQLITE_ROW) id = sqlite3_column_int64 (st, 0);
   sqlite3_finalize (st);
   return id;
}


/* the names given so far, sorted */
GPtrArray *
gimv_index_faces_get_names (void)
{
   GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
   sqlite3 *db = gimv_index_get_db ();   /* reading only */
   sqlite3_stmt *st;

   if (db && sqlite3_prepare_v2 (db, "SELECT name FROM persons p WHERE EXISTS "
                                     "(SELECT 1 FROM faces WHERE person_id = p.id) "
                                     "ORDER BY name", -1, &st, NULL) == SQLITE_OK)
   {
      while (sqlite3_step (st) == SQLITE_ROW)
         g_ptr_array_add (names, g_strdup ((const gchar *) sqlite3_column_text (st, 0)));
      sqlite3_finalize (st);
   }
   return names;
}


/* the regroup waiting is done now (the faces window closes) */
void
gimv_index_faces_flush (void)
{
   if (!regroup_id) return;
   g_source_remove (regroup_id);
   regroup_id = 0;
   gimv_index_faces_regroup ();
}


/* the faces of a row of the "People" tab, as an SQL condition on "fa"
 * (faces): "p:ID" named so, "g:ID" looking like that person, "c:N" a group
 * of unnamed faces, "u" the other unnamed ones, "i" ignored, "t:TAG" the
 * people with that tag.  NULL for an
 * unknown key. */
gchar *
gimv_index_faces_key_where (const gchar *key)
{
   gint64 n;
   gchar *end;

   if (!key) return NULL;
   if (!strcmp (key, "u"))
      return g_strdup ("fa.person_id IS NULL AND fa.guess IS NULL AND fa.cluster IS NULL "
                       "AND fa.ignored = 0");
   if (!strcmp (key, "i"))
      return g_strdup ("fa.ignored = 1");
   /* "t:<tag>": the people with the tag */
   if (g_str_has_prefix (key, "t:") && key[2]) {
      gchar *q = sqlite3_mprintf ("fa.person_id IN (SELECT person_id FROM person_tags "
                                  "WHERE tag = %Q)", key + 2);
      gchar *w = g_strdup (q);
      sqlite3_free (q);
      return w;
   }
   if (strlen (key) < 3 || key[1] != ':') return NULL;
   n = g_ascii_strtoll (key + 2, &end, 10);
   if (*end || n <= 0) return NULL;
   switch (key[0]) {
   case 'p':
      return g_strdup_printf ("fa.person_id = %" G_GINT64_FORMAT, n);
   case 'g':
      return g_strdup_printf ("fa.guess = %" G_GINT64_FORMAT " AND fa.person_id IS NULL "
                              "AND fa.ignored = 0", n);
   case 'c':
      return g_strdup_printf ("fa.cluster = %" G_GINT64_FORMAT " AND fa.person_id IS NULL "
                              "AND fa.guess IS NULL AND fa.ignored = 0", n);
   default:
      return NULL;
   }
}
