/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * The tests of plugins/archiver/lha-lh1.c: the -lh1- method of LHarc 1.x
 * (libarchive does not read it) and the LHA headers.
 */

#include <string.h>
#include <glib.h>

#include "lha-lh1.h"


static void
roundtrip (const guchar *data, gsize len)
{
   gsize plen;
   guchar *packed = gimv_lh1_encode (data, len, &plen);
   guchar *out = g_malloc (len + 1);

   g_assert_true (gimv_lh1_decode (packed, plen, out, len));
   g_assert_cmpmem (out, len, data, len);
   g_free (packed);
   g_free (out);
}


static void
test_roundtrip (void)
{
   GRand *rand = g_rand_new_with_seed (1);
   GString *text = g_string_new (NULL);
   guchar *buf;
   gsize i;

   roundtrip ((const guchar *) "", 0);
   roundtrip ((const guchar *) "a", 1);
   roundtrip ((const guchar *) "    spaces at the start: the window is spaces", 45);
   roundtrip ((const guchar *) "abcabcabcabcabcabcabcabcabc", 27);

   /* text: copies of all lengths and distances */
   for (i = 0; text->len < 60000; i++)
      g_string_append_printf (text, "line %u of %u, %s\n", (guint) (i % 97), (guint) (i % 13),
                              (i % 7) ? "the same again" : "something else");
   roundtrip ((const guchar *) text->str, text->len);

   /* runs (the longest copies) and random bytes (no copies; more than
      0x8000 symbols: the tree is made again) */
   buf = g_malloc (100000);
   memset (buf, 0x41, 5000);
   for (i = 5000; i < 100000; i++) buf[i] = g_rand_int (rand);
   roundtrip (buf, 100000);

   g_free (buf);
   g_string_free (text, TRUE);
   g_rand_free (rand);
}


static void
test_broken (void)
{
   GRand *rand = g_rand_new_with_seed (2);
   const gchar *s = "Broken data must not crash: Broken data must not crash.";
   gsize plen, i, len = strlen (s);
   guchar *packed = gimv_lh1_encode ((const guchar *) s, len, &plen);
   guchar out[256];

   /* cut short */
   for (i = 0; i + 1 < plen; i++)
      g_assert_false (gimv_lh1_decode (packed, i, out, len));

   /* random data: anything but a crash */
   for (i = 0; i < 2000; i++) {
      guchar junk[64];
      guint k;
      for (k = 0; k < sizeof (junk); k++) junk[k] = g_rand_int (rand);
      gimv_lh1_decode (junk, g_rand_int_range (rand, 0, sizeof (junk)), out, sizeof (out));
   }

   g_free (packed);
   g_rand_free (rand);
}


static void
test_crc (void)
{
   g_assert_cmpuint (gimv_lha_crc16 ((const guchar *) "123456789", 9), ==, 0xbb3d);
   g_assert_cmpuint (gimv_lha_crc16 (NULL, 0), ==, 0);
}


static void
put16 (GByteArray *a, guint v)
{
   guchar b[2] = { v & 0xff, v >> 8 };
   g_byte_array_append (a, b, 2);
}


static void
put32 (GByteArray *a, guint32 v)
{
   put16 (a, v & 0xffff);
   put16 (a, v >> 16);
}


/* an archive of one member, data packed, with a header of level */
static GByteArray *
make_member (guint level, const guchar *data, gsize len, guint16 crc)
{
   GByteArray *a = g_byte_array_new ();
   const gchar *name = "PIC.MAG";
   guint namelen = strlen (name), start;

   if (level == 2) {
      put16 (a, 0);                  /* the size, later */
   } else {
      guchar z[2] = { 0, 0 };        /* the size and the checksum, later */
      g_byte_array_append (a, z, 2);
   }
   g_byte_array_append (a, (const guchar *) "-lh1-", 5);
   start = a->len;
   put32 (a, 0);                     /* packed, later */
   put32 (a, 1234);                  /* original */
   put32 (a, 0);                     /* time */
   {
      guchar b[2] = { level == 2 ? 0 : 0x20, level };
      g_byte_array_append (a, b, 2);
   }
   if (level == 2) {
      guchar os = 'M';
      put16 (a, crc);
      g_byte_array_append (a, &os, 1);
      /* an extended header: the file name */
      put16 (a, 3 + namelen);
      {
         guchar t = 1;
         g_byte_array_append (a, &t, 1);
      }
      g_byte_array_append (a, (const guchar *) name, namelen);
      put16 (a, 0);
      a->data[0] = a->len & 0xff;
      a->data[1] = a->len >> 8;
      g_byte_array_append (a, data, len);
      a->data[start] = len & 0xff;
      a->data[start + 1] = len >> 8;
   } else {
      guchar n = namelen;
      guint ext = 0;
      g_byte_array_append (a, &n, 1);
      g_byte_array_append (a, (const guchar *) name, namelen);
      put16 (a, crc);
      if (level == 1) {
         guchar os = 'M';
         g_byte_array_append (a, &os, 1);
         put16 (a, 7);               /* the next: 7 bytes */
      }
      a->data[0] = a->len - 2;
      if (level == 1) {
         /* an extended header of 7 bytes (a comment), then none */
         guchar e[5] = { 0x3f, 'a', 'b', 'c', 'd' };
         g_byte_array_append (a, e, 5);
         put16 (a, 0);
         ext = 7;
      }
      g_byte_array_append (a, data, len);
      a->data[start] = (len + ext) & 0xff;
      a->data[start + 1] = (len + ext) >> 8;
   }
   {
      guchar end = 0;
      g_byte_array_append (a, &end, 1);
   }
   return a;
}


static void
test_headers (void)
{
   guint level;
   const guchar packed[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

   for (level = 0; level <= 2; level++) {
      GByteArray *a = make_member (level, packed, sizeof (packed), 0xabcd);
      GimvLhaMember m;
      gsize i;

      g_assert_true (gimv_lha_member (a->data, a->len, 0, &m));
      g_assert_cmpstr (m.method, ==, "-lh1-");
      g_assert_cmpuint (m.packed, ==, sizeof (packed));
      g_assert_cmpuint (m.original, ==, 1234);
      g_assert_true (m.has_crc);
      g_assert_cmpuint (m.crc, ==, 0xabcd);
      g_assert_cmpmem (a->data + m.data, m.packed, packed, sizeof (packed));

      /* cut short: not a member, no crash */
      for (i = 0; i < a->len - 1; i++)
         gimv_lha_member (a->data, i, 0, &m);
      g_assert_false (gimv_lha_member (a->data, a->len - 2, 0, &m));
      g_byte_array_free (a, TRUE);
   }
}


int
main (int argc, char **argv)
{
   g_test_init (&argc, &argv, NULL);

   g_test_add_func ("/lha/lh1/roundtrip", test_roundtrip);
   g_test_add_func ("/lha/lh1/broken", test_broken);
   g_test_add_func ("/lha/crc", test_crc);
   g_test_add_func ("/lha/headers", test_headers);

   return g_test_run ();
}
