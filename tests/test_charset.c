/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * The tests of src/charset.c: file names that are not in the charset of
 * the system (MS-DOS era Shift_JIS names in a UTF-8 system).
 */

#include <locale.h>
#include <glib.h>

#include "charset.h"


/* ﾏｼﾞﾝｶﾞｰ.LZH in Shift_JIS */
static const gchar sjis_name[] = "\xcf\xbc\xde\xdd\xb6\xde\xb0.LZH";
static const gchar utf8_name[] = "ﾏｼﾞﾝｶﾞｰ.LZH";


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
      g_assert_nonnull (s);
      g_assert_cmpstr (s, ==, utf8_name);
      g_free (s);
   }
}


static void
test_utf8_kept (void)
{
   gchar *s = charset_to_internal ("日本語.jpg", NULL, NULL, CHARSET_TO_INTERNAL_LOCALE);
   g_assert_cmpstr (s, ==, "日本語.jpg");
   g_free (s);
}


static void
test_neither (void)
{
   /* not Shift_JIS either (a lead byte at the end): shown, never NULL */
   gchar *s = charset_to_internal ("caf\xe9", NULL, NULL, CHARSET_TO_INTERNAL_LOCALE);
   g_assert_nonnull (s);
   g_assert_true (g_utf8_validate (s, -1, NULL));
   g_assert_true (g_str_has_prefix (s, "caf"));
   g_free (s);
}


int
main (int argc, char **argv)
{
   /* a UTF-8 system */
   setlocale (LC_ALL, "C.UTF-8");
   g_setenv ("LC_ALL", "C.UTF-8", TRUE);
   g_test_init (&argc, &argv, NULL);

   g_test_add_func ("/charset/not-utf8", test_not_utf8);
   g_test_add_func ("/charset/utf8-kept", test_utf8_kept);
   g_test_add_func ("/charset/neither", test_neither);

   return g_test_run ();
}
