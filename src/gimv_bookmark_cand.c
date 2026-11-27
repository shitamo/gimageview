/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: where a bookmarked file or folder that is not found may
 * have gone (the window of them, gimv_bookmark_missing.c; the tests).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <string.h>

#include "gimv_bookmark_cand.h"

static gint
compare_str (gconstpointer a, gconstpointer b)
{
   return strcmp (*(const gchar **) a, *(const gchar **) b);
}


GPtrArray *
gimv_bookmark_missing_candidates (sqlite3 *db, const gchar *path)
{
   GPtrArray *found = g_ptr_array_new_with_free_func (g_free);
   g_autofree gchar *name = NULL;
   sqlite3_stmt *st;

   if (!db || !path) return found;
   name = g_path_get_basename (path);
   if (!*name || !strcmp (name, "/") || !strcmp (name, ".")) return found;

   /* the files */
   if (sqlite3_prepare_v2 (db, "SELECT d.path, f.name FROM files f "
                               "JOIN dirs d ON d.id = f.dir_id WHERE f.name = ?",
                           -1, &st, NULL) == SQLITE_OK)
   {
      sqlite3_bind_text (st, 1, name, -1, SQLITE_STATIC);
      while (sqlite3_step (st) == SQLITE_ROW) {
         const gchar *dir = (const gchar *) sqlite3_column_text (st, 0);
         const gchar *n = (const gchar *) sqlite3_column_text (st, 1);
         gchar *p;

         if (!dir || !n) continue;
         p = g_build_filename (dir, n, NULL);
         if (strcmp (p, path) && g_file_test (p, G_FILE_TEST_IS_REGULAR))
            g_ptr_array_add (found, p);
         else
            g_free (p);
      }
      sqlite3_finalize (st);
   }

   /* the folders ("/name" at the end; '%' and '_' of the name as they are) */
   if (sqlite3_prepare_v2 (db, "SELECT path FROM dirs WHERE path LIKE ? ESCAPE '\\'",
                           -1, &st, NULL) == SQLITE_OK)
   {
      GString *like = g_string_new ("%/");
      const gchar *c;

      for (c = name; *c; c++) {
         if (*c == '%' || *c == '_' || *c == '\\') g_string_append_c (like, '\\');
         g_string_append_c (like, *c);
      }
      sqlite3_bind_text (st, 1, like->str, -1, SQLITE_TRANSIENT);
      while (sqlite3_step (st) == SQLITE_ROW) {
         const gchar *p = (const gchar *) sqlite3_column_text (st, 0);
         g_autofree gchar *base = p ? g_path_get_basename (p) : NULL;

         /* LIKE is not case sensitive for ASCII */
         if (p && base && !strcmp (base, name) && strcmp (p, path)
             && g_file_test (p, G_FILE_TEST_IS_DIR))
            g_ptr_array_add (found, g_strdup (p));
      }
      sqlite3_finalize (st);
      g_string_free (like, TRUE);
   }

   g_ptr_array_sort (found, compare_str);
   return found;
}
