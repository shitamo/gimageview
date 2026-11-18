/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * The tests of src/charset.c and src/japanese.c: names and texts that are
 * not in the charset of the system (Shift_JIS or EUC-JP of the old days in
 * a UTF-8 system) are guessed, or shown as %XX.
 */

#include <locale.h>
#include <string.h>
#include <glib.h>

#include "charset.h"
#include "japanese.h"


/* ﾏｼﾞﾝｶﾞｰ.LZH in Shift_JIS */
static const gchar sjis_name[] = "\xcf\xbc\xde\xdd\xb6\xde\xb0.LZH";
static const gchar utf8_name[] = "ﾏｼﾞﾝｶﾞｰ.LZH";


static void
check_guess (const gchar *bytes, const gchar *expected)
{
   gchar *s = charset_guess_to_utf8 (bytes, -1, TRUE);
   g_assert_cmpstr (s, ==, expected);
   g_free (s);
}


static void
test_not_utf8 (void)
{
   CharsetToInternalTypes types[] = {
      CHARSET_TO_INTERNAL_NEVER, CHARSET_TO_INTERNAL_LOCALE,
      CHARSET_TO_INTERNAL_AUTO, CHARSET_TO_INTERNAL_ANY
   };
   guint i;

   for (i = 0; i < G_N_ELEMENTS (types); i++) {
      gchar *s = charset_to_internal (sjis_name, "UTF-8",
                                      charset_get_auto_detect_func (CHARSET_AUTODETECT_JAPANESE),
                                      types[i]);
      g_assert_cmpstr (s, ==, utf8_name);
      g_free (s);
   }
}


static void
test_guess (void)
{
   /* Shift_JIS: kanji and kana, half width kana (an even number of
      bytes, also valid as EUC-JP kanji), a backslash as a second byte */
   check_guess ("\x8a\xbf\x8e\x9a\x82\xa9\x82\xc8.jpg", "漢字かな.jpg");
   check_guess ("\xcf\xbc\xde\xdd\xb6\xde", "ﾏｼﾞﾝｶﾞ");
   check_guess ("\x95\x5c\x8e\xa6.mag", "表示.mag");
   /* EUC-JP: kana (also valid as Shift_JIS half width kana), kanji.
      (Half width kana in EUC-JP, rare in names, are also valid Shift_JIS
      kanji and may not be told from them.) */
   check_guess ("\xa4\xa2\xa4\xa4\xa4\xa6.pi", "あいう.pi");
   check_guess ("\xc6\xfc\xcb\xdc\xb8\xec", "日本語");
   /* ISO-2022-JP is 7 bit: UTF-8 already, taken as it is */
   check_guess ("\x1b$B$\"$$\x1b(B", "\x1b$B$\"$$\x1b(B");
   /* UTF-8 as it is */
   check_guess ("日本語.jpg", "日本語.jpg");
   /* neither */
   g_assert_null (charset_guess_to_utf8 ("caf\xe9", -1, TRUE));
   /* not Japanese: no guess */
   g_assert_null (charset_guess_to_utf8 (sjis_name, -1, FALSE));
}


static void
test_escape (void)
{
   gchar *s;

   s = charset_escape_bytes ("caf\xe9 100%.jpg", -1);
   g_assert_cmpstr (s, ==, "caf%E9 100%.jpg");
   g_free (s);

   /* a backslash after a high byte (the second byte of 表 in Shift_JIS) is
      not a separator; one after ASCII is; line breaks stay */
   s = charset_escape_bytes ("a\\\x95\\b\x01\n", -1);
   g_assert_cmpstr (s, ==, "a\\%95%5Cb%01\n");
   g_free (s);

   /* the display: the guess, else the bytes escaped */
   s = charset_to_display (sjis_name, -1, TRUE);
   g_assert_cmpstr (s, ==, utf8_name);
   g_free (s);
   s = charset_to_display (sjis_name, -1, FALSE);
   g_assert_cmpstr (s, ==, "%CF%BC%DE%DD%B6%DE%B0.LZH");
   g_free (s);
   s = charset_to_internal ("caf\xe9", NULL, NULL, CHARSET_TO_INTERNAL_LOCALE);
   g_assert_cmpstr (s, ==, "caf%E9");
   g_free (s);
}


static void
test_utf8_kept (void)
{
   gchar *s = charset_to_internal ("日本語.jpg", NULL, NULL, CHARSET_TO_INTERNAL_LOCALE);
   g_assert_cmpstr (s, ==, "日本語.jpg");
   g_free (s);
}


int
main (int argc, char **argv)
{
   /* a UTF-8 system, not Japanese (the auto detect says so, when it does) */
   setlocale (LC_ALL, "C.UTF-8");
   g_unsetenv ("LANGUAGE");
   g_setenv ("LC_ALL", "C.UTF-8", TRUE);
   g_test_init (&argc, &argv, NULL);

   g_test_add_func ("/charset/not-utf8", test_not_utf8);
   g_test_add_func ("/charset/guess", test_guess);
   g_test_add_func ("/charset/escape", test_escape);
   g_test_add_func ("/charset/utf8-kept", test_utf8_kept);

   return g_test_run ();
}
