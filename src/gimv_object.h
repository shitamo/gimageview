/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001-2004 Takuro Ashie
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
 */

/*
 *  GimvObject: replacement of GtkObject (removed in GTK3).
 *
 *  A floating GObject with a "destroy" signal that is emitted once when the
 *  object is disposed, either explicitly by gimv_object_destroy() or when
 *  the last reference is dropped.  Subclasses override the "destroy" class
 *  method just like they did with GtkObject.
 */

#ifndef __GIMV_OBJECT_H__
#define __GIMV_OBJECT_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define GIMV_TYPE_OBJECT            (gimv_object_get_type ())
#define GIMV_OBJECT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GIMV_TYPE_OBJECT, GimvObject))
#define GIMV_OBJECT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), GIMV_TYPE_OBJECT, GimvObjectClass))
#define GIMV_IS_OBJECT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GIMV_TYPE_OBJECT))
#define GIMV_OBJECT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), GIMV_TYPE_OBJECT, GimvObjectClass))

typedef struct _GimvObject      GimvObject;
typedef struct _GimvObjectClass GimvObjectClass;

struct _GimvObject {
   GInitiallyUnowned parent;
   guint             in_destruction : 1;
   guint             destroyed      : 1;
};

struct _GimvObjectClass {
   GInitiallyUnownedClass parent_class;

   void (*destroy) (GimvObject *object);
};

GType  gimv_object_get_type (void);

/*
 *  Destroy an object: for widgets this destroys the widget (see
 *  gimv_widget_destroy()), for other objects it runs dispose, which emits
 *  "destroy" once.
 */
void   gimv_object_destroy  (GObject *object);

G_END_DECLS

#endif /* __GIMV_OBJECT_H__ */
