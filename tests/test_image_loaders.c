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
 * The image loader plugins (plugins/image_loader): small images made here
 * in each format are loaded through GimvImageLoader with the plugins of
 * the build tree, and their size and some pixels checked.  Broken files
 * (cut short, header values out of range, bytes changed at random) must
 * give no image, or at least not take the program down: those are loaded
 * in a subprocess, so that a crash is a failure of that test only.
 */

#include "config.h"

#include "gimv_test.h"
#include "gimv_test_core.h"
#include "gimv_file_comment.h"
#include "gimv_image_info.h"
#include "charset.h"
#include "prefs.h"

#include "gimv_anim.h"
#include "gimv_image.h"

static gchar *img_dir = NULL;
static guint  file_serial = 0;


/******************************************************************************
 *  helpers
 ******************************************************************************/

/* a new file (a new name each time: gimv remembers what it knows of a
   file by its name) */
static gchar *
write_file (const gchar *ext, const guint8 *data, gsize len)
{
   gchar *name = g_strdup_printf ("img-%04u.%s", file_serial++, ext);
   gchar *path = g_build_filename (img_dir, name, NULL);

   gimv_test_write (path, data, len);
   g_free (name);
   return path;
}


static GimvImage *
load_bytes (const gchar *ext, const guint8 *data, gsize len, gboolean animation)
{
   gchar *path = write_file (ext, data, len);
   GimvImage *image = gimv_test_core_load_image (path, animation);

   g_unlink (path);
   g_free (path);
   return image;
}


static GimvImage *
load_array (const gchar *ext, GByteArray *a)
{
   return load_bytes (ext, a->data, a->len, FALSE);
}


static void
assert_size (GimvImage *image, gint w, gint h)
{
   g_assert_nonnull (image);
   g_assert_cmpint (gimv_image_width (image), ==, w);
   g_assert_cmpint (gimv_image_height (image), ==, h);
}


/* a string literal (that may hold NULs) as the file */
#define LOAD_LIT(ext, lit) load_bytes ((ext), (const guint8 *) (lit), sizeof (lit) - 1, FALSE)


#define assert_pixel(image, x, y, rgb) \
   g_assert_cmphex (gimv_test_core_pixel ((image), (x), (y)), ==, (rgb))


/* broken data: what must happen to it */
typedef enum {
   MUST_FAIL,      /* no image */
   NO_CRASH,       /* an image or none, but no crash */
} Expect;

typedef struct {
   const gchar *what;
   gsize        offset;
   guint        len;           /* 1, 2 or 4 bytes */
   guint32      value;
   gboolean     big_endian;
   Expect       expect;
} Patch;


/* every cut of the file: none may crash; those within the first
 * "header" bytes (or all of them, "strict") give no image */
static void
check_truncated (const gchar *ext, GByteArray *good, gsize header, gboolean strict)
{
   gsize len;

   for (len = 0; len < good->len; len++) {
      GimvImage *image = load_bytes (ext, good->data, len, FALSE);
      if (image && (strict || len < header))
         g_error ("%s cut to %" G_GSIZE_FORMAT " of %u bytes: an image of %dx%d",
                  ext, len, good->len, gimv_image_width (image), gimv_image_height (image));
      if (image) gimv_image_unref (image);
   }
}


static void
check_patches (const gchar *ext, GByteArray *good, const Patch *patches, guint n)
{
   guint i;

   for (i = 0; i < n; i++) {
      const Patch *p = &patches[i];
      GByteArray *a = g_byte_array_sized_new (good->len);
      GimvImage *image;
      guint k;

      g_byte_array_append (a, good->data, good->len);
      g_assert_cmpuint (p->offset + p->len, <=, a->len);
      for (k = 0; k < p->len; k++) {
         guint shift = p->big_endian ? (p->len - 1 - k) * 8 : k * 8;
         a->data[p->offset + k] = (p->value >> shift) & 0xff;
      }
      if (g_test_verbose ())
         g_test_message ("%s: %s", ext, p->what);
      image = load_array (ext, a);
      if (image && p->expect == MUST_FAIL)
         g_error ("%s, %s: an image of %dx%d", ext, p->what,
                  gimv_image_width (image), gimv_image_height (image));
      if (image) gimv_image_unref (image);
      g_byte_array_unref (a);
   }
}


/* bytes changed at random (always the same ones): no crash */
static void
check_fuzz (const gchar *ext, GByteArray *good, gsize from, guint rounds)
{
   GRand *rand = g_rand_new_with_seed (20261104);
   guint r;

   for (r = 0; r < rounds; r++) {
      GByteArray *a = g_byte_array_sized_new (good->len);
      GimvImage *image;
      guint n = g_rand_int_range (rand, 1, 5), k;

      g_byte_array_append (a, good->data, good->len);
      for (k = 0; k < n; k++)
         a->data[g_rand_int_range (rand, from, a->len)] = g_rand_int_range (rand, 0, 256);
      image = load_array (ext, a);
      if (image) gimv_image_unref (image);
      g_byte_array_unref (a);
   }
   g_rand_free (rand);
}


/* run the test of the broken files in a subprocess */
#define IN_SUBPROCESS(body)                                            \
   G_STMT_START {                                                      \
      if (g_test_subprocess ()) { body; return; }                      \
      g_test_trap_subprocess (NULL, 120 * G_USEC_PER_SEC,              \
                              0);              \
      g_test_trap_assert_passed ();                                    \
   } G_STMT_END


/******************************************************************************
 *  ZIM (KID98)
 ******************************************************************************/

#define ZIM_W 16
#define ZIM_H 3

/* the colour index of the pixel (x, y) of the test image */
static guint
zim_index (guint x, guint y)
{
   return (x + y * 5) % 16;
}

static guint32
zim_palette_rgb (guint c)
{
   /* stored as (B, R, G): R = c * 16, G = 255 - c * 16, B = c * 8 */
   return ((c * 16) << 16) | ((255 - c * 16) << 8) | (c * 8);
}


/* the 4 planes of a line of the test image (intensity, G, R, B) */
static void
zim_line_planes (guint y, guint dots, guint xoff, guint8 *out)
{
   guint plane = (dots + 7) / 8, x;

   memset (out, 0, plane * 4);
   for (x = 0; x < dots; x++) {
      guint c = zim_index (x + xoff, y), byte = x / 8, bit = 0x80 >> (x % 8);
      if (c & 8) out[byte]             |= bit;
      if (c & 4) out[byte + plane]     |= bit;
      if (c & 2) out[byte + plane * 2] |= bit;
      if (c & 1) out[byte + plane * 3] |= bit;
   }
}


/* a line compressed as the decoder expands it */
static void
zim_put_compressed (GByteArray *a, const guint8 *line, guint size)
{
   guint8 *t = g_memdup2 (line, size), *s = g_malloc0 (size);
   guint8 h1 = 0, h2[8] = { 0 }, h3[64] = { 0 };
   GByteArray *stream = g_byte_array_new ();
   guint i;

   g_assert_cmpuint (size, <=, 512);
   /* undo "XOR with the one two before", then "with the one before" */
   for (i = size; i-- > 2; ) t[i] ^= t[i - 2];
   for (i = 0; i < size; i++) s[i] = i ? t[i] ^ t[i - 1] : t[i];
   g_free (t);

   for (i = 0; i < size; i++)
      if (s[i]) h3[i / 8] |= 0x80 >> (i % 8);
   for (i = 0; i < 64; i++)
      if (h3[i]) h2[i / 8] |= 0x80 >> (i % 8);
   for (i = 0; i < 8; i++)
      if (h2[i]) h1 |= 0x80 >> i;

   gimv_test_put8 (stream, h1);
   for (i = 0; i < 8; i++)  if (h1 & (0x80 >> i))         gimv_test_put8 (stream, h2[i]);
   for (i = 0; i < 64; i++) if (h2[i / 8] & (0x80 >> (i % 8))) gimv_test_put8 (stream, h3[i]);
   for (i = 0; i < size; i++) if (s[i]) gimv_test_put8 (stream, s[i]);

   gimv_test_put16le (a, stream->len + 2);   /* the length counts the size */
   gimv_test_put16le (a, size);
   g_byte_array_append (a, stream->data, stream->len);
   g_byte_array_unref (stream);
   g_free (s);
}


static GByteArray *
make_zim (gboolean palette, gboolean compressed, guint sx, guint sy)
{
   GByteArray *a = g_byte_array_new ();
   guint8 line[64];
   guint y, i;

   gimv_test_put_bytes (a, "FORMAT-A", 8);
   gimv_test_put_zeros (a, 0x1fa - 8);
   gimv_test_put16le (a, 1);              /* a table of one */
   gimv_test_put16le (a, 0x1234);
   gimv_test_put16le (a, 0);
   gimv_test_put16le (a, 0);
   gimv_test_put16le (a, sx);
   gimv_test_put16le (a, sy);
   gimv_test_put16le (a, sx + ZIM_W - 1);
   gimv_test_put16le (a, sy + ZIM_H - 1);
   gimv_test_put_zeros (a, 12);
   gimv_test_put16le (a, compressed);
   gimv_test_put16le (a, palette);
   if (palette)
      for (i = 0; i < 16; i++) {
         guint32 rgb = zim_palette_rgb (i);
         gimv_test_put8 (a, rgb & 0xff);
         gimv_test_put8 (a, rgb >> 16);
         gimv_test_put8 (a, (rgb >> 8) & 0xff);
         gimv_test_put8 (a, 0);
      }
   gimv_test_put16le (a, 0);              /* an empty table */

   for (y = 0; y < ZIM_H; y++) {
      guint dots = ZIM_W, plane = (dots + 7) / 8;
      gimv_test_put16le (a, dots);
      gimv_test_put16le (a, sx);
      gimv_test_put16le (a, sy + y);
      zim_line_planes (y, dots, 0, line);
      if (compressed)
         zim_put_compressed (a, line, plane * 4);
      else
         gimv_test_put_bytes (a, line, plane * 4);
   }
   gimv_test_put16le (a, 0);              /* the end */
   return a;
}


static guint32
zim_digital_rgb (guint c)
{
   /* PC-9801 digital 8 colours: bit 1 R, bit 2 G, bit 0 B */
   return ((c & 2) ? 0xff0000 : 0) | ((c & 4) ? 0x00ff00 : 0) | ((c & 1) ? 0x0000ff : 0);
}


static void
check_zim_pixels (GimvImage *image, gboolean palette)
{
   guint x, y;

   assert_size (image, ZIM_W, ZIM_H);
   for (y = 0; y < ZIM_H; y++)
      for (x = 0; x < ZIM_W; x++) {
         guint c = zim_index (x, y);
         assert_pixel (image, x, y, palette ? zim_palette_rgb (c) : zim_digital_rgb (c));
      }
}


static void
test_zim (void)
{
   GByteArray *a;
   GimvImage *image;

   a = make_zim (TRUE, FALSE, 0, 0);
   image = load_array ("zim", a);
   check_zim_pixels (image, TRUE);
   gimv_image_unref (image);
   g_byte_array_unref (a);

   /* no palette: the digital colours */
   a = make_zim (FALSE, FALSE, 0, 0);
   image = load_array ("zim", a);
   check_zim_pixels (image, FALSE);
   gimv_image_unref (image);
   g_byte_array_unref (a);

   /* the image placed at (8, 100) of the screen */
   a = make_zim (TRUE, FALSE, 8, 100);
   image = load_array ("zim", a);
   check_zim_pixels (image, TRUE);
   gimv_image_unref (image);
   g_byte_array_unref (a);

   /* not a ZIM */
   image = LOAD_LIT ("zim", "FORMAT-B");
   g_assert_null (image);
}


static void
test_zim_compressed (void)
{
   GByteArray *a = make_zim (TRUE, TRUE, 0, 0);
   GimvImage *image = load_array ("zim", a);

   check_zim_pixels (image, TRUE);
   gimv_image_unref (image);
   g_byte_array_unref (a);
}


static void
zim_broken (void)
{
   GByteArray *a = make_zim (TRUE, FALSE, 0, 0);
   GByteArray *c = make_zim (TRUE, TRUE, 0, 0);
   const gsize head = 0x1fa + 4, rect = head + 4;
   const Patch patches[] = {
      { "table of 65535",   0x1fa, 2, 0xffff, FALSE, MUST_FAIL },
      { "ex < sx",          rect, 2, 100, FALSE, MUST_FAIL },
      { "ey < sy",          rect + 2, 2, 50, FALSE, MUST_FAIL },
      { "65536 x 65536",    rect + 4, 4, 0xffffffff, FALSE, MUST_FAIL },
      { "dots 65535",       head + 28 + 64 + 2, 2, 0xffff, FALSE, NO_CRASH },
      { "line y far away",  head + 28 + 64 + 2 + 4, 2, 0x7fff, FALSE, NO_CRASH },
      { "line x far away",  head + 28 + 64 + 2 + 2, 2, 0x7fff, FALSE, NO_CRASH },
   };
   const Patch cpatches[] = {
      { "compressed length 65535", head + 28 + 64 + 2 + 6, 2, 0xffff, FALSE, NO_CRASH },
      { "compressed length 0",     head + 28 + 64 + 2 + 6, 2, 0, FALSE, NO_CRASH },
      { "compressed size 65535",   head + 28 + 64 + 2 + 8, 2, 0xffff, FALSE, NO_CRASH },
      { "compressed size 0",       head + 28 + 64 + 2 + 8, 2, 0, FALSE, NO_CRASH },
      { "compressed size 3",       head + 28 + 64 + 2 + 8, 2, 3, FALSE, NO_CRASH },
   };

   /* the lines are read as far as they go: only a cut header fails */
   check_truncated ("zim", a, head + 28 + 64 + 2, FALSE);
   check_truncated ("zim", c, head + 28 + 64 + 2, FALSE);
   check_patches ("zim", a, patches, G_N_ELEMENTS (patches));
   check_patches ("zim", c, cpatches, G_N_ELEMENTS (cpatches));
   check_fuzz ("zim", a, 0x1fa, 300);
   check_fuzz ("zim", c, 0x1fa, 300);
   g_byte_array_unref (a);
   g_byte_array_unref (c);
}

static void
test_zim_broken (void)
{
   IN_SUBPROCESS (zim_broken ());
}


/******************************************************************************
 *  Pi
 ******************************************************************************/

/* an encoder as the format says: copies where they fit, new colours
 * otherwise; the decoder has to give the picture back */

typedef struct {
   GByteArray *a;
   guint8      cur;
   guint       n;
} PiBits;

static void
pi_bit (PiBits *b, guint v)
{
   b->cur = (b->cur << 1) | (v & 1);
   if (++b->n == 8) {
      g_byte_array_append (b->a, &b->cur, 1);
      b->cur = 0;
      b->n = 0;
   }
}

static void
pi_bits (PiBits *b, guint v, guint n)
{
   while (n--) pi_bit (b, v >> n);
}

static void
pi_flush (PiBits *b)
{
   while (b->n) pi_bit (b, 0);
}

static void
pi_put_color (PiBits *b, guint8 *table, guint ncolors, guint planes, guint prev, guint c)
{
   guint8 *list = table + prev * ncolors;
   guint idx = 0, k;

   while (list[idx] != c) idx++;
   if (idx < 2) {
      pi_bit (b, 1); pi_bit (b, idx);
   } else if (idx < 4) {
      pi_bit (b, 0); pi_bit (b, 0); pi_bit (b, idx - 2);
   } else {
      pi_bit (b, 0); pi_bit (b, 1);
      for (k = 0; (4u << (k + 1)) <= idx; k++) pi_bit (b, 1);
      if (k < planes - 3) pi_bit (b, 0);
      pi_bits (b, idx - (4u << k), k + 2);
   }
   memmove (list + 1, list, idx);
   list[0] = c;
}

static void
pi_put_length (PiBits *b, guint n)
{
   guint k = 0;
   if (n == 1) { pi_bit (b, 0); return; }
   while ((2u << k) <= n) k++;
   pi_bits (b, (1u << (k + 1)) - 2, k + 1);    /* k ones, a zero */
   pi_bits (b, n - (1u << k), k);
}

/* the colour index of the test picture: stripes, steps and noise */
static guint
pi_index (guint x, guint y, guint ncolors)
{
   if (y < 3) return (x / 4) % ncolors;                  /* copies to the left */
   if (y < 6) return ((x + y) / 2) % ncolors;            /* diagonal copies */
   return (x * 7 + y * 13 + (x * y) % 5) % ncolors;      /* new colours */
}

static GByteArray *
make_pi (guint w, guint h, guint planes, gboolean palette)
{
   guint ncolors = 1u << planes, i, j, x, y;
   GByteArray *a = g_byte_array_new ();
   guint8 *table = g_malloc (ncolors * ncolors), *pix;
   gsize at, end;
   gint prev = -1;
   PiBits b = { a, 0, 0 };
   static const guint8 head[] = { 'P', 'i', 'T', 'e', 's', 't', 0x1a, 0x00 };

   g_byte_array_append (a, head, sizeof (head));
   {
      guint8 h2[] = { palette ? 0x00 : 0x80, 1, 1, planes, 'T', 'E', 'S', 'T', 0, 3, 'x', 'y', 'z',
                      w >> 8, w & 255, h >> 8, h & 255 };
      g_byte_array_append (a, h2, sizeof (h2));
   }
   if (palette)
      for (i = 0; i < ncolors; i++) {
         guint8 rgb[3] = { i * 16 % 256, 255 - i, (i * 40) % 256 };
         g_byte_array_append (a, rgb, 3);
      }

   for (i = 0; i < ncolors; i++)
      for (j = 0; j < ncolors; j++) table[i * ncolors + j] = (i - j) & (ncolors - 1);

   pix = g_malloc ((gsize) w * (h + 2));
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++) pix[(gsize) (y + 2) * w + x] = pi_index (x, y, ncolors);
   for (x = 0; x < w * 2; x++) pix[x] = pix[2 * w + (x & 1)];
   pi_put_color (&b, table, ncolors, planes, 0, pix[0]);
   pi_put_color (&b, table, ncolors, planes, pix[0], pix[1]);

   at = (gsize) w * 2;
   end = (gsize) w * (h + 2);
   while (at < end) {
      gint place, best = -1;
      gsize best_n = 0;
      for (place = 0; place < 5; place++) {
         gsize from, n = 0;
         if (place == prev) continue;
         switch (place) {
         case 0:  from = pix[at - 1] == pix[at - 2] ? 2 : 4; break;
         case 1:  from = w; break;
         case 2:  from = 2 * w; break;
         case 3:  from = w - 1; break;
         default: from = w + 1; break;
         }
         if (from > at) continue;
         while (at + 2 * n + 1 < end && pix[at + 2 * n] == pix[at + 2 * n - from]
                && pix[at + 2 * n + 1] == pix[at + 2 * n + 1 - from])
            n++;
         if (n > best_n) { best_n = n; best = place; }
      }
      if (best >= 0) {
         if (best < 3) pi_bits (&b, best, 2);
         else pi_bits (&b, best == 3 ? 6 : 7, 3);
         pi_put_length (&b, best_n);
         at += 2 * best_n;
         prev = best;
      } else {
         /* new colours: the place of before again (a copy is always
          * found after new colours: the place of before is -1 then) */
         g_assert_cmpint (prev, >=, 0);
         if (prev < 3) pi_bits (&b, prev, 2);
         else pi_bits (&b, prev == 3 ? 6 : 7, 3);
         for (;;) {
            pi_put_color (&b, table, ncolors, planes, pix[at - 1], pix[at]);
            pi_put_color (&b, table, ncolors, planes, pix[at], pix[at + 1]);
            at += 2;
            if (at >= end) break;
            /* go on while no copy fits */
            {
               gboolean fits = FALSE;
               for (place = 0; place < 5 && !fits; place++) {
                  gsize from;
                  switch (place) {
                  case 0:  from = pix[at - 1] == pix[at - 2] ? 2 : 4; break;
                  case 1:  from = w; break;
                  case 2:  from = 2 * w; break;
                  case 3:  from = w - 1; break;
                  default: from = w + 1; break;
                  }
                  fits = from <= at && pix[at] == pix[at - from]
                         && pix[at + 1] == pix[at + 1 - from];
               }
               pi_bit (&b, !fits);
               if (fits) break;
            }
         }
         prev = -1;
      }
   }
   pi_flush (&b);
   g_free (pix);
   g_free (table);
   return a;
}

static guint32
pi_rgb (guint c, guint planes, gboolean palette)
{
   if (palette) {
      guint8 r = c * 16 % 256, g = 255 - c, bl = (c * 40) % 256;
      return (r << 16) | (g << 8) | bl;
   }
   if (planes == 4) {
      guint on = (c & 8) ? 255 : 153;
      return (((c & 2) ? on : 0) << 16) | (((c & 4) ? on : 0) << 8) | ((c & 1) ? on : 0);
   }
   return (c << 16) | (c << 8) | c;
}

static void
check_pi (guint w, guint h, guint planes, gboolean palette)
{
   GByteArray *a = make_pi (w, h, planes, palette);
   GimvImage *image = load_array ("pi", a);
   guint x, y;

   assert_size (image, w, h);
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
         assert_pixel (image, x, y, pi_rgb (pi_index (x, y, 1u << planes), planes, palette));
   gimv_image_unref (image);
   g_byte_array_unref (a);
}

static void
test_pi (void)
{
   GimvImage *image;

   /* 16 colours, the palette in the high 4 bits ... */
   check_pi (32, 10, 4, TRUE);
   /* ... no palette: the PC-9801 colours */
   check_pi (32, 10, 4, FALSE);
   /* 256 colours */
   check_pi (64, 12, 8, TRUE);
   check_pi (40, 9, 8, FALSE);

   /* not a Pi */
   image = LOAD_LIT ("pi", "PI\x1a\x00");
   g_assert_null (image);
}

static void
pi_broken (void)
{
   GByteArray *a = make_pi (32, 10, 4, TRUE);
   const gsize head = 8 + 4 + 4;
   const Patch patches[] = {
      { "3 bits a pixel",     8 + 3, 1, 3, FALSE, MUST_FAIL },
      { "extra data 65535",   head, 2, 0xffff, TRUE, MUST_FAIL },
      { "65535 x 65535",      head + 2 + 3, 4, 0xffffffff, TRUE, MUST_FAIL },
      { "width 0",            head + 2 + 3, 2, 0, TRUE, MUST_FAIL },
      { "width 1",            head + 2 + 3, 2, 1, TRUE, NO_CRASH },
   };

   /* the picture is read as far as it goes: only a cut header fails */
   check_truncated ("pi", a, head + 2 + 3 + 4 + 48, FALSE);
   check_patches ("pi", a, patches, G_N_ELEMENTS (patches));
   check_fuzz ("pi", a, 8, 300);
   g_byte_array_unref (a);
}

static void
test_pi_broken (void)
{
   IN_SUBPROCESS (pi_broken ());
}


/******************************************************************************
 *  PIC (X68000)
 ******************************************************************************/

/* an encoder as the format says: a run for each change of colour, the
 * colours kept in the 128 last used, and chains down the edges (a change
 * of the same colour below, a pixel or two aside) */

static void
pic_put_length (PiBits *b, gsize n)
{
   guint k = 1;

   while (n > (2u << k) - 2) k++;
   pi_bits (b, (1u << k) - 2, k);                  /* k - 1 ones, a zero */
   pi_bits (b, n - ((1u << k) - 1), k);
}

/* the test picture as values of the machine: shapes with straight and
 * slanted edges (chains), a colour seen again (the cache), noise */
static guint
pic_value (guint x, guint y, guint depth)
{
   guint v;

   if (x >= 3 + y / 2 && x < 12 + y / 3) v = 0x1235;
   else if (y >= 6 && x % 7 == 0) v = 0xf00f;
   else v = ((x / 5) * 0x0841 + y * 0x2108 + (x * y % 3)) & 0xffff;
   if (depth == 15) v &= 0xfffe;                   /* no I bit */
   if (depth <= 8) v %= 1u << depth;               /* in the palette */
   return v;
}

static guint
pic_palette (guint i)
{
   return (i * 0x0843 + 0x1001) & 0xffff;
}

static guint32
pic_x68k_rgb (guint c)
{
   guint r = (c >> 6) & 31, g = c >> 11, bl = (c >> 1) & 31, i = c & 1;
   r = r << 3 | i << 2 | r >> 3;
   g = g << 3 | i << 2 | g >> 3;
   bl = bl << 3 | i << 2 | bl >> 3;
   return r << 16 | g << 8 | bl;
}

static guint32
pic_va_rgb (guint c)
{
   guint g = c >> 10, r = (c >> 5) & 31, bl = c & 31;
   return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (bl << 3 | bl >> 2);
}

static guint32
pic_va_byte_rgb (guint c)
{
   guint g = c >> 5, r = (c >> 2) & 7, bl = c & 3;
   return (r * 0x49 >> 1) << 16 | (g * 0x49 >> 1) << 8 | bl * 0x55;
}

static GByteArray *
make_pic (guint w, guint h, guint machine, guint depth)
{
   GByteArray *a = g_byte_array_new ();
   PiBits b = { a, 0, 0 };
   guint32 value[128];
   guint8 prev[128], next[128], head = 0;
   gsize total = (gsize) w * h, i, last = 0, k;
   guint *v = g_new (guint, total);
   gboolean *marked = g_new0 (gboolean, total);
   static const guint8 top[] = { 'P', 'I', 'C', 'T', 'e', 's', 't', 0x1a, 'x', 0x00 };

   g_byte_array_append (a, top, sizeof (top));
   {
      guint8 h2[] = { machine >> 8, machine & 255, depth >> 8, depth & 255,
                      w >> 8, w & 255, h >> 8, h & 255 };
      g_byte_array_append (a, h2, sizeof (h2));
   }
   if (machine == 0x1f || machine == 2) {
      static const guint8 extra[6] = { 1, 2, 3, 4, 5, 6 };
      g_byte_array_append (a, extra, 6);
   }
   if (depth <= 8)
      for (i = 0; i < (1u << depth); i++) {
         guint8 c[2] = { pic_palette (i) >> 8, pic_palette (i) & 255 };
         g_byte_array_append (a, c, 2);
      }
   for (i = 0; i < total; i++) v[i] = pic_value (i % w, i / w, depth);
   for (k = 0; k < 128; k++) {
      value[k] = 0;
      prev[k] = (k + 1) & 127;
      next[k] = (k - 1) & 127;
   }

   for (i = 0; ; ) {
      guint c = v[i];
      gsize p, steps[64];
      guint nsteps = 0;

      pic_put_length (&b, i == 0 ? 1 : i - last);
      if (depth <= 8) {
         pi_bits (&b, c, depth);
      } else {
         for (k = 0; k < 128 && value[k] != c; k++) ;
         if (k < 128) {
            /* the slot becomes the most recently used */
            pi_bit (&b, 1);
            pi_bits (&b, k, 7);
            if (k != head) {
               guint8 pr = prev[k], nx = next[k], tail;
               next[pr] = nx;
               prev[nx] = pr;
               tail = prev[head];
               next[tail] = k;
               prev[k] = tail;
               prev[head] = k;
               next[k] = head;
               head = k;
            }
         } else {
            pi_bit (&b, 0);
            pi_bits (&b, depth == 15 ? c >> 1 : c, depth);
            head = prev[head];
            value[head] = c;
         }
      }
      last = i;
      if (i + 1 >= total) break;

      /* a chain down the changes to this colour */
      for (p = i; nsteps < G_N_ELEMENTS (steps); ) {
         static const gint aside[] = { 0, -1, 1, -2, 2 };
         gssize q = -1;
         for (k = 0; k < 5; k++) {
            q = (gssize) p + w + aside[k];
            if (q > (gssize) p && (gsize) q < total
                && v[q] == c && v[q - 1] != c && !marked[q])
               break;
         }
         if (k == 5) break;
         marked[q] = TRUE;
         steps[nsteps++] = k;
         p = q;
      }
      pi_bit (&b, nsteps > 0);
      if (nsteps > 0) {
         for (k = 0; k < nsteps; k++) {
            switch (steps[k]) {
            case 0:  pi_bits (&b, 2, 2); break;
            case 1:  pi_bits (&b, 1, 2); break;
            case 2:  pi_bits (&b, 3, 2); break;
            case 3:  pi_bits (&b, 2, 4); break;     /* 001 0 */
            default: pi_bits (&b, 3, 4); break;     /* 001 1 */
            }
         }
         pi_bits (&b, 0, 3);                        /* 000: the end */
      }

      for (i++; i < total && (v[i] == v[i - 1] || marked[i]); i++) ;
      if (i >= total) {
         pic_put_length (&b, total - last);
         break;
      }
   }
   pi_flush (&b);
   g_free (v);
   g_free (marked);
   return a;
}

static guint32
pic_expected (guint x, guint y, guint w, guint machine, guint depth)
{
   if (machine == 0x21) {
      gsize o = (gsize) y * w + x;          /* the pixel: two of a value */
      guint c = pic_value ((o / 2) % w, (o / 2) / w, depth);
      return pic_va_byte_rgb (o & 1 ? c >> 8 : c & 0xff);
   }
   if (machine == 0x11) return pic_va_rgb (pic_value (x, y, depth));
   if (depth <= 8) return pic_x68k_rgb (pic_palette (pic_value (x, y, depth)));
   return pic_x68k_rgb (pic_value (x, y, depth));
}

static void
check_pic (guint w, guint h, guint machine, guint depth)
{
   GByteArray *a = make_pic (w, h, machine, depth);
   GimvImage *image = load_array ("pic", a);
   guint x, y, oh = machine == 0x21 ? h * 2 : h;

   assert_size (image, w, oh);
   for (y = 0; y < oh; y++)
      for (x = 0; x < w; x++)
         assert_pixel (image, x, y, pic_expected (x, y, w, machine, depth));
   gimv_image_unref (image);
   g_byte_array_unref (a);
}

static void
test_pic (void)
{
   GimvImage *image;

   /* the X68000: 65536 colours, 32768 (no I bit), extended header */
   check_pic (40, 24, 0x00, 16);
   check_pic (33, 17, 0x00, 15);
   check_pic (24, 10, 0x1f, 16);
   /* palettes */
   check_pic (30, 12, 0x00, 8);
   check_pic (30, 12, 0x00, 4);
   /* FM TOWNS (X68000 colours) */
   check_pic (20, 9, 0x02, 15);
   check_pic (20, 9, 0xc2, 15);
   /* PC-88VA: G6R5B5, and two GGGRRRBB pixels a value */
   check_pic (36, 14, 0x11, 16);
   check_pic (10, 6, 0x21, 16);

   /* not a PIC */
   image = LOAD_LIT ("pic", "PIX\x1a\x00");
   g_assert_null (image);
}

static void
pic_broken (void)
{
   GByteArray *a = make_pic (40, 24, 0x00, 16);
   const gsize head = 10;
   const Patch patches[] = {
      { "the machine 0x99", head, 2, 0x99, TRUE, MUST_FAIL },
      { "12 bits",          head + 2, 2, 12, TRUE, MUST_FAIL },
      { "VA, 15 bits",      head, 4, 0x0011000f, TRUE, MUST_FAIL },
      { "65535 x 65535",    head + 4, 4, 0xffffffff, TRUE, MUST_FAIL },
      { "width 0",          head + 4, 2, 0, TRUE, MUST_FAIL },
      { "width 1",          head + 4, 2, 1, TRUE, NO_CRASH },
      { "height 1000",      head + 6, 2, 1000, TRUE, NO_CRASH },
   };

   /* the picture is read as far as it goes: only a cut header fails */
   check_truncated ("pic", a, head + 8, FALSE);
   check_patches ("pic", a, patches, G_N_ELEMENTS (patches));
   check_fuzz ("pic", a, 8, 300);
   g_byte_array_unref (a);
}

static void
test_pic_broken (void)
{
   IN_SUBPROCESS (pic_broken ());
}


/******************************************************************************
 *  MB (PC-8801)
 ******************************************************************************/

/* the stream packed as literal runs (the decoder's runs of a byte are
 * checked by the boxes, written as repeats) */

static void
mb_put (GByteArray *a, const guint8 *data, gsize n)
{
   while (n > 0) {
      guint8 c = MIN (n, 127);
      guint8 head = 0x80 | c;
      g_byte_array_append (a, &head, 1);
      g_byte_array_append (a, data, c);
      data += c;
      n -= c;
   }
}

static void
mb_put_repeat (GByteArray *a, guint8 value, guint n)
{
   guint8 two[2] = { n & 0x7f, value };     /* n < 128 */
   g_byte_array_append (a, two, 2);
}

/* the 2000 bytes of the mask as the decoder turns them */
static guint8
mb_chain (guint8 v, guint8 after)
{
   guint8 x = (after >> 7) ^ v;
   guint bit;
   for (bit = 1; bit < 8; bit++) x ^= ((x >> (bit - 1)) & 1) << bit;
   return x;
}

/* a plane: boxes, then the bytes where keep (a bit for each byte) is 0 */
static void
mb_put_plane (GByteArray *a, const guint8 *boxes, gsize boxes_len,
              const guint8 *keep, const guint8 *plane)
{
   guint8 x[2000], pre[2000], bits[250];
   GByteArray *s = g_byte_array_new ();
   guint i, v;
   guint8 zero = 0;

   /* the boxes: their width and height as a repeat, the rest literal */
   mb_put (s, boxes, boxes_len);
   mb_put_repeat (s, 0, 1);

   /* the mask before the decoder turns it */
   x[1999] = keep[1999];
   for (i = 1999; i-- > 0; ) {
      for (v = 0; v < 256 && mb_chain (v, keep[i + 1]) != keep[i]; v++) ;
      g_assert_cmpuint (v, <, 256);
      x[i] = v;
   }
   for (i = 0; i < 2000; i++) pre[i] = i + 10 < 2000 ? x[i] ^ x[i + 10] : x[i];
   memset (bits, 0, sizeof (bits));
   for (i = 0; i < 2000; i++)
      if (pre[i]) bits[i / 8] |= 0x80 >> (i % 8);
   mb_put (s, bits, 250);
   for (i = 0; i < 2000; i++)
      if (pre[i]) mb_put (s, &pre[i], 1);
   for (i = 0; i < 16000; i++)
      if (!((keep[i / 8] >> (7 - i % 8)) & 1)) mb_put (s, &plane[i], 1);
   g_byte_array_append (a, s->data, s->len);
   g_byte_array_unref (s);
   (void) zero;
}

typedef struct {
   guint8 plane[3][16000];
} MbPicture;

static GByteArray *
make_mb (MbPicture *pic)
{
   GByteArray *a = g_byte_array_new ();
   guint8 keep[2000];
   guint i, x, y;

   /* blue: a pattern of two lines over the screen, a box, kept on the
      top half, bytes of the stream below */
   {
      static const guint8 boxes[] = {
         80, 200, 0x40, 0x00, 0x55, 0xaa,              /* 2 pattern bytes */
         10, 50, (20 * 80 + 5) >> 8, (20 * 80 + 5) & 255, 0xff,
      };
      for (i = 0; i < 16000; i++)
         pic->plane[0][i] = (i / 80) % 2 ? 0xaa : 0x55;
      for (y = 20; y < 70; y++)
         for (x = 5; x < 15; x++) pic->plane[0][y * 80 + x] = 0xff;
      for (i = 0; i < 2000; i++) keep[i] = i < 1000 ? 0xff : 0x00;
      for (i = 8000; i < 16000; i++) pic->plane[0][i] = (i * 7) & 0xff;
      mb_put_plane (a, boxes, sizeof (boxes), keep, pic->plane[0]);
   }
   /* red: all from the stream */
   {
      for (i = 0; i < 16000; i++) pic->plane[1][i] = (i * 13 + i / 80) & 0xff;
      memset (keep, 0, sizeof (keep));
      mb_put_plane (a, NULL, 0, keep, pic->plane[1]);
   }
   /* green: boxes only (a height of 0: 256 lines, cut at the bottom) */
   {
      static const guint8 boxes[] = {
         3, 0, 0x00, 70, 0xf0,
         0x80 | 4, 9, (100 * 80 + 40) >> 8 | 0xc0, (100 * 80 + 40) & 255, 1, 2, 3, 4,
      };
      memset (pic->plane[2], 0, 16000);
      for (y = 0; y < 200; y++)
         for (x = 70; x < 73; x++) pic->plane[2][y * 80 + x] = 0xf0;
      for (y = 100; y < 109; y++)
         for (x = 40; x < 44; x++) pic->plane[2][y * 80 + x] = 1 + (y - 100) % 4;
      memset (keep, 0xff, sizeof (keep));
      mb_put_plane (a, boxes, sizeof (boxes), keep, pic->plane[2]);
   }
   return a;
}

static guint32
mb_rgb (MbPicture *pic, guint x, guint y)
{
   guint at = y * 80 + x / 8, shift = 7 - x % 8;
   return (((pic->plane[1][at] >> shift) & 1) ? 0xff0000 : 0)
      | (((pic->plane[2][at] >> shift) & 1) ? 0x00ff00 : 0)
      | (((pic->plane[0][at] >> shift) & 1) ? 0x0000ff : 0);
}

static void
test_mb (void)
{
   MbPicture *pic = g_new (MbPicture, 1);
   GByteArray *a = make_mb (pic);
   GimvImage *image = load_array ("mb", a);
   guint x, y;

   assert_size (image, 640, 200);
   for (y = 0; y < 200; y++)
      for (x = 0; x < 640; x++)
         assert_pixel (image, x, y, mb_rgb (pic, x, y));
   gimv_image_unref (image);

   /* a Maya scene, nothing */
   image = LOAD_LIT ("mb", "FOR4\0\0\0\x10" "Maya");
   g_assert_null (image);
   g_byte_array_unref (a);
   g_free (pic);
}

static void
mb_broken (void)
{
   MbPicture *pic = g_new (MbPicture, 1);
   GByteArray *a = make_mb (pic);

   gsize cut;

   /* cut short: nothing (the format can not be told otherwise); some
      cuts only, the file is long */
   for (cut = 0; cut < a->len; cut += cut < 64 ? 1 : 997) {
      GimvImage *image = load_bytes ("mb", a->data, cut, FALSE);
      g_assert_null (image);
   }
   {
      GimvImage *image = load_bytes ("mb", a->data, a->len - 1, FALSE);
      g_assert_null (image);
   }
   check_fuzz ("mb", a, 0, 100);
   g_byte_array_unref (a);
   g_free (pic);
}

static void
test_mb_broken (void)
{
   IN_SUBPROCESS (mb_broken ());
}


/******************************************************************************
 *  MB2 (PC-9801)
 ******************************************************************************/

/* an encoder for what the decoder must give back: boxes of units of 1, 2
 * (as 3 bits: the size of a unit is 1, 3, 4 or 8) and 8 bits, a mask
 * made of runs down a column (a count of 0: 256) and along it, repeats
 * and bytes, and the bytes of the plane where the mask is 0 */

static guint
mb2_next_place (const guchar *given, guint at, guint count, gboolean down)
{
   for (;;) {
      if (++at >= count) return count;
      if (down) {
         at += 9;
         if (at >= count) at = at + 1 - count;
      }
      if (!given[at]) return at;
   }
}

static void
mb2_plane (PiBits *b, guint lines, guint seed, guint8 *expect)
{
   guint size = 80 * lines, count = size / 8, at, i, y, x;
   guint8 *mask = g_malloc (count), *given = g_malloc0 (count);

   /* the boxes: a unit of 1 bit (1: 0xff), of 2 bits as 3 (0b010 again
      and again from the highest: 0x49 ...), of 8 bits, two units by line */
   memset (expect, 0, size);
   pi_bits (b, 2 * 80 + 4, 15);  pi_bits (b, 20 * 80 + 30, 15);
   pi_bits (b, 0, 2);  pi_bits (b, 0, 2);  pi_bits (b, 1, 1);
   for (y = 2; y <= 20; y++)
      for (x = 4; x <= 30; x++) expect[y * 80 + x] = 0xff;
   pi_bits (b, 30 * 80 + 40, 15);  pi_bits (b, 40 * 80 + 50, 15);
   pi_bits (b, 1, 2);  pi_bits (b, 0, 2);  pi_bits (b, 2, 3);
   for (y = 30; y <= 40; y++)
      for (x = 40; x <= 50; x++) expect[y * 80 + x] = 0x49;
   pi_bits (b, 50 * 80 + 10, 15);  pi_bits (b, 60 * 80 + 79, 15);
   pi_bits (b, 3, 2);  pi_bits (b, 1, 2);
   pi_bits (b, 0x12 + seed, 8); pi_bits (b, 0x34, 8); pi_bits (b, 0x56, 8);
   for (y = 50; y <= 60; y++)
      for (x = 10; x <= 79; x++)
         expect[y * 80 + x] = (y - 50) % 3 == 0 ? 0x12 + seed : (y - 50) % 3 == 1 ? 0x34 : 0x56;
   pi_bits (b, 0x7fff, 15);

   /* the mask: column 0 none of the boxes (a run down it: 256, then the
      rest), the boxes kept on the first lines, bytes elsewhere */
   for (i = 0; i < count; i++) {
      guint line = i / 10;
      mask[i] = i % 10 == 0 ? 0 : line < 70 ? 0xff : (guint8) (i * 37 + seed);
   }
   at = 0;
   {
      guint left = count / 10;               /* the column */
      while (left > 0) {
         guint n = left >= 256 ? 256 : MIN (left, 63);
         pi_bit (b, 1);  pi_bit (b, 0);  pi_bits (b, n == 256 ? 0 : n, 6);
         pi_bits (b, 0, 8);
         for (i = 0; i < n; i++) {
            given[at] = 1;
            at = mb2_next_place (given, at, count, TRUE);
         }
         left -= n;
      }
   }
   while (at < count) {
      /* along: a repeat where the bytes are alike, else bytes */
      guint n = 0, p = at, k;
      gboolean same = TRUE;
      guint places[63];
      while (n < 63 && p < count) {
         places[n++] = p;
         p = mb2_next_place (given, p, count, FALSE);
      }
      for (k = 1; k < n; k++)
         if (mask[places[k]] != mask[places[0]]) same = FALSE;
      pi_bit (b, 0);  pi_bit (b, !same);  pi_bits (b, n, 6);
      if (same) pi_bits (b, mask[places[0]], 8);
      for (k = 0; k < n; k++) {
         if (!same) pi_bits (b, mask[places[k]], 8);
         given[places[k]] = 1;
      }
      at = p;
      if (at < count && given[at]) at = mb2_next_place (given, at, count, FALSE);
   }

   /* the bytes */
   for (i = 0; i < size; i++)
      if (!((mask[i / 8] >> (i % 8)) & 1)) {
         expect[i] = (guint8) (i * 11 + seed * 3 + i / 80);
         pi_bits (b, expect[i], 8);
      }
   g_free (mask);
   g_free (given);
}

typedef struct {
   guint   lines, nplanes;
   guint8  plane[4][32000];
   guint8  pal[16][3];
   gboolean palette;
} Mb2Picture;

static GByteArray *
make_mb2 (Mb2Picture *pic, guint flags)
{
   GByteArray *a = g_byte_array_new ();
   PiBits b = { a, 0, 0 };
   guint i;
   guint8 f[2] = { flags & 255, flags >> 8 };
   static const char comment[] = "TEST\r\n\x83\x65\x83\x58\x83\x67";   /* テスト */

   g_byte_array_append (a, f, 2);
   if (flags & 0x0800) {
      guint8 n[2] = { 0, sizeof (comment) - 1 };
      g_byte_array_append (a, n, 2);
      g_byte_array_append (a, (const guint8 *) comment, sizeof (comment) - 1);
   }
   pic->lines = (flags & 0x2000) ? 400 : 200;
   pic->nplanes = (flags & 0xc000) == 0 ? 1 : (flags & 0xc000) == 0x4000 ? 3 : 4;
   pic->palette = (flags & 0xc000) == 0x8000 && (flags & 0x1000);
   for (i = 0; i < 16; i++) {
      pic->pal[i][0] = (i * 5) & 15;
      pic->pal[i][1] = (i * 3 + 1) & 15;
      pic->pal[i][2] = 15 - i;
      if (pic->palette) {
         pi_bits (&b, pic->pal[i][1], 4);
         pi_bits (&b, pic->pal[i][0], 4);
         pi_bits (&b, pic->pal[i][2], 4);
      }
   }
   for (i = 0; i < pic->nplanes; i++)
      mb2_plane (&b, pic->lines, i, pic->plane[i]);
   pi_flush (&b);
   return a;
}

static guint32
mb2_rgb (Mb2Picture *pic, guint x, guint y)
{
   guint at = y * 80 + x / 8, shift = 7 - x % 8, c = 0, i;

   for (i = 0; i < pic->nplanes; i++) c |= ((pic->plane[i][at] >> shift) & 1) << i;
   if (pic->nplanes == 1) return c ? 0xffffff : 0;
   if (pic->nplanes == 3)
      return ((c & 2) ? 0xff0000 : 0) | ((c & 4) ? 0x00ff00 : 0) | ((c & 1) ? 0x0000ff : 0);
   if (pic->palette)
      return (pic->pal[c][0] * 17) << 16 | (pic->pal[c][1] * 17) << 8 | pic->pal[c][2] * 17;
   {
      static const guint32 standard[16] = {
         0x000000, 0x0000ff, 0xff0000, 0xff00ff, 0x00ff00, 0x00ffff, 0xffff00, 0xffffff,
         0x777777, 0x0000aa, 0xaa0000, 0xaa00aa, 0x00aa00, 0x00aaaa, 0xaaaa00, 0xaaaaaa,
      };
      return standard[c];
   }
}

static void
check_mb2 (guint flags)
{
   Mb2Picture *pic = g_new0 (Mb2Picture, 1);
   GByteArray *a = make_mb2 (pic, flags);
   GimvImage *image = load_array ("mb2", a);
   guint x, y;

   assert_size (image, 640, pic->lines);
   for (y = 0; y < pic->lines; y++)
      for (x = 0; x < 640; x++)
         assert_pixel (image, x, y, mb2_rgb (pic, x, y));
   gimv_image_unref (image);
   g_byte_array_unref (a);
   g_free (pic);
}

static void
test_mb2 (void)
{
   GimvImage *image;

   check_mb2 (0x8000 | 0x2000 | 0x1000 | 0x0800);   /* 16 colours, 400 lines, a palette, a comment */
   check_mb2 (0xc000 | 0x2000);                     /* 16 colours, the standard ones */
   check_mb2 (0x4000);                              /* 8 colours, 200 lines */
   check_mb2 (0x0000 | 0x0800);                     /* one plane */

   /* flags it does not know */
   image = LOAD_LIT ("mb2", "\x01\x80\0\0\0\0\0\0");
   g_assert_null (image);
}

/* the comments of Pi and PIC files (the Data tab) */
static void
check_comments (GByteArray *a, const gchar *ext, const gchar * const *expect)
{
   gchar *path = write_file (ext, a->data, a->len);
   GPtrArray *list = gimv_file_comment_read (path);
   guint i;

   for (i = 0; expect[i]; i += 2) {
      GimvFileComment *c;
      g_assert_cmpuint (list->len, >, i / 2);
      c = g_ptr_array_index (list, i / 2);
      g_assert_cmpstr (c->text, ==, expect[i + 1]);
   }
   g_assert_cmpuint (list->len, ==, i / 2);
   g_ptr_array_unref (list);
   g_unlink (path);
   g_free (path);
}

static void
test_old_comments (void)
{
   GByteArray *a;
   static const gchar *pic[] = { "Comment", "Test", "Comment", "x", "Machine", "X68000", NULL };
   static const gchar *pic_va[] = { "Comment", "Test", "Comment", "x", "Machine", "PC-88VA", NULL };
   static const gchar *pi[] = { "Comment", "Test", "Machine", "TEST", NULL };

   a = make_pic (8, 4, 0x00, 16);
   check_comments (a, "pic", pic);
   g_byte_array_unref (a);
   a = make_pic (8, 4, 0x11, 16);
   check_comments (a, "pic", pic_va);
   g_byte_array_unref (a);
   a = make_pi (8, 4, 4, TRUE);
   check_comments (a, "pi", pi);
   g_byte_array_unref (a);
}


static void
test_mb2_comment (void)
{
   Mb2Picture *pic = g_new0 (Mb2Picture, 1);
   GByteArray *a = make_mb2 (pic, 0x8000 | 0x2000 | 0x0800);
   gchar *path = write_file ("mb2", a->data, a->len);
   GPtrArray *list = gimv_file_comment_read (path);
   GimvFileComment *c;

   g_assert_cmpuint (list->len, ==, 1);
   c = g_ptr_array_index (list, 0);
   g_assert_true (strstr (c->text, "テスト") != NULL);
   g_ptr_array_unref (list);
   g_unlink (path);
   g_free (path);
   g_byte_array_unref (a);
   g_free (pic);
}

static void
mb2_broken (void)
{
   Mb2Picture *pic = g_new0 (Mb2Picture, 1);
   GByteArray *a = make_mb2 (pic, 0x8000 | 0x2000 | 0x1000 | 0x0800);
   gsize cut;

   for (cut = 0; cut < a->len; cut += cut < 64 ? 1 : 1997) {
      GimvImage *image = load_bytes ("mb2", a->data, cut, FALSE);
      g_assert_null (image);
   }
   check_fuzz ("mb2", a, 2, 100);
   g_byte_array_unref (a);
   g_free (pic);
}

static void
test_mb2_broken (void)
{
   IN_SUBPROCESS (mb2_broken ());
}


/* the pixels shown as on the machine: the X68000 (512 dots wide) 4/3
 * wide, the PC-9801 1.2 high, the others as they are */
static void
test_dot_aspect (void)
{
   GByteArray *a;
   GimvImage *image;

   g_setenv ("GIMV_LOADER_DOT_ASPECT", "1", TRUE);

   a = make_pic (30, 12, 0x00, 16);                 /* X68000 */
   image = load_array ("pic", a);
   assert_size (image, 40, 12);
   /* stretched, not changed: a flat part keeps its colour */
   assert_pixel (image, 0, 0, pic_expected (0, 0, 30, 0x00, 16));
   gimv_image_unref (image);
   g_byte_array_unref (a);

   a = make_pic (36, 10, 0x11, 16);                 /* PC-88VA */
   image = load_array ("pic", a);
   assert_size (image, 36, 12);
   gimv_image_unref (image);
   g_byte_array_unref (a);

   a = make_pic (20, 9, 0xc2, 15);                  /* FM TOWNS */
   image = load_array ("pic", a);
   assert_size (image, 20, 9);
   gimv_image_unref (image);
   g_byte_array_unref (a);

   a = make_pi (32, 10, 4, TRUE);                   /* "TEST": a PC-9801 */
   image = load_array ("pi", a);
   assert_size (image, 32, 12);
   gimv_image_unref (image);
   g_byte_array_unref (a);

   g_setenv ("GIMV_LOADER_DOT_ASPECT", "0", TRUE);
}


/******************************************************************************
 *  MAKI01 (the format before MAG)
 ******************************************************************************/

/* a 640x400 picture: shapes (bytes that repeat 2 or 4 lines below, which
   MAKI leaves out) and noise */
static guint
maki_index (guint x, guint y)
{
   if (y < 100) return 0;
   if (y < 200) return ((x / 40) + (y / 25)) & 15;
   if (y < 300) return (x * 7 + y * 3) & 15;
   return (x * x + y * 13) % 16;
}

static guint32
maki_rgb (guint c)
{
   /* the palette is G, R, B; 16 levels in the high nibble */
   guint8 g = (c * 16) | c, r = ((15 - c) * 16) | (15 - c), b = ((c * 5) & 15) * 17;
   return (r << 16) | (g << 8) | b;
}

static GByteArray *
make_maki (gboolean b_type)
{
   GByteArray *a = g_byte_array_new ();
   guchar head[96] = { 0 }, flag_a[1000] = { 0 };
   guchar *pic = g_malloc0 (320 * 400);
   GByteArray *flag_b = g_byte_array_new (), *pix = g_byte_array_new ();
   guint x, y, i, dist = b_type ? 4 : 2;

   for (y = 0; y < 400; y++)
      for (x = 0; x < 320; x++)
         pic[y * 320 + x] = (maki_index (x * 2, y) << 4) | maki_index (x * 2 + 1, y);

   for (i = 0; i < 8000; i++) {
      guint bx = i % 80, by = i / 80, bits = 0, k;
      for (k = 0; k < 16; k++) {
         guint xx = bx * 4 + k % 4, yy = by * 4 + k / 4;
         guchar above = yy >= dist ? pic[(yy - dist) * 320 + xx] : 0;
         if (pic[yy * 320 + xx] != above) bits |= 0x8000 >> k;
      }
      if (bits) {
         guchar be[2] = { bits >> 8, bits & 0xff };
         flag_a[i / 8] |= 0x80 >> (i % 8);
         g_byte_array_append (flag_b, be, 2);
      }
   }
   /* the pixel bytes in the order of the picture */
   for (y = 0; y < 400; y++)
      for (x = 0; x < 320; x++) {
         guchar above = y >= dist ? pic[(y - dist) * 320 + x] : 0;
         if (pic[y * 320 + x] != above) {
            guchar v = pic[y * 320 + x] ^ above;
            g_byte_array_append (pix, &v, 1);
         }
      }

   memcpy (head, b_type ? "MAKI01B " : "MAKI01A ", 8);
   memcpy (head + 8, "PC98 test user     ", 19);
   head[32] = flag_b->len >> 8; head[33] = flag_b->len & 0xff;
   head[34] = pix->len / 2 >> 8; head[35] = pix->len / 2 & 0xff;
   head[36] = (pix->len - pix->len / 2) >> 8; head[37] = (pix->len - pix->len / 2) & 0xff;
   head[44] = 0x02; head[45] = 0x80; head[46] = 0x01; head[47] = 0x90;
   for (i = 0; i < 16; i++) {
      head[48 + i * 3]     = i << 4;          /* G */
      head[48 + i * 3 + 1] = (15 - i) << 4;   /* R */
      head[48 + i * 3 + 2] = ((i * 5) & 15) << 4 | 0x0f;   /* B, low bits ignored */
   }
   g_byte_array_append (a, head, 96);
   g_byte_array_append (a, flag_a, 1000);
   g_byte_array_append (a, flag_b->data, flag_b->len);
   g_byte_array_append (a, pix->data, pix->len);

   g_byte_array_unref (flag_b);
   g_byte_array_unref (pix);
   g_free (pic);
   return a;
}

static void
check_maki (gboolean b_type)
{
   GByteArray *a = make_maki (b_type);
   GimvImage *image = load_array ("mki", a);
   guint x, y;

   assert_size (image, 640, 400);
   for (y = 0; y < 400; y += 3)
      for (x = 0; x < 640; x++)
         assert_pixel (image, x, y, maki_rgb (maki_index (x, y)));
   gimv_image_unref (image);
   g_byte_array_unref (a);
}

static void
test_maki (void)
{
   check_maki (FALSE);
   check_maki (TRUE);
   /* MAG is another loader's */
   g_assert_null (LOAD_LIT ("mki", "MAKI02  "));
}

/* the machine and the user of the header as file comments */
static void
test_maki_comment (void)
{
   GByteArray *a = make_maki (FALSE);
   gchar *path;
   GPtrArray *list;
   /* " by " and a name in Shift_JIS (優＆魅衣), ended by 0x1a */
   const guchar user[20] = " by \x97\x44\x81\x95\x96\xa3\x88\xdf r1.00\x1a";

   memcpy (a->data + 8, "PCVA", 4);
   memcpy (a->data + 12, user, 20);
   path = write_file ("mki", a->data, a->len);
   list = gimv_file_comment_read (path);
   g_assert_cmpuint (list->len, ==, 2);
   g_assert_cmpstr (((GimvFileComment *) list->pdata[0])->text, ==, "by 優＆魅衣 r1.00");
   g_assert_cmpstr (((GimvFileComment *) list->pdata[1])->text, ==, "PCVA");
   g_ptr_array_unref (list);
   g_unlink (path);
   g_free (path);

   /* EUC-JP (written by a UNIX tool) */
   {
      const guchar euc[20] = " by \xc6\xfc\xcb\xdc\xb8\xec r1.00\x1a";
      memcpy (a->data + 12, euc, 20);
      path = write_file ("mki", a->data, a->len);
      list = gimv_file_comment_read (path);
      g_assert_cmpstr (((GimvFileComment *) list->pdata[0])->text, ==, "by 日本語 r1.00");
      g_ptr_array_unref (list);
      g_unlink (path);
      g_free (path);
   }
   g_byte_array_unref (a);
}

/* a file name not in UTF-8: guessed (italics, a note) or not readable */
static void
test_name_note (void)
{
   GByteArray *a = make_maki (FALSE);
   gchar *dir = g_dir_make_tmp ("gimv-names-XXXXXX", NULL);
   gchar *path = g_strconcat (dir, "/\x8a\xbf\x8e\x9a.mki", NULL);   /* 漢字 */
   gchar *plain = g_strconcat (dir, "/plain.mki", NULL);
   CharsetAutoDetectFn saved = conf.charset_auto_detect_fn;
   GimvImageInfo *info;
   gchar *note = NULL;

   g_assert_true (g_file_set_contents (path, (gchar *) a->data, a->len, NULL));
   g_assert_true (g_file_set_contents (plain, (gchar *) a->data, a->len, NULL));

   conf.charset_auto_detect_fn = charset_get_auto_detect_func (CHARSET_AUTODETECT_JAPANESE);
   info = gimv_image_info_get (path);
   g_assert_true (gimv_image_info_get_name_note (info, &note));
   g_assert_nonnull (strstr (note, "Shift_JIS"));
   g_assert_nonnull (strstr (note, "%8A%BF%8E%9A.mki"));
   g_clear_pointer (&note, g_free);
   gimv_image_info_unref (info);

   /* not Japanese: not readable, not in italics, still a note */
   conf.charset_auto_detect_fn = NULL;
   info = gimv_image_info_get (path);
   g_assert_false (gimv_image_info_get_name_note (info, &note));
   g_assert_nonnull (note);
   g_assert_nonnull (strstr (note, "%8A%BF%8E%9A.mki"));
   g_clear_pointer (&note, g_free);
   gimv_image_info_unref (info);

   info = gimv_image_info_get (plain);
   g_assert_false (gimv_image_info_get_name_note (info, &note));
   g_assert_null (note);
   gimv_image_info_unref (info);

   conf.charset_auto_detect_fn = saved;
   g_unlink (path);
   g_unlink (plain);
   g_rmdir (dir);
   g_free (path);
   g_free (plain);
   g_free (dir);
   g_byte_array_unref (a);
}


static void
maki_broken (void)
{
   GByteArray *a = make_maki (FALSE);
   const Patch patches[] = {
      { "width 320",       44, 2, 320, TRUE, MUST_FAIL },
      { "flag B 65535",    32, 2, 0xffff, TRUE, NO_CRASH },
   };

   gsize len;

   /* cut: no image without the flags A, the part read after them (every
      cut of the header, then some of the rest: each is a 640x400 load) */
   for (len = 0; len < a->len; len += len < 1200 ? 1 : 397) {
      GimvImage *image = load_bytes ("mki", a->data, len, FALSE);
      if (len < 1096) g_assert_null (image);
      else assert_size (image, 640, 400);
      if (image) gimv_image_unref (image);
   }
   check_patches ("mki", a, patches, G_N_ELEMENTS (patches));
   check_fuzz ("mki", a, 8, 30);
   g_byte_array_unref (a);
}

static void
test_maki_broken (void)
{
   IN_SUBPROCESS (maki_broken ());
}


/******************************************************************************
 *  MAG (MAKI02)
 ******************************************************************************/

static guint32
mag_palette_rgb (guint c)
{
   return (((c * 37) & 0xff) << 16) | (((c * 11 + 5) & 0xff) << 8) | ((255 - c) & 0xff);
}

static guint
mag_index16 (guint x, guint y)
{
   return (x * 3 + y) % 16;
}


/*
 * A 16 colour MAG image of width x height (width a multiple of 8, from
 * x 0), of which the header shows the first "shown" columns (the file
 * holds whole bytes of 8 pixels: the others are padding).
 * copy_rows_from: the rows from it on have every flag 4 (copy from the
 * line above) in place of their pixels.
 */
static GByteArray *
make_mag16_shown (guint width, guint shown, guint height, guint copy_rows_from)
{
   GByteArray *a = g_byte_array_new ();
   GByteArray *flag_a = g_byte_array_new (), *flag_b = g_byte_array_new ();
   GByteArray *pix = g_byte_array_new ();
   guint fpl = width / 8, y, i, nflags = fpl * height;
   guint8 *flags = g_malloc0 (nflags), *stored = g_malloc0 (nflags);
   gsize header;

   /* flags as decoded: 0 (pixels follow) or 0x44 (copy from above) */
   for (y = 0; y < height; y++)
      for (i = 0; i < fpl; i++)
         flags[y * fpl + i] = y >= copy_rows_from ? 0x44 : 0;
   /* stored XORed with the line above */
   for (i = 0; i < nflags; i++)
      stored[i] = flags[i] ^ (i >= fpl ? flags[i - fpl] : 0);
   /* flag A: a bit for each byte of flag B that is not 0 */
   for (i = 0; i < (nflags + 7) / 8; i++) gimv_test_put8 (flag_a, 0);
   for (i = 0; i < nflags; i++)
      if (stored[i]) {
         flag_a->data[i / 8] |= 0x80 >> (i % 8);
         gimv_test_put8 (flag_b, stored[i]);
      }
   /* pixels: 2 bytes (4 pixels) for each nibble 0 */
   for (y = 0; y < height; y++)
      for (i = 0; i < fpl; i++) {
         guint nib;
         if (flags[y * fpl + i]) continue;
         for (nib = 0; nib < 2; nib++) {
            guint x0 = i * 8 + nib * 4, k;
            for (k = 0; k < 4; k += 2)
               gimv_test_put8 (pix, (mag_index16 (x0 + k, y) << 4) | mag_index16 (x0 + k + 1, y));
         }
      }

   gimv_test_put_bytes (a, "MAKI02  ", 8);
   gimv_test_put_bytes (a, "PC98", 4);
   gimv_test_put_bytes (a, "gimv test         ", 18);
   gimv_test_put_zeros (a, 2);
   gimv_test_put_bytes (a, "comment\x1a", 8);
   header = a->len;
   gimv_test_put8 (a, 0);                   /* the start of the header */
   gimv_test_put8 (a, 0);                   /* machine code */
   gimv_test_put8 (a, 0);
   gimv_test_put8 (a, 0);                   /* screen mode: 16 colours */
   gimv_test_put16le (a, 0);
   gimv_test_put16le (a, 0);
   gimv_test_put16le (a, shown - 1);
   gimv_test_put16le (a, height - 1);
   {
      guint32 off_a = 32 + 16 * 3;
      guint32 off_b = off_a + flag_a->len;
      guint32 off_p = off_b + flag_b->len;
      gimv_test_put32le (a, off_a);
      gimv_test_put32le (a, off_b);
      gimv_test_put32le (a, flag_b->len);
      gimv_test_put32le (a, off_p);
      gimv_test_put32le (a, pix->len);
   }
   g_assert_cmpuint (a->len - header, ==, 32);
   for (i = 0; i < 16; i++) {                /* G, R, B */
      guint32 rgb = mag_palette_rgb (i);
      gimv_test_put8 (a, (rgb >> 8) & 0xff);
      gimv_test_put8 (a, rgb >> 16);
      gimv_test_put8 (a, rgb & 0xff);
   }
   g_byte_array_append (a, flag_a->data, flag_a->len);
   g_byte_array_append (a, flag_b->data, flag_b->len);
   g_byte_array_append (a, pix->data, pix->len);

   g_byte_array_unref (flag_a);
   g_byte_array_unref (flag_b);
   g_byte_array_unref (pix);
   g_free (flags);
   g_free (stored);
   return a;
}


static GByteArray *
make_mag16 (guint width, guint height, guint copy_rows_from)
{
   return make_mag16_shown (width, width, height, copy_rows_from);
}


/* a 256 colour one: width a multiple of 4, every pixel given */
static GByteArray *
make_mag256 (guint width, guint height)
{
   GByteArray *a = g_byte_array_new ();
   guint fpl = width / 4, nflags = fpl * height, x, y, i;

   gimv_test_put_bytes (a, "MAKI02  ", 8);
   gimv_test_put_bytes (a, "PC98", 4);
   gimv_test_put_bytes (a, "gimv test         ", 18);
   gimv_test_put_zeros (a, 2);
   gimv_test_put8 (a, 0x1a);
   gimv_test_put8 (a, 0);
   gimv_test_put8 (a, 0);
   gimv_test_put8 (a, 0);
   gimv_test_put8 (a, 0x80);                /* 256 colours */
   gimv_test_put16le (a, 0);
   gimv_test_put16le (a, 0);
   gimv_test_put16le (a, width - 1);
   gimv_test_put16le (a, height - 1);
   {
      guint32 off_a = 32 + 256 * 3;
      guint32 off_b = off_a + (nflags + 7) / 8;
      gimv_test_put32le (a, off_a);
      gimv_test_put32le (a, off_b);
      gimv_test_put32le (a, 0);
      gimv_test_put32le (a, off_b);
      gimv_test_put32le (a, width * height);
   }
   for (i = 0; i < 256; i++) {
      gimv_test_put8 (a, (i * 3) & 0xff);    /* G */
      gimv_test_put8 (a, i);                 /* R */
      gimv_test_put8 (a, 255 - i);           /* B */
   }
   gimv_test_put_zeros (a, (nflags + 7) / 8);
   for (y = 0; y < height; y++)
      for (x = 0; x < width; x++)
         gimv_test_put8 (a, (x * 16 + y * 7) & 0xff);
   return a;
}


static void
test_mag (void)
{
   GByteArray *a;
   GimvImage *image;
   guint x, y;

   a = make_mag16 (16, 3, 99);
   image = load_array ("mag", a);
   assert_size (image, 16, 3);
   for (y = 0; y < 3; y++)
      for (x = 0; x < 16; x++)
         assert_pixel (image, x, y, mag_palette_rgb (mag_index16 (x, y)));
   gimv_image_unref (image);
   g_byte_array_unref (a);

   /* rows 1 and 2 copied from the one above (flags 4) */
   a = make_mag16 (16, 3, 1);
   image = load_array ("mag", a);
   assert_size (image, 16, 3);
   for (y = 0; y < 3; y++)
      for (x = 0; x < 16; x++)
         assert_pixel (image, x, y, mag_palette_rgb (mag_index16 (x, 0)));
   gimv_image_unref (image);
   g_byte_array_unref (a);

   a = make_mag256 (8, 2);
   image = load_array ("mag", a);
   assert_size (image, 8, 2);
   for (y = 0; y < 2; y++)
      for (x = 0; x < 8; x++) {
         guint c = (x * 16 + y * 7) & 0xff;
         assert_pixel (image, x, y, (c << 16) | (((c * 3) & 0xff) << 8) | (255 - c));
      }
   gimv_image_unref (image);
   g_byte_array_unref (a);

   image = LOAD_LIT ("mag", "MAKI01A ");
   g_assert_null (image);
}


/* a width not a multiple of 8: rows padded in the file (to the right
 * here, as rx is 11 of 16), the image the width of the header */
static void
test_mag_odd_width (void)
{
   GByteArray *a;
   GimvImage *image;
   guint x, y;


   a = make_mag16_shown (16, 12, 3, 99);
   image = load_array ("mag", a);
   assert_size (image, 12, 3);
   for (y = 0; y < 3; y++)
      for (x = 0; x < 12; x++)
         assert_pixel (image, x, y, mag_palette_rgb (mag_index16 (x, y)));
   gimv_image_unref (image);
   g_byte_array_unref (a);
}


static void
mag_broken_header (void)
{
   GByteArray *a = make_mag16 (16, 3, 1);
   const gsize h = 8 + 4 + 18 + 2 + 8;     /* the header */
   const Patch patches[] = {
      { "rx < lx",            h + 8,  2, 0xfff0, FALSE, MUST_FAIL },
      { "dy < uy",            h + 6,  2, 10, FALSE, MUST_FAIL },
      { "65535 x 65535",      h + 8,  4, 0xffffffff, FALSE, MUST_FAIL },
      { "flag B before A",    h + 12, 4, 0x10000000, FALSE, MUST_FAIL },
      { "flag A far away",    h + 12, 4, 0x7fffffff, FALSE, MUST_FAIL },
      { "pixels far away",    h + 24, 4, 0x7fffffff, FALSE, MUST_FAIL },
      { "pixel size 2 GB",    h + 28, 4, 0x7fffffff, FALSE, MUST_FAIL },
      { "pixel size 0",       h + 28, 4, 0, FALSE, MUST_FAIL },
   };

   check_patches ("mag", a, patches, G_N_ELEMENTS (patches));
   g_byte_array_unref (a);
}

static void
test_mag_broken_header (void)
{
   IN_SUBPROCESS (mag_broken_header ());
}


static void
mag_truncated (void)
{
   GByteArray *a = make_mag16 (16, 3, 1);
   check_truncated ("mag", a, a->len, TRUE);
   g_byte_array_unref (a);
}

static void
test_mag_truncated (void)
{
   IN_SUBPROCESS (mag_truncated ());
}


static void
mag_fuzz (void)
{
   GByteArray *a = make_mag16 (16, 3, 1);
   check_fuzz ("mag", a, 8, 300);
   g_byte_array_unref (a);
}

static void
test_mag_fuzz (void)
{
   IN_SUBPROCESS (mag_fuzz ());
}


/******************************************************************************
 *  Sun raster
 ******************************************************************************/

static void
ras_header (GByteArray *a, guint w, guint h, guint depth, guint length,
            guint type, guint maptype, guint maplength)
{
   gimv_test_put32be (a, 0x59a66a95);
   gimv_test_put32be (a, w);
   gimv_test_put32be (a, h);
   gimv_test_put32be (a, depth);
   gimv_test_put32be (a, length);
   gimv_test_put32be (a, type);
   gimv_test_put32be (a, maptype);
   gimv_test_put32be (a, maplength);
}


static guint32
ras_rgb (guint x, guint y)
{
   return ((x * 80) << 16) | ((y * 100 + 20) << 8) | (200 - x * 30 - y);
}


/* 3 x 2, 24 bits (BGR, rows padded to 16 bits) or 32 bits */
static GByteArray *
make_ras24 (guint depth, guint type)
{
   GByteArray *a = g_byte_array_new ();
   guint x, y, row = ((3 * depth + 15) / 16) * 2;

   ras_header (a, 3, 2, depth, row * 2, type, 0, 0);
   for (y = 0; y < 2; y++) {
      guint start = a->len;
      for (x = 0; x < 3; x++) {
         guint32 rgb = ras_rgb (x, y);
         if (depth == 32) gimv_test_put8 (a, 0);
         if (type == 3) {          /* RGB */
            gimv_test_put8 (a, rgb >> 16);
            gimv_test_put8 (a, (rgb >> 8) & 0xff);
            gimv_test_put8 (a, rgb & 0xff);
         } else {                  /* BGR */
            gimv_test_put8 (a, rgb & 0xff);
            gimv_test_put8 (a, (rgb >> 8) & 0xff);
            gimv_test_put8 (a, rgb >> 16);
         }
      }
      while (a->len - start < row) gimv_test_put8 (a, 0xee);
   }
   return a;
}


static const guint8 ras_map[3][4] = {
   { 0x00, 0xff, 0x10, 0x80 },   /* R */
   { 0x00, 0x00, 0x20, 0x80 },   /* G */
   { 0x00, 0x00, 0x30, 0xff },   /* B */
};

static guint32
ras_map_rgb (guint i)
{
   return (ras_map[0][i] << 16) | (ras_map[1][i] << 8) | ras_map[2][i];
}

/* 5 x 2, 8 bits with a map of 4 colours; rle: byte encoded */
static GByteArray *
make_ras8 (gboolean rle)
{
   static const guint8 px[2][6] = {
      { 0, 1, 2, 3, 3, 0xee },
      { 3, 3, 3, 3, 1, 0xee },
   };
   GByteArray *a = g_byte_array_new ();
   guint i;

   ras_header (a, 5, 2, 8, 12, rle ? 2 : 1, 1, 12);
   for (i = 0; i < 3; i++) gimv_test_put_bytes (a, ras_map[i], 4);
   if (!rle) {
      gimv_test_put_bytes (a, px, sizeof (px));
   } else {
      static const guint8 enc[] = {
         0, 1, 2, 0x80, 1, 3,           /* 0 1 2 3 3 */
         0xee,
         0x80, 3, 3,                    /* 3 3 3 3 */
         1, 0xee,
      };
      gimv_test_put_bytes (a, enc, sizeof (enc));
   }
   return a;
}


static guint8
ras_px8 (guint x, guint y)
{
   static const guint8 px[2][5] = { { 0, 1, 2, 3, 3 }, { 3, 3, 3, 3, 1 } };
   return px[y][x];
}


static void
test_ras (void)
{
   GByteArray *a;
   GimvImage *image;
   guint x, y, type, depth;

   for (depth = 24; depth <= 32; depth += 8)
      for (type = 1; type <= 3; type += 2) {
         a = make_ras24 (depth, type);
         image = load_array ("ras", a);
         assert_size (image, 3, 2);
         for (y = 0; y < 2; y++)
            for (x = 0; x < 3; x++)
               assert_pixel (image, x, y, ras_rgb (x, y));
         gimv_image_unref (image);
         g_byte_array_unref (a);
      }

   for (type = 0; type < 2; type++) {
      a = make_ras8 (type);
      image = load_array ("ras", a);
      assert_size (image, 5, 2);
      for (y = 0; y < 2; y++)
         for (x = 0; x < 5; x++)
            assert_pixel (image, x, y, ras_map_rgb (ras_px8 (x, y)));
      gimv_image_unref (image);
      g_byte_array_unref (a);
   }

   /* 1 bit without a map: set bits are black; rows padded to 16 bits */
   a = g_byte_array_new ();
   ras_header (a, 10, 2, 1, 4, 1, 0, 0);
   gimv_test_put8 (a, 0xa5); gimv_test_put8 (a, 0x40);
   gimv_test_put8 (a, 0x00); gimv_test_put8 (a, 0xc0);
   image = load_array ("ras", a);
   assert_size (image, 10, 2);
   assert_pixel (image, 0, 0, 0x000000);
   assert_pixel (image, 1, 0, 0xffffff);
   assert_pixel (image, 2, 0, 0x000000);
   assert_pixel (image, 7, 0, 0x000000);
   assert_pixel (image, 8, 0, 0xffffff);
   assert_pixel (image, 9, 0, 0x000000);
   assert_pixel (image, 0, 1, 0xffffff);
   assert_pixel (image, 8, 1, 0x000000);
   gimv_image_unref (image);
   g_byte_array_unref (a);

   /* 8 bits without a map: grey */
   a = g_byte_array_new ();
   ras_header (a, 2, 1, 8, 2, 1, 0, 0);
   gimv_test_put8 (a, 0x00); gimv_test_put8 (a, 0x7f);
   image = load_array ("ras", a);
   assert_size (image, 2, 1);
   assert_pixel (image, 1, 0, 0x7f7f7f);
   gimv_image_unref (image);
   g_byte_array_unref (a);
}


static void
ras_broken (void)
{
   GByteArray *a = make_ras24 (24, 1), *m = make_ras8 (FALSE), *r = make_ras8 (TRUE);
   const Patch patches[] = {
      { "width 0",          4,  4, 0, TRUE, MUST_FAIL },
      { "height 0",         8,  4, 0, TRUE, MUST_FAIL },
      { "width 2^31",       4,  4, 0x80000000, TRUE, MUST_FAIL },
      { "width 65536",      4,  4, 0x10000, TRUE, MUST_FAIL },
      { "depth 16",         12, 4, 16, TRUE, MUST_FAIL },
      { "depth 4G",         12, 4, 0xffffffff, TRUE, MUST_FAIL },
      { "type 4",           20, 4, 4, TRUE, MUST_FAIL },
      { "map type 3",       24, 4, 3, TRUE, MUST_FAIL },
      { "map length 4G",    28, 4, 0xffffffff, TRUE, MUST_FAIL },
      { "map length 1000",  28, 4, 1000, TRUE, MUST_FAIL },
      { "height 3",         8,  4, 3, TRUE, MUST_FAIL },
   };
   const Patch mpatches[] = {
      { "map of 2 bytes",   28, 4, 2, TRUE, NO_CRASH },
      { "map of 1000 colours", 28, 4, 3000, TRUE, MUST_FAIL },
      { "1 bit with the map", 12, 4, 1, TRUE, NO_CRASH },
   };
   const Patch rpatches[] = {
      { "height 1000 (RLE)", 8, 4, 1000, TRUE, NO_CRASH },
   };

   check_truncated ("ras", a, a->len, TRUE);
   check_truncated ("ras", m, m->len, TRUE);
   /* RLE: what is missing is zeros */
   check_truncated ("ras", r, 32 + 12, FALSE);
   check_patches ("ras", a, patches, G_N_ELEMENTS (patches));
   check_patches ("ras", m, mpatches, G_N_ELEMENTS (mpatches));
   check_patches ("ras", r, rpatches, G_N_ELEMENTS (rpatches));
   check_fuzz ("ras", a, 4, 300);
   check_fuzz ("ras", m, 4, 300);
   check_fuzz ("ras", r, 4, 300);
   g_byte_array_unref (a);
   g_byte_array_unref (m);
   g_byte_array_unref (r);
}

static void
test_ras_broken (void)
{
   IN_SUBPROCESS (ras_broken ());
}


/******************************************************************************
 *  WBMP
 ******************************************************************************/

static GByteArray *
make_wbmp (void)
{
   GByteArray *a = g_byte_array_new ();

   gimv_test_put8 (a, 0);            /* type 0 */
   gimv_test_put8 (a, 0);            /* fix header */
   gimv_test_put8 (a, 0x81);         /* width 130: 1 0000010 */
   gimv_test_put8 (a, 0x02);
   gimv_test_put8 (a, 2);            /* height 2 */
   {
      guint i;
      for (i = 0; i < 17; i++) gimv_test_put8 (a, i == 0 ? 0x80 : i == 16 ? 0x40 : 0);
      for (i = 0; i < 17; i++) gimv_test_put8 (a, 0xff);
   }
   return a;
}


static void
test_wbmp (void)
{
   GByteArray *a = make_wbmp ();
   GimvImage *image;
   gchar *path;

   image = load_array ("wbmp", a);
   assert_size (image, 130, 2);
   assert_pixel (image, 0, 0, 0xffffff);   /* 1 is white */
   assert_pixel (image, 1, 0, 0x000000);
   assert_pixel (image, 128, 0, 0x000000);
   assert_pixel (image, 129, 0, 0xffffff);
   assert_pixel (image, 77, 1, 0xffffff);
   gimv_image_unref (image);

   /* only files named .wbmp (the format has no magic) */
   path = write_file ("WBMP", a->data, a->len);
   image = gimv_test_core_load_image (path, FALSE);
   assert_size (image, 130, 2);
   gimv_image_unref (image);
   g_free (path);
   image = load_array ("bin", a);
   g_assert_null (image);
   g_byte_array_unref (a);
}


static void
wbmp_broken (void)
{
   GByteArray *a = make_wbmp ();
   const Patch patches[] = {
      { "type 1",          0, 1, 1, FALSE, MUST_FAIL },
      { "width 0",         2, 2, 0, TRUE, NO_CRASH },
      { "width endless",   2, 2, 0xffff, TRUE, MUST_FAIL },
      { "height 3",        4, 1, 3, FALSE, MUST_FAIL },
      { "height 1",        4, 1, 1, FALSE, MUST_FAIL },
   };
   GByteArray *big = g_byte_array_new ();

   check_truncated ("wbmp", a, a->len, TRUE);
   check_patches ("wbmp", a, patches, G_N_ELEMENTS (patches));
   check_fuzz ("wbmp", a, 0, 300);

   /* 2^28 x 2^28 (multi-byte integers of 4 bytes) */
   gimv_test_put8 (big, 0); gimv_test_put8 (big, 0);
   gimv_test_put_bytes (big, "\xff\xff\xff\x7f", 4);
   gimv_test_put_bytes (big, "\xff\xff\xff\x7f", 4);
   gimv_test_put_zeros (big, 64);
   g_assert_null (load_array ("wbmp", big));
   g_byte_array_unref (big);
   g_byte_array_unref (a);
}

static void
test_wbmp_broken (void)
{
   IN_SUBPROCESS (wbmp_broken ());
}


/******************************************************************************
 *  PNM
 ******************************************************************************/

static void
test_pnm (void)
{
   GimvImage *image;
   GByteArray *a;

   /* ASCII bitmap, with comments: 1 is black */
   image = LOAD_LIT ("pbm", "P1\n# a comment\n3 2\n1 0 1\n0#x\n1 0\n");
   assert_size (image, 3, 2);
   assert_pixel (image, 0, 0, 0x000000);
   assert_pixel (image, 1, 0, 0xffffff);
   assert_pixel (image, 0, 1, 0xffffff);
   assert_pixel (image, 1, 1, 0x000000);
   gimv_image_unref (image);

   /* ASCII graymap, maxval 15: scaled to 255 */
   image = LOAD_LIT ("pgm", "P2 2 1 15\n0 15\n");
   assert_size (image, 2, 1);
   assert_pixel (image, 0, 0, 0x000000);
   assert_pixel (image, 1, 0, 0xffffff);
   gimv_image_unref (image);

   /* ASCII pixmap, a value over maxval is maxval */
   image = LOAD_LIT ("ppm", "P3\n2 1\n255\n255 0 0  10 20 999\n");
   assert_size (image, 2, 1);
   assert_pixel (image, 0, 0, 0xff0000);
   assert_pixel (image, 1, 0, 0x0a14ff);
   gimv_image_unref (image);

   /* raw bitmap: rows padded to bytes */
   a = g_byte_array_new ();
   gimv_test_put_bytes (a, "P4\n10 2\n", 8);
   gimv_test_put8 (a, 0x80); gimv_test_put8 (a, 0x40);
   gimv_test_put8 (a, 0x7f); gimv_test_put8 (a, 0xff);
   image = load_array ("pbm", a);
   assert_size (image, 10, 2);
   assert_pixel (image, 0, 0, 0x000000);
   assert_pixel (image, 1, 0, 0xffffff);
   assert_pixel (image, 9, 0, 0x000000);
   assert_pixel (image, 8, 0, 0xffffff);
   assert_pixel (image, 0, 1, 0xffffff);
   assert_pixel (image, 1, 1, 0x000000);
   gimv_image_unref (image);
   g_byte_array_unref (a);

   /* raw graymap */
   image = LOAD_LIT ("pgm", "P5 3 1 255\n\x00\x80\xff");
   assert_size (image, 3, 1);
   assert_pixel (image, 1, 0, 0x808080);
   assert_pixel (image, 2, 0, 0xffffff);
   gimv_image_unref (image);

   /* raw pixmap, 16 bits a sample (maxval 65535) */
   image = LOAD_LIT ("ppm", "P6 1 1 65535\n\xff\xff\x80\x00\x00\x00");
   assert_size (image, 1, 1);
   assert_pixel (image, 0, 0, 0xff8000);
   gimv_image_unref (image);

   /* raw pixmap, maxval 100 */
   image = LOAD_LIT ("ppm", "P6\n1 1\n100\n\x64\x32\x00");
   assert_size (image, 1, 1);
   assert_pixel (image, 0, 0, 0xff8000);
   gimv_image_unref (image);

   /* P7 is not PNM here (xvpics) */
   g_assert_null (LOAD_LIT ("pnm", "P7 332\n"));
}


static void
pnm_broken (void)
{
   static const gchar *bad[] = {
      "P6 0 1 255\n",                 /* width 0 */
      "P6 1 0 255\n",                 /* height 0 */
      "P6 1 1 0\n\x01\x02\x03",       /* maxval 0 */
      "P6 1 1 65536\n\x01\x02\x03\x04\x05\x06",
      "P6 99999999999 1 255\n",       /* more than 32 bits */
      "P6 65536 65536 255\n",
      "P6 1 1 255",                    /* no white space after the header */
      "P6 1 1 255\nab",               /* a byte short */
      "P5 2 2 300\n\x01\x01\x01\x01\x01\x01\x01",   /* 7 of 8 bytes */
      "P3 1 1 255\n1 2",              /* a sample short */
      "P3 1 1 255\n1 2 x",
      "P1 2 1\n1 2",
      "P2 -1 1 255\n0",
      "P6 x 1 255\n",
   };
   GByteArray *a = g_byte_array_new ();
   guint i;

   for (i = 0; i < G_N_ELEMENTS (bad); i++) {
      GimvImage *image = load_bytes ("ppm", (const guint8 *) bad[i], strlen (bad[i]), FALSE);
      if (image)
         g_error ("PNM \"%s\": an image of %dx%d", bad[i],
                  gimv_image_width (image), gimv_image_height (image));
   }

   gimv_test_put_bytes (a, "P6\n# c\n4 3\n255\n", 15);
   for (i = 0; i < 4 * 3 * 3; i++) gimv_test_put8 (a, i * 7);
   check_truncated ("ppm", a, a->len, TRUE);
   check_fuzz ("ppm", a, 0, 300);
   g_byte_array_set_size (a, 0);
   gimv_test_put_bytes (a, "P3\n4 1\n255\n1 2 3 4 5 6 7 8 9 10 11 12\n", 38);
   /* "... 11 1" is a whole image too: the last number cut is a number */
   check_truncated ("ppm", a, a->len - 2, FALSE);
   check_fuzz ("ppm", a, 0, 300);
   g_byte_array_unref (a);
}

static void
test_pnm_broken (void)
{
   IN_SUBPROCESS (pnm_broken ());
}


/******************************************************************************
 *  XPM
 ******************************************************************************/

static const gchar xpm_image[] =
   "/* XPM */\n"
   "static char *test[] = {\n"
   "/* width height ncolors cpp */\n"
   "\"4 2 3 1\",\n"
   "\". c None\",\n"
   "\"# c #ff0000\",\n"
   "\"+ c light blue m white\",\n"
   "\".#+#\",\n"
   "\"++..\"\n"
   "};\n";

static const gchar xpm_image2[] =
   "/* XPM */\n"
   "static const char *const t2[] = {\n"
   "\"3 1 2 2\",\n"
   "\"aa g #102030\",\n"
   "\"bb c #00FF00 g #000000\",\n"
   "\"aabbzz\"};\n";


static void
test_xpm (void)
{
   GimvImage *image;
   guchar *p;

   image = load_bytes ("xpm", (const guint8 *) xpm_image, strlen (xpm_image), FALSE);
   assert_size (image, 4, 2);
   g_assert_true (gimv_image_has_alpha (image));
   p = gimv_image_get_pixels (image);
   g_assert_cmpint (p[3], ==, 0);                  /* None: transparent */
   assert_pixel (image, 1, 0, 0xff0000);
   g_assert_cmpint (p[4 + 3], ==, 255);
   assert_pixel (image, 2, 0, 0xadd8e6);           /* "light blue" */
   assert_pixel (image, 0, 1, 0xadd8e6);
   gimv_image_unref (image);

   /* 2 characters a pixel; c over g; an unknown pixel black */
   image = load_bytes ("xpm", (const guint8 *) xpm_image2, strlen (xpm_image2), FALSE);
   assert_size (image, 3, 1);
   g_assert_false (gimv_image_has_alpha (image));
   assert_pixel (image, 0, 0, 0x102030);
   assert_pixel (image, 1, 0, 0x00ff00);
   assert_pixel (image, 2, 0, 0x000000);
   gimv_image_unref (image);
}


static void
xpm_broken (void)
{
   static const gchar *bad[] = {
      "/* XPM */ static char *x[] = { \"0 1 1 1\", \"a c red\", \"a\" };",
      "/* XPM */ static char *x[] = { \"1 1 0 1\", \"a\" };",
      "/* XPM */ static char *x[] = { \"1 1 1 9\", \"a c red\", \"a\" };",
      "/* XPM */ static char *x[] = { \"100000 100000 1 1\", \"a c red\", \"a\" };",
      "/* XPM */ static char *x[] = { \"1 2 1 1\", \"a c red\", \"a\" };",   /* a line short */
      "/* XPM */ static char *x[] = { \"1 1 2 1\", \"a c red\", \"a\" };",   /* a colour short */
      "/* XPM */ static char *x[] = { \"1 1 1 2\", \"a\", \"aa\" };",       /* colour line short */
      "/* XPM */ static char *x[] = { \"one 1 1 1\", \"a c red\", \"a\" };",
      "/* XPM */ static char *x[] = { \"1 1 1 1\", \"a c red\", \"a",       /* not closed */
   };
   GByteArray *a = g_byte_array_new ();
   guint i;

   for (i = 0; i < G_N_ELEMENTS (bad); i++) {
      GimvImage *image = load_bytes ("xpm", (const guint8 *) bad[i], strlen (bad[i]), FALSE);
      if (image && i != G_N_ELEMENTS (bad) - 1)
         g_error ("XPM %u: an image of %dx%d", i,
                  gimv_image_width (image), gimv_image_height (image));
      if (image) gimv_image_unref (image);
   }

   gimv_test_put_bytes (a, xpm_image, strlen (xpm_image));
   /* cut after the start of the first string (before it: no strings, see
    * /image-loader/xpm/no-strings), none or an image, no crash */
   for (i = strchr (xpm_image, '"') - xpm_image + 1; i < a->len; i++) {
      GimvImage *image = load_bytes ("xpm", a->data, i, FALSE);
      if (image) gimv_image_unref (image);
   }
   /* (one without a comment: a comment left open leaves no strings) */
   g_byte_array_set_size (a, 0);
   gimv_test_put_bytes (a, xpm_image2, strlen (xpm_image2));
   check_fuzz ("xpm", a, 9, 300);
   g_byte_array_unref (a);
}

static void
test_xpm_broken (void)
{
   IN_SUBPROCESS (xpm_broken ());
}


static void
xpm_no_strings (void)
{
   static const gchar *bad[] = {
      "/* XPM */\n",
      "/* XPM */ static char *x[] = { };",
   };
   guint i;

   for (i = 0; i < G_N_ELEMENTS (bad); i++)
      g_assert_null (load_bytes ("xpm", (const guint8 *) bad[i], strlen (bad[i]), FALSE));
}

static void
test_xpm_no_strings (void)
{
   IN_SUBPROCESS (xpm_no_strings ());
}


/******************************************************************************
 *  AK5 / MSL (MASL)
 ******************************************************************************/

static void
ak5_put_name (GByteArray *a, const gchar *name)
{
   guint8 b[16] = { 0 };
   memcpy (b, name, MIN (strlen (name), sizeof (b)));
   gimv_test_put_bytes (a, b, sizeof (b));
}


static void
ak5_put_palette (GByteArray *a)
{
   guint i;

   gimv_test_put8 (a, 0);
   ak5_put_name (a, "PAL");
   for (i = 0; i < 16; i++) {
      gimv_test_put8 (a, i == 1 ? 15 : i == 4 ? 7 : 0);    /* R */
      gimv_test_put8 (a, i == 3 ? 15 : i == 4 ? 7 : 0);    /* G */
      gimv_test_put8 (a, i == 2 ? 15 : i == 4 ? 7 : 0);    /* B */
   }
}


static void
ak5_put_cg (GByteArray *a, guint type, const guint8 *data, gsize len)
{
   gimv_test_put8 (a, type);
   ak5_put_name (a, "CG");
   if (type == 2 || type == 4) ak5_put_name (a, "SRC");
   gimv_test_put16le (a, 0);
   gimv_test_put16le (a, 0);
   gimv_test_put16le (a, 639);
   gimv_test_put16le (a, 399);
   gimv_test_put16le (a, len);
   gimv_test_put_bytes (a, data, len);
}


/* the first CG: colour 1 at (0..7, 0..2), colour 3 at (40..47, 3) */
static const guint8 ak5_cg1[] = {
   0x13, 0xff,          /* plane 0 a new byte (colour 1), one line */
   0x09,                /* two more lines of it */
   0x00, 0x05,          /* 5 bytes on */
   0x33, 0xff, 0xff,    /* planes 0 and 1 (colour 3), one line */
   0x0f,
};

/* the second: colour 1 at (0..7, 10) */
static const guint8 ak5_cg2[] = {
   0x06, 0x20,          /* to 3 * 256 + 32 = 800: line 10 */
   0x33, 0xff, 0x00,    /* colour 1 */
   0x0f,
};


static GByteArray *
make_ak5 (gboolean palette, gboolean two)
{
   GByteArray *a = g_byte_array_new ();

   gimv_test_put_bytes (a, "AK5\0", 4);
   gimv_test_put_zeros (a, 4);
   gimv_test_put_bytes (a, "gimv test user                  ", 32);
   if (palette) ak5_put_palette (a);
   ak5_put_cg (a, 1, ak5_cg1, sizeof (ak5_cg1));
   if (two) ak5_put_cg (a, 2, ak5_cg2, sizeof (ak5_cg2));
   return a;
}


static void
check_ak5_first (GimvImage *image, guint32 c1, guint32 c3)
{
   assert_size (image, 640, 400);
   assert_pixel (image, 0, 0, c1);
   assert_pixel (image, 7, 2, c1);
   assert_pixel (image, 8, 0, 0x000000);
   assert_pixel (image, 0, 3, 0x000000);
   assert_pixel (image, 39, 3, 0x000000);
   assert_pixel (image, 40, 3, c3);
   assert_pixel (image, 47, 3, c3);
   assert_pixel (image, 48, 3, 0x000000);
   assert_pixel (image, 0, 10, 0x000000);
   assert_pixel (image, 639, 399, 0x000000);
}


static void
test_ak5 (void)
{
   GByteArray *a;
   GimvImage *image;

   a = make_ak5 (TRUE, FALSE);
   image = load_array ("ak5", a);
   g_assert_false (GIMV_IS_ANIM (image));
   check_ak5_first (image, 0xff0000, 0x00ff00);
   gimv_image_unref (image);
   g_byte_array_unref (a);

   /* no palette: the digital colours (1 blue, 3 magenta) */
   a = make_ak5 (FALSE, FALSE);
   image = load_array ("ak5", a);
   check_ak5_first (image, 0x0000ff, 0xff00ff);
   gimv_image_unref (image);
   g_byte_array_unref (a);

   /* two pictures, not as an animation: the first one */
   a = make_ak5 (TRUE, TRUE);
   image = load_array ("ak5", a);
   g_assert_false (GIMV_IS_ANIM (image));
   check_ak5_first (image, 0xff0000, 0x00ff00);
   gimv_image_unref (image);
   g_byte_array_unref (a);

   /* only the palette: no picture */
   a = g_byte_array_new ();
   gimv_test_put_bytes (a, "AK5\0", 4);
   gimv_test_put_zeros (a, 36);
   ak5_put_palette (a);
   g_assert_null (load_array ("ak5", a));
   g_byte_array_unref (a);
}


static void
test_ak5_animation (void)
{
   GByteArray *a = make_ak5 (TRUE, TRUE);
   GimvImage *image = load_bytes ("ak5", a->data, a->len, TRUE);
   GimvAnim *anim;

   g_assert_true (GIMV_IS_ANIM (image));
   anim = GIMV_ANIM (image);
   g_assert_cmpint (gimv_anim_get_length (anim), ==, 2);
   g_assert_cmpint (gimv_anim_get_interval (anim), >, 0);
   check_ak5_first (image, 0xff0000, 0x00ff00);

   /* the second is a difference over the first */
   g_assert_cmpint (gimv_anim_iterate (anim), ==, 1);
   assert_pixel (image, 0, 0, 0xff0000);
   assert_pixel (image, 0, 10, 0xff0000);
   assert_pixel (image, 7, 10, 0xff0000);
   assert_pixel (image, 8, 10, 0x000000);
   g_assert_cmpint (gimv_anim_iterate (anim), ==, -1);

   g_assert_true (gimv_anim_seek (anim, 0));
   assert_pixel (image, 0, 10, 0x000000);
   g_assert_true (gimv_anim_seek (anim, 1));
   assert_pixel (image, 0, 10, 0xff0000);
   g_assert_false (gimv_anim_seek (anim, 2));

   gimv_image_unref (image);
   g_byte_array_unref (a);

   /* one picture: no animation even so */
   a = make_ak5 (TRUE, FALSE);
   image = load_bytes ("ak5", a->data, a->len, TRUE);
   g_assert_false (GIMV_IS_ANIM (image));
   gimv_image_unref (image);
   g_byte_array_unref (a);
}


/* an .msl: a scenario (here none) and its files, an .ak5 among them */
static GByteArray *
make_msl (void)
{
   GByteArray *ak5 = make_ak5 (TRUE, TRUE), *a = g_byte_array_new ();

   gimv_test_put_bytes (a, "MSL\0", 4);
   gimv_test_put_zeros (a, 4);
   gimv_test_put16le (a, 0);              /* no scenario */
   gimv_test_put32le (a, 10);             /* a file that is neither */
   gimv_test_put_bytes (a, "XYZ\0abcdef", 10);
   gimv_test_put32le (a, ak5->len);
   g_byte_array_append (a, ak5->data, ak5->len);
   g_byte_array_unref (ak5);
   return a;
}


static void
test_msl (void)
{
   GByteArray *a = make_msl ();
   GimvImage *image;

   image = load_array ("msl", a);
   g_assert_false (GIMV_IS_ANIM (image));
   check_ak5_first (image, 0xff0000, 0x00ff00);
   gimv_image_unref (image);

   /* no MASL scenario: the pictures one after another */
   image = load_bytes ("msl", a->data, a->len, TRUE);
   g_assert_true (GIMV_IS_ANIM (image));
   g_assert_cmpint (gimv_anim_get_length (GIMV_ANIM (image)), ==, 2);
   gimv_image_unref (image);
   g_byte_array_unref (a);
}


static void
ak5_broken (void)
{
   GByteArray *a = make_ak5 (TRUE, TRUE), *m = make_msl ();
   const gsize cg = 40 + 1 + 16 + 48;
   const Patch patches[] = {
      { "CG size 65535",       cg + 1 + 16 + 8, 2, 0xffff, FALSE, MUST_FAIL },
      { "entry type 9",        cg, 1, 9, FALSE, MUST_FAIL },
      { "op 0x4f",             cg + 1 + 16 + 10, 1, 0x4f, FALSE, NO_CRASH },
      { "skip far",            cg + 1 + 16 + 10 + 3, 2, 0xfeff, FALSE, NO_CRASH },
      { "op 0x07, 255 lines",  cg + 1 + 16 + 10, 2, 0xff17, FALSE, NO_CRASH },
      { "op 0x07, 0 lines",    cg + 1 + 16 + 10, 2, 0x0017, FALSE, NO_CRASH },
      { "op 0x2f, 255",        cg + 1 + 16 + 10, 2, 0xff2f, FALSE, NO_CRASH },
      { "op 0x2f, 4",          cg + 1 + 16 + 10, 2, 0x042f, FALSE, NO_CRASH },
   };
   const Patch mpatches[] = {
      { "scenario 65535",   8, 2, 0xffff, FALSE, MUST_FAIL },
      { "file size 4G",     10, 4, 0xffffffff, FALSE, MUST_FAIL },
      { "scenario 3",       8, 2, 3, FALSE, NO_CRASH },
   };
   guint i;

   check_truncated ("ak5", a, cg + 1 + 16 + 10, FALSE);
   check_truncated ("msl", m, m->len - a->len + cg + 1 + 16 + 10, FALSE);
   check_patches ("ak5", a, patches, G_N_ELEMENTS (patches));
   check_patches ("msl", m, mpatches, G_N_ELEMENTS (mpatches));
   check_fuzz ("ak5", a, 4, 300);
   check_fuzz ("msl", m, 4, 300);

   /* the same as animations */
   for (i = 0; i < a->len; i += 7) {
      GimvImage *image = load_bytes ("ak5", a->data, i, TRUE);
      if (image && GIMV_IS_ANIM (image))
         while (gimv_anim_iterate (GIMV_ANIM (image)) >= 0);
      if (image) gimv_image_unref (image);
   }
   g_byte_array_unref (a);
   g_byte_array_unref (m);
}

static void
test_ak5_broken (void)
{
   IN_SUBPROCESS (ak5_broken ());
}


/******************************************************************************
 *  main
 ******************************************************************************/

int
main (int argc, char *argv[])
{
   const gchar *home;
   gint ret;

   g_test_init (&argc, &argv, NULL);
   /* the decoders give their pixels as they are (the shape of the pixels
      of the machines: /image-loader/dot-aspect) */
   g_setenv ("GIMV_LOADER_DOT_ASPECT", "0", TRUE);

   home = gimv_test_core_init ();
   img_dir = g_build_filename (home, "images", NULL);
   gimv_test_core_load_plugin ("image_loader", "gimv_zim_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_pi_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_pic_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_mb_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_maki_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_mag_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_ras_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_wbmp_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_pnm_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_xpm_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_ak5_loader");

   g_test_add_func ("/image-loader/zim", test_zim);
   g_test_add_func ("/image-loader/zim/compressed", test_zim_compressed);
   g_test_add_func ("/image-loader/zim/broken", test_zim_broken);
   g_test_add_func ("/image-loader/pi", test_pi);
   g_test_add_func ("/image-loader/pi/broken", test_pi_broken);
   g_test_add_func ("/image-loader/pic", test_pic);
   g_test_add_func ("/image-loader/pic/broken", test_pic_broken);
   g_test_add_func ("/image-loader/dot-aspect", test_dot_aspect);
   g_test_add_func ("/image-loader/mb", test_mb);
   g_test_add_func ("/image-loader/mb/broken", test_mb_broken);
   g_test_add_func ("/image-loader/mb2", test_mb2);
   g_test_add_func ("/image-loader/mb2/comment", test_mb2_comment);
   g_test_add_func ("/image-loader/old-comments", test_old_comments);
   g_test_add_func ("/image-loader/mb2/broken", test_mb2_broken);
   g_test_add_func ("/image-loader/maki", test_maki);
   g_test_add_func ("/image-loader/maki/broken", test_maki_broken);
   g_test_add_func ("/image-loader/maki/comment", test_maki_comment);
   g_test_add_func ("/image-loader/name-note", test_name_note);
   g_test_add_func ("/image-loader/mag", test_mag);
   g_test_add_func ("/image-loader/mag/odd-width", test_mag_odd_width);
   g_test_add_func ("/image-loader/mag/broken-header", test_mag_broken_header);
   g_test_add_func ("/image-loader/mag/truncated", test_mag_truncated);
   g_test_add_func ("/image-loader/mag/fuzz", test_mag_fuzz);
   g_test_add_func ("/image-loader/ras", test_ras);
   g_test_add_func ("/image-loader/ras/broken", test_ras_broken);
   g_test_add_func ("/image-loader/wbmp", test_wbmp);
   g_test_add_func ("/image-loader/wbmp/broken", test_wbmp_broken);
   g_test_add_func ("/image-loader/pnm", test_pnm);
   g_test_add_func ("/image-loader/pnm/broken", test_pnm_broken);
   g_test_add_func ("/image-loader/xpm", test_xpm);
   g_test_add_func ("/image-loader/xpm/broken", test_xpm_broken);
   g_test_add_func ("/image-loader/xpm/no-strings", test_xpm_no_strings);
   g_test_add_func ("/image-loader/ak5", test_ak5);
   g_test_add_func ("/image-loader/ak5/animation", test_ak5_animation);
   g_test_add_func ("/image-loader/msl", test_msl);
   g_test_add_func ("/image-loader/ak5/broken", test_ak5_broken);

   ret = g_test_run ();

   g_free (img_dir);
   gimv_test_core_finish ();
   return ret;
}
