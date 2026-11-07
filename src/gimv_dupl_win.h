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
 */

/*
 * The window of the duplicate images: the groups found on the left (the
 * same files, the same picture in other sizes, similar), the images of the
 * group selected side by side on the right to compare, each to keep or to
 * delete, and the ones to delete moved to the trash.
 */

#ifndef __GIMV_DUPL_WIN_H__
#define __GIMV_DUPL_WIN_H__

#include "gimageview.h"

#include <gtk/gtk.h>

#include "gimv_thumb_view.h"

#define GIMV_TYPE_DUPL_WIN            (gimv_dupl_win_get_type ())
#define GIMV_DUPL_WIN(obj)            (G_TYPE_CHECK_INSTANCE_CAST (obj, gimv_dupl_win_get_type (), GimvDuplWin))
#define GIMV_IS_DUPL_WIN(obj)         (G_TYPE_CHECK_INSTANCE_TYPE (obj, gimv_dupl_win_get_type ()))

typedef struct GimvDuplWin_Tag      GimvDuplWin;
typedef struct GimvDuplWinClass_Tag GimvDuplWinClass;
typedef struct GimvDuplWinPriv_Tag  GimvDuplWinPriv;


struct GimvDuplWin_Tag
{
   GtkWindow         parent;

   GimvThumbView    *tv;     /* the images came from (NULL when it is gone) */

   GimvDuplWinPriv  *priv;
};


struct GimvDuplWinClass_Tag
{
   GtkWindowClass parent_class;
};


GType         gimv_dupl_win_get_type       (void);
GimvDuplWin  *gimv_dupl_win_new            (void);
void          gimv_dupl_win_set_relation   (GimvDuplWin   *sw,
                                            GimvThumbView *tv);
void          gimv_dupl_win_unset_relation (GimvDuplWin   *sw);
/* look for the duplicates among the images of tv (in a thread) */
void          gimv_dupl_win_start          (GimvDuplWin   *sw,
                                            GimvThumbView *tv);

#endif /* __GIMV_DUPL_WIN_H__ */
