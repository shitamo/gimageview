/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2026 shitamo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * AK5 / MSL loader: the pictures of MASL (Memorymapped Animation Scenario
 * Language, KUNI-soft/TDU 1994), an animation player for the NEC
 * PC-9801.  The format is not documented; this follows what akinf.exe of
 * the MASL data development set (maslt120) does to show them.
 *
 * .ak5: "AK5\0", 4 bytes, the user name (32 bytes), then entries:
 *   type 0 (palette):  u8 0, name[16], 16 * (R, G, B) of 0..15
 *   type 1..4 (CG):    u8 type, name[16], [source name[16] for 2, 4],
 *                      u16 x1, y1, x2, y2, u16 size, data[size]
 *   (1 normal, 2 difference, 3 and 4 the same "fast" ones; the area is
 *   informative, the data gives every position itself)
 * .msl (a scenario with its files): "MSL\0", 4 bytes, u16 n, the
 *   scenario (n bytes), then (u32 size, a .ak5 or .pkn file) ...
 *
 * The data of a CG writes to the 640x400 16 colour screen (4 planes, 80
 * bytes a line) column by column, as the GRCG did on the PC-9801: the
 * colour (a byte of each plane) stays from one write to the next.  Ops:
 *   even op:         skip ((op >> 1) << 8 | next byte) bytes
 *   op & 0x0f == 1, 5, 9, 0x0d:  op >> 2 lines of the colour as it is
 *   op & 0x0f == 3, 7, 0x0b:     lines with new bytes for the planes
 *                    of the bits of op >> 4 (B, R, G, I): 3 one line,
 *                    0x0b two lines, 7 (next byte + 2) lines
 *   0x0f end, 0x1f lines 2 apart / 1 apart (switches), 0x2f n: n / 4
 *   (2 apart: n / 5) lines of 2 bytes of the colour as it is
 * Positions are modulo 0x8000 (the VRAM window of a plane).
 *
 * In a view, an .msl is played by its scenario (masl.c); an .ak5 (no
 * scenario) shows its pictures one after another (a difference over the
 * one before), as an animation.  A thumbnail is the first picture.
 */

#include "config.h"

#include <string.h>

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_anim.h"
#include "gimv_image.h"
#include "gimv_image_loader.h"
#include "gimv_plugin.h"
#include "loader_utils.h"
#include "masl.h"

#define AK5_HEAD      40
#define AK5_NAME      16
/* not real time: the scenario of MASL times the pictures */
#define AK5_INTERVAL  250

static GimvImage *ak5_load (GimvImageLoader *loader, gpointer data);

static GimvImageLoaderPlugin gimv_ak5_loader[] =
{
   {
      if_version:    GIMV_IMAGE_LOADER_IF_VERSION,
      id:            "AK5",
      priority_hint: GIMV_IMAGE_LOADER_PRIORITY_CAN_CANCEL,
      check_type:    NULL,
      get_info:      NULL,
      loader:        ak5_load,
   }
};

static const gchar *ak5_extensions[] = { "ak5" };
static const gchar *msl_extensions[] = { "msl" };

static GimvMimeTypeEntry ak5_mime_types[] =
{
   {
      mime_type:      "image/x-ak5",
      description:    N_("The AK5 image format (MASL)"),
      extensions:     ak5_extensions,
      extensions_len: G_N_ELEMENTS (ak5_extensions),
      icon:           NULL,
   },
   {
      mime_type:      "image/x-msl",
      description:    N_("The MSL animation format (MASL)"),
      extensions:     msl_extensions,
      extensions_len: G_N_ELEMENTS (msl_extensions),
      icon:           NULL,
   },
};

GIMV_PLUGIN_GET_IMPL(gimv_ak5_loader, GIMV_PLUGIN_IMAGE_LOADER)
GIMV_PLUGIN_GET_MIME_TYPE(ak5_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("AK5 Image Loader"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   N_("AK5 and MSL pictures of MASL (PC-9801)"),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  NULL,
};


typedef struct {
   GByteArray *file;
   GArray     *entries;      /* Ak5Entry */
   guint       n_frames;     /* the CGs */
   guint       next;         /* the entry to read next */
   Ak5Planes   planes;
   guchar      color[4];
   guchar      pal[48];

   /* an .msl played by its scenario */
   const guchar *scenario;
   gsize         scenario_len;
   GArray       *pcms;       /* MaslPcm */
   GArray       *cgs;        /* Ak5Entry: the CGs alone */
   GPtrArray    *pals;       /* the palettes alone */
   MaslPlayer   *player;
   MaslSound    *sound;
   guchar       *next_rgb;   /* the picture after the one shown */
   gdouble       shown_ms, next_ms;
   gint          interval;
   gint64        start_us;   /* the time 0 (g_get_monotonic_time ()) */
} Ak5;


static inline guint
ak5_le16 (const guchar *p)
{
   return p[0] | (p[1] << 8);
}


static inline guint32
ak5_le32 (const guchar *p)
{
   return p[0] | (p[1] << 8) | (p[2] << 16) | ((guint32) p[3] << 24);
}


/* the entries of an .ak5 file (buf, len); FALSE when it is not one */
static gboolean
ak5_parse (Ak5 *ak, const guchar *buf, gsize len)
{
   gsize pos = AK5_HEAD;

   if (len < AK5_HEAD || memcmp (buf, "AK5\0", 4)) return FALSE;

   while (pos < len) {
      Ak5Entry e;
      gint type = buf[pos];

      if (type == 0) {
         if (pos + 1 + AK5_NAME + 48 > len) break;
         e.type = 0;
         e.data = buf + pos + 1 + AK5_NAME;
         e.len  = 48;
         pos += 1 + AK5_NAME + 48;
      } else if (type >= 1 && type <= 4) {
         gsize q = pos + 1 + AK5_NAME;
         if (type == 2 || type == 4) q += AK5_NAME;   /* source name */
         if (q + 10 > len) break;
         e.type = type;
         e.len  = ak5_le16 (buf + q + 8);
         e.data = buf + q + 10;
         if (q + 10 + e.len > len) break;
         pos = q + 10 + e.len;
         ak->n_frames++;
      } else {
         break;
      }
      g_array_append_val (ak->entries, e);
   }

   return TRUE;
}


/* the PCMs of a .pkn file: "PKN\0", 4 bytes, the user name (32 bytes),
 * then (u8 type (1: ADPCM of the X68000), name[16], u16 rate, u32 size,
 * data[size]) ... */
static void
pkn_parse (Ak5 *ak, const guchar *buf, gsize len)
{
   gsize pos = AK5_HEAD;

   if (len < AK5_HEAD || memcmp (buf, "PKN\0", 4)) return;
   while (pos + 23 <= len) {
      MaslPcm pcm;
      pcm.adpcm = buf[pos] == 1;
      pcm.rate  = ak5_le16 (buf + pos + 17);
      pcm.len   = ak5_le32 (buf + pos + 19);
      pcm.data  = buf + pos + 23;
      if (pcm.len > len - pos - 23) break;
      g_array_append_val (ak->pcms, pcm);
      pos += 23 + pcm.len;
   }
}


/* the scenario and the .ak5 and .pkn files of an .msl file */
static void
msl_parse (Ak5 *ak, const guchar *buf, gsize len)
{
   gsize pos;

   if (len < 10) return;
   ak->scenario_len = ak5_le16 (buf + 8);
   if (ak->scenario_len > len - 10) {
      ak->scenario_len = 0;
      return;
   }
   ak->scenario = buf + 10;
   pos = 10 + ak->scenario_len;

   while (pos + 4 <= len) {
      guint32 size = ak5_le32 (buf + pos);
      pos += 4;
      if (size > len - pos) break;
      if (size >= 4 && !memcmp (buf + pos, "AK5\0", 4))
         ak5_parse (ak, buf + pos, size);
      else if (size >= 4 && !memcmp (buf + pos, "PKN\0", 4))
         pkn_parse (ak, buf + pos, size);
      pos += size;
   }
}


/* one CG onto the screen; FALSE when the data is broken (what was
 * drawn stays) */
gboolean
ak5_decode_cg (Ak5Planes planes, guchar color[4], const guchar *p, gsize len,
               guint start)
{
   const guchar *end = p + len;
   guint di = start & 0xffff, stride = AK5_LINE;

#define NEXT(v)  G_STMT_START { if (p >= end) return FALSE; (v) = *p++; } G_STMT_END
#define PUT(o)   G_STMT_START {                                 \
      guint _o = (o) & (AK5_VRAM - 1);                          \
      planes[0][_o] = color[0];                                 \
      planes[1][_o] = color[1];                                 \
      planes[2][_o] = color[2];                                 \
      planes[3][_o] = color[3];                                 \
   } G_STMT_END

   for (;;) {
      guint op, lo, n, i, b;

      NEXT (op);

      if (!(op & 1)) {                                 /* skip */
         NEXT (b);
         di = (di + (((op >> 1) << 8) | b)) & 0xffff;
         continue;
      }

      lo = op & 0x0f;
      if (lo == 0x0f) {
         switch (op >> 4) {
         case 0:                                       /* end */
            return TRUE;
         case 1:                                       /* line spacing */
            stride = stride == AK5_LINE ? AK5_LINE * 2 : AK5_LINE;
            break;
         case 2: {                                     /* 2 bytes wide */
            guint unit = stride == AK5_LINE ? 4 : 5;   /* code bytes a line */
            NEXT (n);
            if (n % unit) return FALSE;
            for (i = 0; i < n / unit; i++) {
               PUT (di);
               PUT (di + 1);
               di = (di + stride) & 0xffff;
            }
            break;
         }
         default:
            return FALSE;
         }
         continue;
      }

      if (lo == 1 || lo == 5 || lo == 9 || lo == 0x0d) {   /* as it is */
         n = op >> 2;
         if (n == 0) return FALSE;
         for (i = 0; i < n; i++) {
            PUT (di);
            di = (di + stride) & 0xffff;
         }
         continue;
      }

      /* new bytes for some planes */
      {
         guint mask = op >> 4, rows, pl;

         if (mask == 0) return FALSE;
         if (lo == 3) {
            rows = 1;
         } else if (lo == 0x0b) {
            rows = 2;
         } else {
            NEXT (b);
            rows = (b ? b : 0x10000) + 2;   /* the loop of the 8086 */
         }
         for (i = 0; i < rows; i++) {
            for (pl = 0; pl < 4; pl++)
               if (mask & (1 << pl)) NEXT (color[pl]);
            PUT (di);
            di = (di + stride) & 0xffff;
         }
      }
   }

#undef NEXT
#undef PUT
}


/* the screen as RGB (a new buffer) */
static guchar *
ak5_render (Ak5 *ak)
{
   guchar *rgb = g_try_malloc ((gsize) AK5_WIDTH * AK5_HEIGHT * 3), *d;
   guint x, y;

   if (!rgb) return NULL;
   d = rgb;
   for (y = 0; y < AK5_HEIGHT; y++) {
      for (x = 0; x < AK5_WIDTH; x++) {
         guint o = y * AK5_LINE + (x >> 3), s = 7 - (x & 7), c;
         c = ((ak->planes[0][o] >> s) & 1)
           | (((ak->planes[1][o] >> s) & 1) << 1)
           | (((ak->planes[2][o] >> s) & 1) << 2)
           | (((ak->planes[3][o] >> s) & 1) << 3);
         *d++ = ak->pal[c * 3]     * 17;
         *d++ = ak->pal[c * 3 + 1] * 17;
         *d++ = ak->pal[c * 3 + 2] * 17;
      }
   }
   return rgb;
}


static void
ak5_reset (Ak5 *ak)
{
   guint i;

   memset (ak->planes, 0, sizeof (ak->planes));
   memset (ak->color, 0, sizeof (ak->color));
   /* the digital 8 colours of the PC-9801 until a palette comes */
   for (i = 0; i < 16; i++) {
      ak->pal[i * 3]     = (i & 2) ? 15 : 0;
      ak->pal[i * 3 + 1] = (i & 4) ? 15 : 0;
      ak->pal[i * 3 + 2] = (i & 1) ? 15 : 0;
   }
   ak->next = 0;
}


/* on to the next CG (the palettes before it set); FALSE at the end */
static gboolean
ak5_step (Ak5 *ak)
{
   while (ak->next < ak->entries->len) {
      Ak5Entry *e = &g_array_index (ak->entries, Ak5Entry, ak->next++);
      if (e->type == 0) {
         guint i;
         for (i = 0; i < 48; i++) ak->pal[i] = e->data[i] & 0x0f;
         continue;
      }
      ak5_decode_cg (ak->planes, ak->color, e->data, e->len, 0);
      return TRUE;
   }
   return FALSE;
}


static void
ak5_free (Ak5 *ak)
{
   if (!ak) return;
   masl_sound_free (ak->sound);
   masl_player_free (ak->player);
   g_free (ak->next_rgb);
   if (ak->pcms) g_array_free (ak->pcms, TRUE);
   if (ak->cgs) g_array_free (ak->cgs, TRUE);
   if (ak->pals) g_ptr_array_free (ak->pals, TRUE);
   if (ak->entries) g_array_free (ak->entries, TRUE);
   if (ak->file) g_byte_array_free (ak->file, TRUE);
   g_free (ak);
}


/******************************************************************************
 *  GimvAnim methods: the CGs of the file one after another
 ******************************************************************************/

static gint
ak5_anim_get_length (GimvAnim *anim)
{
   Ak5 *ak = anim->anim;
   return ak ? (gint) ak->n_frames : 0;
}


static gint
ak5_anim_get_idx (GimvAnim *anim)
{
   return anim->current_frame_idx;
}


static gint
ak5_anim_get_interval (GimvAnim *anim)
{
   Ak5 *ak = anim->anim;
   return ak && ak->n_frames > 1 ? AK5_INTERVAL : -1;
}


static gboolean
ak5_anim_show (GimvAnim *anim)
{
   guchar *rgb = ak5_render (anim->anim);
   if (!rgb) return FALSE;
   return gimv_anim_update_frame (anim, rgb, AK5_WIDTH, AK5_HEIGHT, FALSE);
}


static gint
ak5_anim_iterate (GimvAnim *anim)
{
   Ak5 *ak = anim->anim;

   if (!ak || !ak5_step (ak)) return -1;
   anim->current_frame_idx++;
   if (!ak5_anim_show (anim)) return -1;
   return anim->current_frame_idx;
}


static gboolean
ak5_anim_seek (GimvAnim *anim, gint idx)
{
   Ak5 *ak = anim->anim;
   gint i;

   if (!ak || idx < 0 || idx >= (gint) ak->n_frames) return FALSE;

   /* the differences need the pictures before them */
   ak5_reset (ak);
   for (i = 0; i <= idx; i++)
      if (!ak5_step (ak)) return FALSE;
   anim->current_frame_idx = idx;
   return ak5_anim_show (anim);
}


static void
ak5_anim_delete (GimvAnim *anim)
{
   ak5_free (anim->anim);
   anim->anim = NULL;
}


static GimvAnimFuncTable ak5_anim_table = {
   get_length   : ak5_anim_get_length,
   get_idx      : ak5_anim_get_idx,
   get_interval : ak5_anim_get_interval,
   iterate      : ak5_anim_iterate,
   seek         : ak5_anim_seek,
   delete       : ak5_anim_delete,
};


/******************************************************************************
 *  GimvAnim methods: an .msl played by its scenario (masl.c)
 *
 *  The player gives a picture and its time; the one after is read ahead,
 *  for the time the shown one stays.
 ******************************************************************************/

#define MASL_LAST_HOLD   2000     /* ms the last picture stays */
#define MASL_FRAME_SIZE  ((gsize) AK5_WIDTH * AK5_HEIGHT * 3)

/* reads the picture after the one shown (now at "shown_ms") */
static void
masl_read_ahead (Ak5 *ak)
{
   gboolean got;

   if (!ak->next_rgb) ak->next_rgb = g_malloc (MASL_FRAME_SIZE);
   /* pictures of the same time: the last of them */
   do {
      got = masl_player_next (ak->player, ak->next_rgb, &ak->next_ms);
   } while (got && ak->next_ms - ak->shown_ms < 1.0);
   if (got) masl_sound_set_time (ak->sound, ak->next_ms);
   if (got) {
      ak->interval = (gint) (ak->next_ms - ak->shown_ms + 0.5);
   } else {
      g_clear_pointer (&ak->next_rgb, g_free);
      ak->interval = MASL_LAST_HOLD;
   }
}


static gint
masl_anim_get_length (GimvAnim *anim)
{
   return 0;     /* not known before it is played */
}


/* up to the time of the next picture from the start, not from now: the
 * work of showing a picture (and the lateness of the timer) is not added
 * up, the pictures keep to the clock of the sound */
static gint
masl_anim_get_interval (GimvAnim *anim)
{
   Ak5 *ak = anim->anim;
   gint64 left;

   if (!ak) return -1;
   if (!ak->next_rgb) return ak->interval;          /* the last one stays */
   left = ak->start_us + (gint64) (ak->next_ms * 1000) - g_get_monotonic_time ();
   return (gint) CLAMP (left / 1000, 1, G_MAXINT);
}


/* the read ahead picture shown, the next one read */
static gint
masl_anim_iterate (GimvAnim *anim)
{
   Ak5 *ak = anim->anim;
   guchar *rgb;

   if (!ak || !ak->next_rgb) return -1;     /* ended: from the start again */
   rgb = ak->next_rgb;
   ak->next_rgb = NULL;
   ak->shown_ms = ak->next_ms;
   if (!gimv_anim_update_frame (anim, rgb, AK5_WIDTH, AK5_HEIGHT, FALSE)) return -1;
   masl_read_ahead (ak);
   return ++anim->current_frame_idx;
}


static gboolean
masl_anim_start (GimvAnim *anim)
{
   Ak5 *ak = anim->anim;
   guchar *rgb = g_malloc (MASL_FRAME_SIZE);

   masl_player_reset (ak->player);
   masl_sound_start (ak->sound);     /* before the player tells of PCMs */
   ak->start_us = g_get_monotonic_time ();
   if (!masl_player_next (ak->player, rgb, &ak->shown_ms)
       || !gimv_anim_update_frame (anim, rgb, AK5_WIDTH, AK5_HEIGHT, FALSE))
   {
      g_free (rgb);
      return FALSE;
   }
   anim->current_frame_idx = 0;
   masl_read_ahead (ak);
   return TRUE;
}


static gboolean
masl_anim_seek (GimvAnim *anim, gint idx)
{
   /* only to the start (the scenario is played on, not stepped back) */
   if (!anim->anim || idx != 0) return FALSE;
   return masl_anim_start (anim);
}


static GimvAnimFuncTable masl_anim_table = {
   get_length   : masl_anim_get_length,
   get_idx      : ak5_anim_get_idx,
   get_interval : masl_anim_get_interval,
   iterate      : masl_anim_iterate,
   seek         : masl_anim_seek,
   delete       : ak5_anim_delete,
};


static GimvImage *
ak5_load (GimvImageLoader *loader, gpointer data)
{
   guchar head[4];
   Ak5 *ak;
   guchar *rgb;

   g_return_val_if_fail (loader, NULL);

   if (gimv_loader_peek (loader, head, 4) < 4) return NULL;
   if (memcmp (head, "AK5\0", 4) && memcmp (head, "MSL\0", 4)) return NULL;

   ak = g_new0 (Ak5, 1);
   ak->entries = g_array_new (FALSE, FALSE, sizeof (Ak5Entry));
   ak->pcms = g_array_new (FALSE, FALSE, sizeof (MaslPcm));
   ak->file = gimv_loader_read_all (loader);
   if (!ak->file) goto ERROR;

   if (head[0] == 'A')
      ak5_parse (ak, ak->file->data, ak->file->len);
   else
      msl_parse (ak, ak->file->data, ak->file->len);
   if (ak->n_frames == 0) goto ERROR;

   /* an .msl in a view: played by its scenario (not a MASL scenario:
    * the CGs one after another) */
   if (ak->scenario && gimv_image_loader_load_as_animation (loader)) {
      guint i;

      /* the scenario counts the CGs and the palettes apart */
      ak->cgs  = g_array_new (FALSE, FALSE, sizeof (Ak5Entry));
      ak->pals = g_ptr_array_new ();
      for (i = 0; i < ak->entries->len; i++) {
         Ak5Entry *e = &g_array_index (ak->entries, Ak5Entry, i);
         if (e->type == 0) g_ptr_array_add (ak->pals, (gpointer) e->data);
         else g_array_append_val (ak->cgs, *e);
      }
      ak->player = masl_player_new (ak->scenario, ak->scenario_len,
                                    (const Ak5Entry *) ak->cgs->data, ak->cgs->len,
                                    (const guchar **) ak->pals->pdata, ak->pals->len,
                                    (const MaslPcm *) ak->pcms->data, ak->pcms->len);
      if (ak->player) {
         GimvAnim *anim = gimv_anim_new ();
         ak->sound = masl_sound_new ((const MaslPcm *) ak->pcms->data, ak->pcms->len);
         if (ak->sound)
            masl_player_set_sound_func (ak->player, masl_sound_event, ak->sound);
         anim->anim  = ak;
         anim->table = &masl_anim_table;
         if (!masl_anim_start (anim)) {
            gimv_image_unref ((GimvImage *) anim);
            return NULL;
         }
         return (GimvImage *) anim;
      }
   }

   ak5_reset (ak);
   ak5_step (ak);

   /* more pictures and shown in a view (not a thumbnail): an animation */
   if (ak->n_frames > 1 && gimv_image_loader_load_as_animation (loader)) {
      GimvAnim *anim = gimv_anim_new ();
      anim->anim  = ak;
      anim->table = &ak5_anim_table;
      anim->current_frame_idx = 0;
      if (!ak5_anim_show (anim)) {
         gimv_image_unref ((GimvImage *) anim);
         return NULL;
      }
      return (GimvImage *) anim;
   }

   rgb = ak5_render (ak);
   ak5_free (ak);
   if (!rgb) return NULL;
   return gimv_image_create_from_data (rgb, AK5_WIDTH, AK5_HEIGHT, FALSE);

ERROR:
   ak5_free (ak);
   return NULL;
}
