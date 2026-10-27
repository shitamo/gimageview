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

#endif /* __GIMV_INDEX_VIEW_H__ */
