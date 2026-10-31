/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: the comments written in an image file (MAG, PNG, JPEG, GIF),
 * read from its header without loading the image.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <string.h>

#include "gimageview.h"
#include "gimv_file_comment.h"

/* a comment longer than this is cut (the rest is not shown anyway) */
#define MAX_COMMENT   (64 * 1024)


static void
file_comment_free (GimvFileComment *c)
{
   g_free (c->name);
   g_free (c->text);
   g_free (c);
}


/* bytes of an unknown charset -> UTF-8: UTF-8 as it is, else Shift_JIS
 * (CP932: the MAG files and the Japanese software of their time), else
 * Latin-1 (what PNG text is meant to be) */
static gchar *
to_utf8 (const gchar *data, gsize len)
{
   gchar *text;

   if (g_utf8_validate (data, len, NULL))
      return g_strndup (data, len);
   text = g_convert (data, len, "UTF-8", "CP932", NULL, NULL, NULL);
   if (!text)
      text = g_convert (data, len, "UTF-8", "SHIFT_JIS", NULL, NULL, NULL);
   if (!text)
      text = g_convert (data, len, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);
   return text;
}


/* lines ended by "\n", no NUL, EOF (0x1a) or spaces at the end */
static gchar *
tidy (gchar *text)
{
   GString *s;
   const gchar *p;

   if (!text) return NULL;
   s = g_string_sized_new (strlen (text));
   for (p = text; *p; p++) {
      if (*p == '\r') {
         if (p[1] != '\n') g_string_append_c (s, '\n');
      } else if (*p != 0x1a) {
         g_string_append_c (s, *p);
      }
   }
   g_free (text);
   while (s->len && g_ascii_isspace (s->str[s->len - 1]))
      g_string_truncate (s, s->len - 1);
   if (!s->len) {
      g_string_free (s, TRUE);
      return NULL;
   }
   return g_string_free (s, FALSE);
}


static void
add (GPtrArray *list, const gchar *name, const gchar *data, gsize len)
{
   GimvFileComment *c;
   gchar *text;

   if (len > MAX_COMMENT) len = MAX_COMMENT;
   len = strnlen (data, len);
   text = tidy (to_utf8 (data, len));
   if (!text) return;
   c = g_new0 (GimvFileComment, 1);
   c->name = g_strdup (name);
   c->text = text;
   g_ptr_array_add (list, c);
}


/******************************************************************************
 *  MAG (MAKI02): machine 4 bytes, user name 18, 2 more, the comment up to
 *  0x1a (EOF) and the header starting with 0
 ******************************************************************************/
static void
read_mag (FILE *f, GPtrArray *list)
{
   guchar head[8 + 4 + 18 + 2];
   GString *comment;
   gint ch;

   if (fread (head, 1, sizeof (head), f) != sizeof (head)) return;
   if (memcmp (head, "MAKI02  ", 8)) return;

   comment = g_string_new (NULL);
   while ((ch = fgetc (f)) != EOF && ch != 0x1a && ch != 0
          && comment->len < MAX_COMMENT)
      g_string_append_c (comment, ch);

   add (list, _("Comment"), comment->str, comment->len);
   /* the machine and the user name: by the format 4 bytes of ASCII and 18
    * bytes, then 2 unused (spaces); but some write the name over all the 24
    * (Shift_JIS from the first byte, its last letter in the unused bytes):
    * with no ASCII machine the 24 are the name, else the name takes the
    * unused 2 too (a letter cut at the 18th byte is kept whole) */
   {
      gboolean ascii = TRUE;
      gint i;

      for (i = 0; i < 4; i++)
         if (head[8 + i] && !g_ascii_isprint (head[8 + i])) ascii = FALSE;
      if (ascii) {
         add (list, _("User Name"), (gchar *) head + 12, 18 + 2);
         add (list, _("Machine"), (gchar *) head + 8, 4);
      } else {
         /* a binary machine code (not Shift_JIS text): the name as given */
         gchar *sjis = g_convert ((gchar *) head + 8, 4 + 18 + 2, "UTF-8", "CP932",
                                  NULL, NULL, NULL);
         if (sjis)
            add (list, _("User Name"), (gchar *) head + 8, 4 + 18 + 2);
         else
            add (list, _("User Name"), (gchar *) head + 12, 18 + 2);
         g_free (sjis);
      }
   }
   g_string_free (comment, TRUE);
}


/******************************************************************************
 *  PNG: tEXt (keyword NUL text) and iTXt (uncompressed) chunks before IDAT
 ******************************************************************************/
static guint32
be32 (const guchar *p)
{
   return ((guint32) p[0] << 24) | ((guint32) p[1] << 16) | ((guint32) p[2] << 8) | p[3];
}


static void
read_png (FILE *f, GPtrArray *list)
{
   guchar sig[8], head[8];

   if (fread (sig, 1, 8, f) != 8 || memcmp (sig, "\x89PNG\r\n\x1a\n", 8)) return;

   while (fread (head, 1, 8, f) == 8) {
      guint32 len = be32 (head);
      gboolean text = !memcmp (head + 4, "tEXt", 4), itext = !memcmp (head + 4, "iTXt", 4);

      if (!memcmp (head + 4, "IDAT", 4) || !memcmp (head + 4, "IEND", 4)) break;
      if ((text || itext) && len > 0 && len <= MAX_COMMENT * 2) {
         gchar *data = g_malloc (len + 1);
         if (fread (data, 1, len, f) != len) { g_free (data); break; }
         data[len] = '\0';
         {
            gsize klen = strnlen (data, len);
            if (klen < len) {
               const gchar *key = data, *body = data + klen + 1;
               gsize blen = len - klen - 1;
               if (itext) {
                  /* compression flag, method, language NUL, translated
                   * keyword NUL, then the text (UTF-8) */
                  const gchar *p = body, *end = data + len;
                  gboolean compressed = blen > 0 && p[0];
                  p += 2;
                  if (p < end) p += strnlen (p, end - p) + 1;   /* language */
                  if (p < end) p += strnlen (p, end - p) + 1;   /* translated */
                  if (compressed || p > end) { g_free (data); goto NEXT_CRC; }
                  body = p;
                  blen = end - p;
               }
               add (list, key, body, blen);
            }
         }
         g_free (data);
      NEXT_CRC:
         if (fseek (f, 4, SEEK_CUR)) break;
      } else {
         if (fseek (f, (long) len + 4, SEEK_CUR)) break;
      }
   }
}


/******************************************************************************
 *  JPEG: COM segments before the image data (SOS)
 ******************************************************************************/
static void
read_jpeg (FILE *f, GPtrArray *list)
{
   gint ch;

   if (fgetc (f) != 0xff || fgetc (f) != 0xd8) return;

   for (;;) {
      guchar len2[2];
      gint marker;
      guint len;

      if ((ch = fgetc (f)) != 0xff) return;
      while ((marker = fgetc (f)) == 0xff) ;      /* fill bytes */
      if (marker == EOF || marker == 0xda || marker == 0xd9) return;
      if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
      if (fread (len2, 1, 2, f) != 2) return;
      len = (len2[0] << 8) | len2[1];
      if (len < 2) return;
      len -= 2;
      if (marker == 0xfe && len > 0) {
         gchar *data = g_malloc (len);
         if (fread (data, 1, len, f) != len) { g_free (data); return; }
         add (list, _("Comment"), data, len);
         g_free (data);
      } else if (fseek (f, len, SEEK_CUR)) {
         return;
      }
   }
}


/******************************************************************************
 *  GIF: comment extensions (0x21 0xfe), anywhere in the file
 ******************************************************************************/
static gboolean
skip_sub_blocks (FILE *f, GString *keep)
{
   gint n;

   while ((n = fgetc (f)) > 0) {
      if (keep && keep->len < MAX_COMMENT) {
         gchar buf[256];
         if (fread (buf, 1, n, f) != (gsize) n) return FALSE;
         g_string_append_len (keep, buf, n);
      } else if (fseek (f, n, SEEK_CUR)) {
         return FALSE;
      }
   }
   return n == 0;
}


static void
read_gif (FILE *f, GPtrArray *list)
{
   guchar head[13];
   gint ch;

   if (fread (head, 1, 13, f) != 13) return;
   if (memcmp (head, "GIF87a", 6) && memcmp (head, "GIF89a", 6)) return;
   if (head[10] & 0x80)                                    /* global colors */
      if (fseek (f, 3 * (2 << (head[10] & 7)), SEEK_CUR)) return;

   while ((ch = fgetc (f)) != EOF && ch != 0x3b) {
      if (ch == 0x21) {
         gint label = fgetc (f);
         if (label == 0xfe) {
            GString *s = g_string_new (NULL);
            gboolean ok = skip_sub_blocks (f, s);
            add (list, _("Comment"), s->str, s->len);
            g_string_free (s, TRUE);
            if (!ok) return;
         } else if (label == EOF || !skip_sub_blocks (f, NULL)) {
            return;
         }
      } else if (ch == 0x2c) {                                    /* an image */
         guchar desc[9];
         if (fread (desc, 1, 9, f) != 9) return;
         if (desc[8] & 0x80)
            if (fseek (f, 3 * (2 << (desc[8] & 7)), SEEK_CUR)) return;
         if (fgetc (f) == EOF) return;                     /* LZW code size */
         if (!skip_sub_blocks (f, NULL)) return;
      } else {
         return;
      }
   }
}


GPtrArray *
gimv_file_comment_read (const gchar *filename)
{
   GPtrArray *list = g_ptr_array_new_with_free_func ((GDestroyNotify) file_comment_free);
   guchar magic[8];
   FILE *f;

   if (!filename || !(f = fopen (filename, "rb"))) return list;

   if (fread (magic, 1, 8, f) == 8) {
      rewind (f);
      if (!memcmp (magic, "MAKI02  ", 8))
         read_mag (f, list);
      else if (!memcmp (magic, "\x89PNG", 4))
         read_png (f, list);
      else if (magic[0] == 0xff && magic[1] == 0xd8)
         read_jpeg (f, list);
      else if (!memcmp (magic, "GIF8", 4))
         read_gif (f, list);
   }
   fclose (f);
   return list;
}
