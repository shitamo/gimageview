/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: the tags of a song or a movie (artist, album, length, ...),
 * read by a plugin (GStreamer) away from the main thread.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_MEDIA_TAGS_H__
#define __GIMV_MEDIA_TAGS_H__

#include <gio/gio.h>

/* pairs: name (shown, translated), value, name, value, ... (freed by the
   caller); NULL when nothing could be read.  Called in the main thread,
   also when cancelled (the caller checks its cancellable). */
typedef void (*GimvMediaTagsDone)   (GPtrArray    *pairs,
                                     gpointer      data);
typedef void (*GimvMediaTagsReader) (const gchar  *path,
                                     GCancellable *cancellable,
                                     GimvMediaTagsDone done,
                                     gpointer      data);

/* a plugin that can read them */
void     gimv_media_tags_set_reader (GimvMediaTagsReader reader);

/* FALSE (and done is not called) when no plugin reads them */
gboolean gimv_media_tags_read       (const gchar  *path,
                                     GCancellable *cancellable,
                                     GimvMediaTagsDone done,
                                     gpointer      data);

#endif /* __GIMV_MEDIA_TAGS_H__ */
