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
   GIMV_INDEX_TAB_BOOKMARK,   /* not of the index: any image (gimv_bookmark.c) */
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
/*
 * The keywords of other programs (the rule tags "keyword/<name>") are kept
 * apart in the index, but shown and looked for as the tags put by a
 * person: a key "Kyoto" is the tag Kyoto and the keyword Kyoto.
 * GIMV_INDEX_TAG_FILES_SQL: the file ids of such a key (the name bound to
 * both '?').  A key ",keyword/Kyoto" (made before) is "Kyoto".
 */
#define GIMV_INDEX_TAG_FILES_SQL \
   "SELECT ft.file_id FROM file_tags ft JOIN tags t ON t.id = ft.tag_id " \
   "WHERE (t.name = ? AND ft.auto = 0) OR (t.name = 'keyword/' || ? AND ft.auto = 1)"
/* the tags of file_tags ft, t as one name n: the keywords as plain names */
#define GIMV_INDEX_TAG_NAME_SQL \
   "CASE WHEN ft.auto = 1 AND t.name >= 'keyword/' AND t.name < 'keyword0' " \
   "THEN substr (t.name, 9) ELSE t.name END"
#define GIMV_INDEX_TAG_AUTO_SQL \
   "(ft.auto = 1 AND NOT (t.name >= 'keyword/' AND t.name < 'keyword0'))"
const gchar *gimv_index_plain_key     (const gchar         *key);

/* the images of the tag in the result tab, as a click on its row in the
 * tag tab (opened when it is not) */
void       gimv_index_open_tag        (GimvThumbWin        *tw,
                                       const gchar         *key);

#endif /* __GIMV_INDEX_VIEW_H__ */
