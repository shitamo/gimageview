/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/* Eye of Gnome image viewer - mouse cursors
 *
 * Copyright (C) 2000 The Free Software Foundation
 *
 * Author: Federico Mena-Quintero <federico@gnu.org>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 *
 * $Id: cursors.c,v 1.4 2002/10/03 12:27:36 makeinu Exp $
 */

#include <config.h>
#include "cursors.h"



/* Cursor definitions.  Keep in sync with the CursorType enumeration in
 * cursors.h.
 */

#include "cursors/hand-open-data.xbm"
#include "cursors/hand-open-mask.xbm"
#include "cursors/hand-closed-data.xbm"
#include "cursors/hand-closed-mask.xbm"
#include "cursors/void-data.xbm"
#include "cursors/void-mask.xbm"

static struct {
	char *data;
	char *mask;
	int data_width;
	int data_height;
	int mask_width;
	int mask_height;
	int hot_x, hot_y;
} cursors[] = {
	{ hand_open_data_bits, hand_open_mask_bits,
	  hand_open_data_width, hand_open_data_height,
	  hand_open_mask_width, hand_open_mask_height,
	  hand_open_data_width / 2, hand_open_data_height / 2 },
	{ hand_closed_data_bits, hand_closed_mask_bits,
	  hand_closed_data_width, hand_closed_data_height,
	  hand_closed_mask_width, hand_closed_mask_height,
	  hand_closed_data_width / 2, hand_closed_data_height / 2 },
	{ void_data_bits, void_mask_bits,
	  void_data_width, void_data_height,
	  void_mask_width, void_mask_height,	  void_data_width / 2, void_data_height / 2 },
	{ NULL, NULL, 0, 0, 0, 0 }
};



/* named cursors of the GTK4 cursor theme; the xbm data above is used to
   build a fallback texture cursor */
static const gchar *cursor_names[] = {
   "grab",
   "grabbing",
   "none",
};



static GdkTexture *
cursor_texture_from_xbm (CursorType type)
{
	GdkPixbuf *pixbuf;
	GdkTexture *texture;
	guchar *pixels, *p;
	gint width, height, rowstride, bpl, x, y;

	width  = cursors[type].data_width;
	height = cursors[type].data_height;
	bpl = (width + 7) / 8;

	pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, width, height);
	pixels = gdk_pixbuf_get_pixels (pixbuf);
	rowstride = gdk_pixbuf_get_rowstride (pixbuf);

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			gint idx = y * bpl + x / 8;
			gboolean fg   = (cursors[type].data[idx] >> (x % 8)) & 1;
			gboolean mask = (cursors[type].mask[idx] >> (x % 8)) & 1;
			guchar v = fg ? 0xff : 0x00;   /* fg: white, bg: black */

			p = pixels + y * rowstride + x * 4;
			p[0] = p[1] = p[2] = v;
			p[3] = mask ? 0xff : 0x00;
		}
	}

	texture = gdk_texture_new_for_pixbuf (pixbuf);
	g_object_unref (pixbuf);

	return texture;
}



/**
 * cursor_get:
 * @widget: Widget the cursor will be used for (unused in GTK4, kept for API).
 * @type: A cursor type.
 * 
 * Creates a cursor.  Use it with gtk_widget_set_cursor().
 * 
 * Return value: The newly-created cursor.
 **/
GdkCursor *
cursor_get (GtkWidget *widget, CursorType type)
{
	GdkTexture *texture;
	GdkCursor *fallback, *cursor;

	g_return_val_if_fail (type >= 0 && type < CURSOR_NUM_CURSORS, NULL);

	g_assert (cursors[type].data_width == cursors[type].mask_width);
	g_assert (cursors[type].data_height == cursors[type].mask_height);

	texture = cursor_texture_from_xbm (type);
	fallback = gdk_cursor_new_from_texture (texture,
                                           cursors[type].hot_x,
                                           cursors[type].hot_y,
                                           NULL);
	g_object_unref (texture);

	cursor = gdk_cursor_new_from_name (cursor_names[type], fallback);
	g_object_unref (fallback);

	if (!cursor) {
		texture = cursor_texture_from_xbm (type);
		cursor = gdk_cursor_new_from_texture (texture,
                                            cursors[type].hot_x,
                                            cursors[type].hot_y,
                                            NULL);
		g_object_unref (texture);
	}

	g_assert (cursor != NULL);

	return cursor;
}
