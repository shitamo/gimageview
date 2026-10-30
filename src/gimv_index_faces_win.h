/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: the faces window (the faces of a person or of a
 * group, to give names to).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_INDEX_FACES_WIN_H__
#define __GIMV_INDEX_FACES_WIN_H__

#include "gimv_thumb_win.h"

/* show the faces of a row of the "People" tab (gimv_index_faces_key_where) */
void     gimv_index_faces_win_show    (GimvThumbWin *tw,
                                       const gchar  *key,
                                       const gchar  *title);
gboolean gimv_index_faces_win_is_open (void);

/* asks for a name (a new one or one given before) */
typedef void (*GimvIndexFacesNameFunc) (const gchar *name, gpointer data);
void     gimv_index_faces_ask_name    (GtkWindow              *parent,
                                       GimvIndexFacesNameFunc  done,
                                       gpointer                data);

/* asks before "not this person" (n faces, of name) */
typedef void (*GimvIndexFacesConfirmFunc) (gpointer data);
void     gimv_index_faces_confirm_reject (GtkWindow                *parent,
                                          const gchar              *name,
                                          guint                     n,
                                          GimvIndexFacesConfirmFunc done,
                                          gpointer                  data);

/* asks for the tags of a person; done gets them one a line ("" for none) */
void     gimv_index_faces_ask_person_tags (GtkWindow              *parent,
                                           const gchar            *person,
                                           gint64                  person_id,
                                           GimvIndexFacesNameFunc  done,
                                           gpointer                data);

/* the small image of a face, NULL when there is none */
GdkTexture *gimv_index_faces_get_texture (gint64 face_id);

#endif /* __GIMV_INDEX_FACES_WIN_H__ */
