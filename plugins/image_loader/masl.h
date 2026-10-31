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
 * MASL (PC-9801 animations): the pictures of .ak5 files (ak5.c) and the
 * player of the scenarios of .msl files (masl.c).
 */

#ifndef __GIMV_MASL_H__
#define __GIMV_MASL_H__

#include <glib.h>

#define AK5_WIDTH     640
#define AK5_HEIGHT    400
#define AK5_LINE      80
#define AK5_VRAM      0x8000        /* bytes of a plane (the window) */

typedef guchar Ak5Planes[4][AK5_VRAM];

typedef struct {
   gint          type;       /* 0 palette, 1..4 CG */
   const guchar *data;       /* the palette (48 bytes, R G B of 0..15) or the CG */
   gsize         len;
} Ak5Entry;

typedef struct {
   gint          adpcm;      /* the data are ADPCM (2 samples a byte) */
   guint         rate;
   gsize         len;
} MaslPcm;

/* a CG onto the planes from the byte "start" on (colour: the 4 bytes that
 * stay from one write to the next); FALSE when the data is broken */
gboolean     ak5_decode_cg      (Ak5Planes      planes,
                                 guchar         color[4],
                                 const guchar  *data,
                                 gsize          len,
                                 guint          start);

typedef struct MaslPlayer_Tag MaslPlayer;

/* the scenario (the text of the .mas part) with the CGs, palettes and
 * PCMs of the files it names; NULL when it is not a MASL scenario */
MaslPlayer  *masl_player_new    (const guchar   *scenario,
                                 gsize           len,
                                 const Ak5Entry *cgs,
                                 guint           n_cgs,
                                 const guchar  **palettes,
                                 guint           n_palettes,
                                 const MaslPcm  *pcms,
                                 guint           n_pcms);
void         masl_player_free   (MaslPlayer     *player);
/* from the start again */
void         masl_player_reset  (MaslPlayer     *player);
/* plays on to the next picture shown: rgb (640 x 400 x 3) and its time
 * (ms from the start); FALSE when the scenario has ended (nothing set) */
gboolean     masl_player_next   (MaslPlayer     *player,
                                 guchar         *rgb,
                                 gdouble        *time_ms);

#endif /* __GIMV_MASL_H__ */
