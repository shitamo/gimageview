/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001-2003 Takuro Ashie
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
 * $Id: compare_md5.c,v 1.4 2004/04/08 10:38:50 makeinu Exp $
 */

/*
 * GTK4 port: the MD5 code taken from textutils-2.1 (md5.c) is replaced by
 * GLib's GChecksum.  The digest is kept as a hex string: the binary digest
 * was copied with g_strdup () and cut at its first zero byte.
 */

#include <stdio.h>
#include <string.h>

#include "gimv_dupl_finder.h"
#include "gimv_image_info.h"
#include "gimv_thumb.h"


/* MD5 of a file as a hex string, NULL on error */
static gchar *
digest_file (const gchar *filename)
{
   GChecksum *checksum;
   FILE *fp;
   guchar buf[65536];
   gsize n;
   gchar *ret = NULL;

   fp = fopen (filename, "rb");
   if (!fp) return NULL;

   checksum = g_checksum_new (G_CHECKSUM_MD5);
   while ((n = fread (buf, 1, sizeof (buf), fp)) > 0)
      g_checksum_update (checksum, buf, n);

   if (!ferror (fp))
      ret = g_strdup (g_checksum_get_string (checksum));

   g_checksum_free (checksum);
   fclose (fp);

   return ret;
}


static gpointer
duplicates_md5_get_data (GimvThumb *thumb)
{
   const gchar *filename;
   gchar *temp_file = NULL;
   gchar *digest;

   g_return_val_if_fail (GIMV_IS_THUMB(thumb), NULL);
   g_return_val_if_fail (thumb->info, NULL);

   if (gimv_image_info_need_temp_file (thumb->info)) {
      temp_file = gimv_image_info_get_temp_file (thumb->info);
      filename = temp_file;
   } else {
      filename = gimv_image_info_get_path (thumb->info);
   }

   g_return_val_if_fail (filename && *filename, NULL);

   digest = digest_file (filename);
   g_free (temp_file);

   return digest;
}


static gint
duplicates_md5_compare (gpointer data1, gpointer data2, gfloat *similarity)
{
   const gchar *str1 = data1, *str2 = data2;
   gint retval;

   if (!str1)
      return str2 ? 1 : 0;
   else if (!str2)
      return 1;

   retval = strcmp (str1, str2);

   if (!retval)
      *similarity = 1.0;
   else
      *similarity = 0.0;

   return retval;
}


static void
duplicates_md5_data_delete (gpointer data)
{
   g_free (data);
}


GimvDuplCompFuncTable gimv_dupl_md5_funcs =
{
   label:       N_("md5sum"),
   get_data:    duplicates_md5_get_data,
   compare:     duplicates_md5_compare,
   data_delete: duplicates_md5_data_delete,
};
