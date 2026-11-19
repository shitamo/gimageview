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

#ifndef __GIMV_PAGE_LAYOUT_DIALOG_H__
#define __GIMV_PAGE_LAYOUT_DIALOG_H__

#include <gtk/gtk.h>

#include "gimv_image_view.h"

/* [Two Pages | Edit Layouts...]: the layouts of pages side by side, edited
   as they are shown (one window; iv: the view it was opened from) */
void gimv_page_layout_dialog_open (GtkWindow     *parent,
                                   GimvImageView *iv);

#endif /* __GIMV_PAGE_LAYOUT_DIALOG_H__ */
