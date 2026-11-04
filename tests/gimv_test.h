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
 * Helpers shared by the tests (GLib only).
 *
 * Known bugs: a test of a bug not fixed yet starts with
 *
 *    if (gimv_test_known_bug ("what goes wrong")) return;
 *
 * It is then reported as TODO (not a failure) unless GIMV_TEST_KNOWN_BUGS
 * is set in the environment, which runs it (and makes it fail).
 */

#ifndef __GIMV_TEST_H__
#define __GIMV_TEST_H__

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

static inline gboolean
gimv_test_known_bug (const gchar *what)
{
   if (g_getenv ("GIMV_TEST_KNOWN_BUGS")) return FALSE;
   g_test_incomplete (what);
   return TRUE;
}


/* a new empty directory (to remove with gimv_test_rm_rf ()) */
static inline gchar *
gimv_test_mkdtemp (void)
{
   GError *error = NULL;
   gchar *dir = g_dir_make_tmp ("gimv-test-XXXXXX", &error);

   g_assert_no_error (error);
   g_assert_nonnull (dir);
   return dir;
}


static inline void
gimv_test_rm_rf (const gchar *path)
{
   GDir *dir;

   if (!path) return;
   if (!g_file_test (path, G_FILE_TEST_IS_SYMLINK)
       && g_file_test (path, G_FILE_TEST_IS_DIR)
       && (dir = g_dir_open (path, 0, NULL)))
   {
      const gchar *name;
      while ((name = g_dir_read_name (dir))) {
         gchar *p = g_build_filename (path, name, NULL);
         gimv_test_rm_rf (p);
         g_free (p);
      }
      g_dir_close (dir);
      g_rmdir (path);
   } else {
      g_unlink (path);
   }
}


/* path (its directories made) with len bytes of data */
static inline void
gimv_test_write (const gchar *path, gconstpointer data, gsize len)
{
   GError *error = NULL;
   gchar *dir = g_path_get_dirname (path);

   g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
   g_free (dir);
   g_file_set_contents (path, data, len, &error);
   g_assert_no_error (error);
}


static inline void
gimv_test_write_string (const gchar *path, const gchar *text)
{
   gimv_test_write (path, text, strlen (text));
}


/* the mtime (and atime) of path set to t (seconds since the epoch) */
static inline void
gimv_test_set_mtime (const gchar *path, gint64 t)
{
   struct timespec times[2];

   times[0].tv_sec = t; times[0].tv_nsec = 0;
   times[1].tv_sec = t; times[1].tv_nsec = 0;
   g_assert_cmpint (utimensat (AT_FDCWD, path, times, 0), ==, 0);
}


/* a GByteArray to build test files in */
static inline void
gimv_test_put8 (GByteArray *a, guint v)
{
   guint8 b = v;
   g_byte_array_append (a, &b, 1);
}

static inline void
gimv_test_put16le (GByteArray *a, guint v)
{
   gimv_test_put8 (a, v & 0xff);
   gimv_test_put8 (a, (v >> 8) & 0xff);
}

static inline void
gimv_test_put32le (GByteArray *a, guint32 v)
{
   gimv_test_put16le (a, v & 0xffff);
   gimv_test_put16le (a, v >> 16);
}

static inline void
gimv_test_put32be (GByteArray *a, guint32 v)
{
   gimv_test_put8 (a, v >> 24);
   gimv_test_put8 (a, (v >> 16) & 0xff);
   gimv_test_put8 (a, (v >> 8) & 0xff);
   gimv_test_put8 (a, v & 0xff);
}

static inline void
gimv_test_put_bytes (GByteArray *a, gconstpointer data, gsize len)
{
   g_byte_array_append (a, data, len);
}

static inline void
gimv_test_put_zeros (GByteArray *a, gsize len)
{
   while (len--) gimv_test_put8 (a, 0);
}

#endif /* __GIMV_TEST_H__ */
