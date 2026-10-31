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
#include <gio/gio.h>

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

/* what a person says about the faces (ids: gint64 face ids), written in
 * a worker thread in one transaction; the tabs are told and the groups
 * made again a little later (or at gimv_index_faces_flush ()) */
typedef enum {
   GIMV_INDEX_FACES_SET_NAME,     /* name: the person's name */
   GIMV_INDEX_FACES_CONFIRM,      /* the guess is right */
   GIMV_INDEX_FACES_CLEAR_NAME,
   GIMV_INDEX_FACES_REJECT,       /* person: not this one */
   GIMV_INDEX_FACES_IGNORE,
   GIMV_INDEX_FACES_UNIGNORE,
   GIMV_INDEX_FACES_RENAME,       /* person: to "name"; when another person
                                   * has that name, the two become one */
   GIMV_INDEX_FACES_PERSON_TAGS,  /* person: the tags are "name" (one a line) */
} GimvIndexFacesEdit;

void                gimv_index_faces_edit_async     (GimvIndexFacesEdit  edit,
                                                     GArray             *ids,
                                                     const gchar        *name,
                                                     gint64              person,
                                                     GAsyncReadyCallback callback,
                                                     gpointer            data);
/* person: the person after RENAME (the other one when merged), may be NULL */
gboolean            gimv_index_faces_edit_finish    (GAsyncResult       *res,
                                                     gint64             *person,
                                                     GError            **error);
/* the tags of a person (sorted), and all the tags people have */
GPtrArray          *gimv_index_faces_get_person_tags (gint64       person);
GPtrArray          *gimv_index_faces_get_all_person_tags (void);

/* the id of the person of that name, 0 when there is none */
gint64              gimv_index_faces_find_person    (const gchar        *name);
GPtrArray          *gimv_index_faces_get_names      (void);
void                gimv_index_faces_flush          (void);
/* the indexer gave names back to faces (from the comment files) */
void                gimv_index_faces_named_by_indexer (void);

/* the faces of a row of the "People" tab ("p:3", "c:12", "u", ...) as an
 * SQL condition on the table faces named "fa" */
gchar              *gimv_index_faces_key_where      (const gchar *key);

#endif /* __GIMV_INDEX_FACES_H__ */
