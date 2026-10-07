/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001-2002 Takuro Ashie
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
 * $Id: gimv_anim.c,v 1.17 2004/06/17 06:37:26 makeinu Exp $
 */

#include "gimv_anim.h"

#  include <gdk-pixbuf/gdk-pixbuf.h>


static void gimv_anim_class_init    (GimvAnimClass *klass);
static void gimv_anim_init          (GimvAnim      *anim);
static void gimv_anim_destroy       (GimvObject     *object);


static GimvImageClass *parent_class = NULL;


GType
gimv_anim_get_type (void)
{
   static GType gimv_anim_type = 0;

   if (!gimv_anim_type) {
      static const GTypeInfo gimv_anim_info = {
         sizeof (GimvAnimClass),
         NULL, /* base_init */
         NULL, /* base_finalize */
         (GClassInitFunc) gimv_anim_class_init,
         NULL, /* class_finalize */
         NULL, /* class_data */
         sizeof (GimvAnim),
         0,    /* n_preallocs */
         (GInstanceInitFunc) gimv_anim_init,
         NULL, /* value_table */
      };

      gimv_anim_type = g_type_register_static (GIMV_TYPE_IMAGE,
                                               "GimvAnim",
                                               &gimv_anim_info,
                                               0);
   }

   return gimv_anim_type;
}


static void
gimv_anim_class_init (GimvAnimClass *klass)
{
   GimvObjectClass *object_class;

   object_class = (GimvObjectClass *) klass;
   parent_class = g_type_class_peek_parent (klass);

   object_class->destroy  = gimv_anim_destroy;
}


static void
gimv_anim_init (GimvAnim *anim)
{
   anim->anim = NULL;
   anim->current_frame_idx = -1;
   anim->table = NULL;
}


static void
gimv_anim_destroy (GimvObject *object)
{
   GimvAnim *anim;

   g_return_if_fail (GIMV_IS_ANIM (object));

   anim = GIMV_ANIM (object);

   g_return_if_fail (anim->table);
   g_return_if_fail (anim->table->delete);

   if (anim->anim && anim->table && anim->table->delete) {
      anim->table->delete (anim);
      anim->anim  = NULL;
   }
   anim->table = NULL;

   if (GIMV_OBJECT_CLASS (parent_class)->destroy)
      (*GIMV_OBJECT_CLASS (parent_class)->destroy) (object);
}


GimvAnim *
gimv_anim_new (void)
{
   GimvAnim *anim = GIMV_ANIM (g_object_new (GIMV_TYPE_ANIM, NULL));
   return anim;
}


gint
gimv_anim_get_length (GimvAnim *anim)
{
   g_return_val_if_fail (anim, -1);
   g_return_val_if_fail (anim->table, -1);
   
   if (anim->table->get_length)
      return anim->table->get_length (anim);

   return -1;
}


gint
gimv_anim_iterate (GimvAnim *anim)
{
   g_return_val_if_fail (anim, -1);
   g_return_val_if_fail (anim->table, -1);
   g_return_val_if_fail (anim->table->iterate, -1);

   return anim->table->iterate (anim);
}


gboolean
gimv_anim_seek (GimvAnim *anim, gint idx)
{
   g_return_val_if_fail (anim, FALSE);
   g_return_val_if_fail (anim->table, FALSE);

   if (anim->table->seek)
      return anim->table->seek (anim, idx);

   return FALSE;
}


gint
gimv_anim_get_interval (GimvAnim *anim)
{
   g_return_val_if_fail (anim, -1);
   g_return_val_if_fail (anim->table, -1);
   g_return_val_if_fail (anim->table->get_interval, -1);

   return anim->table->get_interval (anim);
}


static void
free_rgb_buffer (guchar *pixels, gpointer data)
{
   g_free(pixels);
}

gboolean
gimv_anim_update_frame (GimvAnim *anim,
                        guchar   *frame,
                        gint      width,
                        gint      height,
                        gboolean  has_alpha)
{
   GimvImage *image = (GimvImage *) anim;

   g_return_val_if_fail (anim, FALSE);

   {
      gint bytes = 3;

      if (has_alpha)
         bytes = 4;

      if (image->image)
         g_object_unref (image->image);

      image->image = gdk_pixbuf_new_from_data (frame, GDK_COLORSPACE_RGB, FALSE, 8,
                                               width, height, bytes * width,
                                               free_rgb_buffer, NULL);
   }

   if (image->image)
      return TRUE;
   else
      return FALSE;
}
