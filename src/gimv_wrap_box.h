/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: a box that lays its children out as words in a paragraph
 * (each at its own width, wrapping to the next line when the line is
 * full).  GtkFlowBox lines its children up in columns, which leaves wide
 * gaps between short tags.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_WRAP_BOX_H__
#define __GIMV_WRAP_BOX_H__

#include <gtk/gtk.h>

#define GIMV_TYPE_WRAP_BOX (gimv_wrap_box_get_type ())
G_DECLARE_FINAL_TYPE (GimvWrapBox, gimv_wrap_box, GIMV, WRAP_BOX, GtkWidget)

GtkWidget *gimv_wrap_box_new    (gint         column_spacing,
                                 gint         row_spacing);
void       gimv_wrap_box_append (GimvWrapBox *box,
                                 GtkWidget   *child);
void       gimv_wrap_box_remove (GimvWrapBox *box,
                                 GtkWidget   *child);
void       gimv_wrap_box_clear  (GimvWrapBox *box);

#endif /* __GIMV_WRAP_BOX_H__ */
