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
 * The freedesktop.org thumbnails (plugins/thumbnail/freedesktop.c): the
 * file name (the MD5 of the URI of the image), the directory of the size,
 * the size, the PNG written (its tEXt keys), when one is old, and the key
 * X-GImageView::Oriented (thumbnails turned by the EXIF orientation).
 */

#include <unistd.h>
#include <sys/stat.h>

#include "config.h"

#include "gimv_test.h"
#include "gimv_test_core.h"
#ifdef ENABLE_EXIF
#  include "gimv_test_exif.h"
#endif

#include "gimv_image.h"
#include "gimv_thumb.h"
#include "gimv_thumb_cache.h"
#include "prefs.h"

#define TYPE "freedesktop.org"

static const gchar *home;
static gchar *img_dir;
static gchar *cache_dir;     /* $XDG_CACHE_HOME/thumbnails */


/* <cache>/thumbnails/<size>/<md5 of uri>.png, computed here */
static gchar *
expected_path (const gchar *uri, const gchar *size)
{
   gchar *md5 = g_compute_checksum_for_string (G_CHECKSUM_MD5, uri, -1);
   gchar *name = g_strconcat (md5, ".png", NULL);
   gchar *path = g_build_filename (cache_dir, size, name, NULL);
   g_free (md5);
   g_free (name);
   return path;
}


static gchar *
image_path (const gchar *name)
{
   return g_build_filename (img_dir, name, NULL);
}


static void
write_png (const gchar *path, gint w, gint h)
{
   GdkPixbuf *p = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, w, h);
   GError *error = NULL;
   gchar *dir = g_path_get_dirname (path);

   g_mkdir_with_parents (dir, 0700);
   g_free (dir);
   gdk_pixbuf_fill (p, 0x336699ff);
   gdk_pixbuf_save (p, path, "png", &error, NULL);
   g_assert_no_error (error);
   g_object_unref (p);
}


static GimvImage *
new_image (gint w, gint h)
{
   GimvImage *image = gimv_image_new ();
   image->image = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, w, h);
   gdk_pixbuf_fill (image->image, 0x808080ff);
   return image;
}


static void
set_thumb_size (guint size)
{
   conf.thumbwin_thumb_size = size;
}


/* the name: the MD5 of the URI (the example of the specification) */
static void
test_name (void)
{
   gchar *path;

   set_thumb_size (96);
   path = gimv_thumb_cache_get_path ("/home/jens/photos/me.png", TYPE);
   g_assert_nonnull (path);
   g_assert_true (g_str_has_suffix (path, "/thumbnails/normal/c6ee772d9e49320e97ec29a7eb5b1697.png"));
   g_assert_true (g_str_has_prefix (path, cache_dir));
   g_free (path);
}


/* the URI: escaped, absolute, symbolic links as they are */
static void
test_uri (void)
{
   gchar *file = image_path ("a b \xc3\xa4%.png"), *uri, *path, *want, *link, *cwd;

   set_thumb_size (96);
   write_png (file, 10, 10);
   uri = g_strconcat ("file://", img_dir, "/a%20b%20%C3%A4%25.png", NULL);
   want = expected_path (uri, "normal");
   path = gimv_thumb_cache_get_path (file, TYPE);
   g_assert_cmpstr (path, ==, want);
   g_free (path);

   /* relative to the current directory, with . and .. */
   cwd = g_get_current_dir ();
   g_assert_cmpint (chdir (home), ==, 0);
   path = gimv_thumb_cache_get_path ("images/./sub/../a b \xc3\xa4%.png", TYPE);
   g_assert_cmpstr (path, ==, want);
   g_free (path);
   g_assert_cmpint (chdir (cwd), ==, 0);
   g_free (cwd);
   g_free (want);
   g_free (uri);

   /* a symbolic link: its own URI (as file managers do) */
   link = image_path ("link.png");
   g_assert_cmpint (symlink (file, link), ==, 0);
   uri = g_strconcat ("file://", img_dir, "/link.png", NULL);
   want = expected_path (uri, "normal");
   path = gimv_thumb_cache_get_path (link, TYPE);
   g_assert_cmpstr (path, ==, want);
   g_free (path);
   g_free (want);
   g_free (uri);
   g_free (link);
   g_free (file);
}


static void
test_relative_dots (void)
{
   gchar *path, *want, *cwd, *uri;

   set_thumb_size (96);
   uri = g_strconcat ("file://", img_dir, "/c.png", NULL);
   want = expected_path (uri, "normal");
   cwd = g_get_current_dir ();
   g_assert_cmpint (chdir (home), ==, 0);
   path = gimv_thumb_cache_get_path ("images/././c.png", TYPE);
   g_assert_cmpint (chdir (cwd), ==, 0);
   g_assert_cmpstr (path, ==, want);
   g_free (path);
   g_free (want);
   g_free (uri);
   g_free (cwd);
}


/* the directory: the smallest size not smaller than the thumbnails of gimv */
static void
test_size_dir (void)
{
   static const struct { guint size; const gchar *dir; } t[] = {
      { 0, "normal" }, { 64, "normal" }, { 96, "normal" }, { 128, "normal" },
      { 129, "large" }, { 256, "large" }, { 300, "x-large" }, { 512, "x-large" },
      { 513, "xx-large" }, { 1024, "xx-large" }, { 5000, "xx-large" },
   };
   gchar *file = image_path ("size.png"), *uri = g_strconcat ("file://", img_dir, "/size.png", NULL);
   guint i;

   for (i = 0; i < G_N_ELEMENTS (t); i++) {
      gchar *want = expected_path (uri, t[i].dir), *path;
      set_thumb_size (t[i].size);
      path = gimv_thumb_cache_get_path (file, TYPE);
      g_assert_cmpstr (path, ==, want);
      g_free (path);
      g_free (want);
   }

   /* one of a larger size there: that one is read */
   {
      gchar *large = expected_path (uri, "large"), *path;
      write_png (large, 256, 10);
      set_thumb_size (96);
      path = gimv_thumb_cache_get_path (file, TYPE);
      g_assert_cmpstr (path, ==, large);
      g_free (path);
      /* not a smaller one */
      set_thumb_size (300);
      path = gimv_thumb_cache_get_path (file, TYPE);
      g_assert_true (g_str_has_suffix (path, ".png"));
      g_assert_nonnull (strstr (path, "/x-large/"));
      g_free (path);
      g_unlink (large);
      g_free (large);
   }
   g_free (uri);
   g_free (file);
}


static void
check_size (gint w, gint h, gint ew, gint eh)
{
   gint rw = 0, rh = 0;
   g_assert_true (gimv_thumb_cache_get_size (w, h, TYPE, &rw, &rh));
   g_assert_cmpint (rw, ==, ew);
   g_assert_cmpint (rh, ==, eh);
}


static void
test_get_size (void)
{
   gint rw, rh;

   set_thumb_size (96);                  /* normal: 128 */
   check_size (1000, 500, 128, 64);
   check_size (500, 1000, 64, 128);
   check_size (300, 200, 128, 85);
   check_size (128, 128, 128, 128);
   check_size (100, 50, 100, 50);        /* not enlarged */
   check_size (1, 1, 1, 1);
   check_size (10000, 1, 128, 1);        /* at least 1 */
   check_size (1, 10000, 1, 128);
   g_assert_false (gimv_thumb_cache_get_size (0, 10, TYPE, &rw, &rh));
   g_assert_false (gimv_thumb_cache_get_size (10, -1, TYPE, &rw, &rh));

   set_thumb_size (400);                 /* x-large: 512 */
   check_size (1024, 768, 512, 384);
}


static GdkPixbuf *
read_png (const gchar *path)
{
   GError *error = NULL;
   GdkPixbuf *p = gdk_pixbuf_new_from_file (path, &error);
   g_assert_no_error (error);
   return p;
}


/* saved: the PNG and its keys; loaded again; old when the image changed */
static void
test_save_load (void)
{
   gchar *file = image_path ("photo.png"), *uri, *want, *dir;
   GimvImage *image, *saved, *loaded;
   GdkPixbuf *png;
   GStatBuf st;
   gchar *mtime, *size;

   set_thumb_size (96);
   write_png (file, 300, 200);
   gimv_test_set_mtime (file, 1500000000);
   g_assert_cmpint (g_stat (file, &st), ==, 0);

   uri = g_strconcat ("file://", img_dir, "/photo.png", NULL);
   want = expected_path (uri, "normal");
   g_assert_false (g_file_test (want, G_FILE_TEST_EXISTS));
   g_assert_null (gimv_thumb_cache_load (file, TYPE, NULL));

   image = new_image (300, 200);
   saved = gimv_thumb_cache_save (file, TYPE, image, NULL);
   g_assert_nonnull (saved);
   g_assert_cmpint (gimv_image_width (saved), ==, 128);
   g_assert_cmpint (gimv_image_height (saved), ==, 85);
   gimv_image_unref (saved);

   g_assert_true (g_file_test (want, G_FILE_TEST_IS_REGULAR));
   {
      GStatBuf ts;
      g_assert_cmpint (g_stat (want, &ts), ==, 0);
      g_assert_cmpint (ts.st_mode & 0777, ==, 0600);
      dir = g_path_get_dirname (want);
      g_assert_cmpint (g_stat (dir, &ts), ==, 0);
      g_assert_cmpint (ts.st_mode & 0777, ==, 0700);
      g_free (dir);
   }
   png = read_png (want);
   g_assert_cmpint (gdk_pixbuf_get_width (png), ==, 128);
   g_assert_cmpint (gdk_pixbuf_get_height (png), ==, 85);
   mtime = g_strdup_printf ("%" G_GINT64_FORMAT, (gint64) st.st_mtime);
   size = g_strdup_printf ("%" G_GINT64_FORMAT, (gint64) st.st_size);
   g_assert_cmpstr (gdk_pixbuf_get_option (png, "tEXt::Thumb::URI"), ==, uri);
   g_assert_cmpstr (gdk_pixbuf_get_option (png, "tEXt::Thumb::MTime"), ==, mtime);
   g_assert_cmpstr (gdk_pixbuf_get_option (png, "tEXt::Thumb::Size"), ==, size);
   g_assert_cmpstr (gdk_pixbuf_get_option (png, "tEXt::Software"), ==, "GImageView");
   g_assert_cmpstr (gdk_pixbuf_get_option (png, "tEXt::X-GImageView::Oriented"), ==, "1");
   g_object_unref (png);
   g_free (mtime);
   g_free (size);

   /* no temporary file left */
   {
      gchar *d = g_path_get_dirname (want);
      GDir *gd = g_dir_open (d, 0, NULL);
      const gchar *n;
      guint count = 0;
      while ((n = g_dir_read_name (gd))) {
         g_assert_false (strstr (n, ".gimv-"));
         count++;
      }
      g_assert_cmpuint (count, ==, 1);
      g_dir_close (gd);
      g_free (d);
   }

   loaded = gimv_thumb_cache_load (file, TYPE, NULL);
   g_assert_nonnull (loaded);
   g_assert_cmpint (gimv_image_width (loaded), ==, 128);
   g_assert_cmpint (gimv_image_height (loaded), ==, 85);
   gimv_image_unref (loaded);

   /* the image changed after: old */
   gimv_test_set_mtime (file, 1500000100);
   g_assert_null (gimv_thumb_cache_load (file, TYPE, NULL));

   /* a small image is not enlarged */
   gimv_image_unref (image);
   write_png (file, 40, 30);
   image = new_image (40, 30);
   saved = gimv_thumb_cache_save (file, TYPE, image, NULL);
   g_assert_cmpint (gimv_image_width (saved), ==, 40);
   g_assert_cmpint (gimv_image_height (saved), ==, 30);
   gimv_image_unref (saved);
   gimv_image_unref (image);

   g_free (want);
   g_free (uri);
   g_free (file);
}


#ifdef ENABLE_EXIF
/* a thumbnail written by someone: software, with or without the key */
static void
put_thumb (const gchar *file, const gchar *software, gboolean oriented, gint w, gint h)
{
   gchar *path = gimv_thumb_cache_get_path (file, TYPE), *dir, *uri, *abs, *mtime;
   GdkPixbuf *p = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, w, h);
   GError *error = NULL;
   GStatBuf st;

   g_assert_cmpint (g_stat (file, &st), ==, 0);
   dir = g_path_get_dirname (path);
   g_mkdir_with_parents (dir, 0700);
   abs = g_canonicalize_filename (file, NULL);
   uri = g_filename_to_uri (abs, NULL, NULL);
   mtime = g_strdup_printf ("%" G_GINT64_FORMAT, (gint64) st.st_mtime);
   gdk_pixbuf_fill (p, 0x000000ff);
   if (oriented)
      gdk_pixbuf_save (p, path, "png", &error, "tEXt::Thumb::URI", uri,
                       "tEXt::Thumb::MTime", mtime, "tEXt::Software", software,
                       "tEXt::X-GImageView::Oriented", "1", NULL);
   else
      gdk_pixbuf_save (p, path, "png", &error, "tEXt::Thumb::URI", uri,
                       "tEXt::Thumb::MTime", mtime, "tEXt::Software", software, NULL);
   g_assert_no_error (error);
   g_object_unref (p);
   g_free (mtime);
   g_free (uri);
   g_free (abs);
   g_free (dir);
   g_free (path);
}


static gboolean
loads (const gchar *file)
{
   GimvImage *image = gimv_thumb_cache_load (file, TYPE, NULL);
   if (image) gimv_image_unref (image);
   return image != NULL;
}


/* X-GImageView::Oriented: thumbnails an older GImageView made of turned
 * images (EXIF orientation 2 to 8) were not turned: made again */
static void
test_oriented (void)
{
   gchar *turned = image_path ("turned.jpg"), *plain = image_path ("plain.jpg");

   set_thumb_size (96);
   gimv_test_write_jpeg (turned, 60, 40, 6, NULL, NULL, NULL);
   gimv_test_write_jpeg (plain, 60, 40, 1, NULL, NULL, NULL);
   g_assert_cmpint (gimv_thumb_file_orientation (turned), ==, 6);
   g_assert_cmpint (gimv_thumb_file_orientation (plain), ==, 1);

   /* an older GImageView (no key) */
   put_thumb (turned, "GImageView", FALSE, 60, 40);
   g_assert_false (loads (turned));
   put_thumb (plain, "GImageView", FALSE, 60, 40);
   g_assert_true (loads (plain));

   /* this one */
   put_thumb (turned, "GImageView", TRUE, 40, 60);
   g_assert_true (loads (turned));

   /* the others keep them turned */
   put_thumb (turned, "GNOME::ThumbnailFactory", FALSE, 40, 60);
   g_assert_true (loads (turned));

   g_free (turned);
   g_free (plain);
}


/* a thumbnail of the size kept from an image loaded smaller (the
 * thumbnails of gimv): the image loaded again, turned as it is meant
 * to be seen */
static void
test_save_reload_turned (void)
{
   gchar *file = image_path ("big.jpg"), *path;
   GimvImage *small, *saved;
   GdkPixbuf *png;

   set_thumb_size (96);
   gimv_test_write_jpeg (file, 300, 200, 6, NULL, NULL, NULL);
   small = new_image (64, 96);           /* already turned, as gimv does */
   saved = gimv_thumb_cache_save (file, TYPE, small, NULL);
   g_assert_nonnull (saved);
   g_assert_cmpint (gimv_image_width (saved), ==, 85);
   g_assert_cmpint (gimv_image_height (saved), ==, 128);
   gimv_image_unref (saved);
   gimv_image_unref (small);

   path = gimv_thumb_cache_get_path (file, TYPE);
   png = read_png (path);
   g_assert_cmpint (gdk_pixbuf_get_width (png), ==, 85);
   g_assert_cmpint (gdk_pixbuf_get_height (png), ==, 128);
   g_assert_cmpstr (gdk_pixbuf_get_option (png, "tEXt::X-GImageView::Oriented"), ==, "1");
   /* turned 90 degrees clockwise: the left of the image (green to the
    * bottom: x 0) is at the top, its top (y 0, no green) at the right */
   {
      guchar *px = gdk_pixbuf_get_pixels (png);
      gint rs = gdk_pixbuf_get_rowstride (png);
      guchar *top_right = px + 84 * 3, *top_left = px;
      g_assert_cmpint (top_right[1], <, 40);    /* y 0 of the image: no green */
      g_assert_cmpint (top_left[1], >, 215);    /* y 199: green */
      g_assert_cmpint (top_left[0], <, 40);     /* x 0: no red */
      g_assert_cmpint ((px + 127 * rs)[0], >, 215);   /* x 299: red */
   }
   g_object_unref (png);
   g_free (path);
   g_free (file);
}
#endif


int
main (int argc, char *argv[])
{
   gint ret;

   g_test_init (&argc, &argv, NULL);

   home = gimv_test_core_init ();
   img_dir = g_build_filename (home, "images", NULL);
   g_mkdir_with_parents (img_dir, 0700);
   cache_dir = g_build_filename (g_get_user_cache_dir (), "thumbnails", NULL);
   g_assert_true (g_str_has_prefix (cache_dir, home));
   gimv_test_core_load_plugin ("thumbnail", "gimv_freedesktop_thumb");
   /* for the images loaded again for the size kept */
   gimv_test_core_load_plugin ("image_loader", "gimv_pixbuf_loader");

   g_test_add_func ("/thumbnail/freedesktop/name", test_name);
   g_test_add_func ("/thumbnail/freedesktop/uri", test_uri);
   g_test_add_func ("/thumbnail/freedesktop/relative-dots", test_relative_dots);
   g_test_add_func ("/thumbnail/freedesktop/size-dir", test_size_dir);
   g_test_add_func ("/thumbnail/freedesktop/get-size", test_get_size);
   g_test_add_func ("/thumbnail/freedesktop/save-load", test_save_load);
#ifdef ENABLE_EXIF
   g_test_add_func ("/thumbnail/freedesktop/oriented", test_oriented);
   g_test_add_func ("/thumbnail/freedesktop/save-reload-turned", test_save_reload_turned);
#endif

   ret = g_test_run ();

   g_free (cache_dir);
   g_free (img_dir);
   gimv_test_core_finish ();
   return ret;
}
