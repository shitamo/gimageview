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
 *
 * $Id: gimv_paned.h,v 1.2 2003/06/13 09:43:32 makeinu Exp $
 */

/*
 * These codes are taken from gThumb.
 * gThumb code Copyright (C) 2001 The Free Software Foundation, Inc.
 * gThumb author: Paolo Bacchilega
 */

#ifndef __GIMV_PANED_H__
#define __GIMV_PANED_H__

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"

G_BEGIN_DECLS

/*
 *  GTK4: the own GimvPaned implementation (gimv_paned.c, gimv_hpaned.c,
 *  gimv_vpaned.c) is not used any more (USE_NORMAL_PANED), GimvPaned is a
 *  plain GtkPaned.  The helpers below are implemented with GtkPaned API.
 */

#define GimvPaned GtkPaned
#define GIMV_TYPE_PANED                      GTK_TYPE_PANED
#define GIMV_PANED(paned)                    GTK_PANED(paned)
#define GIMV_IS_PANED(obj)                   GTK_IS_PANED(obj)
#define gimv_paned_get_child1(paned)         gtk_paned_get_start_child (GTK_PANED (paned))
#define gimv_paned_get_child2(paned)         gtk_paned_get_end_child (GTK_PANED (paned))
#define gimv_paned_add1(paned, widget)       gimv_paned_pack1 (GTK_PANED (paned), widget, FALSE, TRUE)
#define gimv_paned_add2(paned, widget)       gimv_paned_pack2 (GTK_PANED (paned), widget, TRUE, TRUE)
#define gimv_paned_set_position(paned, size) gtk_paned_set_position (GTK_PANED (paned), size)
#define gimv_paned_get_position(paned)       gtk_paned_get_position (GTK_PANED (paned))

static inline void
gimv_paned_split (GtkPaned *paned)
{
   GtkWidget *child1 = gtk_paned_get_start_child (paned);
   GtkWidget *child2 = gtk_paned_get_end_child (paned);

   if (child1) gtk_widget_set_visible (child1, TRUE);
   if (child2) gtk_widget_set_visible (child2, TRUE);
}

static inline void
gimv_paned_hide_child1 (GtkPaned *paned)
{
   GtkWidget *child = gtk_paned_get_start_child (paned);
   if (child) gtk_widget_set_visible (child, FALSE);
}

static inline void
gimv_paned_hide_child2 (GtkPaned *paned)
{
   GtkWidget *child = gtk_paned_get_end_child (paned);
   if (child) gtk_widget_set_visible (child, FALSE);
}

/* 0: both visible (or hidden), 1: child1 is hidden, 2: child2 is hidden */
static inline guint
gimv_paned_which_hidden (GtkPaned *paned)
{
   GtkWidget *child1, *child2;

   g_return_val_if_fail (GTK_IS_PANED (paned), 0);

   child1 = gtk_paned_get_start_child (paned);
   child2 = gtk_paned_get_end_child (paned);
   g_return_val_if_fail (child1 && child2, 0);

   if (!gtk_widget_get_visible (child1) && gtk_widget_get_visible (child2))
      return 1;
   else if (gtk_widget_get_visible (child1) && !gtk_widget_get_visible (child2))
      return 2;
   else
      return 0;
}

G_END_DECLS

#endif /* __GIMV_PANED_H__ */
