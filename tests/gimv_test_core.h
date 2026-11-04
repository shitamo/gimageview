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
 * For the tests linked with the objects of gimv (gimv-test-core): the
 * preferences read from an empty HOME of their own, and plugins loaded
 * one by one from the build tree.
 */

#ifndef __GIMV_TEST_CORE_H__
#define __GIMV_TEST_CORE_H__

#include "gimv_image.h"

/* HOME (and the XDG directories) a new temporary directory, the default
   preferences, no plugin loaded; returns the HOME (removed by
   gimv_test_core_finish ()) */
const gchar *gimv_test_core_init      (void);
void         gimv_test_core_finish    (void);

/* the plugin <build>/plugins/<dir>/lib<name>.so loaded (asserts) */
void         gimv_test_core_load_plugin (const gchar *dir,
                                         const gchar *name);

/* an image file loaded with the plugins loaded (NULL: none could);
   animation: as for the image window */
GimvImage   *gimv_test_core_load_image (const gchar *path,
                                        gboolean     animation);

/* the pixel (x, y) of an image as 0xRRGGBB */
guint32      gimv_test_core_pixel     (GimvImage   *image,
                                       gint         x,
                                       gint         y);

#endif /* __GIMV_TEST_CORE_H__ */
