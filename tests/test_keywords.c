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
 * The keywords other programs wrote (src/indexer/gimv_index_keywords.c):
 * XMP in JPEG, PNG, WebP and TIFF, IPTC Keywords, the sidecar files, and
 * broken files.
 */

#include <string.h>
#include <glib/gstdio.h>

#include "gimv_test.h"
#include "gimv_index_keywords.h"

static gchar *dir;

#define XMP_HEAD \
   "<?xpacket begin=\"\xef\xbb\xbf\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n" \
   "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n" \
   " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
#define XMP_TAIL \
   " </rdf:RDF>\n</x:xmpmeta>\n<?xpacket end=\"w\"?>"

static const gchar xmp_flat[] =
   XMP_HEAD
   "  <rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
   "     xmlns:lr=\"http://ns.adobe.com/lightroom/1.0/\">\n"
   "   <dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">Not a keyword</rdf:li></rdf:Alt></dc:title>\n"
   "   <dc:subject><rdf:Bag>\n"
   "    <rdf:li>京都</rdf:li>\n"
   "    <rdf:li> Temple &amp; Garden </rdf:li>\n"
   "    <rdf:li>京都</rdf:li>\n"
   "    <rdf:li></rdf:li>\n"
   "   </rdf:Bag></dc:subject>\n"
   "   <lr:hierarchicalSubject><rdf:Bag>\n"
   "    <rdf:li>Places|Japan|Kyoto</rdf:li>\n"
   "   </rdf:Bag></lr:hierarchicalSubject>\n"
   "  </rdf:Description>\n"
   XMP_TAIL;

/* another prefix for dc, only the hierarchy of digiKam */
static const gchar xmp_hier[] =
   XMP_HEAD
   "  <rdf:Description rdf:about=\"\" xmlns:d=\"http://purl.org/dc/elements/1.1/\"\n"
   "     xmlns:digiKam=\"http://www.digikam.org/ns/1.0/\">\n"
   "   <digiKam:TagsList><rdf:Seq>\n"
   "    <rdf:li>Events/Wedding</rdf:li>\n"
   "    <rdf:li>People/Alice</rdf:li>\n"
   "   </rdf:Seq></digiKam:TagsList>\n"
   "  </rdf:Description>\n"
   XMP_TAIL;

static const gchar xmp_other[] =
   XMP_HEAD
   "  <rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
   "   <dc:subject><rdf:Bag><rdf:li>sidecar</rdf:li></rdf:Bag></dc:subject>\n"
   "  </rdf:Description>\n"
   XMP_TAIL;


static gchar *
joined (GPtrArray *a)
{
   GString *s = g_string_new (NULL);
   guint i;

   for (i = 0; a && i < a->len; i++) {
      if (i) g_string_append (s, ", ");
      g_string_append (s, g_ptr_array_index (a, i));
   }
   if (a) g_ptr_array_unref (a);
   return g_string_free (s, FALSE);
}

#define assert_keywords(path, expected)                                \
   G_STMT_START {                                                      \
      gchar *_k = joined (gimv_index_keywords_read (path));            \
      g_assert_cmpstr (_k, ==, (expected));                            \
      g_free (_k);                                                     \
   } G_STMT_END


static gchar *
file_in (const gchar *name)
{
   return g_build_filename (dir, name, NULL);
}


static void
write_bytes (const gchar *path, GByteArray *b)
{
   g_assert_true (g_file_set_contents (path, (const gchar *) b->data, b->len, NULL));
   g_byte_array_unref (b);
}


static void
put16be (GByteArray *b, guint v) { guint8 x[2] = { v >> 8, v }; g_byte_array_append (b, x, 2); }
static void
put32be (GByteArray *b, guint32 v) { guint8 x[4] = { v >> 24, v >> 16, v >> 8, v }; g_byte_array_append (b, x, 4); }
static void
put32le (GByteArray *b, guint32 v) { guint8 x[4] = { v, v >> 8, v >> 16, v >> 24 }; g_byte_array_append (b, x, 4); }
static void
put16le (GByteArray *b, guint v) { guint8 x[2] = { v, v >> 8 }; g_byte_array_append (b, x, 2); }
static void
put (GByteArray *b, const void *d, gsize n) { g_byte_array_append (b, d, n); }


/* a JPEG with segments (marker, data) before a frame header; no image data */
static GByteArray *
jpeg_start (void)
{
   GByteArray *b = g_byte_array_new ();
   put (b, "\xff\xd8", 2);
   return b;
}

static void
jpeg_segment (GByteArray *b, guint marker, const void *d, gsize n)
{
   guint8 m[2] = { 0xff, marker };
   put (b, m, 2);
   put16be (b, n + 2);
   put (b, d, n);
}

static void
jpeg_end (GByteArray *b)
{
   static const guint8 sof[] = { 8, 0, 4, 0, 4, 1, 1, 0x11, 0 };
   jpeg_segment (b, 0xc0, sof, sizeof sof);
   put (b, "\xff\xd9", 2);
}

static void
jpeg_xmp (GByteArray *b, const gchar *xmp)
{
   GByteArray *d = g_byte_array_new ();
   put (d, "http://ns.adobe.com/xap/1.0/", 29);   /* with the NUL */
   put (d, xmp, strlen (xmp));
   jpeg_segment (b, 0xe1, d->data, d->len);
   g_byte_array_unref (d);
}


static void
test_xmp_parse (void)
{
   GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
   gchar *k;

   gimv_index_keywords_from_xmp (xmp_flat, -1, a);
   k = joined (a);
   /* dc:subject only (not the title, not the hierarchy), each once */
   g_assert_cmpstr (k, ==, "京都, Temple & Garden");
   g_free (k);

   a = g_ptr_array_new_with_free_func (g_free);
   gimv_index_keywords_from_xmp (xmp_hier, -1, a);
   k = joined (a);
   g_assert_cmpstr (k, ==, "Wedding, Alice");
   g_free (k);

   /* broken: what was read before the error */
   a = g_ptr_array_new_with_free_func (g_free);
   {
      gchar *broken = g_strndup (xmp_flat, strstr (xmp_flat, "</rdf:Bag>") - xmp_flat);
      gchar *t = g_strconcat (broken, "<rdf:li>a & b</rdf:li>", NULL);
      gimv_index_keywords_from_xmp (t, -1, a);
      g_free (broken);
      g_free (t);
   }
   k = joined (a);
   g_assert_cmpstr (k, ==, "京都, Temple & Garden");
   g_free (k);

   /* not XML at all */
   a = g_ptr_array_new_with_free_func (g_free);
   gimv_index_keywords_from_xmp ("\x01\x02 garbage", -1, a);
   g_assert_cmpuint (a->len, ==, 0);
   g_ptr_array_unref (a);
}


static void
test_jpeg (void)
{
   gchar *p = file_in ("x.jpg");
   GByteArray *b = jpeg_start ();

   jpeg_segment (b, 0xe0, "JFIF\0\1\1\0\0\1\0\1\0\0", 14);
   jpeg_xmp (b, xmp_flat);
   jpeg_end (b);
   write_bytes (p, b);
   assert_keywords (p, "京都, Temple & Garden");
   g_free (p);
}


/* IPTC Keywords in Photoshop's APP13: UTF-8 when said so, else Shift_JIS */
static void
test_iptc (void)
{
   gchar *p = file_in ("iptc.jpg");
   GByteArray *b = jpeg_start (), *ps = g_byte_array_new (), *iim = g_byte_array_new ();
   gchar *sjis = g_convert ("桜", -1, "CP932", "UTF-8", NULL, NULL, NULL);

   /* without a charset: Shift_JIS */
   put (iim, "\x1c\x02\x19", 3); put16be (iim, strlen (sjis)); put (iim, sjis, strlen (sjis));
   put (iim, "\x1c\x02\x19", 3); put16be (iim, 5); put (iim, "Sakura", 5);   /* "Sakur" */
   put (ps, "Photoshop 3.0", 14);
   put (ps, "8BIM", 4); put16be (ps, 0x0404); put (ps, "\0\0", 2);
   put32be (ps, iim->len); put (ps, iim->data, iim->len);
   if (iim->len & 1) put (ps, "", 1);
   jpeg_segment (b, 0xed, ps->data, ps->len);
   jpeg_end (b);
   write_bytes (p, b);
   assert_keywords (p, "桜, Sakur");
   g_byte_array_unref (ps);
   g_byte_array_unref (iim);
   g_free (sjis);
   g_free (p);
}


static guint32
crc_of (const guint8 *d, gsize n)
{
   /* not checked by the reader: any value */
   return n;
}

static void
png_chunk (GByteArray *b, const gchar *type, const void *d, gsize n)
{
   put32be (b, n);
   put (b, type, 4);
   put (b, d, n);
   put32be (b, crc_of (d, n));
}


static void
test_png (void)
{
   gchar *p = file_in ("x.png");
   GByteArray *b = g_byte_array_new (), *t = g_byte_array_new ();
   static const guint8 ihdr[13] = { 0, 0, 0, 4, 0, 0, 0, 4, 8, 2, 0, 0, 0 };

   put (b, "\x89PNG\r\n\x1a\n", 8);
   png_chunk (b, "IHDR", ihdr, sizeof ihdr);
   put (t, "XML:com.adobe.xmp", 18);
   put (t, "\0\0", 2);     /* not compressed */
   put (t, "\0", 1);       /* language */
   put (t, "\0", 1);       /* translated keyword */
   put (t, xmp_hier, strlen (xmp_hier));
   png_chunk (b, "iTXt", t->data, t->len);
   png_chunk (b, "IDAT", "", 0);
   png_chunk (b, "IEND", "", 0);
   write_bytes (p, b);
   g_byte_array_unref (t);
   assert_keywords (p, "Wedding, Alice");
   g_free (p);
}


static void
test_webp (void)
{
   gchar *p = file_in ("x.webp");
   GByteArray *b = g_byte_array_new ();
   gsize n = strlen (xmp_flat);

   put (b, "RIFF", 4); put32le (b, 0); put (b, "WEBP", 4);
   put (b, "VP8X", 4); put32le (b, 10); put (b, "\x04\0\0\0\3\0\0\3\0\0", 10);
   put (b, "XMP ", 4); put32le (b, n); put (b, xmp_flat, n);
   if (n & 1) put (b, "", 1);
   write_bytes (p, b);
   assert_keywords (p, "京都, Temple & Garden");
   g_free (p);
}


/* little-endian TIFF, IFD 0 with the tag 700 */
static void
test_tiff (void)
{
   gchar *p = file_in ("x.tif");
   GByteArray *b = g_byte_array_new ();
   gsize n = strlen (xmp_flat);

   put (b, "II*\0", 4); put32le (b, 8);
   put16le (b, 1);
   put16le (b, 700); put16le (b, 1); put32le (b, n); put32le (b, 8 + 2 + 12 + 4);
   put32le (b, 0);       /* no next IFD */
   put (b, xmp_flat, n);
   write_bytes (p, b);
   assert_keywords (p, "京都, Temple & Garden");
   g_free (p);
}


/* "x.jpg.xmp" and "x.xmp" next to the image, with its own keywords */
static void
test_sidecar (void)
{
   gchar *p = file_in ("side.jpg"), *s1 = file_in ("side.jpg.xmp"), *s2 = file_in ("side.xmp");
   GByteArray *b = jpeg_start ();

   jpeg_xmp (b, xmp_flat);
   jpeg_end (b);
   write_bytes (p, b);
   gimv_test_write_string (s1, xmp_other);
   assert_keywords (p, "京都, Temple & Garden, sidecar");
   g_remove (s1);
   gimv_test_write_string (s2, xmp_hier);
   /* the hierarchy only when there are no flat keywords at all */
   assert_keywords (p, "京都, Temple & Garden");
   g_remove (s2);

   /* not an image, a sidecar alone */
   {
      gchar *q = file_in ("alone.png"), *qs = file_in ("alone.xmp");
      gimv_test_write_string (q, "not a png");
      gimv_test_write_string (qs, xmp_hier);
      assert_keywords (q, "Wedding, Alice");
      g_free (q);
      g_free (qs);
   }

   g_assert_true (gimv_index_is_sidecar_name ("a.jpg.xmp"));
   g_assert_true (gimv_index_is_sidecar_name ("A.XMP"));
   g_assert_false (gimv_index_is_sidecar_name (".xmp"));
   g_assert_false (gimv_index_is_sidecar_name ("a.jpg"));
   g_free (p);
   g_free (s1);
   g_free (s2);
}


/* UTF-8 read as Shift_JIS (Windows Photo Gallery): put right, or left
 * out when bytes were lost; real half-width text kept */
static void
test_unmangle (void)
{
   static const struct { const gchar *in, *out; } cases[] = {
      { "繧ｪ繝ｪ繧ｸ繝翫Ν", "オリジナル" },
      { "蝠・･ｭ",          NULL },
      { "ｶﾒﾗ",            "ｶﾒﾗ" },
      { "ﾃｽﾄ画像",        "ﾃｽﾄ画像" },
      { "ｱ画像",          "ｱ画像" },
      { "京都",           "京都" },
      { "Temple",         "Temple" },
   };
   guint i;

   for (i = 0; i < G_N_ELEMENTS (cases); i++) {
      gchar *r = gimv_index_keyword_unmangle (cases[i].in);
      g_assert_cmpstr (r, ==, cases[i].out);
      g_free (r);
   }
}


/* as a JPEG of 2010 had it: IPTC in UTF-8 with a broken keyword, the XMP
 * dc:subject mangled, the right ones in MicrosoftPhoto */
static void
test_photo_gallery (void)
{
   gchar *p = file_in ("gallery.jpg");
   GByteArray *b = jpeg_start (), *ps = g_byte_array_new (), *iim = g_byte_array_new ();
   static const gchar xmp[] =
      XMP_HEAD
      "  <rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
      "     xmlns:MicrosoftPhoto=\"http://ns.microsoft.com/photo/1.0/\">\n"
      "   <dc:subject><rdf:Bag><rdf:li>繧ｪ繝ｪ繧ｸ繝翫Ν</rdf:li><rdf:li>蝠・･ｭ</rdf:li></rdf:Bag></dc:subject>\n"
      "   <MicrosoftPhoto:LastKeywordXMP><rdf:Bag><rdf:li>オリジナル</rdf:li>"
      "<rdf:li>商業</rdf:li></rdf:Bag></MicrosoftPhoto:LastKeywordXMP>\n"
      "  </rdf:Description>\n"
      XMP_TAIL;

   put (iim, "\x1c\x02\x19", 3); put16be (iim, 15); put (iim, "オリジナル", 15);
   put (iim, "\x1c\x02\x19", 3); put16be (iim, 6); put (iim, "\xe5\x95\x81\x45\xa5\xad", 6);
   put (ps, "Photoshop 3.0", 14);
   put (ps, "8BIM", 4); put16be (ps, 0x0404); put (ps, "\0\0", 2);
   put32be (ps, iim->len); put (ps, iim->data, iim->len);
   if (iim->len & 1) put (ps, "", 1);
   jpeg_segment (b, 0xed, ps->data, ps->len);
   jpeg_xmp (b, xmp);
   jpeg_end (b);
   write_bytes (p, b);
   assert_keywords (p, "オリジナル, 商業");
   g_byte_array_unref (ps);
   g_byte_array_unref (iim);
   g_free (p);
}


/* nothing, and broken files: no crash, nothing read */
static void
test_broken (void)
{
   static const gchar *const contents[] = {
      "", "\xff\xd8", "\xff\xd8\xff\xe1\xff\xff", "\x89PNG\r\n\x1a\n\x7f\xff\xff\xffiTXt",
      "RIFF\0\0\0\0WEBPXMP \xff\xff\xff\x7f", "II*\0\xff\xff\xff\x7f", "MM\0*\0\0\0\x08\xff\xff",
   };
   static const gsize lens[] = { 0, 2, 6, 16, 20, 8, 10 };
   guint i;

   for (i = 0; i < G_N_ELEMENTS (contents); i++) {
      gchar *name = g_strdup_printf ("broken%u.img", i);
      gchar *p = file_in (name);
      g_assert_true (g_file_set_contents (p, contents[i], lens[i], NULL));
      assert_keywords (p, "");
      g_free (p);
      g_free (name);
   }
   {
      gchar *p = file_in ("plain.jpg");
      GByteArray *b = jpeg_start ();
      jpeg_end (b);
      write_bytes (p, b);
      g_assert_null (gimv_index_keywords_read (p));
      g_free (p);
   }
   g_assert_null (gimv_index_keywords_read ("/nonexistent/x.jpg"));
}


int
main (int argc, char *argv[])
{
   gint ret;

   g_test_init (&argc, &argv, NULL);
   dir = gimv_test_mkdtemp ();

   g_test_add_func ("/keywords/xmp-parse", test_xmp_parse);
   g_test_add_func ("/keywords/jpeg", test_jpeg);
   g_test_add_func ("/keywords/iptc", test_iptc);
   g_test_add_func ("/keywords/png", test_png);
   g_test_add_func ("/keywords/webp", test_webp);
   g_test_add_func ("/keywords/tiff", test_tiff);
   g_test_add_func ("/keywords/sidecar", test_sidecar);
   g_test_add_func ("/keywords/unmangle", test_unmangle);
   g_test_add_func ("/keywords/photo-gallery", test_photo_gallery);
   g_test_add_func ("/keywords/broken", test_broken);

   ret = g_test_run ();
   gimv_test_rm_rf (dir);
   g_free (dir);
   return ret;
}
