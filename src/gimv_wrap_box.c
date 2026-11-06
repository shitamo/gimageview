/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: a box that lays its children out as words in a paragraph.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimv_wrap_box.h"

struct _GimvWrapBox {
   GtkWidget parent;
   gint      column_spacing;
   gint      row_spacing;
};

G_DEFINE_TYPE (GimvWrapBox, gimv_wrap_box, GTK_TYPE_WIDGET)


/* lays the children out in width (no allocation when alloc is FALSE);
 * returns the height used */
static gint
layout (GimvWrapBox *box, gint width, gboolean alloc)
{
   GtkWidget *child;
   gint x = 0, y = 0, line_h = 0;
   gboolean first_in_line = TRUE;

   for (child = gtk_widget_get_first_child (GTK_WIDGET (box)); child;
        child = gtk_widget_get_next_sibling (child))
   {
      gint min_w, nat_w, w, h;

      if (!gtk_widget_should_layout (child)) continue;
      gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1, &min_w, &nat_w, NULL, NULL);
      w = width > 0 ? MAX (min_w, MIN (nat_w, width)) : nat_w;
      if (!first_in_line && x + box->column_spacing + w > width) {
         /* the next line */
         x = 0;
         y += line_h + box->row_spacing;
         line_h = 0;
         first_in_line = TRUE;
      }
      if (!first_in_line) x += box->column_spacing;
      gtk_widget_measure (child, GTK_ORIENTATION_VERTICAL, w, NULL, &h, NULL, NULL);
      if (alloc) {
         GtkAllocation a = { x, y, w, h };
         gtk_widget_size_allocate (child, &a, -1);
      }
      x += w;
      line_h = MAX (line_h, h);
      first_in_line = FALSE;
   }
   return y + line_h;
}


static GtkSizeRequestMode
wrap_box_get_request_mode (GtkWidget *widget)
{
   return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}


static void
wrap_box_measure (GtkWidget *widget, GtkOrientation orientation, gint for_size,
                  gint *minimum, gint *natural, gint *min_baseline, gint *nat_baseline)
{
   GimvWrapBox *box = GIMV_WRAP_BOX (widget);
   GtkWidget *child;

   *minimum = *natural = 0;
   if (orientation == GTK_ORIENTATION_HORIZONTAL) {
      gboolean first = TRUE;
      /* at least the widest child; at best all in one line */
      for (child = gtk_widget_get_first_child (widget); child;
           child = gtk_widget_get_next_sibling (child))
      {
         gint min_w, nat_w;
         if (!gtk_widget_should_layout (child)) continue;
         gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1, &min_w, &nat_w, NULL, NULL);
         *minimum = MAX (*minimum, min_w);
         *natural += nat_w + (first ? 0 : box->column_spacing);
         first = FALSE;
      }
   } else {
      gint h = layout (box, for_size < 0 ? G_MAXINT : for_size, FALSE);
      *minimum = *natural = h;
   }
}


static void
wrap_box_size_allocate (GtkWidget *widget, gint width, gint height, gint baseline)
{
   layout (GIMV_WRAP_BOX (widget), width, TRUE);
}


static void
wrap_box_dispose (GObject *object)
{
   GtkWidget *child;

   while ((child = gtk_widget_get_first_child (GTK_WIDGET (object))))
      gtk_widget_unparent (child);
   G_OBJECT_CLASS (gimv_wrap_box_parent_class)->dispose (object);
}


static void
gimv_wrap_box_class_init (GimvWrapBoxClass *klass)
{
   GObjectClass *object_class = G_OBJECT_CLASS (klass);
   GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

   object_class->dispose = wrap_box_dispose;
   widget_class->get_request_mode = wrap_box_get_request_mode;
   widget_class->measure = wrap_box_measure;
   widget_class->size_allocate = wrap_box_size_allocate;
}


static void
gimv_wrap_box_init (GimvWrapBox *box)
{
}


GtkWidget *
gimv_wrap_box_new (gint column_spacing, gint row_spacing)
{
   GimvWrapBox *box = g_object_new (GIMV_TYPE_WRAP_BOX, NULL);
   box->column_spacing = column_spacing;
   box->row_spacing = row_spacing;
   return GTK_WIDGET (box);
}


void
gimv_wrap_box_append (GimvWrapBox *box, GtkWidget *child)
{
   g_return_if_fail (GIMV_IS_WRAP_BOX (box));
   gtk_widget_set_parent (child, GTK_WIDGET (box));
}


void
gimv_wrap_box_remove (GimvWrapBox *box, GtkWidget *child)
{
   g_return_if_fail (GIMV_IS_WRAP_BOX (box));
   g_return_if_fail (gtk_widget_get_parent (child) == GTK_WIDGET (box));
   gtk_widget_unparent (child);
}


void
gimv_wrap_box_clear (GimvWrapBox *box)
{
   GtkWidget *child;

   g_return_if_fail (GIMV_IS_WRAP_BOX (box));
   while ((child = gtk_widget_get_first_child (GTK_WIDGET (box))))
      gtk_widget_unparent (child);
}
