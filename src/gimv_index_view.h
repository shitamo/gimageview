/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: the "Date" and "Tag" tabs of the thumbnail window.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_INDEX_VIEW_H__
#define __GIMV_INDEX_VIEW_H__

#include "gimv_thumb_win.h"

typedef enum {
   GIMV_INDEX_TAB_DATE,
   GIMV_INDEX_TAB_TAG,
   GIMV_INDEX_TAB_PERSON,
} GimvIndexTabType;

/* open / close a tab (nothing happens when it is already so) */
void       gimv_index_tab_open        (GimvThumbWin     *tw,
                                       GimvIndexTabType  type);
void       gimv_index_tab_close       (GimvThumbWin     *tw,
                                       GimvIndexTabType  type);
/* the page widget of the tab, NULL when it is not open */
GtkWidget *gimv_index_tab_get_page    (GimvThumbWin     *tw,
                                       GimvIndexTabType  type);
/* open or close the tabs as the preferences say */
void       gimv_index_tabs_sync       (GimvThumbWin     *tw);

/* the tags of one image, from the index (in a worker thread): keys as in
 * the tag tab, in the order to show them: the tags put by a person, the
 * names of the people in it ("person/<name>"), the tags of these people
 * (",person/<tag>"), the tags of the rules (",folder/...", ...).
 * NULL (no error) for an image not in the index. */
void       gimv_index_get_tags_async  (const gchar         *path,
                                       GCancellable        *cancel,
                                       GAsyncReadyCallback  callback,
                                       gpointer             data);
GPtrArray *gimv_index_get_tags_finish (GAsyncResult        *res);
/* how a key is shown ("Kyoto", "Folder: 2019", "Eileen (Name)", ...) */
gchar     *gimv_index_tag_label       (const gchar         *key);
/* the kind of a key: 0 put by a person, 1 a name, 2 a tag of people,
 * 3 put by a rule */
gint       gimv_index_tag_kind        (const gchar         *key);
/* the images of the tag in the result tab, as a click on its row in the
 * tag tab (opened when it is not) */
void       gimv_index_open_tag        (GimvThumbWin        *tw,
                                       const gchar         *key);

#endif /* __GIMV_INDEX_VIEW_H__ */
