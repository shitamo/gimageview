/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: cropping the image shown, saved as a new file.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_CROP_H__
#define __GIMV_CROP_H__

#include <gtk/gtk.h>

#include "gimv_image_view.h"

/* a rectangle on the image, a bar over it (detect the edges, the ratio,
 * the size, save, stop); Enter saves "name_crop.jpg", Esc stops */
void     gimv_crop_start     (GimvImageView *iv);
/* nothing when not cropping */
void     gimv_crop_stop      (GimvImageView *iv);
gboolean gimv_crop_is_active (GimvImageView *iv);

#endif /* __GIMV_CROP_H__ */
