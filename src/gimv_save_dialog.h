/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: saving the image shown in another format (JPEG, PNG, WebP,
 * ...), with the image saver plugins.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_SAVE_DIALOG_H__
#define __GIMV_SAVE_DIALOG_H__

#include <gtk/gtk.h>

#include "gimv_image_view.h"

/* the dialog for the image of iv, over parent */
void gimv_save_dialog_open (GimvImageView *iv, GtkWindow *parent);

#endif /* __GIMV_SAVE_DIALOG_H__ */
