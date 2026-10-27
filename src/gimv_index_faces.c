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
   GSubprocess *old = stop_faces ();

   if (old) g_object_unref (old);
   g_clear_pointer (&fx.error, g_free);
}
