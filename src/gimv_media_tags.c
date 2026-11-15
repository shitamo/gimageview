/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: the tags of a song or a movie (see gimv_media_tags.h).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimv_media_tags.h"

static GimvMediaTagsReader media_tags_reader = NULL;


void
gimv_media_tags_set_reader (GimvMediaTagsReader reader)
{
   media_tags_reader = reader;
}


gboolean
gimv_media_tags_read (const gchar *path, GCancellable *cancellable,
                      GimvMediaTagsDone done, gpointer data)
{
   g_return_val_if_fail (path && done, FALSE);

   if (!media_tags_reader) return FALSE;
   media_tags_reader (path, cancellable, done, data);
   return TRUE;
}
