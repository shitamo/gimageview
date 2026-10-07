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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gtk/gtk.h>

#include "gimv_object.h"
#include "gimv_gtk4_compat.h"

enum {
   DESTROY_SIGNAL,
   LAST_SIGNAL
};

static guint object_signals[LAST_SIGNAL] = {0};

G_DEFINE_TYPE (GimvObject, gimv_object, G_TYPE_INITIALLY_UNOWNED)


static void
gimv_object_dispose (GObject *gobject)
{
   GimvObject *object = GIMV_OBJECT (gobject);

   if (!object->in_destruction && !object->destroyed) {
      object->in_destruction = TRUE;
      g_signal_emit (object, object_signals[DESTROY_SIGNAL], 0);
      object->in_destruction = FALSE;
      object->destroyed = TRUE;
   }

   G_OBJECT_CLASS (gimv_object_parent_class)->dispose (gobject);
}


static void
gimv_object_real_destroy (GimvObject *object)
{
   g_signal_handlers_destroy (object);
}


static void
gimv_object_class_init (GimvObjectClass *klass)
{
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   gobject_class->dispose = gimv_object_dispose;
   klass->destroy = gimv_object_real_destroy;

   object_signals[DESTROY_SIGNAL]
      = g_signal_new ("destroy",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_CLEANUP | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
                      G_STRUCT_OFFSET (GimvObjectClass, destroy),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);
}


static void
gimv_object_init (GimvObject *object)
{
}


void
gimv_object_destroy (GObject *object)
{
   if (!object) return;

   if (GTK_IS_WIDGET (object)) {
      gimv_widget_destroy (GTK_WIDGET (object));
      return;
   }

   g_return_if_fail (G_IS_OBJECT (object));

   g_object_ref (object);
   g_object_run_dispose (object);
   g_object_unref (object);
}
