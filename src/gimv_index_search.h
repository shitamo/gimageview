/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2026 shitamo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * The search window: images of the index by several tags at once (all of
 * them, and none of those excluded) and a period; the tags of the images
 * found, with their numbers, to narrow it further.  Searches can be kept
 * (bookmarks).
 */

#ifndef __GIMV_INDEX_SEARCH_H__
#define __GIMV_INDEX_SEARCH_H__

#include "gimv_thumb_win.h"

/* the results go to a tab of tw */
void gimv_index_search_open (GimvThumbWin *tw);

#endif /* __GIMV_INDEX_SEARCH_H__ */
