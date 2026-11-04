/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2026 shitamo
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

#include <string.h>

#include "gimv_orient.h"


/* the turnings of the EXIF orientations, as matrices on (x, y) (y down):
 * the image as stored -> the image as seen */
static const gint orient_matrix[9][4] = {
   { 0 },
   {  1,  0,  0,  1 },   /* 1 */
   { -1,  0,  0,  1 },   /* 2: flipped left and right */
   { -1,  0,  0, -1 },   /* 3: 180 degrees */
   {  1,  0,  0, -1 },   /* 4: flipped upside down */
   {  0,  1,  1,  0 },   /* 5: transposed */
   {  0, -1,  1,  0 },   /* 6: 90 degrees clockwise */
   {  0, -1, -1,  0 },   /* 7: transversed */
   {  0,  1, -1,  0 },   /* 8: 90 degrees counterclockwise */
};

static gint
orient_from_matrix (const gint m[4])
{
   gint o;
   for (o = 1; o <= 8; o++)
      if (!memcmp (orient_matrix[o], m, sizeof (gint) * 4)) return o;
   return 1;
}

/* a then b */
gint
gimv_orient_compose (gint a, gint b)
{
   const gint *x = orient_matrix[b], *y = orient_matrix[a];
   gint m[4] = { x[0] * y[0] + x[1] * y[2], x[0] * y[1] + x[1] * y[3],
                 x[2] * y[0] + x[3] * y[2], x[2] * y[1] + x[3] * y[3] };
   return orient_from_matrix (m);
}

gint
gimv_orient_inverse (gint o)
{
   const gint *x = orient_matrix[o];
   gint m[4] = { x[0], x[2], x[1], x[3] };   /* orthogonal: the transpose */
   return orient_from_matrix (m);
}


/* the pixbuf turned by the orientation o (its options not looked at) */
GdkPixbuf *
gimv_orient_pixbuf_turn (GdkPixbuf *pixbuf, gint o)
{
   GdkPixbuf *a, *b;

   switch (o) {
   case 2: return gdk_pixbuf_flip (pixbuf, TRUE);
   case 3: return gdk_pixbuf_rotate_simple (pixbuf, GDK_PIXBUF_ROTATE_UPSIDEDOWN);
   case 4: return gdk_pixbuf_flip (pixbuf, FALSE);
   case 6: return gdk_pixbuf_rotate_simple (pixbuf, GDK_PIXBUF_ROTATE_CLOCKWISE);
   case 8: return gdk_pixbuf_rotate_simple (pixbuf, GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
   case 5:
   case 7:
      a = gdk_pixbuf_rotate_simple (pixbuf, o == 5 ? GDK_PIXBUF_ROTATE_CLOCKWISE
                                                   : GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
      if (!a) return g_object_ref (pixbuf);
      b = gdk_pixbuf_flip (a, TRUE);
      g_object_unref (a);
      return b;
   default:
      return g_object_ref (pixbuf);
   }
}


gint
gimv_orient_user_turn (gint exif, gint rotate, gboolean flip)
{
   gint wanted;

   /* as the image window: turned over left and right first, then turned
    * (1: 90 degrees counterclockwise, 2: 180, 3: 90 clockwise) */
   wanted = gimv_orient_compose (flip ? 2 : 1,
                                 rotate == 1 ? 8 : rotate == 2 ? 3 : rotate == 3 ? 6 : 1);
   /* undo the EXIF one, then the user's */
   return gimv_orient_compose (gimv_orient_inverse (exif), wanted);
}
