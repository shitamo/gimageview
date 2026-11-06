/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: the keywords other programs wrote into an image
 * (XMP dc:subject, IPTC Keywords) or next to it (an XMP sidecar file).
 *
 * Read here from the file headers, as the size of the images is in
 * gimv_indexer.c: no image library, nothing but what is asked for read,
 * every length checked (millions of files of any kind, some broken).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <glib/gstdio.h>

#include "gimv_index_keywords.h"

#define XMP_MAX   (4 * 1024 * 1024)    /* a packet or a sidecar larger: left */
#define IPTC_MAX  (1024 * 1024)

#define NS_DC      "http://purl.org/dc/elements/1.1/"
#define NS_RDF     "http://www.w3.org/1999/02/22-rdf-syntax-ns#"
#define NS_LR      "http://ns.adobe.com/lightroom/1.0/"
#define NS_DIGIKAM "http://www.digikam.org/ns/1.0/"
#define NS_MSPHOTO "http://ns.microsoft.com/photo/1.0/"

static guint32 be16 (const guchar *p) { return (p[0] << 8) | p[1]; }
static guint32 be32 (const guchar *p) { return ((guint32) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static guint32 le16 (const guchar *p) { return p[0] | (p[1] << 8); }
static guint32 le32 (const guchar *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((guint32) p[3] << 24); }


/******************************************************************************
 *  the keywords found
 ******************************************************************************/

typedef struct {
   GPtrArray *flat;   /* dc:subject, IPTC Keywords, MicrosoftPhoto */
   GPtrArray *hier;   /* the last part of lr:hierarchicalSubject, digiKam:TagsList */
} Found;


/* UTF-8 read as Shift_JIS and written again as UTF-8 (Windows Photo
 * Gallery wrote XMP so): "オリジナル" became "繧ｪ繝ｪ繧ｸ繝翫Ν".  The
 * mark is the half-width katakana the UTF-8 continuation bytes become.
 * Returns the text put right, the text as it is, or NULL when it is
 * such a text that cannot be put right (bytes lost on the way). */
gchar *
gimv_index_keyword_unmangle (const gchar *k)
{
   gboolean half = FALSE, other = FALSE;
   const gchar *p;
   gchar *bytes;

   for (p = k; *p; p = g_utf8_next_char (p)) {
      gunichar c = g_utf8_get_char (p);
      if (c >= 0xff61 && c <= 0xff9f) half = TRUE;
      else if (c >= 0x80) other = TRUE;
   }
   if (!half) return g_strdup (k);   /* "ｶﾒﾗ" alone is half-width text */

   bytes = g_convert (k, -1, "CP932", "UTF-8", NULL, NULL, NULL);
   if (!bytes) return g_strdup (k);
   if (g_utf8_validate (bytes, -1, NULL) && strcmp (bytes, k)) {
      /* Japanese again (not "ﾃｽﾄ" read as "Ã½Ä"): kept */
      gboolean japanese = TRUE;
      for (p = bytes; *p && japanese; p = g_utf8_next_char (p)) {
         gunichar c = g_utf8_get_char (p);
         japanese = c < 0x80 || (c >= 0x3000 && c <= 0x9fff)
                    || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xff00 && c <= 0xffef);
      }
      if (japanese) return bytes;
   } else if (other) {
      /* not to be put right, but it starts as Japanese in UTF-8 does
         (E3-E9 and two continuation bytes): mangled, left out */
      const guchar *b = (const guchar *) bytes;
      if (b[0] >= 0xe3 && b[0] <= 0xe9 && (b[1] & 0xc0) == 0x80 && (b[2] & 0xc0) == 0x80) {
         g_free (bytes);
         return NULL;
      }
   }
   g_free (bytes);
   return g_strdup (k);
}


/* one keyword: on one line, trimmed, UTF-8, not mangled, not too long, once */
static void
add_keyword (GPtrArray *a, const gchar *text, gssize len)
{
   gchar *k, *p;

   if (!text) return;
   k = len < 0 ? g_strdup (text) : g_strndup (text, len);
   if (!g_utf8_validate (k, -1, NULL)) {
      g_free (k);
      return;
   }
   p = gimv_index_keyword_unmangle (k);
   g_free (k);
   if (!p) return;
   k = p;
   for (p = k; *p; p++)
      if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
   g_strstrip (k);
   if (!*k || strlen (k) > GIMV_INDEX_KEYWORD_MAX
       || g_ptr_array_find_with_equal_func (a, k, g_str_equal, NULL))
      g_free (k);
   else
      g_ptr_array_add (a, k);
}


/******************************************************************************
 *  XMP
 ******************************************************************************/

typedef enum {
   IN_NONE,
   IN_FLAT,        /* a bag of keywords */
   IN_HIER_BAR,    /* "a|b|c" (Lightroom) */
   IN_HIER_SLASH,  /* "a/b/c" (digiKam) */
} XmpIn;

typedef struct {
   GHashTable *ns;      /* prefix -> URI (all the declarations seen) */
   Found      *found;
   XmpIn       in;
   gint        in_depth;
   gint        depth;
   gboolean    in_li;
   GString    *text;
} XmpParse;


/* "dc:subject" -> TRUE when it is local in the namespace uri */
static gboolean
is_name (XmpParse *x, const gchar *qname, const gchar *uri, const gchar *local)
{
   const gchar *colon = strchr (qname, ':');
   gchar *prefix;
   const gchar *u;

   if (!colon || strcmp (colon + 1, local)) return FALSE;
   prefix = g_strndup (qname, colon - qname);
   u = g_hash_table_lookup (x->ns, prefix);
   g_free (prefix);
   return u && !strcmp (u, uri);
}


static void
xmp_start (GMarkupParseContext *ctx, const gchar *name, const gchar **anames,
           const gchar **avalues, gpointer data, GError **error)
{
   XmpParse *x = data;
   guint i;

   for (i = 0; anames[i]; i++) {
      if (g_str_has_prefix (anames[i], "xmlns:"))
         g_hash_table_replace (x->ns, g_strdup (anames[i] + 6), g_strdup (avalues[i]));
   }
   x->depth++;

   if (x->in == IN_NONE) {
      if (is_name (x, name, NS_DC, "subject") || is_name (x, name, NS_MSPHOTO, "LastKeywordXMP"))
         x->in = IN_FLAT;
      else if (is_name (x, name, NS_LR, "hierarchicalSubject"))
         x->in = IN_HIER_BAR;
      else if (is_name (x, name, NS_DIGIKAM, "TagsList"))
         x->in = IN_HIER_SLASH;
      if (x->in != IN_NONE) x->in_depth = x->depth;
   } else if (is_name (x, name, NS_RDF, "li")) {
      x->in_li = TRUE;
      g_string_truncate (x->text, 0);
   }
}


static void
xmp_end (GMarkupParseContext *ctx, const gchar *name, gpointer data, GError **error)
{
   XmpParse *x = data;

   if (x->in != IN_NONE && x->in_li && is_name (x, name, NS_RDF, "li")) {
      if (x->in == IN_FLAT) {
         add_keyword (x->found->flat, x->text->str, x->text->len);
      } else {
         /* the last part: "Places|Japan|Kyoto" -> "Kyoto" */
         gchar sep = x->in == IN_HIER_BAR ? '|' : '/';
         const gchar *last = strrchr (x->text->str, sep);
         add_keyword (x->found->hier, last ? last + 1 : x->text->str, -1);
      }
      x->in_li = FALSE;
   }
   if (x->in != IN_NONE && x->depth == x->in_depth) x->in = IN_NONE;
   x->depth--;
}


static void
xmp_text (GMarkupParseContext *ctx, const gchar *text, gsize len, gpointer data,
          GError **error)
{
   XmpParse *x = data;

   if (x->in != IN_NONE && x->in_li)
      g_string_append_len (x->text, text, len);
}


static void
xmp_parse (const gchar *xmp, gssize len, Found *found)
{
   static const GMarkupParser parser = { xmp_start, xmp_end, xmp_text, NULL, NULL };
   XmpParse x = { 0 };
   GMarkupParseContext *ctx;
   const gchar *start;
   gsize n;

   if (!xmp) return;
   n = len < 0 ? strlen (xmp) : (gsize) len;
   /* from the first element or processing instruction (a BOM, padding
      before it); stops at the first NUL */
   start = memchr (xmp, '<', n);
   if (!start) return;
   n -= start - xmp;
   {
      const gchar *nul = memchr (start, '\0', n);
      if (nul) n = nul - start;
   }

   x.ns = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
   x.found = found;
   x.text = g_string_new (NULL);
   ctx = g_markup_parse_context_new (&parser, G_MARKUP_TREAT_CDATA_AS_TEXT, &x, NULL);
   /* a broken packet: what was found before the error is kept */
   if (g_markup_parse_context_parse (ctx, start, n, NULL))
      g_markup_parse_context_end_parse (ctx, NULL);
   g_markup_parse_context_free (ctx);
   g_string_free (x.text, TRUE);
   g_hash_table_unref (x.ns);
}


void
gimv_index_keywords_from_xmp (const gchar *xmp, gssize len, GPtrArray *out)
{
   Found f = { out, g_ptr_array_new_with_free_func (g_free) };
   guint i;

   xmp_parse (xmp, len, &f);
   if (!out->len)
      for (i = 0; i < f.hier->len; i++)
         add_keyword (out, g_ptr_array_index (f.hier, i), -1);
   g_ptr_array_unref (f.hier);
}


/******************************************************************************
 *  IPTC (IIM): the records of the Photoshop resource 0x0404 of a JPEG, or
 *  the TIFF tag 33723
 ******************************************************************************/

/* a keyword of IPTC: UTF-8 when the record says so (or it is valid
 * UTF-8), else often Shift_JIS from Japanese programs, else Latin-1 */
static void
add_iptc_keyword (GPtrArray *a, const gchar *d, gsize len, gboolean utf8)
{
   gchar *k = NULL;

   if (utf8 || g_utf8_validate (d, len, NULL))
      k = g_strndup (d, len);
   else
      k = g_convert (d, len, "UTF-8", "CP932", NULL, NULL, NULL);
   if (!k)
      k = g_convert (d, len, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);
   add_keyword (a, k, -1);
   g_free (k);
}


/* the keywords of an IPTC record, in one charset: UTF-8 when the record
 * says so or one of them is UTF-8 not ASCII (then one that is not UTF-8
 * is broken: left out), else each as add_iptc_keyword () reads it */
static void
iptc_keywords (GPtrArray *a, GPtrArray *raw, gboolean utf8)
{
   guint i;

   for (i = 0; !utf8 && i < raw->len; i++) {
      GBytes *b = g_ptr_array_index (raw, i);
      gsize len;
      const gchar *d = g_bytes_get_data (b, &len);
      gsize j;
      for (j = 0; j < len && !(d[j] & 0x80); j++) ;
      if (j < len && g_utf8_validate (d, len, NULL)) utf8 = TRUE;
   }
   for (i = 0; i < raw->len; i++) {
      GBytes *b = g_ptr_array_index (raw, i);
      gsize len;
      const gchar *d = g_bytes_get_data (b, &len);
      if (utf8 && !g_utf8_validate (d, len, NULL)) continue;
      add_iptc_keyword (a, d, len, utf8);
   }
}


static void
iptc_parse (const guchar *d, gsize n, Found *found)
{
   GPtrArray *raw = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
   gboolean utf8 = FALSE;
   gsize i = 0;

   while (i + 5 <= n && d[i] == 0x1c) {
      guint rec = d[i + 1], ds = d[i + 2];
      gsize len = be16 (d + i + 3);

      i += 5;
      if (len & 0x8000) break;          /* extended length: not for keywords */
      if (i + len > n) break;
      if (rec == 1 && ds == 90 && len >= 3 && !memcmp (d + i, "\x1b%G", 3))
         utf8 = TRUE;
      if (rec == 2 && ds == 25 && len)
         g_ptr_array_add (raw, g_bytes_new (d + i, len));
      i += len;
   }
   iptc_keywords (found->flat, raw, utf8);
   g_ptr_array_unref (raw);
}


/* the 8BIM resources of an APP13 "Photoshop 3.0" segment */
static void
photoshop_parse (const guchar *d, gsize n, Found *found)
{
   gsize i = 0;

   while (i + 12 <= n && !memcmp (d + i, "8BIM", 4)) {
      guint id = be16 (d + i + 4);
      gsize name_len = d[i + 6], size;

      i += 6 + ((1 + name_len + 1) & ~1);   /* the Pascal name, padded to even */
      if (i + 4 > n) break;
      size = be32 (d + i);
      i += 4;
      if (size > n - i) break;
      if (id == 0x0404) iptc_parse (d + i, size, found);
      i += (size + 1) & ~1;
   }
}


/******************************************************************************
 *  the files
 ******************************************************************************/

static guchar *
read_at (FILE *f, long offset, gsize len)
{
   guchar *buf;

   if (len == 0 || len > XMP_MAX) return NULL;
   if (fseek (f, offset, SEEK_SET) != 0) return NULL;
   buf = g_malloc (len + 1);
   if (fread (buf, 1, len, f) != len) {
      g_free (buf);
      return NULL;
   }
   buf[len] = '\0';
   return buf;
}


static void
jpeg_read (FILE *f, Found *found)
{
   static const gchar xmp_id[] = "http://ns.adobe.com/xap/1.0/";   /* and a NUL */
   guchar b[4];
   gint n;

   if (fseek (f, 2, SEEK_SET) != 0) return;
   for (n = 0; n < 1000; n++) {
      gint c;
      guint len;
      long pos;

      do { c = fgetc (f); } while (c != EOF && c != 0xff);
      do { c = fgetc (f); } while (c == 0xff);
      if (c == EOF) return;
      if (c == 0xd8 || (c >= 0xd0 && c <= 0xd7) || c == 0x01) continue;
      if (c == 0xd9 || c == 0xda) return;            /* EOI, SOS: no more metadata */
      if (fread (b, 1, 2, f) != 2) return;
      len = be16 (b);
      if (len < 2) return;
      pos = ftell (f);
      if (c == 0xe1 || c == 0xed) {                  /* APP1, APP13 */
         guchar *d = read_at (f, pos, len - 2);
         gsize dn = len - 2;
         if (d && c == 0xe1 && dn > sizeof xmp_id && !memcmp (d, xmp_id, sizeof xmp_id))
            xmp_parse ((const gchar *) d + sizeof xmp_id, dn - sizeof xmp_id, found);
         else if (d && c == 0xed && dn > 14 && !memcmp (d, "Photoshop 3.0\0", 14))
            photoshop_parse (d + 14, dn - 14, found);
         g_free (d);
      }
      if (fseek (f, pos + len - 2, SEEK_SET) != 0) return;
   }
}


static void
png_read (FILE *f, Found *found)
{
   static const gchar key[] = "XML:com.adobe.xmp";   /* and a NUL */
   guchar h[8];
   long pos = 8;
   gint n;

   for (n = 0; n < 10000; n++) {
      guint32 len;

      if (fseek (f, pos, SEEK_SET) != 0 || fread (h, 1, 8, f) != 8) return;
      len = be32 (h);
      if (len > 0x7fffffff) return;
      if (!memcmp (h + 4, "IDAT", 4) || !memcmp (h + 4, "IEND", 4)) return;
      if (!memcmp (h + 4, "iTXt", 4) && len > sizeof key + 4) {
         guchar *d = read_at (f, pos + 8, len);
         if (d && !memcmp (d, key, sizeof key) && d[sizeof key] == 0) {
            /* not compressed: keyword NUL, flag, method, language NUL,
               translated keyword NUL, text */
            gsize i = sizeof key + 2;
            const guchar *lang_end = memchr (d + i, 0, len - i);
            const guchar *tk_end = lang_end ? memchr (lang_end + 1, 0, d + len - lang_end - 1) : NULL;
            if (tk_end)
               xmp_parse ((const gchar *) tk_end + 1, d + len - tk_end - 1, found);
         }
         g_free (d);
      }
      pos += 12 + len;
   }
}


static void
webp_read (FILE *f, Found *found)
{
   guchar h[8];
   long pos = 12;
   gint n;

   for (n = 0; n < 1000; n++) {
      guint32 len;

      if (fseek (f, pos, SEEK_SET) != 0 || fread (h, 1, 8, f) != 8) return;
      len = le32 (h + 4);
      if (len > 0x7ffffff0) return;
      if (!memcmp (h, "XMP ", 4)) {
         guchar *d = read_at (f, pos + 8, len);
         xmp_parse ((const gchar *) d, d ? (gssize) len : 0, found);
         g_free (d);
         return;
      }
      pos += 8 + ((len + 1) & ~1);
   }
}


/* TIFF and the raw files built on it (CR2, NEF, ARW, DNG, ORF, RW2, PEF):
 * IFD 0, tags 700 (XMP) and 33723 (IPTC) */
static void
tiff_read (FILE *f, const guchar *head, Found *found)
{
   gboolean le = head[0] == 'I';
   guint32 (*u16) (const guchar *) = le ? le16 : be16;
   guint32 (*u32) (const guchar *) = le ? le32 : be32;
   guint32 ifd = u32 (head + 4);
   guchar cnt[2], e[12];
   guint i, entries;

   if (fseek (f, ifd, SEEK_SET) != 0 || fread (cnt, 1, 2, f) != 2) return;
   entries = u16 (cnt);
   if (entries > 1000) return;
   for (i = 0; i < entries; i++) {
      guint tag, type;
      guint32 count;
      gsize size;
      guchar *d = NULL;

      if (fseek (f, ifd + 2 + 12 * i, SEEK_SET) != 0 || fread (e, 1, 12, f) != 12) return;
      tag = u16 (e);
      if (tag != 700 && tag != 33723) continue;
      type = u16 (e + 2);
      count = u32 (e + 4);
      /* BYTE, UNDEFINED; IPTC is also written as LONG */
      if (type == 1 || type == 7 || type == 2) size = count;
      else if (type == 4) size = (gsize) count * 4;
      else continue;
      if (size > (tag == 700 ? XMP_MAX : IPTC_MAX)) continue;
      if (size <= 4) continue;
      d = read_at (f, u32 (e + 8), size);
      if (d) {
         if (tag == 700) xmp_parse ((const gchar *) d, size, found);
         else            iptc_parse (d, size, found);
      }
      g_free (d);
   }
}


static void
sidecar_read (const gchar *path, Found *found)
{
   gchar *text = NULL;
   gsize len = 0;
   GStatBuf st;

   if (g_stat (path, &st) != 0 || !S_ISREG (st.st_mode) || st.st_size > XMP_MAX) return;
   if (g_file_get_contents (path, &text, &len, NULL))
      xmp_parse (text, len, found);
   g_free (text);
}


gboolean
gimv_index_is_sidecar_name (const gchar *name)
{
   gsize n = name ? strlen (name) : 0;
   return n > 4 && !g_ascii_strcasecmp (name + n - 4, ".xmp");
}


/* "Places|Japan|Kyoto" -> "Kyoto" */
static void
add_last_part (GPtrArray *a, const gchar *text, gchar sep)
{
   const gchar *last = text ? strrchr (text, sep) : NULL;
   add_keyword (a, last ? last + 1 : text, -1);
}


GPtrArray *
gimv_index_keywords_read (const gchar *path)
{
   return gimv_index_keywords_read_with (path, NULL);
}


GPtrArray *
gimv_index_keywords_read_with (const gchar *path, const GimvIndexEmbedded *em)
{
   Found found;
   GPtrArray *ret;
   guchar head[16];
   gsize n;
   FILE *f = NULL;
   guint i;

   g_return_val_if_fail (path, NULL);
   found.flat = g_ptr_array_new_with_free_func (g_free);
   found.hier = g_ptr_array_new_with_free_func (g_free);

   if (em) {
      /* read by Exiv2 already */
      for (i = 0; em->flat && em->flat[i]; i++)
         add_keyword (found.flat, em->flat[i], -1);
      if (em->iptc) {
         GPtrArray *raw = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
         for (i = 0; em->iptc[i]; i++)
            g_ptr_array_add (raw, g_bytes_new (em->iptc[i], strlen (em->iptc[i])));
         iptc_keywords (found.flat, raw, em->iptc_utf8);
         g_ptr_array_unref (raw);
      }
      for (i = 0; em->hier_bar && em->hier_bar[i]; i++)
         add_last_part (found.hier, em->hier_bar[i], '|');
      for (i = 0; em->hier_slash && em->hier_slash[i]; i++)
         add_last_part (found.hier, em->hier_slash[i], '/');
   } else {
      f = fopen (path, "rb");
   }
   if (f) {
      n = fread (head, 1, sizeof head, f);
      if (n >= 4 && head[0] == 0xff && head[1] == 0xd8)
         jpeg_read (f, &found);
      else if (n >= 8 && !memcmp (head, "\x89PNG\r\n\x1a\n", 8))
         png_read (f, &found);
      else if (n >= 12 && !memcmp (head, "RIFF", 4) && !memcmp (head + 8, "WEBP", 4))
         webp_read (f, &found);
      else if (n >= 8 && (!memcmp (head, "II*\0", 4) || !memcmp (head, "MM\0*", 4)
                          || !memcmp (head, "IIRO", 4) || !memcmp (head, "IIRS", 4)
                          || !memcmp (head, "IIU\0", 4)))
         tiff_read (f, head, &found);
      fclose (f);
   }

   /* the sidecars: "x.jpg.xmp" (digiKam, darktable), "x.xmp" (Lightroom) */
   {
      gchar *side = g_strconcat (path, ".xmp", NULL);
      gchar *base = g_path_get_basename (path);
      const gchar *dot = strrchr (base, '.');
      sidecar_read (side, &found);
      g_free (side);
      side = g_strconcat (path, ".XMP", NULL);
      sidecar_read (side, &found);
      g_free (side);
      if (dot && dot != base) {
         gchar *dir = g_path_get_dirname (path);
         gchar *stem = g_strndup (base, dot - base);
         gchar *a = g_strconcat (stem, ".xmp", NULL), *b = g_strconcat (stem, ".XMP", NULL);
         gchar *pa = g_build_filename (dir, a, NULL), *pb = g_build_filename (dir, b, NULL);
         sidecar_read (pa, &found);
         sidecar_read (pb, &found);
         g_free (pa); g_free (pb); g_free (a); g_free (b); g_free (stem); g_free (dir);
      }
      g_free (base);
   }

   /* the flat keywords; the hierarchies only when there are none (the
      programs that write them write the flat ones too) */
   ret = found.flat;
   if (!ret->len)
      for (i = 0; i < found.hier->len; i++)
         add_keyword (ret, g_ptr_array_index (found.hier, i), -1);
   g_ptr_array_unref (found.hier);
   if (!ret->len) {
      g_ptr_array_unref (ret);
      return NULL;
   }
   return ret;
}
