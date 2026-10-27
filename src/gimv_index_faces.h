/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: runs gimv-faces (faces in the images of the
 * folders chosen for it).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_INDEX_FACES_H__
#define __GIMV_INDEX_FACES_H__

#include <glib.h>

typedef enum {
   GIMV_INDEX_FACES_OFF,
   GIMV_INDEX_FACES_RUNNING,
   GIMV_INDEX_FACES_DONE,
   GIMV_INDEX_FACES_NO_MODELS,
   GIMV_INDEX_FACES_ERROR,
} GimvIndexFacesState;

/* built with OpenCV */
gboolean            gimv_index_faces_available      (void);
/* the index configured and folders given for the faces */
gboolean            gimv_index_faces_is_configured  (void);
/* ~/.local/share/gimageview/models */
gchar              *gimv_index_faces_user_model_dir (void);
/* the folder with both model files, NULL when there is none */
gchar              *gimv_index_faces_model_dir      (void);

GimvIndexFacesState gimv_index_faces_get_state      (void);
gchar              *gimv_index_faces_status         (void);
gchar              *gimv_index_faces_tooltip        (void);

void                gimv_index_faces_kick           (void);
void                gimv_index_faces_restart        (void);
void                gimv_index_faces_regroup        (void);
void                gimv_index_faces_quit           (void);

/* what a person says about the faces (ids: gint64 face ids); the groups
 * are made again a little later, or at gimv_index_faces_flush () */
gboolean            gimv_index_faces_set_name       (GArray      *ids,
                                                     const gchar *name);
gboolean            gimv_index_faces_confirm_guess  (GArray      *ids);
gboolean            gimv_index_faces_clear_name     (GArray      *ids);
gboolean            gimv_index_faces_reject         (GArray      *ids,
                                                     gint64       person_id);
gboolean            gimv_index_faces_set_ignored    (GArray      *ids,
                                                     gboolean     ignored);
GPtrArray          *gimv_index_faces_get_names      (void);
void                gimv_index_faces_flush          (void);

/* the faces of a row of the "People" tab ("p:3", "c:12", "u", ...) as an
 * SQL condition on the table faces named "fa" */
gchar              *gimv_index_faces_key_where      (const gchar *key);

#endif /* __GIMV_INDEX_FACES_H__ */
