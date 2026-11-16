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
 * gimv-faces with the model files: OpenCV reads them and runs them (an
 * OpenCV that cannot is found here, not by the faces never coming), a
 * model file of the same name with something else in it is not used, the
 * recognizer is recorded in the index, and the features are made again
 * when it is another one.
 *
 * The model files are not in the source (about 40 MB): they are taken
 * from GIMV_TEST_FACE_MODELS, else ~/.local/share/gimageview/models; the
 * tests are skipped without them.  GIMV_TEST_FACE_IMAGE: a photo with a
 * face in it, to see one found (no photo of a person is in the source).
 */

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <sqlite3.h>
#include <string.h>

#include "gimv_test.h"

#define DETECT_MODEL    "face_detection_yunet_2023mar.onnx"
#define RECOGNIZE_MODEL "face_recognition_sface_2021dec.onnx"
#define FEATURE_KEY     "face_feature_model"
#define FEATURE_LEN     128

static gchar *tmp;
static gchar *home;
static gchar *models;     /* NULL: the tests skipped */
static guint  serial;


static gchar *
find_models (void)
{
   const gchar *env = g_getenv ("GIMV_TEST_FACE_MODELS");
   gchar *dir = env ? g_strdup (env)
                    : g_build_filename (g_get_user_data_dir (), "gimageview", "models", NULL);
   gchar *a = g_build_filename (dir, DETECT_MODEL, NULL);
   gchar *b = g_build_filename (dir, RECOGNIZE_MODEL, NULL);
   gboolean ok = g_file_test (a, G_FILE_TEST_IS_REGULAR) && g_file_test (b, G_FILE_TEST_IS_REGULAR);

   g_free (a);
   g_free (b);
   if (ok) return dir;
   g_free (dir);
   return NULL;
}


static gboolean
skip (void)
{
   if (!g_getenv ("GIMV_TEST_FACES")) {
      g_test_skip ("built without gimv-faces");
      return TRUE;
   }
   if (!models) {
      g_test_skip ("no model files (GIMV_TEST_FACE_MODELS or "
                   "~/.local/share/gimageview/models)");
      return TRUE;
   }
   return FALSE;
}


/* an image with no face: a gradient */
static void
write_png (const gchar *path, gint w, gint h)
{
   GdkPixbuf *p = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, w, h);
   guchar *px = gdk_pixbuf_get_pixels (p);
   gint rs = gdk_pixbuf_get_rowstride (p), x, y;
   GError *error = NULL;

   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++) {
         guchar *q = px + y * rs + x * 3;
         q[0] = x * 255 / w;
         q[1] = y * 255 / h;
         q[2] = 128;
      }
   gdk_pixbuf_save (p, path, "png", &error, NULL);
   g_assert_no_error (error);
   g_object_unref (p);
}


/* a new folder with the image "name" (copied from src, else a gradient),
 * indexed into <folder>.db */
static gchar *
new_index (const gchar *src, gchar **db, gchar **image)
{
   gchar *name = g_strdup_printf ("root%u", serial++);
   gchar *root = g_build_filename (tmp, name, NULL);
   const gchar *exe = g_getenv ("GIMV_TEST_INDEXER");
   GSubprocessLauncher *l;
   GSubprocess *proc;
   GDataInputStream *out;
   GError *error = NULL;
   gchar *line;
   gboolean idle = FALSE;

   g_assert_nonnull (exe);
   g_assert_cmpint (g_mkdir_with_parents (root, 0700), ==, 0);
   if (src) {
      gchar *base = g_path_get_basename (src), *data;
      gsize len;
      *image = g_build_filename (root, base, NULL);
      g_assert_true (g_file_get_contents (src, &data, &len, &error));
      g_assert_true (g_file_set_contents (*image, data, len, &error));
      g_free (data);
      g_free (base);
   } else {
      *image = g_build_filename (root, "plain.png", NULL);
      write_png (*image, 640, 480);
   }
   *db = g_strconcat (root, ".db", NULL);

   l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE);
   g_subprocess_launcher_setenv (l, "HOME", home, TRUE);
   proc = g_subprocess_launcher_spawn (l, &error, exe, "--db", *db, root, NULL);
   g_assert_no_error (error);
   g_object_unref (l);
   out = g_data_input_stream_new (g_subprocess_get_stdout_pipe (proc));
   while (!idle && (line = g_data_input_stream_read_line_utf8 (out, NULL, NULL, &error))) {
      idle = g_str_has_prefix (line, "idle");
      g_free (line);
   }
   g_assert_no_error (error);
   g_assert_true (idle);
   g_output_stream_write_all (g_subprocess_get_stdin_pipe (proc), "quit\n", 5, NULL, NULL, &error);
   g_assert_no_error (error);
   g_subprocess_wait (proc, NULL, &error);
   g_assert_no_error (error);
   g_object_unref (out);
   g_object_unref (proc);
   g_free (name);
   return root;
}


/* gimv-faces --db db --models dir [extra] -- folder: all it said (its
 * stdin open until it ends: closed, it stops) */
static gchar *
run_faces (const gchar *db, const gchar *dir, const gchar *folder, gint *status)
{
   const gchar *exe = g_getenv ("GIMV_TEST_FACES");
   GSubprocess *proc;
   GDataInputStream *out;
   GString *all = g_string_new (NULL);
   GError *error = NULL;
   gchar *line;

   proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
                            &error, exe, "--db", db, "--models", dir, "--", folder, NULL);
   g_assert_no_error (error);
   out = g_data_input_stream_new (g_subprocess_get_stdout_pipe (proc));
   while ((line = g_data_input_stream_read_line_utf8 (out, NULL, NULL, &error))) {
      g_string_append (all, line);
      g_string_append_c (all, '\n');
      g_free (line);
   }
   g_assert_no_error (error);
   g_output_stream_close (g_subprocess_get_stdin_pipe (proc), NULL, NULL);
   g_subprocess_wait (proc, NULL, &error);
   g_assert_no_error (error);
   *status = g_subprocess_get_exit_status (proc);
   g_object_unref (out);
   g_object_unref (proc);
   if (g_test_verbose ()) g_printerr ("%s", all->str);
   return g_string_free (all, FALSE);
}


static gboolean
said (const gchar *all, const gchar *start)
{
   gchar **lines = g_strsplit (all, "\n", -1);
   gboolean ret = FALSE;
   for (guint i = 0; lines[i] && !ret; i++)
      ret = g_str_has_prefix (lines[i], start);
   g_strfreev (lines);
   return ret;
}


static sqlite3 *
db_open (const gchar *db)
{
   sqlite3 *d = NULL;
   g_assert_cmpint (sqlite3_open (db, &d), ==, SQLITE_OK);
   sqlite3_busy_timeout (d, 5000);
   return d;
}


static gchar *
meta (const gchar *db, const gchar *key)
{
   sqlite3 *d = db_open (db);
   sqlite3_stmt *st;
   gchar *v = NULL;

   g_assert_cmpint (sqlite3_prepare_v2 (d, "SELECT value FROM meta WHERE key = ?", -1, &st, NULL),
                    ==, SQLITE_OK);
   sqlite3_bind_text (st, 1, key, -1, SQLITE_STATIC);
   if (sqlite3_step (st) == SQLITE_ROW) v = g_strdup ((const gchar *) sqlite3_column_text (st, 0));
   sqlite3_finalize (st);
   sqlite3_close (d);
   return v;
}


static void
sql (const gchar *db, const gchar *text)
{
   sqlite3 *d = db_open (db);
   gchar *err = NULL;
   if (sqlite3_exec (d, text, NULL, NULL, &err) != SQLITE_OK)
      g_error ("%s: %s", text, err);
   sqlite3_close (d);
}


/* the models read and run on an image (no face in it); the recognizer
 * recorded */
static void
test_run (void)
{
   gchar *db, *image, *root, *all, *rec;
   gint status;

   if (skip ()) return;
   root = new_index (NULL, &db, &image);
   all = run_faces (db, models, root, &status);
   /* what it said first: OpenCV's message when it cannot run the models */
   if (!said (all, "done 1 0 "))
      g_error ("gimv-faces did not read the image (exit status %d):\n%s", status, all);
   g_assert_cmpint (status, ==, 0);
   rec = meta (db, FEATURE_KEY);
   g_assert_nonnull (rec);
   g_assert_true (g_str_has_prefix (rec, RECOGNIZE_MODEL " "));
   g_assert_cmpuint (strlen (rec), ==, strlen (RECOGNIZE_MODEL) + 1 + 64);
   g_free (rec);
   g_free (all);
   g_free (root);
   g_free (db);
   g_free (image);
}


/* a model file of the name with something else in it: not used */
static void
test_unknown (void)
{
   gchar *db, *image, *root, *all, *dir, *src, *dst, *data;
   const gchar *names[] = { DETECT_MODEL, RECOGNIZE_MODEL };
   GError *error = NULL;
   gsize len;
   gint status;

   if (skip ()) return;
   root = new_index (NULL, &db, &image);
   dir = g_build_filename (tmp, "badmodels", NULL);
   g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
   for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
      src = g_build_filename (models, names[i], NULL);
      dst = g_build_filename (dir, names[i], NULL);
      g_assert_true (g_file_get_contents (src, &data, &len, &error));
      /* the recognizer one byte different */
      if (i == 1) data[len / 2] ^= 1;
      g_assert_true (g_file_set_contents (dst, data, len, &error));
      g_free (data);
      g_free (src);
      g_free (dst);
   }
   all = run_faces (db, dir, root, &status);
   g_assert_cmpint (status, ==, 0);
   if (!said (all, "models-unknown " RECOGNIZE_MODEL))
      g_error ("not said:\n%s", all);
   g_assert_false (said (all, "start "));
   g_assert_null (meta (db, FEATURE_KEY));
   g_free (all);
   g_free (dir);
   g_free (root);
   g_free (db);
   g_free (image);
}


/* another recognizer recorded: the features of the faces made again, the
 * faces and their names kept */
static void
test_refeature (void)
{
   gchar *db, *image, *root, *all, *rec, *q;
   gint status;
   sqlite3 *d;
   sqlite3_stmt *st;
   float zero[FEATURE_LEN] = { 0 };

   if (skip ()) return;
   root = new_index (NULL, &db, &image);
   all = run_faces (db, models, root, &status);
   g_assert_cmpint (status, ==, 0);
   g_free (all);

   /* a face put by hand (a person marked it) with a name, as if made with
    * another recognizer */
   zero[0] = 1.0f;
   d = db_open (db);
   g_assert_cmpint (sqlite3_exec (d, "INSERT INTO persons (id, name) VALUES (1, 'P')",
                                  NULL, NULL, NULL), ==, SQLITE_OK);
   g_assert_cmpint (sqlite3_prepare_v2 (d, "INSERT INTO faces (id, file_id, x, y, w, h, score, "
                                           "feature, person_id) SELECT 1, id, 200, 150, 120, 120, "
                                           "0, ?, 1 FROM files", -1, &st, NULL), ==, SQLITE_OK);
   sqlite3_bind_blob (st, 1, zero, sizeof (zero), SQLITE_STATIC);
   g_assert_cmpint (sqlite3_step (st), ==, SQLITE_DONE);
   sqlite3_finalize (st);
   sqlite3_exec (d, "INSERT INTO face_manual (face_id) VALUES (1)", NULL, NULL, NULL);
   sqlite3_close (d);
   q = g_strdup_printf ("UPDATE meta SET value = 'face_recognition_old.onnx 00' "
                        "WHERE key = '%s'", FEATURE_KEY);
   sql (db, q);
   g_free (q);

   all = run_faces (db, models, root, &status);
   g_assert_cmpint (status, ==, 0);
   if (!said (all, "refeature 1"))
      g_error ("the features not made again:\n%s", all);
   g_assert_true (said (all, "done "));
   rec = meta (db, FEATURE_KEY);
   g_assert_true (g_str_has_prefix (rec, RECOGNIZE_MODEL " "));
   g_free (rec);
   g_assert_null (meta (db, "face_refeature_file"));

   /* the face kept with its name, a new feature (of length 1) */
   d = db_open (db);
   g_assert_cmpint (sqlite3_prepare_v2 (d, "SELECT feature, person_id FROM faces WHERE id = 1",
                                        -1, &st, NULL), ==, SQLITE_OK);
   g_assert_cmpint (sqlite3_step (st), ==, SQLITE_ROW);
   g_assert_cmpint (sqlite3_column_bytes (st, 0), ==, sizeof (zero));
   {
      const float *f = sqlite3_column_blob (st, 0);
      double n = 0;
      for (gint i = 0; i < FEATURE_LEN; i++) n += (double) f[i] * f[i];
      g_assert_cmpfloat_with_epsilon (n, 1.0, 1e-3);
      g_assert_cmpfloat (f[0], !=, 1.0f);
   }
   g_assert_cmpint (sqlite3_column_int (st, 1), ==, 1);
   sqlite3_finalize (st);
   sqlite3_close (d);

   g_free (all);
   g_free (root);
   g_free (db);
   g_free (image);
}


/* a photo of a person (GIMV_TEST_FACE_IMAGE): a face found */
static void
test_photo (void)
{
   const gchar *photo = g_getenv ("GIMV_TEST_FACE_IMAGE");
   gchar *db, *image, *root, *all;
   gint status;

   if (skip ()) return;
   if (!photo) {
      g_test_skip ("no GIMV_TEST_FACE_IMAGE");
      return;
   }
   root = new_index (photo, &db, &image);
   all = run_faces (db, models, root, &status);
   g_assert_cmpint (status, ==, 0);
   if (!said (all, "done 1 ") || said (all, "done 1 0 "))
      g_error ("no face found:\n%s", all);
   g_free (all);
   g_free (root);
   g_free (db);
   g_free (image);
}


int
main (int argc, char *argv[])
{
   gint ret;

   g_test_init (&argc, &argv, NULL);
   models = find_models ();
   tmp = gimv_test_mkdtemp ();
   home = g_build_filename (tmp, "home", NULL);
   g_mkdir_with_parents (home, 0700);

   g_test_add_func ("/faces/run", test_run);
   g_test_add_func ("/faces/unknown-model", test_unknown);
   g_test_add_func ("/faces/refeature", test_refeature);
   g_test_add_func ("/faces/photo", test_photo);

   ret = g_test_run ();
   gimv_test_rm_rf (tmp);
   g_free (home);
   g_free (tmp);
   g_free (models);
   return ret;
}
