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

#include <locale.h>

#include "gimv_test.h"
#include "gimv_test_core.h"

#include "gimageview.h"
#include "gimv_image_loader.h"
#include "gimv_plugin.h"
#include "prefs.h"

static gchar *test_home = NULL;


const gchar *
gimv_test_core_init (void)
{
   gchar *rc, *dir;

   setlocale (LC_ALL, "C");
   test_home = gimv_test_mkdtemp ();
   g_setenv ("HOME", test_home, TRUE);
   dir = g_build_filename (test_home, ".cache", NULL);
   g_setenv ("XDG_CACHE_HOME", dir, TRUE);
   g_free (dir);
   dir = g_build_filename (test_home, ".local", "share", NULL);
   g_setenv ("XDG_DATA_HOME", dir, TRUE);
   g_free (dir);
   dir = g_build_filename (test_home, ".config", NULL);
   g_setenv ("XDG_CONFIG_HOME", dir, TRUE);
   g_free (dir);

   /* the preferences files there (prefs_load_config () warns of missing
    * ones): the defaults, and plugins only from where the tests say */
   rc = g_build_filename (test_home, GIMV_RC_DIR, GIMV_RC, NULL);
   gimv_test_write_string (rc,
                           "plugin_use_default_search_dir_list=FALSE\n"
                           "plugin_search_dir_list=\n");
   g_free (rc);
   rc = g_build_filename (test_home, GIMV_RC_DIR, GIMV_KEYCONF_RC, NULL);
   gimv_test_write_string (rc, "\n");
   g_free (rc);

   prefs_load_config ();
   g_assert_false (conf.plugin_use_default_search_dir_list);
   gimv_plugin_init ();

   return test_home;
}


void
gimv_test_core_finish (void)
{
   gimv_test_rm_rf (test_home);
   g_clear_pointer (&test_home, g_free);
}


void
gimv_test_core_load_plugin (const gchar *dir, const gchar *name)
{
   const gchar *root = g_getenv ("GIMV_TEST_BUILD_ROOT");
   gchar *file, *path;

   g_assert_nonnull (root);
   file = g_strconcat ("lib", name, ".", G_MODULE_SUFFIX, NULL);
   path = g_build_filename (root, "plugins", dir, file, NULL);
   if (!gimv_plugin_load_module (path, NULL))
      g_error ("cannot load the plugin %s", path);
   g_free (file);
   g_free (path);
}


GimvImage *
gimv_test_core_load_image (const gchar *path, gboolean animation)
{
   GimvImageLoader *loader = gimv_image_loader_new_with_file_name (path);
   GimvImage *image;

   g_assert_nonnull (loader);
   gimv_image_loader_set_as_animation (loader, animation);
   gimv_image_loader_load (loader);
   image = gimv_image_loader_get_image (loader);
   if (image) gimv_image_ref (image);
   gimv_image_loader_unref (loader);
   return image;
}


guint32
gimv_test_core_pixel (GimvImage *image, gint x, gint y)
{
   guchar *p;
   gint n = gimv_image_has_alpha (image) ? 4 : 3;

   g_assert_cmpint (x, >=, 0);
   g_assert_cmpint (y, >=, 0);
   g_assert_cmpint (x, <, gimv_image_width (image));
   g_assert_cmpint (y, <, gimv_image_height (image));
   p = gimv_image_get_pixels (image) + y * gimv_image_rowstride (image) + x * n;
   return ((guint32) p[0] << 16) | (p[1] << 8) | p[2];
}
