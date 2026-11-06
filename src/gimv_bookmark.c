/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: bookmarks of images (~/.gimv/bookmarks).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <string.h>
#include <stdlib.h>
#include <glib/gstdio.h>

#include "intl.h"

#include "gimv_bookmark.h"

#define HEADER "# GImageView bookmarks\n"

typedef struct {
   GimvBookmarkChangedFunc func;
   gpointer                data;
   guint                   id;
} Watch;

static gchar      *file      = NULL;    /* NULL: the default */
static GHashTable *bookmarks = NULL;    /* path -> GimvBookmark */
static gint64      file_mtime = -1;     /* of the file as read / written */
static GList      *watches   = NULL;
static guint       next_watch = 1;


/******************************************************************************
 *  the file
 ******************************************************************************/

static const gchar *
file_name (void)
{
   if (!file)
      file = g_build_filename (g_get_home_dir (), ".gimv", "bookmarks", NULL);
   return file;
}


void
gimv_bookmark_set_file (const gchar *filename)
{
   g_free (file);
   file = filename ? g_strdup (filename) : NULL;
   g_clear_pointer (&bookmarks, g_hash_table_unref);
   file_mtime = -1;
}


gchar *
gimv_bookmark_escape (const gchar *text)
{
   GString *s;

   if (!text) return g_strdup ("");
   s = g_string_sized_new (strlen (text) + 8);
   for (; *text; text++) {
      switch (*text) {
      case '\\': g_string_append (s, "\\\\"); break;
      case '\t': g_string_append (s, "\\t");  break;
      case '\n': g_string_append (s, "\\n");  break;
      case '\r': g_string_append (s, "\\r");  break;
      default:   g_string_append_c (s, *text);
      }
   }
   return g_string_free (s, FALSE);
}


gchar *
gimv_bookmark_unescape (const gchar *text)
{
   GString *s;

   if (!text) return g_strdup ("");
   s = g_string_sized_new (strlen (text));
   for (; *text; text++) {
      if (*text != '\\' || !text[1]) {
         g_string_append_c (s, *text);
         continue;
      }
      text++;
      switch (*text) {
      case 't': g_string_append_c (s, '\t'); break;
      case 'n': g_string_append_c (s, '\n'); break;
      case 'r': g_string_append_c (s, '\r'); break;
      default:  g_string_append_c (s, *text);   /* "\\" and anything else */
      }
   }
   return g_string_free (s, FALSE);
}


GimvBookmark *
gimv_bookmark_copy (const GimvBookmark *bm)
{
   GimvBookmark *c;

   g_return_val_if_fail (bm, NULL);
   c = g_new0 (GimvBookmark, 1);
   c->path  = g_strdup (bm->path);
   c->added = bm->added;
   c->note  = g_strdup (bm->note);
   return c;
}


void
gimv_bookmark_free (GimvBookmark *bm)
{
   if (!bm) return;
   g_free (bm->path);
   g_free (bm->note);
   g_free (bm);
}


static gint64
current_mtime (void)
{
   GStatBuf st;

   if (g_stat (file_name (), &st)) return -1;
   return (gint64) st.st_mtime * 1000000000 + st.st_mtim.tv_nsec;
}


static void
load (void)
{
   gchar *contents = NULL, **lines;
   guint i;

   if (bookmarks) g_hash_table_unref (bookmarks);
   bookmarks = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
                                      (GDestroyNotify) gimv_bookmark_free);
   file_mtime = current_mtime ();
   if (!g_file_get_contents (file_name (), &contents, NULL, NULL)) return;

   lines = g_strsplit (contents, "\n", -1);
   g_free (contents);
   for (i = 0; lines[i]; i++) {
      gchar **f;
      GimvBookmark *bm;

      if (!*lines[i] || lines[i][0] == '#') continue;
      f = g_strsplit (lines[i], "\t", 3);
      if (g_strv_length (f) >= 2 && *f[1]) {
         bm = g_new0 (GimvBookmark, 1);
         bm->added = g_ascii_strtoll (f[0], NULL, 10);
         bm->path  = gimv_bookmark_unescape (f[1]);
         if (f[2] && *f[2]) bm->note = gimv_bookmark_unescape (f[2]);
         g_hash_table_replace (bookmarks, bm->path, bm);
      }
      g_strfreev (f);
   }
   g_strfreev (lines);
}


/* the bookmarks as the file has them now (another gimv may have written it) */
static void
ensure (void)
{
   if (!bookmarks || current_mtime () != file_mtime)
      load ();
}


static gint
cmp_newest (gconstpointer a, gconstpointer b)
{
   const GimvBookmark *x = *(GimvBookmark * const *) a;
   const GimvBookmark *y = *(GimvBookmark * const *) b;

   if (x->added != y->added) return x->added > y->added ? -1 : 1;
   return strcmp (x->path, y->path);
}


static GPtrArray *
sorted (void)
{
   GPtrArray *a = g_ptr_array_new ();
   GHashTableIter it;
   gpointer value;

   g_hash_table_iter_init (&it, bookmarks);
   while (g_hash_table_iter_next (&it, NULL, &value))
      g_ptr_array_add (a, value);
   g_ptr_array_sort (a, cmp_newest);
   return a;
}


static void
changed (void)
{
   GString *out = g_string_new (HEADER);
   GPtrArray *a = sorted ();
   GError *error = NULL;
   gchar *dir;
   GList *node;
   guint i;

   for (i = 0; i < a->len; i++) {
      GimvBookmark *bm = g_ptr_array_index (a, i);
      gchar *path = gimv_bookmark_escape (bm->path);
      gchar *note = gimv_bookmark_escape (bm->note);
      g_string_append_printf (out, "%" G_GINT64_FORMAT "\t%s\t%s\n", bm->added, path, note);
      g_free (path);
      g_free (note);
   }
   g_ptr_array_unref (a);

   dir = g_path_get_dirname (file_name ());
   g_mkdir_with_parents (dir, 0700);
   g_free (dir);
   if (!g_file_set_contents (file_name (), out->str, out->len, &error)) {
      g_warning ("bookmarks: %s", error->message);
      g_error_free (error);
   }
   g_string_free (out, TRUE);
   file_mtime = current_mtime ();

   for (node = watches; node; ) {
      Watch *w = node->data;
      node = node->next;   /* a watch may remove itself */
      w->func (w->data);
   }
}


/******************************************************************************
 *  the bookmarks
 ******************************************************************************/

gboolean
gimv_bookmark_has (const gchar *path)
{
   g_return_val_if_fail (path, FALSE);
   ensure ();
   return g_hash_table_contains (bookmarks, path);
}


GimvBookmark *
gimv_bookmark_get (const gchar *path)
{
   GimvBookmark *bm;

   g_return_val_if_fail (path, NULL);
   ensure ();
   bm = g_hash_table_lookup (bookmarks, path);
   return bm ? gimv_bookmark_copy (bm) : NULL;
}


gboolean
gimv_bookmark_add (const gchar *path)
{
   GimvBookmark *bm;

   g_return_val_if_fail (path && *path, FALSE);
   ensure ();
   if (g_hash_table_contains (bookmarks, path)) return FALSE;
   bm = g_new0 (GimvBookmark, 1);
   bm->path  = g_strdup (path);
   bm->added = g_get_real_time () / G_USEC_PER_SEC;
   g_hash_table_replace (bookmarks, bm->path, bm);
   changed ();
   return TRUE;
}


void
gimv_bookmark_set_note (const gchar *path, const gchar *note)
{
   GimvBookmark *bm;
   gchar *text;

   g_return_if_fail (path);
   ensure ();
   bm = g_hash_table_lookup (bookmarks, path);
   if (!bm) return;
   text = note ? g_strstrip (g_strdup (note)) : NULL;
   if (text && !*text) g_clear_pointer (&text, g_free);
   if (!g_strcmp0 (text, bm->note)) {
      g_free (text);
      return;
   }
   g_free (bm->note);
   bm->note = text;
   changed ();
}


void
gimv_bookmark_remove (const gchar *path)
{
   g_return_if_fail (path);
   ensure ();
   if (g_hash_table_remove (bookmarks, path))
      changed ();
}


void
gimv_bookmark_move (const gchar *old_path, const gchar *new_path)
{
   GPtrArray *moving;
   gsize len;
   guint i;

   g_return_if_fail (old_path && new_path);
   if (!strcmp (old_path, new_path)) return;
   ensure ();

   /* the path itself, and what is below it (a folder) */
   len = strlen (old_path);
   moving = g_ptr_array_new ();
   {
      GHashTableIter it;
      gpointer value;

      g_hash_table_iter_init (&it, bookmarks);
      while (g_hash_table_iter_next (&it, NULL, &value)) {
         GimvBookmark *bm = value;
         if (!strncmp (bm->path, old_path, len)
             && (bm->path[len] == '\0' || bm->path[len] == '/'))
         {
            g_hash_table_iter_steal (&it);
            g_ptr_array_add (moving, bm);
         }
      }
   }
   for (i = 0; i < moving->len; i++) {
      GimvBookmark *bm = g_ptr_array_index (moving, i);
      gchar *path = g_strconcat (new_path, bm->path + len, NULL);
      g_free (bm->path);
      bm->path = path;
      /* one already there keeps its own */
      if (g_hash_table_contains (bookmarks, bm->path))
         gimv_bookmark_free (bm);
      else
         g_hash_table_replace (bookmarks, bm->path, bm);
   }
   if (moving->len) changed ();
   g_ptr_array_unref (moving);
}


GPtrArray *
gimv_bookmark_list (void)
{
   GPtrArray *a, *copy;
   guint i;

   ensure ();
   a = sorted ();
   copy = g_ptr_array_new_full (a->len, (GDestroyNotify) gimv_bookmark_free);
   for (i = 0; i < a->len; i++)
      g_ptr_array_add (copy, gimv_bookmark_copy (g_ptr_array_index (a, i)));
   g_ptr_array_unref (a);
   return copy;
}


guint
gimv_bookmark_count (void)
{
   ensure ();
   return g_hash_table_size (bookmarks);
}


guint
gimv_bookmark_add_watch (GimvBookmarkChangedFunc func, gpointer data)
{
   Watch *w;

   g_return_val_if_fail (func, 0);
   w = g_new0 (Watch, 1);
   w->func = func;
   w->data = data;
   w->id   = next_watch++;
   watches = g_list_append (watches, w);
   return w->id;
}


void
gimv_bookmark_remove_watch (guint id)
{
   GList *node;

   for (node = watches; node; node = node->next) {
      Watch *w = node->data;
      if (w->id == id) {
         watches = g_list_delete_link (watches, node);
         g_free (w);
         return;
      }
   }
}


gchar *
gimv_bookmark_tooltip (const gchar *path)
{
   GimvBookmark *bm;
   gchar *text;

   if (!path) return NULL;
   ensure ();
   bm = g_hash_table_lookup (bookmarks, path);
   if (!bm) return NULL;
   text = g_strconcat ("\u2605 ", bm->note ? bm->note : _("Bookmarked"), NULL);
   return text;
}
