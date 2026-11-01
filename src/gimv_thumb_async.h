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
 * Thumbnails made from the image files in worker threads: the thumbnail
 * embedded in a JPEG (EXIF), a JPEG read at 1/2 .. 1/8, the other formats
 * of GdkPixbuf read at the size.  The rest (the formats of the plugins
 * only, images in archives, movies) are made the usual way.
 */

#ifndef __GIMV_THUMB_ASYNC_H__
#define __GIMV_THUMB_ASYNC_H__

#include "gimageview.h"
#include "gimv_thumb.h"

/* done in the main thread: made (the thumbnail set), or not (made the
 * usual way by the caller if it still wants it) */
typedef void (*GimvThumbAsyncFunc) (GimvThumb *thumb, gboolean made, gboolean cancelled,
                                    gpointer view);

/* the image can be read in a worker thread */
gboolean gimv_thumb_async_can       (GimvThumb          *thumb);
/* started (TRUE; also when it was waiting, cancelled: wanted again), done
 * later with func (view: a GObject, held weakly; the func is not called
 * when it has gone) */
gboolean gimv_thumb_async_load      (GimvThumb          *thumb,
                                     gint                thumb_size,
                                     ThumbLoadType       type,
                                     GimvThumbAsyncFunc  func,
                                     gpointer            view);
gboolean gimv_thumb_async_pending   (GimvThumb          *thumb);
/* not wanted any more (far from the view): skipped if not begun */
void     gimv_thumb_async_cancel    (GimvThumb          *thumb);
/* the thumbnails waiting or being made */
guint    gimv_thumb_async_n_pending (void);

#endif /* __GIMV_THUMB_ASYNC_H__ */
