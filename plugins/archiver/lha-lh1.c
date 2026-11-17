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
 * -lh1-: LZSS with a 4 KB window (the window starts filled with spaces)
 * and adaptive Huffman codes (Okumura's LZHUF, as in LHarc 1.x):
 *
 *   a symbol of 314: 0..255 a byte, 256..313 a copy of 3..60 bytes;
 *   coded by a Huffman tree updated after each symbol
 *   for a copy, how far back (0..4095, from the byte before): the upper 6
 *   bits by a fixed code of 3..8 bits, the lower 6 bits as they are
 *
 * The bits are read from the highest.
 */

#include <string.h>

#include "lha-lh1.h"

#define N         4096                 /* the window */
#define F         60                   /* the longest copy */
#define THRESHOLD 2
#define N_CHAR    (256 - THRESHOLD + F)
#define T         (N_CHAR * 2 - 1)     /* the nodes of the tree */
#define R         (T - 1)              /* its root */
#define MAX_FREQ  0x8000


/******************************************************************************
 *
 *   the headers
 *
 ******************************************************************************/
static inline guint
u16 (const guchar *p)
{
   return p[0] | (p[1] << 8);
}


static inline guint32
u32 (const guchar *p)
{
   return p[0] | (p[1] << 8) | (p[2] << 16) | ((guint32) p[3] << 24);
}


gboolean
gimv_lha_member (const guchar *buf, gsize len, gsize off, GimvLhaMember *m)
{
   const guchar *h;
   guint level;

   g_return_val_if_fail (buf && m, FALSE);

   memset (m, 0, sizeof (*m));
   if (off > len || len - off < 22) return FALSE;
   h = buf + off;
   if (h[2] != '-' || h[6] != '-') return FALSE;
   memcpy (m->method, h + 2, 5);
   m->packed   = u32 (h + 7);
   m->original = u32 (h + 11);
   level = h[20];

   if (level == 0 || level == 1) {
      gsize hsize = h[0], namelen = h[21], end = off + 2 + hsize;

      if (hsize < 20 + namelen || end > len) return FALSE;
      if (hsize >= 22 + namelen) {
         m->crc = u16 (h + 22 + namelen);
         m->has_crc = TRUE;
      }
      m->data = end;
      if (level == 1) {
         /* the extended headers are counted in the packed size */
         gsize next;

         if (hsize < 25 + namelen) return FALSE;
         next = u16 (buf + end - 2);
         while (next > 0) {
            if (next < 3 || next > len - m->data || next > m->packed)
               return FALSE;
            m->packed -= next;
            m->data += next;
            next = u16 (buf + m->data - 2);
         }
      }
   } else if (level == 2) {
      gsize hsize = u16 (h);

      if (hsize < 26 || hsize > len - off) return FALSE;
      m->crc = u16 (h + 21);
      m->has_crc = TRUE;
      m->data = off + hsize;
   } else {
      return FALSE;
   }

   return m->packed <= len - m->data;
}


guint16
gimv_lha_crc16 (const guchar *buf, gsize len)
{
   guint crc = 0;
   gsize i;
   gint k;

   for (i = 0; i < len; i++) {
      crc ^= buf[i];
      for (k = 0; k < 8; k++)
         crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
   }
   return crc;
}


/******************************************************************************
 *
 *   the adaptive Huffman tree
 *
 ******************************************************************************/
typedef struct {
   guint freq[T + 1];
   gint  prnt[T + N_CHAR];   /* the parents; prnt[T + c]: the leaf of c */
   gint  son[T];             /* the children (son, son + 1); >= T: a leaf */
} Huff;


static void
huff_start (Huff *h)
{
   gint i, j;

   for (i = 0; i < N_CHAR; i++) {
      h->freq[i] = 1;
      h->son[i] = i + T;
      h->prnt[i + T] = i;
   }
   for (i = 0, j = N_CHAR; j <= R; i += 2, j++) {
      h->freq[j] = h->freq[i] + h->freq[i + 1];
      h->son[j] = i;
      h->prnt[i] = h->prnt[i + 1] = j;
   }
   h->freq[T] = 0xffff;
   h->prnt[R] = 0;
}


/* the frequencies halved, the tree made again */
static void
huff_rebuild (Huff *h)
{
   gint i, j, k;
   guint f;

   for (i = 0, j = 0; i < T; i++) {
      if (h->son[i] >= T) {
         h->freq[j] = (h->freq[i] + 1) / 2;
         h->son[j] = h->son[i];
         j++;
      }
   }
   for (i = 0, j = N_CHAR; j < T; i += 2, j++) {
      f = h->freq[j] = h->freq[i] + h->freq[i + 1];
      for (k = j - 1; f < h->freq[k]; k--);
      k++;
      memmove (h->freq + k + 1, h->freq + k, sizeof (h->freq[0]) * (j - k));
      h->freq[k] = f;
      memmove (h->son + k + 1, h->son + k, sizeof (h->son[0]) * (j - k));
      h->son[k] = i;
   }
   for (i = 0; i < T; i++) {
      k = h->son[i];
      if (k >= T)
         h->prnt[k] = i;
      else
         h->prnt[k] = h->prnt[k + 1] = i;
   }
}


static void
huff_update (Huff *h, gint c)
{
   gint i, j, l;
   guint k;

   if (h->freq[R] == MAX_FREQ) huff_rebuild (h);
   c = h->prnt[c + T];
   do {
      k = ++h->freq[c];
      /* kept in order: swapped with the last node of a lower frequency */
      if (k > h->freq[l = c + 1]) {
         while (k > h->freq[++l]);
         l--;
         h->freq[c] = h->freq[l];
         h->freq[l] = k;

         i = h->son[c];
         h->prnt[i] = l;
         if (i < T) h->prnt[i + 1] = l;

         j = h->son[l];
         h->son[l] = i;

         h->prnt[j] = c;
         if (j < T) h->prnt[j + 1] = c;
         h->son[c] = j;

         c = l;
      }
   } while ((c = h->prnt[c]) != 0);
}


/* the upper 6 bits of a distance: its code (8 bits, from the highest) and
   its length */
static void
position_code (guint upper, guint *code, guint *bits)
{
   /* 1 of 3 bits, 3 of 4, 8 of 5, 12 of 6, 24 of 7, 16 of 8 */
   static const guint count[] = { 1, 3, 8, 12, 24, 16 };
   guint len, i, c = 0, n = upper;

   for (len = 3, i = 0; i < G_N_ELEMENTS (count); i++, len++) {
      if (n < count[i]) {
         *code = c + (n << (8 - len));
         *bits = len;
         return;
      }
      c += count[i] << (8 - len);
      n -= count[i];
   }
   g_assert_not_reached ();
}


/******************************************************************************
 *
 *   unpacking
 *
 ******************************************************************************/
typedef struct {
   const guchar *p, *end;
   guint         buf;        /* bits not used yet, from the highest */
   gint          n;
   gboolean      over;
} Bits;


static inline guint
get_bit (Bits *b)
{
   if (b->n == 0) {
      if (b->p < b->end) {
         b->buf = *b->p++;
      } else {
         b->buf = 0;
         b->over = TRUE;
      }
      b->n = 8;
   }
   b->n--;
   return (b->buf >> b->n) & 1;
}


gboolean
gimv_lh1_decode (const guchar *in, gsize in_len, guchar *out, gsize out_len)
{
   Huff *h;
   guchar text[N];
   guint upper_of[256], len_of[256];
   Bits b = { in, in + in_len, 0, 0, FALSE };
   gsize count = 0;
   guint r = N - F, i, j;

   g_return_val_if_fail (in || in_len == 0, FALSE);
   g_return_val_if_fail (out || out_len == 0, FALSE);

   /* the fixed code of the distances, as a table of its first 8 bits */
   for (i = 0; i < 64; i++) {
      guint code, bits;
      position_code (i, &code, &bits);
      for (j = 0; j < (1u << (8 - bits)); j++) {
         upper_of[code + j] = i;
         len_of[code + j] = bits;
      }
   }

   h = g_new (Huff, 1);
   huff_start (h);
   memset (text, ' ', sizeof (text));

   while (count < out_len && !b.over) {
      gint c = h->son[R];

      while (c < T) c = h->son[c + get_bit (&b)];
      c -= T;
      huff_update (h, c);

      if (c < 256) {
         out[count++] = c;
         text[r++] = c;
         r &= N - 1;
      } else {
         guint first = 0, len, from, k;

         for (k = 0; k < 8; k++) first = (first << 1) | get_bit (&b);
         len = len_of[first];
         /* the 8 bits read: the code and 8 - len of the lower 6 bits */
         from = first;
         for (k = 0; k < len - 2; k++) from = (from << 1) | get_bit (&b);
         from = (upper_of[first] << 6) | (from & 0x3f);
         from = (r - from - 1) & (N - 1);

         len = c - 255 + THRESHOLD;
         for (k = 0; k < len && count < out_len; k++) {
            guchar v = text[(from + k) & (N - 1)];
            out[count++] = v;
            text[r++] = v;
            r &= N - 1;
         }
      }
   }

   g_free (h);
   return count == out_len && !b.over;
}


#ifdef GIMV_LH1_ENCODER
/******************************************************************************
 *
 *   packing (for the tests: the longest match, looked for slowly)
 *
 ******************************************************************************/
typedef struct {
   GByteArray *out;
   guint       buf;
   gint        n;
} PutBits;


static void
put_bits (PutBits *p, guint v, guint n)
{
   while (n--) {
      p->buf = (p->buf << 1) | ((v >> n) & 1);
      if (++p->n == 8) {
         guchar c = p->buf;
         g_byte_array_append (p->out, &c, 1);
         p->buf = 0;
         p->n = 0;
      }
   }
}


static void
put_symbol (PutBits *p, Huff *h, gint c)
{
   guint bits[T], n = 0;
   gint k = h->prnt[c + T];

   /* from the leaf up to the root: the bits backwards */
   do {
      bits[n++] = k & 1;   /* son + 1 is the odd one (son is even) */
      k = h->prnt[k];
   } while (k != R);
   while (n--) put_bits (p, bits[n], 1);
   huff_update (h, c);
}


guchar *
gimv_lh1_encode (const guchar *in, gsize in_len, gsize *out_len)
{
   Huff *h = g_new (Huff, 1);
   guchar text[N];
   PutBits p = { g_byte_array_new (), 0, 0 };
   gsize at = 0;
   guint r = N - F;

   huff_start (h);
   memset (text, ' ', sizeof (text));

   while (at < in_len) {
      guint best = 0, best_d = 0, d, k, take, i;

      for (d = 0; d < N - F; d++) {
         guint from = (r - d - 1) & (N - 1);
         for (k = 0; k < F && at + k < in_len; k++) {
            /* bytes not in the window yet are the ones being copied */
            guint pos = (from + k) & (N - 1);
            guchar v = ((r - from) & (N - 1)) > k ? text[pos] : in[at + k - ((r - from) & (N - 1))];
            if (v != in[at + k]) break;
         }
         if (k > best) {
            best = k;
            best_d = d;
         }
      }

      if (best > THRESHOLD) {
         guint code, bits;
         put_symbol (&p, h, 255 - THRESHOLD + best);
         position_code (best_d >> 6, &code, &bits);
         put_bits (&p, code >> (8 - bits), bits);
         put_bits (&p, best_d & 0x3f, 6);
         take = best;
      } else {
         put_symbol (&p, h, in[at]);
         take = 1;
      }
      for (i = 0; i < take; i++) {
         text[r++] = in[at++];
         r &= N - 1;
      }
   }
   if (p.n) put_bits (&p, 0, 8 - p.n);

   g_free (h);
   *out_len = p.out->len;
   return g_byte_array_free (p.out, FALSE);
}
#endif
