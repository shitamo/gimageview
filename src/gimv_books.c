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

#include "gimv_books.h"

#include <string.h>

/* a group of the key file for each book, named by the SHA-1 of its path
   (paths may have '[', ']' and other bytes groups cannot) */

static GKeyFile *books = NULL;
static gchar    *books_file = NULL;
static guint     save_id = 0;


void
gimv_books_set_file (const gchar *path)
{
   if (save_id) {
      g_source_remove (save_id);
      save_id = 0;
   }
   g_clear_pointer (&books, g_key_file_unref);
   g_free (books_file);
   books_file = g_strdup (path);
}


static GKeyFile *
get_books (void)
{
   if (books) return books;
   if (!books_file)
      books_file = g_build_filename (g_get_home_dir (), ".gimv", "books", NULL);
   books = g_key_file_new ();
   g_key_file_load_from_file (books, books_file, G_KEY_FILE_NONE, NULL);
   return books;
}


static gchar *
group_of (const gchar *book)
{
   return g_compute_checksum_for_string (G_CHECKSUM_SHA1, book, -1);
}


static gchar *
clean_book (const gchar *book)
{
   gchar *b = g_strdup (book);
   gsize len = strlen (b);

   while (len > 1 && b[len - 1] == '/') b[--len] = '\0';
   return b;
}


gboolean
gimv_books_get (const gchar *book, gchar **page, gchar **layout)
{
   GKeyFile *kf = get_books ();
   gchar *b, *group, *path, *p;
   gboolean found = FALSE;

   if (page) *page = NULL;
   if (layout) *layout = NULL;
   g_return_val_if_fail (book, FALSE);

   b = clean_book (book);
   group = group_of (b);
   path = g_key_file_get_string (kf, group, "path", NULL);
   p = g_key_file_get_string (kf, group, "page", NULL);
   /* the same book (not another of the same SHA-1) */
   if (path && p && !strcmp (path, b)) {
      found = TRUE;
      if (page) *page = g_strdup (p);
      if (layout) *layout = g_key_file_get_string (kf, group, "layout", NULL);
   }
   g_free (p);
   g_free (path);
   g_free (group);
   g_free (b);
   return found;
}


static gboolean
idle_save (gpointer data)
{
   save_id = 0;
   gimv_books_save ();
   return G_SOURCE_REMOVE;
}


static void
schedule_save (void)
{
   if (!save_id) save_id = g_timeout_add_seconds (2, idle_save, NULL);
}


void
gimv_books_set (const gchar *book, const gchar *page, const gchar *layout)
{
   GKeyFile *kf = get_books ();
   gchar *b, *group, *old;

   g_return_if_fail (book && page);

   b = clean_book (book);
   group = group_of (b);
   old = g_key_file_get_string (kf, group, "page", NULL);
   if (!old || strcmp (old, page)) {
      g_key_file_set_string (kf, group, "path", b);
      g_key_file_set_string (kf, group, "page", page);
      schedule_save ();
   }
   if (layout) {
      gchar *l = g_key_file_get_string (kf, group, "layout", NULL);
      if (g_strcmp0 (l, layout)) {
         g_key_file_set_string (kf, group, "layout", layout);
         schedule_save ();
      }
      g_free (l);
   }
   g_key_file_set_int64 (kf, group, "time", g_get_real_time () / G_USEC_PER_SEC);
   g_free (old);
   g_free (group);
   g_free (b);
}


void
gimv_books_forget (const gchar *book)
{
   GKeyFile *kf = get_books ();
   gchar *b, *group;

   g_return_if_fail (book);

   b = clean_book (book);
   group = group_of (b);
   if (g_key_file_remove_group (kf, group, NULL)) schedule_save ();
   g_free (group);
   g_free (b);
}


typedef struct {
   gchar  *group;
   gint64  time;
} Entry;


static gint
cmp_newer (gconstpointer a, gconstpointer b)
{
   const Entry *ea = a, *eb = b;
   return ea->time > eb->time ? -1 : ea->time < eb->time ? 1 : 0;
}


void
gimv_books_save (void)
{
   GKeyFile *kf;
   gchar **groups, *dir;
   gsize n, i;
   GError *error = NULL;

   if (save_id) {
      g_source_remove (save_id);
      save_id = 0;
   }
   if (!books) return;
   kf = books;

   /* the most recent only */
   groups = g_key_file_get_groups (kf, &n);
   if (n > GIMV_BOOKS_MAX) {
      GArray *entries = g_array_sized_new (FALSE, FALSE, sizeof (Entry), n);
      for (i = 0; i < n; i++) {
         Entry e = { groups[i], g_key_file_get_int64 (kf, groups[i], "time", NULL) };
         g_array_append_val (entries, e);
      }
      g_array_sort (entries, cmp_newer);
      for (i = GIMV_BOOKS_MAX; i < entries->len; i++)
         g_key_file_remove_group (kf, g_array_index (entries, Entry, i).group, NULL);
      g_array_unref (entries);
   }
   g_strfreev (groups);

   dir = g_path_get_dirname (books_file);
   g_mkdir_with_parents (dir, 0700);
   g_free (dir);
   if (!g_key_file_save_to_file (kf, books_file, &error)) {
      g_warning ("%s: %s", books_file, error->message);
      g_error_free (error);
   }
}


void
gimv_books_split (const gchar *path, gchar **book, gchar **page)
{
   g_return_if_fail (path && book && page);

   *book = g_path_get_dirname (path);
   *page = g_path_get_basename (path);
}
