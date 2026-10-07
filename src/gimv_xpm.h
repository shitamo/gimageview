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

/*
 * XPM (XPM3) decoder.  gdk-pixbuf builds its XPM loader only as an optional
 * "weakly maintained" loader, and gdk_pixbuf_new_from_xpm_data () needs it,
 * so the built-in icons and the XPM image loader plugin use this instead.
 */

#ifndef __GIMV_XPM_H__
#define __GIMV_XPM_H__

#include <gdk-pixbuf/gdk-pixbuf.h>

/* the C string literals of an XPM file, in order (comments skipped) */
GPtrArray *gimv_xpm_get_strings            (const gchar         *text,
                                            gsize                len);

/* decode lines[0 .. n_lines - 1] (header, colours, pixels); returns packed
   RGB or RGBA (has_alpha) data to free with g_free (), or NULL */
guchar    *gimv_xpm_decode                 (const gchar * const *lines,
                                            guint                n_lines,
                                            gint                *width,
                                            gint                *height,
                                            gboolean            *has_alpha);

/* replacements for gdk_pixbuf_new_from_xpm_data () and loading *.xpm */
GdkPixbuf *gimv_xpm_pixbuf_new_from_data   (const gchar * const *data);
GdkPixbuf *gimv_xpm_pixbuf_new_from_file   (const gchar         *filename);

#endif /* __GIMV_XPM_H__ */
