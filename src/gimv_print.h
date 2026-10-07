/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001-2004 Takuro Ashie
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Printing with GtkPrintOperation (GTK 4 port).
 */

#ifndef __GIMV_PRINT_H__
#define __GIMV_PRINT_H__

#include <gtk/gtk.h>
#include "gimv_image_info.h"
#include "gimv_image_view.h"

/* one page per image; list of GimvImageInfo (not taken over) */
void gimv_print_images     (GList         *infos,
                            GtkWindow     *parent);

/* the image of the view, with its rotation */
void gimv_print_image_view (GimvImageView *iv,
                            GtkWindow     *parent);

#endif /* __GIMV_PRINT_H__ */
