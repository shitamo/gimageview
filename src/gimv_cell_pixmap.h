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
 * $Id: gimv_cell_pixmap.h,v 1.1 2003/07/06 16:46:21 makeinu Exp $
 */

/*
 *  These codes are based on gtk/gtkcellrendererpixbuf.h in gtk+-2.0.6
 *  Copyright (C) 2000  Red Hat, Inc.,  Jonathan Blandford <jrb@redhat.com>
 */

#ifndef __GIMV_CELL_RENDERER_PIXMAP_H__
#define __GIMV_CELL_RENDERER_PIXMAP_H__

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_object.h"


#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


#define GIMV_TYPE_CELL_RENDERER_PIXMAP			     (gimv_cell_renderer_pixmap_get_type ())
#define GIMV_CELL_RENDERER_PIXMAP(obj)			     (G_TYPE_CHECK_INSTANCE_CAST ((obj), GIMV_TYPE_CELL_RENDERER_PIXMAP, GimvCellRendererPixmap))
#define GIMV_CELL_RENDERER_PIXMAP_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), GIMV_TYPE_CELL_RENDERER_PIXMAP, GimvCellRendererPixmapClass))
#define GIMV_IS_CELL_RENDERER_PIXMAP(obj)		     (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GIMV_TYPE_CELL_RENDERER_PIXMAP))
#define GIMV_IS_CELL_RENDERER_PIXMAP_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GIMV_TYPE_CELL_RENDERER_PIXMAP))
#define GIMV_CELL_RENDERER_PIXMAP_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), GIMV_TYPE_CELL_RENDERER_PIXMAP, GimvCellRendererPixmapClass))

typedef struct GimvCellRendererPixmap_Tag         GimvCellRendererPixmap;
typedef struct GimvCellRendererPixmapClass_Tag    GimvCellRendererPixmapClass;

struct GimvCellRendererPixmap_Tag
{
   GtkCellRenderer parent;

   GdkTexture *pixmap;
   GdkTexture *mask;
   GdkTexture *pixmap_expander_open;
   GdkTexture *mask_expander_open;
   GdkTexture *pixmap_expander_closed;
   GdkTexture *mask_expander_closed;
};

struct GimvCellRendererPixmapClass_Tag
{
   GtkCellRendererClass parent_class;

   /* Padding for future expansion */
   void (*_gimv_reserved1) (void);
   void (*_gimv_reserved2) (void);
   void (*_gimv_reserved3) (void);
   void (*_gimv_reserved4) (void);
};

GType          gimv_cell_renderer_pixmap_get_type (void);
GtkCellRenderer *gimv_cell_renderer_pixmap_new      (void);

#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif /* (GTK_MAJOR_VERSION >= 2) */
