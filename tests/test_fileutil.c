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

/* the small functions of src/fileutil.c */

#include <grp.h>
#include <locale.h>
#include <pwd.h>
#include <stdlib.h>
#include <unistd.h>

#include "gimv_test.h"
#include "fileutil.h"


static void
check_size (size_t size, const gchar *expected)
{
   gchar *s = fileutil_size2str (size, FALSE);
   g_assert_cmpstr (s, ==, expected);
   g_free (s);
}


static void
test_size2str (void)
{
   gchar *s;

   /* the C locale has no thousands separator: "," is put */
   check_size (0, "0");
   check_size (7, "7");
   check_size (999, "999");
   check_size (1000, "1,000");
   check_size (12345, "12,345");
   check_size (123456, "123,456");
   check_size (1234567, "1,234,567");
   check_size (G_GUINT64_CONSTANT (10000000000), "10,000,000,000");

   /* right aligned in 13 columns */
   s = fileutil_size2str (1234567, TRUE);
   g_assert_cmpstr (s, ==, "    1,234,567");
   g_free (s);
   s = fileutil_size2str (G_GUINT64_CONSTANT (1234567890123), TRUE);
   g_assert_cmpstr (s, ==, "1,234,567,890,123");   /* longer: not cut */
   g_free (s);
}


static void
test_time2str (void)
{
   gchar *s;

   /* TZ=UTC (tests/meson.build) */
   s = fileutil_time2str (0);
   g_assert_cmpstr (s, ==, "1970/01/01 Thu 00:00");
   g_free (s);
   s = fileutil_time2str (1556878500);            /* 2019-05-03 10:15:00 */
   g_assert_cmpstr (s, ==, "2019/05/03 Fri 10:15");
   g_free (s);
}


static void
test_uid_gid (void)
{
   struct passwd *pw = getpwuid (getuid ());
   struct group *gr = getgrgid (getgid ());
   gchar *s, *again;

   s = fileutil_uid2str (getuid ());
   g_assert_nonnull (pw);
   g_assert_cmpstr (s, ==, pw->pw_name);
   g_free (s);

   s = fileutil_gid2str (getgid ());
   g_assert_nonnull (gr);
   g_assert_cmpstr (s, ==, gr->gr_name);
   g_free (s);

   /* root is uid 0 and gid 0 everywhere */
   s = fileutil_uid2str (0);
   g_assert_cmpstr (s, ==, "root");
   /* remembered: the same again, a new string each time */
   again = fileutil_uid2str (0);
   g_assert_cmpstr (again, ==, "root");
   g_assert_true (again != s);
   g_free (again);
   g_free (s);

   /* an id without a name: the number */
   s = fileutil_uid2str ((uid_t) 4000000123u);
   g_assert_cmpstr (s, ==, "4000000123");
   g_free (s);
   s = fileutil_gid2str ((gid_t) 4000000124u);
   g_assert_cmpstr (s, ==, "4000000124");
   g_free (s);
}


static void
check_mode (mode_t mode, const gchar *expected)
{
   gchar *s = fileutil_mode2str (mode);
   g_assert_cmpstr (s, ==, expected);
   g_free (s);
}


static void
test_mode2str (void)
{
   check_mode (S_IFREG | 0644, "-rw-r--r--");
   check_mode (S_IFREG | 0755, "-rwxr-xr-x");
   check_mode (S_IFDIR | 0700, "drwx------");
   check_mode (S_IFLNK | 0777, "lrwxrwxrwx");
   check_mode (S_IFIFO | 0600, "?rw-------");
   check_mode (S_IFREG, "----------");
   /* set-id and sticky bits: always the capital letter (as it was) */
   check_mode (S_IFREG | S_ISUID | 0755, "-rwSr-xr-x");
   check_mode (S_IFREG | S_ISGID | 0755, "-rwxr-Sr-x");
   check_mode (S_IFDIR | S_ISVTX | 0777, "drwxrwxrwT");
}


static void
test_mode2str_setid_exec (void)
{
   /* ls -l: "s" / "t" when the execute bit is set too, "S" / "T" when not */
   if (gimv_test_known_bug ("fileutil_mode2str () shows S and T also with the x bit set "
                            "(ls -l shows s and t)"))
      return;
   check_mode (S_IFREG | S_ISUID | 0755, "-rwsr-xr-x");
   check_mode (S_IFDIR | S_ISVTX | 0777, "drwxrwxrwt");
}


static void
check_tilde (const gchar *path, const gchar *expected)
{
   gchar *s = fileutil_home2tilde (path);
   g_assert_cmpstr (s, ==, expected);
   g_free (s);
}


static void
test_home2tilde (void)
{
   g_setenv ("HOME", "/home/gimv", TRUE);
   check_tilde ("/home/gimv/a.jpg", "~/a.jpg");
   check_tilde ("/home/gimv/dir/b.png", "~/dir/b.png");
   check_tilde ("/home/gimv/", "~/");
   check_tilde ("/home/gimv", "/home/gimv");          /* HOME itself: as it is */
   check_tilde ("/home/gimvx/a.jpg", "/home/gimvx/a.jpg");
   check_tilde ("/tmp/a.jpg", "/tmp/a.jpg");
   check_tilde ("relative/a.jpg", "relative/a.jpg");
}


static void
test_home2tilde_long (void)
{
   GString *path;
   gchar *expected;

   if (gimv_test_known_bug ("fileutil_home2tilde () writes up to BUF_SIZE (4096) bytes "
                            "into a buffer of MAX_PATH_LEN (1024): a long path below "
                            "HOME overflows the stack"))
      return;

   if (g_test_subprocess ()) {
      gchar *s;

      g_setenv ("HOME", "/home/gimv", TRUE);
      path = g_string_new ("/home/gimv/");
      while (path->len < 3000) g_string_append (path, "long-directory-name/");
      g_string_append (path, "a.jpg");
      expected = g_strconcat ("~/", path->str + strlen ("/home/gimv/"), NULL);
      s = fileutil_home2tilde (path->str);
      g_assert_cmpstr (s, ==, expected);
      g_free (s);
      g_free (expected);
      g_string_free (path, TRUE);
      return;
   }
   g_test_trap_subprocess (NULL, 0, 0);
   g_test_trap_assert_passed ();
}


static void
test_basename (void)
{
   gchar *s;

   g_assert_cmpstr (fileutil_basename ("/a/b/c.jpg"), ==, "c.jpg");
   g_assert_cmpstr (fileutil_basename ("c.jpg"), ==, "c.jpg");
   g_assert_cmpstr (fileutil_basename ("/a/b/"), ==, "");

   s = fileutil_dir_basename ("/a/b/c");
   g_assert_cmpstr (s, ==, "c");
   g_free (s);
   s = fileutil_dir_basename ("/a/b/c/");
   g_assert_cmpstr (s, ==, "c");
   g_free (s);
   s = fileutil_dir_basename ("/a");
   g_assert_cmpstr (s, ==, "a");
   g_free (s);
   s = fileutil_dir_basename ("/");
   g_assert_cmpstr (s, ==, "");
   g_free (s);
   g_assert_null (fileutil_dir_basename ("noslash"));
   g_assert_null (fileutil_dir_basename (NULL));
}


static void
test_extension (void)
{
   g_assert_cmpstr (fileutil_get_extention ("a.jpg"), ==, "jpg");
   g_assert_cmpstr (fileutil_get_extention ("a.tar.gz"), ==, "gz");
   g_assert_null (fileutil_get_extention ("a."));
   g_assert_null (fileutil_get_extention ("noext"));
   g_assert_null (fileutil_get_extention (NULL));

   g_assert_true (fileutil_extension_is ("a.JPG", ".jpg"));
   g_assert_true (fileutil_extension_is ("a.tar.gz", "tar.gz"));
   g_assert_false (fileutil_extension_is ("a.jpg", ".png"));
   g_assert_false (fileutil_extension_is ("jpg", ".jpg"));
   g_assert_false (fileutil_extension_is ("", ".jpg"));
   g_assert_false (fileutil_extension_is ("a.jpg", ""));
   g_assert_false (fileutil_extension_is (NULL, ".jpg"));
}


static void
test_file_tests (void)
{
   gchar *dir = gimv_test_mkdtemp ();
   gchar *file = g_build_filename (dir, "f.txt", NULL);
   gchar *sub = g_build_filename (dir, "sub", NULL);
   gchar *deep = g_build_filename (dir, "x", "y", "z", NULL);
   gchar *deep_file = g_build_filename (deep, "file.txt", NULL);
   gchar *full = g_build_filename (dir, "p", "q", "r", NULL);
   gchar *link = g_build_filename (dir, "link", NULL);

   gimv_test_write_string (file, "x");
   g_assert_true (file_exists (file));
   g_assert_true (isfile (file));
   g_assert_false (isdir (file));
   g_assert_true (isdir (dir));
   g_assert_false (isfile (dir));
   g_assert_false (file_exists (sub));
   g_assert_false (file_exists (""));
   g_assert_false (isdir (NULL));

   /* not a directory */
   g_assert_cmpint (has_subdirs (file), ==, -1);
   g_assert_cmpint (has_subdirs (sub), ==, -1);

   g_assert_true (makedir (sub));
   g_assert_true (isdir (sub));
   g_assert_false (makedir (sub));               /* there already */

   /* mkdirs (): the directories of a file path (not the last part) */
   g_assert_true (mkdirs (deep_file));
   g_assert_true (isdir (deep));
   g_assert_false (file_exists (deep_file));
   {
      gchar *below = g_build_filename (file, "below", NULL);
      g_assert_false (mkdirs (below));            /* a file in the way */
      g_free (below);
   }

   /* ensure_dir_exists (): the whole path */
   g_assert_true (ensure_dir_exists (full));
   g_assert_true (isdir (full));
   g_assert_true (ensure_dir_exists (full));

   g_assert_cmpint (symlink (file, link), ==, 0);
   g_assert_true (islink (link));
   g_assert_false (islink (file));
   g_assert_true (isfile (link));                 /* followed */

   gimv_test_set_mtime (file, 1234567890);
   g_assert_cmpint (get_file_mtime (file), ==, 1234567890);
   g_assert_cmpint (get_file_mtime (""), ==, 0);
   g_assert_cmpint (get_file_mtime (NULL), ==, 0);

   g_assert_true (iswritable (dir));
   g_assert_false (iswritable (file));            /* only directories */

   gimv_test_rm_rf (dir);
   g_free (dir);
   g_free (file);
   g_free (sub);
   g_free (deep);
   g_free (deep_file);
   g_free (full);
   g_free (link);
}


static void
mkdirs_long (void)
{
   gchar *dir, *path, *file;
   GString *s;

   dir = gimv_test_mkdtemp ();
   s = g_string_new (dir);
   while (s->len < 1200) g_string_append (s, "/directory-name-of-some-length");
   path = g_strdup (s->str);
   file = g_build_filename (path, "file.txt", NULL);
   /* all of them made, or FALSE */
   if (mkdirs (file))
      g_assert_true (isdir (path));
   gimv_test_rm_rf (dir);
   g_string_free (s, TRUE);
   g_free (dir);
   g_free (path);
   g_free (file);
}

static void
test_mkdirs_long (void)
{
   if (gimv_test_known_bug ("mkdirs () copies a path of MAX_PATH_LEN (1024) bytes or "
                            "more into its buffer of 1024 with the NUL one past the "
                            "end (the stack is smashed), and stops there"))
      return;
   if (g_test_subprocess ()) {
      mkdirs_long ();
      return;
   }
   g_test_trap_subprocess (NULL, 0, 0);
   g_test_trap_assert_passed ();
}


int
main (int argc, char *argv[])
{
   setlocale (LC_ALL, "C");
   g_test_init (&argc, &argv, NULL);

   g_test_add_func ("/fileutil/size2str", test_size2str);
   g_test_add_func ("/fileutil/time2str", test_time2str);
   g_test_add_func ("/fileutil/uid-gid", test_uid_gid);
   g_test_add_func ("/fileutil/mode2str", test_mode2str);
   g_test_add_func ("/fileutil/mode2str-setid-exec", test_mode2str_setid_exec);
   g_test_add_func ("/fileutil/home2tilde", test_home2tilde);
   g_test_add_func ("/fileutil/home2tilde-long", test_home2tilde_long);
   g_test_add_func ("/fileutil/basename", test_basename);
   g_test_add_func ("/fileutil/extension", test_extension);
   g_test_add_func ("/fileutil/file-tests", test_file_tests);
   g_test_add_func ("/fileutil/mkdirs-long", test_mkdirs_long);

   return g_test_run ();
}
