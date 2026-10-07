/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001 Takuro Ashie
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
 *  GTK4 port: light / dark color scheme of the user interface
 *  (conf.color_scheme).
 */

#ifndef __GIMV_COLOR_SCHEME_H__
#define __GIMV_COLOR_SCHEME_H__

#include <glib.h>

typedef enum {
   GIMV_COLOR_SCHEME_DESKTOP,   /* follow the desktop theme */
   GIMV_COLOR_SCHEME_LIGHT,
   GIMV_COLOR_SCHEME_DARK
} GimvColorScheme;

/* apply conf.color_scheme; call after gtk_init () and after it changed */
void gimv_color_scheme_apply (void);

#endif /* __GIMV_COLOR_SCHEME_H__ */
