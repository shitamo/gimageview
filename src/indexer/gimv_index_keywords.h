/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: the keywords other programs wrote into an image
 * (XMP dc:subject, IPTC Keywords) or next to it (an XMP sidecar file).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_INDEX_KEYWORDS_H__
#define __GIMV_INDEX_KEYWORDS_H__

#include <glib.h>

/* the longest keyword taken (bytes); longer ones are left out */
#define GIMV_INDEX_KEYWORD_MAX 200

/*
 * The keywords of the image at path: from the XMP in the file (JPEG, PNG,
 * WebP, TIFF and the raw files built on TIFF), the IPTC Keywords of a JPEG,
 * and the sidecar files "<path>.xmp" (digiKam, darktable) and
 * "<path without its extension>.xmp" (Lightroom, ...).  Each once, in the
 * order found; NULL when there are none.  Only the header part of the
 * image is read.
 */
GPtrArray *gimv_index_keywords_read      (const gchar *path);

/* the keywords in the image as Exiv2 (gexiv2) read them, for
 * gimv_index_keywords_read_with (): NULL-terminated, any may be NULL */
typedef struct {
   gchar    **flat;        /* Xmp.dc.subject, Xmp.MicrosoftPhoto.LastKeywordXMP */
   gchar    **iptc;        /* Iptc.Application2.Keywords as written */
   gboolean   iptc_utf8;   /* Iptc.Envelope.CharacterSet says UTF-8 */
   gchar    **hier_bar;    /* Xmp.lr.hierarchicalSubject ("a|b|c") */
   gchar    **hier_slash;  /* Xmp.digiKam.TagsList ("a/b/c") */
} GimvIndexEmbedded;

/* as gimv_index_keywords_read (), with the keywords in the image given
 * (em; NULL: read here from the file) and the sidecars read here */
GPtrArray *gimv_index_keywords_read_with (const gchar             *path,
                                          const GimvIndexEmbedded *em);

/* the keywords of an XMP packet (for the tests): appended to out */
void       gimv_index_keywords_from_xmp  (const gchar *xmp,
                                          gssize       len,
                                          GPtrArray   *out);

/* a file name that may be a sidecar ("x.jpg.xmp", "x.xmp", case ignored) */
gboolean   gimv_index_is_sidecar_name    (const gchar *name);

#endif /* __GIMV_INDEX_KEYWORDS_H__ */
