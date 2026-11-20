/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * The tests of src/gimv_cover.c: the cover of a folder, of an archive.
 */

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <archive.h>
#include <archive_entry.h>

#include "gimv_cover.h"

static gchar *dir;


static gboolean
is_image (const gchar *name)
{
   return g_str_has_suffix (name, ".jpg") || g_str_has_suffix (name, ".png");
}


static void
touch (const gchar *name, const gchar *data)
{
   gchar *p = g_build_filename (dir, name, NULL);
   g_assert_true (g_file_set_contents (p, data, -1, NULL));
   g_free (p);
}


static void
test_dir (void)
{
   gchar *book = g_build_filename (dir, "book", NULL), *sub, *c;

   g_mkdir (book, 0755);
   touch ("book/10.png", "x");
   touch ("book/2.png", "x");
   touch ("book/0.txt", "x");
   touch ("book/.1.png", "x");          /* hidden */
   sub = g_build_filename (book, "1.png", NULL);   /* a folder */
   g_mkdir (sub, 0755);
   c = gimv_cover_of_dir (book, is_image);
   g_assert_cmpstr (strrchr (c, '/'), ==, "/2.png");
   g_free (c);
   g_rmdir (sub);
   g_free (sub);

   sub = g_build_filename (dir, "empty", NULL);
   g_mkdir (sub, 0755);
   g_assert_null (gimv_cover_of_dir (sub, is_image));
   g_assert_null (gimv_cover_of_dir ("/nonexistent", is_image));
   g_rmdir (sub);
   g_free (sub);
   g_free (book);
}


/* an archive of the members in this order */
static gchar *
make_archive (const gchar *name, int format, int filter, const gchar **members)
{
   gchar *path = g_build_filename (dir, name, NULL);
   struct archive *a = archive_write_new ();
   guint i;

   archive_write_set_format (a, format);
   if (filter) archive_write_add_filter (a, filter);
   g_assert_cmpint (archive_write_open_filename (a, path), ==, ARCHIVE_OK);
   for (i = 0; members[i]; i++) {
      struct archive_entry *e = archive_entry_new ();
      archive_entry_set_pathname (e, members[i]);
      archive_entry_set_size (e, strlen (members[i]));
      archive_entry_set_filetype (e, AE_IFREG);
      archive_entry_set_perm (e, 0644);
      archive_write_header (a, e);
      /* the data is the name: what was read tells which */
      archive_write_data (a, members[i], strlen (members[i]));
      archive_entry_free (e);
   }
   archive_write_close (a);
   archive_write_free (a);
   return path;
}


static void
test_archive (void)
{
   const gchar *members[] = { "b/readme.txt", "b/010.jpg", "b/02.jpg", "b/1.png.txt", NULL };
   gchar *p, *name;
   guchar *data;
   gsize len;

   /* zip: listed, the first by name (numbers by value) */
   p = make_archive ("book.cbz", ARCHIVE_FORMAT_ZIP, 0, members);
   g_assert_true (gimv_cover_archive_ext (p));
   g_assert_true (gimv_cover_of_archive (p, is_image, &data, &len, &name));
   g_assert_cmpstr (name, ==, "b/02.jpg");
   g_assert_cmpmem (data, len, "b/02.jpg", 8);
   g_free (data); g_free (name);
   g_unlink (p); g_free (p);

   /* tar not compressed: listed too */
   p = make_archive ("book.tar", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, 0, members);
   g_assert_true (gimv_cover_of_archive (p, is_image, &data, &len, &name));
   g_assert_cmpstr (name, ==, "b/02.jpg");
   g_free (data); g_free (name);
   g_unlink (p); g_free (p);

   /* compressed tar: gone over (small: within the limits), the first by
      name too */
   p = make_archive ("book.tar.gz", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_GZIP, members);
   g_assert_true (gimv_cover_archive_ext (p));
   g_assert_true (gimv_cover_of_archive (p, is_image, &data, &len, &name));
   g_assert_cmpstr (name, ==, "b/02.jpg");
   g_assert_cmpmem (data, len, "b/02.jpg", 8);
   g_free (data); g_free (name);
   g_unlink (p); g_free (p);

   /* no image; not an archive */
   {
      const gchar *none[] = { "a.txt", NULL };
      p = make_archive ("none.zip", ARCHIVE_FORMAT_ZIP, 0, none);
      g_assert_false (gimv_cover_of_archive (p, is_image, &data, &len, &name));
      g_assert_null (data);
      g_unlink (p); g_free (p);
   }
   touch ("junk.zip", "not an archive");
   p = g_build_filename (dir, "junk.zip", NULL);
   g_assert_false (gimv_cover_of_archive (p, is_image, &data, &len, &name));
   g_unlink (p); g_free (p);
   g_assert_false (gimv_cover_archive_ext ("/a/b.jpg"));
   g_assert_true (gimv_cover_archive_ext ("/a/B.CBR"));
}


int
main (int argc, char **argv)
{
   gint ret;

   g_test_init (&argc, &argv, NULL);
   dir = g_dir_make_tmp ("gimv-cover-XXXXXX", NULL);

   g_test_add_func ("/cover/dir", test_dir);
   g_test_add_func ("/cover/archive", test_archive);

   ret = g_test_run ();
   {
      gchar *p = g_build_filename (dir, "book", "10.png", NULL);
      g_unlink (p); g_free (p);
      p = g_build_filename (dir, "book", "2.png", NULL); g_unlink (p); g_free (p);
      p = g_build_filename (dir, "book", "0.txt", NULL); g_unlink (p); g_free (p);
      p = g_build_filename (dir, "book", ".1.png", NULL); g_unlink (p); g_free (p);
      p = g_build_filename (dir, "book", NULL); g_rmdir (p); g_free (p);
   }
   g_rmdir (dir);
   g_free (dir);
   return ret;
}
