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
 * The -lh1- method of LHarc 1.x, which libarchive does not read: many
 * old LZH archives of the PC-9801 era use it.  GLib only (for the tests).
 */

#ifndef __GIMV_LHA_LH1_H__
#define __GIMV_LHA_LH1_H__

#include <glib.h>

typedef struct {
   gchar   method[6];     /* "-lh1-", NUL terminated */
   gsize   data;          /* where the packed data starts in the file */
   guint64 packed;        /* its size */
   guint64 original;      /* the size unpacked */
   guint16 crc;           /* CRC-16 of the unpacked data */
   gboolean has_crc;
} GimvLhaMember;

/* the member whose header is at offset in buf (levels 0, 1, 2) */
gboolean  gimv_lha_member  (const guchar  *buf,
                            gsize          len,
                            gsize          offset,
                            GimvLhaMember *m);

/* the CRC-16 of LHA */
guint16   gimv_lha_crc16   (const guchar  *buf,
                            gsize          len);

/* in (in_len bytes, -lh1-) unpacked to out (out_len bytes); FALSE if in
   ends first */
gboolean  gimv_lh1_decode  (const guchar  *in,
                            gsize          in_len,
                            guchar        *out,
                            gsize          out_len);

#ifdef GIMV_LH1_ENCODER
/* -lh1- packed data of in (for the tests); g_free () it */
guchar   *gimv_lh1_encode  (const guchar  *in,
                            gsize          in_len,
                            gsize         *out_len);
#endif

#endif /* __GIMV_LHA_LH1_H__ */
