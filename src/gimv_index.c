/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: runs gimv-indexer and reads its database.
 *
 * The indexer is a child process that lives only while gimv runs.  It is
 * started at startup (when enabled in the preferences) and stops by itself
 * when its stdin is closed, which also happens when gimv crashes.
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
#include "gimv_index_db.h"
#include "gimv_mime_types.h"
#include "gimv_thumb_win.h"
#include "prefs.h"

/* while scanning, tell the views at most this often (they count again) */
#define NOTIFY_INTERVAL_USEC (30 * G_USEC_PER_SEC)

typedef struct {
   guint                id;
   GimvIndexChangedFunc func;
   gpointer             data;
} Watch;

static struct {
   GSubprocess      *proc;
   GOutputStream    *in;
   GDataInputStream *out;
   GCancellable     *cancel;

   GimvIndexState    state;
   gint              stage;
   guint             n_dirs, n_files;
   guint             todo, done;      /* stage 2 */
   gchar            *status;

   gchar            *dbfile;
   sqlite3          *db;

   GList            *watches;
   guint             watch_id;
   gint64            last_notify;
} ix;


/******************************************************************************
 *  status
 ******************************************************************************/

static void
update_status (void)
{
   GList *node;

   g_free (ix.status);
   switch (ix.state) {
   case GIMV_INDEX_STATE_STARTING:
      ix.status = g_strdup (_("Index: starting"));
      break;
   case GIMV_INDEX_STATE_SCANNING:
      if (ix.stage == 2)
         ix.status = g_strdup_printf (_("Index: reading dates %u/%u"),
                                      ix.done, ix.todo);
      else if (ix.stage == 3)
         ix.status = g_strdup_printf (_("Index: reading tags %u"), ix.done);
      else
         /* the total is not known in advance: just count up */
         ix.status = g_strdup_printf (_("Index: scanning %u folders, %u images"),
                                      ix.n_dirs, ix.n_files);
      break;
   case GIMV_INDEX_STATE_IDLE:
      ix.status = g_strdup_printf (_("Index: %u images"), ix.n_files);
      break;
   case GIMV_INDEX_STATE_BUSY:
      ix.status = g_strdup (_("Index: used by another GImageView"));
      break;
   case GIMV_INDEX_STATE_ERROR:
      ix.status = g_strdup (_("Index: error"));
      break;
   case GIMV_INDEX_STATE_OFF:
   default:
      ix.status = g_strdup ("");
      break;
   }

   for (node = gimv_thumb_win_get_list (); node; node = g_list_next (node)) {
      GimvThumbWin *tw = node->data;
      if (!tw || !tw->index_label) continue;
      gtk_label_set_text (GTK_LABEL (tw->index_label), ix.status);
      gtk_widget_set_visible (tw->index_label, ix.state != GIMV_INDEX_STATE_OFF);
   }
}


const gchar *
gimv_index_get_status_text (void)
{
   return ix.status ? ix.status : "";
}


GimvIndexState
gimv_index_get_state (void)
{
   return ix.state;
}


static void
notify_watches (GimvIndexChange what)
{
   GList *node = ix.watches;

   ix.last_notify = g_get_monotonic_time ();
   while (node) {
      Watch *w = node->data;
      node = g_list_next (node);   /* the callback may remove itself */
      w->func (what, w->data);
   }
}


/* which views a stage changes: stage 1 adds and removes files (and so
 * their tags), stage 2 dates, stage 3 tags */
static GimvIndexChange
stage_changes (gint stage)
{
   switch (stage) {
   case 1:  return GIMV_INDEX_CHANGED_DATES | GIMV_INDEX_CHANGED_TAGS;
   case 3:  return GIMV_INDEX_CHANGED_TAGS;
   default: return GIMV_INDEX_CHANGED_DATES;
   }
}


guint
gimv_index_add_watch (GimvIndexChangedFunc func, gpointer data)
{
   Watch *w = g_new0 (Watch, 1);
   w->id = ++ix.watch_id;
   w->func = func;
   w->data = data;
   ix.watches = g_list_append (ix.watches, w);
   return w->id;
}


void
gimv_index_remove_watch (guint id)
{
   GList *node;

   for (node = ix.watches; node; node = g_list_next (node)) {
      Watch *w = node->data;
      if (w->id == id) {
         ix.watches = g_list_delete_link (ix.watches, node);
         g_free (w);
         return;
      }
   }
}


/******************************************************************************
 *  database
 ******************************************************************************/

sqlite3 *
gimv_index_get_db (void)
{
   if (!ix.dbfile)
      ix.dbfile = gimv_index_db_default_path ();
   if (!ix.db)
      gimv_index_db_open (ix.dbfile, FALSE, &ix.db, NULL);
   return ix.db;
}


/******************************************************************************
 *  the indexer process
 ******************************************************************************/

static gchar *
find_indexer (void)
{
   gchar *self, *dir, *path;

   /* next to gimv itself: running from the build directory */
   self = g_file_read_link ("/proc/self/exe", NULL);
   if (self) {
      dir = g_path_get_dirname (self);
      path = g_build_filename (dir, "gimv-indexer", NULL);
      g_free (dir);
      g_free (self);
      if (g_file_test (path, G_FILE_TEST_IS_EXECUTABLE))
         return path;
      g_free (path);
   }

   path = g_build_filename (LIBEXECDIR, "gimv-indexer", NULL);
   if (g_file_test (path, G_FILE_TEST_IS_EXECUTABLE))
      return path;
   g_free (path);
   return NULL;
}


static gchar *
ext_list (void)
{
   GString *s = g_string_new (NULL);
   GList *lists[2], *node;
   guint i;

   lists[0] = gimv_mime_types_get_known_enabled_ext_list ();
   lists[1] = gimv_mime_types_get_userdef_ext_list ();
   for (i = 0; i < G_N_ELEMENTS (lists); i++) {
      for (node = lists[i]; node; node = g_list_next (node)) {
         const gchar *ext = node->data;
         if (!ext || !*ext || strchr (ext, ',')) continue;
         if (s->len) g_string_append_c (s, ',');
         g_string_append (s, ext);
      }
   }
   return g_string_free (s, FALSE);
}


static void read_line (void);

static void
handle_line (const gchar *line)
{
   gchar **v = g_strsplit (line, " ", -1);
   guint n = g_strv_length (v);

   if (n == 0) {
      /* nothing */
   } else if (!strcmp (v[0], "busy")) {
      ix.state = GIMV_INDEX_STATE_BUSY;
      update_status ();
   } else if (!strcmp (v[0], "stage") && n >= 3) {
      ix.stage = atoi (v[1]);
      if (!strcmp (v[2], "start")) {
         ix.state = GIMV_INDEX_STATE_SCANNING;
         if (ix.stage == 1) {
            ix.n_dirs = ix.n_files = 0;
         } else if (ix.stage == 3) {
            ix.done = 0;
         } else {
            ix.todo = n >= 4 ? strtoul (v[3], NULL, 10) : 0;
            ix.done = 0;
         }
      } else if (!strcmp (v[2], "done") && n >= 5) {
         /* the number of images comes from stage 1 */
         if (ix.stage == 1) {
            ix.n_dirs  = strtoul (v[3], NULL, 10);
            ix.n_files = strtoul (v[4], NULL, 10);
         }
         notify_watches (stage_changes (ix.stage));
      }
      update_status ();
   } else if (!strcmp (v[0], "progress") && n >= 4) {
      if (atoi (v[1]) >= 2) {
         ix.done = strtoul (v[3], NULL, 10);
      } else {
         ix.n_dirs  = strtoul (v[2], NULL, 10);
         ix.n_files = strtoul (v[3], NULL, 10);
      }
      update_status ();
      if (g_get_monotonic_time () - ix.last_notify > NOTIFY_INTERVAL_USEC)
         notify_watches (stage_changes (atoi (v[1])));
   } else if (!strcmp (v[0], "tags-updated")) {
      notify_watches (GIMV_INDEX_CHANGED_TAGS);
   } else if (!strcmp (v[0], "idle")) {
      ix.state = GIMV_INDEX_STATE_IDLE;
      update_status ();
   } else if (!strcmp (v[0], "error")) {
      g_warning ("gimv-indexer: %s", line + 6);
   }

   g_strfreev (v);
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
      /* EOF: the indexer has exited */
      if (ix.state != GIMV_INDEX_STATE_BUSY && ix.state != GIMV_INDEX_STATE_OFF)
         ix.state = GIMV_INDEX_STATE_ERROR;
      update_status ();
      g_clear_object (&ix.out);
      g_clear_object (&ix.proc);
      ix.in = NULL;
      return;
   }

   handle_line (line);
   g_free (line);
   read_line ();
}


static void
read_line (void)
{
   g_data_input_stream_read_line_async (ix.out, G_PRIORITY_DEFAULT,
                                        ix.cancel, cb_read_line, NULL);
}


static void
start_indexer (void)
{
   GPtrArray *argv;
   gchar *exe, *exts, **dirs;
   GError *error = NULL;
   guint i, n_dirs = 0;

   if (ix.proc) return;

   g_free (ix.dbfile);
   ix.dbfile = gimv_index_db_default_path ();

   if (!conf.index_enable || !conf.index_dirs || !*conf.index_dirs) {
      ix.state = GIMV_INDEX_STATE_OFF;
      update_status ();
      return;
   }

   exe = find_indexer ();
   if (!exe) {
      g_warning ("gimv-indexer not found");
      ix.state = GIMV_INDEX_STATE_ERROR;
      update_status ();
      return;
   }

   argv = g_ptr_array_new_with_free_func (g_free);
   g_ptr_array_add (argv, exe);
   g_ptr_array_add (argv, g_strdup ("--db"));
   g_ptr_array_add (argv, g_strdup (ix.dbfile));
   if (conf.read_dotfile)
      g_ptr_array_add (argv, g_strdup ("--dotfiles"));
   exts = ext_list ();
   if (*exts) {
      g_ptr_array_add (argv, g_strdup ("--ext"));
      g_ptr_array_add (argv, exts);
   } else {
      g_free (exts);
   }
   g_ptr_array_add (argv, g_strdup ("--"));
   dirs = g_strsplit (conf.index_dirs, ";", -1);
   for (i = 0; dirs[i]; i++) {
      gchar *d = g_strstrip (dirs[i]);
      if (!*d) continue;
      g_ptr_array_add (argv, g_strdup (d));
      n_dirs++;
   }
   g_strfreev (dirs);
   g_ptr_array_add (argv, NULL);

   if (n_dirs == 0) {
      g_ptr_array_unref (argv);
      ix.state = GIMV_INDEX_STATE_OFF;
      update_status ();
      return;
   }

   ix.proc = g_subprocess_newv ((const gchar * const *) argv->pdata,
                                G_SUBPROCESS_FLAGS_STDIN_PIPE
                                | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
                                &error);
   g_ptr_array_unref (argv);
   if (!ix.proc) {
      g_warning ("cannot start gimv-indexer: %s", error->message);
      g_error_free (error);
      ix.state = GIMV_INDEX_STATE_ERROR;
      update_status ();
      return;
   }

   ix.in  = g_subprocess_get_stdin_pipe (ix.proc);
   ix.out = g_data_input_stream_new (g_subprocess_get_stdout_pipe (ix.proc));
   ix.cancel = g_cancellable_new ();
   ix.state = GIMV_INDEX_STATE_STARTING;
   ix.n_dirs = ix.n_files = 0;
   update_status ();
   read_line ();
}


static void
send_command (const gchar *cmd)
{
   if (!ix.in) return;
   g_output_stream_write_all (ix.in, cmd, strlen (cmd), NULL, NULL, NULL);
   g_output_stream_flush (ix.in, NULL, NULL);
}


static void
stop_indexer (void)
{
   if (ix.cancel) {
      g_cancellable_cancel (ix.cancel);
      g_clear_object (&ix.cancel);
   }
   if (ix.proc) {
      /* the indexer commits what it has and exits at EOF of stdin */
      send_command ("quit\n");
      g_output_stream_close (ix.in, NULL, NULL);
      ix.in = NULL;
      g_clear_object (&ix.out);
      g_clear_object (&ix.proc);
   }
   ix.state = GIMV_INDEX_STATE_OFF;
}


gboolean
gimv_index_is_configured (void)
{
   gchar **dirs;
   gboolean found = FALSE;
   gint i;

   if (!conf.index_enable || !conf.index_dirs) return FALSE;
   dirs = g_strsplit (conf.index_dirs, ";", -1);
   for (i = 0; dirs[i] && !found; i++)
      found = *g_strstrip (dirs[i]) != '\0';
   g_strfreev (dirs);
   return found;
}


void
gimv_index_init (void)
{
   start_indexer ();
}


void
gimv_index_quit (void)
{
   stop_indexer ();
   if (ix.db) {
      sqlite3_close (ix.db);
      ix.db = NULL;
   }
}


static void
cb_old_exited (GObject *source, GAsyncResult *res, gpointer data)
{
   g_subprocess_wait_finish (G_SUBPROCESS (source), res, NULL);
   g_object_unref (source);
   start_indexer ();
}


void
gimv_index_restart (void)
{
   GSubprocess *old = ix.proc ? g_object_ref (ix.proc) : NULL;

   stop_indexer ();
   if (old)
      /* the old one holds the lock until it has committed and exited */
      g_subprocess_wait_async (old, NULL, cb_old_exited, NULL);
   else
      start_indexer ();
}


void
gimv_index_tags_changed (const gchar *path)
{
   gchar *cmd;

   if (!path || path[0] != '/' || strchr (path, '\n')) return;
   /* without the indexer the next stage 3 reads it */
   if (!ix.proc) return;
   cmd = g_strdup_printf ("tags %s\n", path);
   send_command (cmd);
   g_free (cmd);
}


void
gimv_index_rescan (void)
{
   if (ix.proc)
      send_command ("rescan\n");
   else
      start_indexer ();
}
