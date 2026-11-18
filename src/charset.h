/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001 Takuro Ashie
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
 *
 * $Id: charset.h,v 1.10 2004/09/21 08:44:31 makeinu Exp $
 */

#ifndef __CHARSET_H__
#define __CHARSET_H__

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <glib.h>




#define CHARSET_ASCII  "US-ASCII"
#define CHARSET_UTF8   "UTF-8"

/* japanese character set */
#define CHARSET_SJIS   "SHIFT_JIS"
#define CHARSET_EUC_JP "EUC-JP"
#define CHARSET_JIS    "ISO-2022-JP"

/* for detecting locale charset */
typedef const gchar *(*CharsetDetectLocaleFn) (const gchar *locale);
/* for auto detecting charset */
typedef const gchar *(*CharsetAutoDetectFn)   (const gchar *string);
/* for alternative character set conversion method */
typedef       gchar *(*CharsetConvFn)         (const gchar *string,
                                               const gchar *src_codeset,
                                               const gchar *dest_codeset);


typedef enum {
   CHARSET_TO_INTERNAL_NEVER,   /* do not convert */
   CHARSET_TO_INTERNAL_LOCALE,  /* convert to locale charset */
   CHARSET_TO_INTERNAL_AUTO,    /* use auto detect function */
   CHARSET_TO_INTERNAL_ANY      /* convert from specified charset */
} CharsetToInternalTypes;

typedef enum {
   CHARSET_TO_LOCALE_NEVER,
   CHARSET_TO_LOCALE_INTERNAL,
   CHARSET_TO_LOCALE_AUTO,
   CHARSET_TO_LOCALE_ANY
} CharsetToLocaleTypes;

typedef enum {
   CHARSET_AUTODETECT_NONE,
   CHARSET_AUTODETECT_JAPANESE
} CharsetAutoDetectType;

extern const gchar *charset_auto_detect_labels[];


GList       *charset_get_known_list (const gchar *lang);
const gchar *get_lang               (void);

/* wrapper for nl_langinfo() */
void         charset_set_locale_charset   (const gchar *charset);
void         charset_set_internal_charset (const gchar *charset);
const gchar *charset_get_locale           (void);
const gchar *charset_get_internal         (void);
CharsetAutoDetectFn charset_get_auto_detect_func (CharsetAutoDetectType type);


/*
 *  any code -> internal converter
 */

/* wrapper for all converter */
/* src_codeset can be NULL if type is not CHARSET_TO_INTERNAL_ANY */
/* func can be NULL if type is not CHARSET_TO_INTERNAL_AUTO */
/* Bytes in a charset not known (a file name of the MS-DOS days in a UTF-8
 * system, a comment in an old image):
 *   charset_japanese_likely ():  whether the Japanese charsets are worth a
 *                                try (the auto detect is Japanese, or the
 *                                language is)
 *   charset_guess_to_utf8 ():    UTF-8 as it is; else (japanese) the
 *                                Japanese charset they decode in best
 *                                (*charset: its name, may be NULL); else
 *                                NULL
 *   charset_escape_bytes ():     the bytes that cannot be shown (0x80 and
 *                                more, controls but line breaks) as %XX,
 *                                as in URLs: "%8A%BF%8E%9A.jpg"
 *   charset_to_display ():       the guess, else the escaped bytes; never
 *                                NULL.  For showing only: the file is still
 *                                opened or renamed by its own bytes. */
gboolean  charset_japanese_likely      (CharsetAutoDetectFn func);
gchar    *charset_guess_to_utf8        (const gchar *src,
                                        gssize       len,
                                        gboolean     japanese,
                                        const gchar **charset);
gchar    *charset_escape_bytes         (const gchar *src,
                                        gssize       len);
gchar    *charset_to_display           (const gchar *src,
                                        gssize       len,
                                        gboolean     japanese);

/* how charset_to_internal_full () got its text */
typedef enum {
   CHARSET_NAME_PLAIN,     /* by the settings (or UTF-8) */
   CHARSET_NAME_GUESSED,   /* not by them: guessed (*guessed: the charset) */
   CHARSET_NAME_ESCAPED    /* not readable: the bytes as %XX */
} CharsetNameKind;

gchar    *charset_to_internal_full     (const gchar *src,
                                        const gchar *src_codeset,
                                        CharsetAutoDetectFn func,
                                        CharsetToInternalTypes type,
                                        CharsetNameKind *kind,
                                        const gchar **guessed);
gchar    *charset_to_internal          (const gchar *src,
                                        const gchar *src_codeset,
                                        CharsetAutoDetectFn func,
                                        CharsetToInternalTypes type);

gchar    *charset_locale_to_internal   (const gchar *src);
gchar    *charset_to_internal_auto     (const gchar *src,
                                        CharsetAutoDetectFn func);

/*
 *  any code -> locale converter
 */

/* wrapper for all converter */
/* src_codeset can be NULL if type is not CHARSET_TO_INTERNAL_ANY */
/* func can be NULL if type is not CHARSET_TO_INTERNAL_AUTO */
gchar    *charset_to_locale            (const gchar *src,
                                        const gchar *src_codeset,
                                        CharsetAutoDetectFn func,
                                        CharsetToLocaleTypes type);

gchar    *charset_internal_to_locale   (const gchar *src);
gchar    *charset_to_locale_auto       (const gchar *src,
                                        CharsetAutoDetectFn func);

/*
 *  internal -> any code converter
 */
gchar    *charset_from_internal        (const gchar *src,
                                        const gchar *dest_codeset);

/*
 *  locale -> any code converter
 */
gchar    *charset_from_locale          (const gchar *src,
                                        const gchar *dest_codeset);

/*
 *  any -> any code converter
 */
gchar    *charset_conv                 (const gchar *src,
                                        const gchar *src_codeset,
                                        const gchar *dest_codeset);
gchar    *charset_conv_auto            (const gchar *src,
                                        const gchar *dest_codeset,
                                        CharsetAutoDetectFn func);

#endif /* __CHARSET_H__ */
