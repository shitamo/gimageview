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
 * $Id: gimv_paned.c,v 1.2 2003/06/13 09:43:32 makeinu Exp $
 */

/*
 * These codes are taken from gThumb.
 * gThumb code Copyright (C) 2001 The Free Software Foundation, Inc.
 * gThumb author: Paolo Bacchilega
 */

#include "gimv_paned.h"


guint
gimv_paned_which_hidden (GimvPaned *paned)
{
   g_return_val_if_fail (GIMV_IS_PANED (paned), 0);
   g_return_val_if_fail (paned->child1 && paned->child2, 0);

   if (!gtk_widget_get_visible (GTK_WIDGET (paned->child1))
       && gtk_widget_get_visible (GTK_WIDGET (paned->child2)))
   {
      return 1;

   } else if (gtk_widget_get_visible (GTK_WIDGET (paned->child1))
              && !gtk_widget_get_visible (GTK_WIDGET (paned->child2)))
   {
      return 2;

   } else {
      return 0;
   }
}

