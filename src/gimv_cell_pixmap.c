/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/* 
 * Copyright (C) 2002 Takuro Ashie
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * $Id: gimv_cell_pixmap.c,v 1.1 2003/07/06 16:46:21 makeinu Exp $
 */

/*
 *  These codes are based on gtk/gtkcellrendererpixbuf.c in gtk+-2.0.6
 *  Copyright (C) 2000  Red Hat, Inc.,  Jonathan Blandford <jrb@redhat.com>
 */

#include "gimv_cell_pixmap.h"


#include <stdlib.h>
#include "intl.h"

static void gimv_cell_renderer_pixmap_get_property  (GObject                    *object,
                                                     guint                       param_id,
                                                     GValue                     *value,
                                                     GParamSpec                 *pspec);
static void gimv_cell_renderer_pixmap_set_property  (GObject                    *object,
                                                     guint                       param_id,
                                                     const GValue               *value,
                                                     GParamSpec                 *pspec);
static void gimv_cell_renderer_pixmap_init          (GimvCellRendererPixmap      *celltext);
static void gimv_cell_renderer_pixmap_class_init    (GimvCellRendererPixmapClass *class);
static void gimv_cell_renderer_pixmap_finalize      (GObject                    *object);
static void gimv_cell_renderer_pixmap_get_preferred_width
                                                    (GtkCellRenderer            *cell,
                                                     GtkWidget                  *widget,
                                                     gint                       *minimum,
                                                     gint                       *natural);
static void gimv_cell_renderer_pixmap_get_preferred_height
                                                    (GtkCellRenderer            *cell,
                                                     GtkWidget                  *widget,
                                                     gint                       *minimum,
                                                     gint                       *natural);
static void gimv_cell_renderer_pixmap_snapshot      (GtkCellRenderer            *cell,
                                                     GtkSnapshot                *snapshot,
                                                     GtkWidget                  *widget,
                                                     const GdkRectangle         *background_area,
                                                     const GdkRectangle         *cell_area,
                                                     GtkCellRendererState        flags);


enum {
   PROP_ZERO,
   PROP_PIXMAP,
   PROP_MASK,
   PROP_PIXMAP_EXPANDER_OPEN,
   PROP_MASK_EXPANDER_OPEN,
   PROP_PIXMAP_EXPANDER_CLOSED,
   PROP_MASK_EXPANDER_CLOSED
};


GType
gimv_cell_renderer_pixmap_get_type (void)
{
   static GType cell_pixmap_type = 0;

   if (!cell_pixmap_type) {
      static const GTypeInfo cell_pixmap_info = {
         sizeof (GimvCellRendererPixmapClass),
         NULL,		/* base_init */
         NULL,		/* base_finalize */
         (GClassInitFunc) gimv_cell_renderer_pixmap_class_init,
         NULL,		/* class_finalize */
         NULL,		/* class_data */
         sizeof (GimvCellRendererPixmap),
         0,       /* n_preallocs */
         (GInstanceInitFunc) gimv_cell_renderer_pixmap_init,
      };

      cell_pixmap_type = g_type_register_static (GTK_TYPE_CELL_RENDERER,
                                                 "Gimvcellrendererpixmap",
                                                 &cell_pixmap_info, 0);
   }

   return cell_pixmap_type;
}


static void
gimv_cell_renderer_pixmap_init (GimvCellRendererPixmap *cellpixmap)
{
}


static void
gimv_cell_renderer_pixmap_class_init (GimvCellRendererPixmapClass *class)
{
   GObjectClass *object_class       = G_OBJECT_CLASS (class);
   GtkCellRendererClass *cell_class = GTK_CELL_RENDERER_CLASS (class);

   object_class->get_property       = gimv_cell_renderer_pixmap_get_property;
   object_class->set_property       = gimv_cell_renderer_pixmap_set_property;

   object_class->finalize           = gimv_cell_renderer_pixmap_finalize;

   cell_class->get_preferred_width  = gimv_cell_renderer_pixmap_get_preferred_width;
   cell_class->get_preferred_height = gimv_cell_renderer_pixmap_get_preferred_height;
   cell_class->snapshot             = gimv_cell_renderer_pixmap_snapshot;

   g_object_class_install_property (object_class,
                                    PROP_PIXMAP,
                                    g_param_spec_object ("pixmap",
                                                         _("Pixmap Object"),
                                                         _("The pixmap to render."),
                                                         GDK_TYPE_TEXTURE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE));

   g_object_class_install_property (object_class,
                                    PROP_MASK,
                                    g_param_spec_object ("mask",
                                                         _("Mask Object"),
                                                         _("The mask to render."),
                                                         GDK_TYPE_TEXTURE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE));

   g_object_class_install_property (object_class,
                                    PROP_PIXMAP_EXPANDER_OPEN,
                                    g_param_spec_object ("pixmap_expander_open",
                                                         _("Pixmap Expander Open"),
                                                         _("Pixmap for open expander."),
                                                         GDK_TYPE_TEXTURE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE));

   g_object_class_install_property (object_class,
                                    PROP_MASK_EXPANDER_OPEN,
                                    g_param_spec_object ("mask_expander_open",
                                                         _("Mask Expander Open"),
                                                         _("Mask for open expander."),
                                                         GDK_TYPE_TEXTURE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE));

   g_object_class_install_property (object_class,
                                    PROP_PIXMAP_EXPANDER_CLOSED,
                                    g_param_spec_object ("pixmap_expander_closed",
                                                         _("Pixmap Expander Closed"),
                                                         _("Pixmap for closed expander."),
                                                         GDK_TYPE_TEXTURE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE));

   g_object_class_install_property (object_class,
                                    PROP_MASK_EXPANDER_CLOSED,
                                    g_param_spec_object ("mask_expander_closed",
                                                         _("Mask Expander Closed"),
                                                         _("Mask for closed expander."),
                                                         GDK_TYPE_TEXTURE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE));
}


static void
gimv_cell_renderer_pixmap_get_property (GObject        *object,
                                        guint           param_id,
                                        GValue         *value,
                                        GParamSpec     *pspec)
{
   GimvCellRendererPixmap *cellpixmap = GIMV_CELL_RENDERER_PIXMAP (object);
  
   switch (param_id) {
   case PROP_PIXMAP:
      g_value_set_object (value,
                          cellpixmap->pixmap
                          ? G_OBJECT (cellpixmap->pixmap) : NULL);
      break;
   case PROP_MASK:
      g_value_set_object (value,
                          cellpixmap->mask
                          ? G_OBJECT (cellpixmap->mask) : NULL);
      break;
   case PROP_PIXMAP_EXPANDER_OPEN:
      g_value_set_object (value,
                          cellpixmap->pixmap_expander_open
                          ? G_OBJECT (cellpixmap->pixmap_expander_open) : NULL);
      break;
   case PROP_MASK_EXPANDER_OPEN:
      g_value_set_object (value,
                          cellpixmap->mask_expander_open
                          ? G_OBJECT (cellpixmap->mask_expander_open) : NULL);
      break;
   case PROP_PIXMAP_EXPANDER_CLOSED:
      g_value_set_object (value,
                          cellpixmap->pixmap_expander_closed
                          ? G_OBJECT (cellpixmap->pixmap_expander_closed) : NULL);
      break;
   case PROP_MASK_EXPANDER_CLOSED:
      g_value_set_object (value,
                          cellpixmap->mask_expander_closed
                          ? G_OBJECT (cellpixmap->mask_expander_closed) : NULL);
      break;
   default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
      break;
   }
}


static void
gimv_cell_renderer_pixmap_set_property (GObject      *object,
                                        guint         param_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
   GdkTexture *pixmap;
   GdkTexture *mask;
   GimvCellRendererPixmap *cellpixmap = GIMV_CELL_RENDERER_PIXMAP (object);
  
   switch (param_id) {
   case PROP_PIXMAP:
      pixmap = (GdkTexture*) g_value_get_object (value);
      if (pixmap)
         g_object_ref (G_OBJECT (pixmap));
      if (cellpixmap->pixmap)
         g_object_unref (G_OBJECT (cellpixmap->pixmap));
      cellpixmap->pixmap = pixmap;
      break;
   case PROP_MASK:
      mask = (GdkTexture*) g_value_get_object (value);
      if (mask)
         g_object_ref (G_OBJECT (mask));
      if (cellpixmap->mask)
         g_object_unref (G_OBJECT (cellpixmap->mask));
      cellpixmap->mask = mask;
      break;
   case PROP_PIXMAP_EXPANDER_OPEN:
      pixmap = (GdkTexture*) g_value_get_object (value);
      if (pixmap)
         g_object_ref (G_OBJECT (pixmap));
      if (cellpixmap->pixmap_expander_open)
         g_object_unref (G_OBJECT (cellpixmap->pixmap_expander_open));
      cellpixmap->pixmap_expander_open = pixmap;
      break;
   case PROP_MASK_EXPANDER_OPEN:
      mask = (GdkTexture*) g_value_get_object (value);
      if (mask)
         g_object_ref (G_OBJECT (mask));
      if (cellpixmap->mask_expander_open)
         g_object_unref (G_OBJECT (cellpixmap->mask_expander_open));
      cellpixmap->mask_expander_open = mask;
      break;
   case PROP_PIXMAP_EXPANDER_CLOSED:
      pixmap = (GdkTexture*) g_value_get_object (value);
      if (pixmap)
         g_object_ref (G_OBJECT (pixmap));
      if (cellpixmap->pixmap_expander_closed)
         g_object_unref (G_OBJECT (cellpixmap->pixmap_expander_closed));
      cellpixmap->pixmap_expander_closed = pixmap;
      break;
   case PROP_MASK_EXPANDER_CLOSED:
      mask = (GdkTexture*) g_value_get_object (value);
      if (mask)
         g_object_ref (G_OBJECT (mask));
      if (cellpixmap->mask_expander_closed)
         g_object_unref (G_OBJECT (cellpixmap->mask_expander_closed));
      cellpixmap->mask_expander_closed = mask;
      break;
   default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
      break;
   }
}


GtkCellRenderer *
gimv_cell_renderer_pixmap_new (void)
{
   return GTK_CELL_RENDERER (g_object_new (gimv_cell_renderer_pixmap_get_type (), NULL));
}


static void
gimv_cell_renderer_pixmap_finalize (GObject *object)
{
   GimvCellRendererPixmap *cellpixmap = GIMV_CELL_RENDERER_PIXMAP (object);

   g_clear_object (&cellpixmap->pixmap);
   g_clear_object (&cellpixmap->mask);
   g_clear_object (&cellpixmap->pixmap_expander_open);
   g_clear_object (&cellpixmap->mask_expander_open);
   g_clear_object (&cellpixmap->pixmap_expander_closed);
   g_clear_object (&cellpixmap->mask_expander_closed);

   G_OBJECT_CLASS (g_type_class_peek_parent (G_OBJECT_GET_CLASS (object)))->finalize (object);
}


static void
gimv_cell_renderer_pixmap_get_pixmap_size (GimvCellRendererPixmap *cellpixmap,
                                           gint *width, gint *height)
{
   GdkTexture *textures[3];
   gint i, pixmap_width = 0, pixmap_height = 0;

   textures[0] = cellpixmap->pixmap;
   textures[1] = cellpixmap->pixmap_expander_open;
   textures[2] = cellpixmap->pixmap_expander_closed;

   for (i = 0; i < 3; i++) {
      if (!textures[i]) continue;
      pixmap_width  = MAX (pixmap_width,  gdk_texture_get_width  (textures[i]));
      pixmap_height = MAX (pixmap_height, gdk_texture_get_height (textures[i]));
   }

   if (width)  *width  = pixmap_width;
   if (height) *height = pixmap_height;
}


static void
gimv_cell_renderer_pixmap_get_size (GtkCellRenderer    *cell,
                                    const GdkRectangle *cell_area,
                                    gint               *x_offset,
                                    gint               *y_offset,
                                    gint               *width,
                                    gint               *height)
{
   GimvCellRendererPixmap *cellpixmap = (GimvCellRendererPixmap *) cell;
   gint pixmap_width = 0;
   gint pixmap_height = 0;
   gint calc_width;
   gint calc_height;
   gint xpad, ypad;
   gfloat xalign, yalign;

   gtk_cell_renderer_get_padding (cell, &xpad, &ypad);
   gtk_cell_renderer_get_alignment (cell, &xalign, &yalign);

   gimv_cell_renderer_pixmap_get_pixmap_size (cellpixmap,
                                              &pixmap_width, &pixmap_height);

   calc_width  = xpad * 2 + pixmap_width;
   calc_height = ypad * 2 + pixmap_height;

   if (x_offset) *x_offset = 0;
   if (y_offset) *y_offset = 0;

   if (cell_area && pixmap_width > 0 && pixmap_height > 0) {
      if (x_offset) {
         *x_offset = xalign * (cell_area->width - calc_width - (2 * xpad));
         *x_offset = MAX (*x_offset, 0) + xpad;
      }
      if (y_offset) {
         *y_offset = yalign * (cell_area->height - calc_height - (2 * ypad));
         *y_offset = MAX (*y_offset, 0) + ypad;
      }
   }

   if (width)
      *width = calc_width;
  
   if (height)
      *height = calc_height;
}


static void
gimv_cell_renderer_pixmap_get_preferred_width (GtkCellRenderer *cell,
                                               GtkWidget       *widget,
                                               gint            *minimum,
                                               gint            *natural)
{
   gint width;

   gimv_cell_renderer_pixmap_get_size (cell, NULL, NULL, NULL, &width, NULL);
   if (minimum) *minimum = width;
   if (natural) *natural = width;
}


static void
gimv_cell_renderer_pixmap_get_preferred_height (GtkCellRenderer *cell,
                                                GtkWidget       *widget,
                                                gint            *minimum,
                                                gint            *natural)
{
   gint height;

   gimv_cell_renderer_pixmap_get_size (cell, NULL, NULL, NULL, NULL, &height);
   if (minimum) *minimum = height;
   if (natural) *natural = height;
}


static void
gimv_cell_renderer_pixmap_snapshot (GtkCellRenderer      *cell,
                                    GtkSnapshot          *snapshot,
                                    GtkWidget            *widget,
                                    const GdkRectangle   *background_area,
                                    const GdkRectangle   *cell_area,
                                    GtkCellRendererState  flags)

{
   GimvCellRendererPixmap *cellpixmap = (GimvCellRendererPixmap *) cell;
   GdkTexture *pixmap;
   GdkRectangle pix_rect;
   GdkRectangle draw_rect;
   gint xpad, ypad;

   pixmap = cellpixmap->pixmap;
   if (gtk_cell_renderer_get_is_expander (cell)) {
      gboolean expanded = gtk_cell_renderer_get_is_expanded (cell);

      if (expanded && cellpixmap->pixmap_expander_open != NULL) {
         pixmap = cellpixmap->pixmap_expander_open;
      } else if (!expanded && cellpixmap->pixmap_expander_closed != NULL) {
         pixmap = cellpixmap->pixmap_expander_closed;
      }
   }

   if (!pixmap) return;

   gimv_cell_renderer_pixmap_get_size (cell, cell_area,
                                       &pix_rect.x,
                                       &pix_rect.y,
                                       &pix_rect.width,
                                       &pix_rect.height);

   gtk_cell_renderer_get_padding (cell, &xpad, &ypad);
   pix_rect.x += cell_area->x;
   pix_rect.y += cell_area->y;
   pix_rect.width -= xpad * 2;
   pix_rect.height -= ypad * 2;

   if (!gdk_rectangle_intersect (cell_area, &pix_rect, &draw_rect)) return;

   /* the mask is part of the texture (alpha channel) in GTK4 */
   gtk_snapshot_push_clip (snapshot,
                           &GRAPHENE_RECT_INIT (draw_rect.x, draw_rect.y,
                                                draw_rect.width, draw_rect.height));
   gtk_snapshot_append_texture (snapshot, pixmap,
                                &GRAPHENE_RECT_INIT (pix_rect.x, pix_rect.y,
                                                     gdk_texture_get_width (pixmap),
                                                     gdk_texture_get_height (pixmap)));
   gtk_snapshot_pop (snapshot);
}
