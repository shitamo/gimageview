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
 *
 * $Id: gtk2-compat.h,v 1.7 2003/06/13 09:43:34 makeinu Exp $
 */

#ifndef __GTK2_COMPAT_H__
#define __GTK2_COMPAT_H__

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <glib.h>
#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_object.h"


#  define gtk_object_class_add_signals(class, func, type)
#
#  define OBJECT_CLASS_SET_FINALIZE_FUNC(klass, func) \
      G_OBJECT_CLASS(klass)->finalize = func
#
#  define OBJECT_CLASS_FINALIZE_SUPER(parent_class, object) \
      if (G_OBJECT_CLASS(parent_class)->finalize) \
         G_OBJECT_CLASS(parent_class)->finalize (object);


#endif /* __GTK2_COMPAT_H__ */
