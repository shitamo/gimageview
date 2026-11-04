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

   home = gimv_test_core_init ();
   img_dir = g_build_filename (home, "images", NULL);
   gimv_test_core_load_plugin ("image_loader", "gimv_zim_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_mag_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_ras_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_wbmp_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_pnm_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_xpm_loader");
   gimv_test_core_load_plugin ("image_loader", "gimv_ak5_loader");

   g_test_add_func ("/image-loader/zim", test_zim);
   g_test_add_func ("/image-loader/zim/compressed", test_zim_compressed);
   g_test_add_func ("/image-loader/zim/broken", test_zim_broken);
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
