/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: the faces of the image shown, in a panel next to
 * an image view.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_FACE_PANEL_H__
#define __GIMV_FACE_PANEL_H__

#include "gimv_image_view.h"

/* a box with the image view and the panel (hidden) at its right: put it
 * where the image view would go */
GtkWidget *gimv_face_panel_wrap        (GimvImageView *iv);
/* the image view has a panel */
gboolean   gimv_face_panel_has         (GimvImageView *iv);
gboolean   gimv_face_panel_get_visible (GimvImageView *iv);
void       gimv_face_panel_set_visible (GimvImageView *iv,
                                        gboolean       visible);

#endif /* __GIMV_FACE_PANEL_H__ */
