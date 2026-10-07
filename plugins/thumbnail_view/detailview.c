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
 * $Id: detailview.c,v 1.15 2004/09/21 08:44:29 makeinu Exp $
 */

#include "detailview.h"
#include "gimv_comment.h"

#include <time.h>
#include <stdio.h>
#include <string.h>

#include "dnd.h"
#include "detailview_priv.h"
#include "detailview_prefs.h"
#include "fileutil.h"
#include "gimv_image_info.h"
#include "gimv_plugin.h"
#include "gimv_thumb.h"
#include "gimv_thumb_view.h"

static GimvThumbViewPlugin detailview_modes[] =
{
   {GIMV_THUMBNAIL_VIEW_IF_VERSION,
    N_("Detail"),
    100,
    detailview_create,
    detailview_freeze,
    detailview_thaw,
    detailview_append_thumb_frame,
    detailview_update_thumbnail,
    detailview_remove_thumbnail,
    detailview_get_load_list,
    detailview_adjust,
    detailview_set_selection,
    detailview_set_focus,
    detailview_get_focus,
    detailview_thumbnail_is_in_viewport},

   {GIMV_THUMBNAIL_VIEW_IF_VERSION,
    N_("Detail + Icon"),
    110,
    detailview_create,
    detailview_freeze,
    detailview_thaw,
    detailview_append_thumb_frame,
    detailview_update_thumbnail,
    detailview_remove_thumbnail,
    detailview_get_load_list,
    detailview_adjust,
    detailview_set_selection,
    detailview_set_focus,
    detailview_get_focus,
    detailview_thumbnail_is_in_viewport},

   {GIMV_THUMBNAIL_VIEW_IF_VERSION,
    N_("Detail + Thumbnail"),
    120,
    detailview_create,
    detailview_freeze,
    detailview_thaw,
    detailview_append_thumb_frame,
    detailview_update_thumbnail,
    detailview_remove_thumbnail,
    detailview_get_load_list,
    detailview_adjust,
    detailview_set_selection,
    detailview_set_focus,
    detailview_get_focus,
    detailview_thumbnail_is_in_viewport},
};

GIMV_PLUGIN_GET_IMPL(detailview_modes, GIMV_PLUGIN_THUMBVIEW_EMBEDER)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("Thumbnail View Detail Mode"),
   version:       "0.0.2",
   author:        N_("Takuro Ashie"),
   description:   NULL,
   get_implement: gimv_plugin_get_impl,
   get_mime_type: NULL,
   get_prefs_ui:  gimv_prefs_ui_detailview_get_page,
};


/* for setting clist text data */
static gchar *column_data_filename   (GimvThumb *thumb);
static gchar *column_data_image_size (GimvThumb *thumb);
static gchar *column_data_size       (GimvThumb *thumb);
static gchar *column_data_type       (GimvThumb *thumb);
static gchar *column_data_atime      (GimvThumb *thumb);
static gchar *column_data_mtime      (GimvThumb *thumb);
static gchar *column_data_ctime      (GimvThumb *thumb);
static gchar *column_data_uid        (GimvThumb *thumb);
static gchar *column_data_gid        (GimvThumb *thumb);
static gchar *column_data_mode       (GimvThumb *thumb);
static gchar *column_data_cache      (GimvThumb *thumb);
static gchar *column_data_subject    (GimvThumb *thumb);
static gchar *column_data_comment    (GimvThumb *thumb);


DetailViewColumn detailview_columns [] =
{
   {NULL,                    0,   FALSE, NULL,                  GTK_JUSTIFY_CENTER, FALSE},
   {N_("Name"),              100, TRUE,  column_data_filename,  GTK_JUSTIFY_LEFT,   FALSE},
   {N_("Size (byte)"),       100, TRUE,  column_data_size,      GTK_JUSTIFY_RIGHT,  FALSE},
   {N_("Type"),              100, TRUE,  column_data_type,      GTK_JUSTIFY_LEFT,   FALSE},
   {N_("Cache type"),        100, FALSE, column_data_cache,     GTK_JUSTIFY_LEFT,   TRUE},
   {N_("Access time"),       150, TRUE,  column_data_atime,     GTK_JUSTIFY_LEFT,   FALSE},
   {N_("Modification time"), 150, TRUE,  column_data_mtime,     GTK_JUSTIFY_LEFT,   FALSE},
   {N_("Change time"),       150, TRUE,  column_data_ctime,     GTK_JUSTIFY_LEFT,   FALSE},
   {N_("User"),              40,  TRUE,  column_data_uid,       GTK_JUSTIFY_LEFT,   FALSE},
   {N_("Group"),             40,  TRUE,  column_data_gid,       GTK_JUSTIFY_LEFT,   FALSE},
   {N_("Mode"),              100, TRUE,  column_data_mode,      GTK_JUSTIFY_LEFT,   FALSE},
   {N_("Image size"),        100, TRUE,  column_data_image_size,GTK_JUSTIFY_CENTER, TRUE},
   /* GTK4 port: comments (TODO "display comments on thumbnail view") */
   {N_("Subject"),           150, TRUE,  column_data_subject,   GTK_JUSTIFY_LEFT,   TRUE},
   {N_("Comment"),           200, TRUE,  column_data_comment,   GTK_JUSTIFY_LEFT,   TRUE},
};
gint detailview_columns_num = sizeof (detailview_columns) / sizeof (DetailViewColumn);


GimvTargetEntry detailview_dnd_targets[] = {
   {"text/uri-list", 0, TARGET_URI_LIST},
};
const gint detailview_dnd_targets_num = sizeof(detailview_dnd_targets) / sizeof(GimvTargetEntry);


GList *detailview_title_idx_list     = NULL;
gint   detailview_title_idx_list_num = 0;




/******************************************************************************
 *
 *   for title index list.
 *
 ******************************************************************************/
void
detailview_create_title_idx_list (void)
{
   static gchar *config_columns_string = NULL;
   gchar **titles, *data_order;
   gint i = 0;

   detailview_prefs_get_value("data_order", (gpointer) &data_order);
 
   if (!data_order) {
      config_columns_string = NULL;
      if (detailview_title_idx_list)
         g_list_free (detailview_title_idx_list);
      detailview_title_idx_list_num = 0;
      return;
   }

   if (data_order == config_columns_string) return;

   if (detailview_title_idx_list) g_list_free (detailview_title_idx_list);
   detailview_title_idx_list = NULL;

   titles = g_strsplit (data_order, ",", -1);

   g_return_if_fail (titles);

   detailview_title_idx_list_num = 0;
   config_columns_string = data_order;

   while (titles[i]) {
      gint idx;
      idx = detailview_get_title_idx (titles[i]);
      if (idx > 0) {
         detailview_title_idx_list
            = g_list_append (detailview_title_idx_list, GINT_TO_POINTER (idx));
         detailview_title_idx_list_num++;
      }
      i++;
   }

   g_strfreev (titles);
}


gint
detailview_get_titles_num (void)
{
   return detailview_columns_num;
}


gchar *
detailview_get_title (gint idx)
{
   g_return_val_if_fail (idx > 0 && idx < detailview_columns_num, NULL);

   return detailview_columns[idx].title;
}


gint
detailview_get_title_idx (const gchar *title)
{
   gint i;

   g_return_val_if_fail (title, -1);

   for (i = 1; i < detailview_columns_num; i++) {
      if (!detailview_columns[i].title) continue;
      if (!strcmp (detailview_columns[i].title, title))
         return i;
   }

   return -1;
}


void
detailview_apply_config (void)
{
   GList *node;

   detailview_create_title_idx_list ();

   node = gimv_thumb_view_get_list();

   while (node) {
      GimvThumbView *tv = node->data;

      if (!strcmp (tv->summary_mode, DETAIL_VIEW_LABEL)
          || !strcmp (tv->summary_mode, DETAIL_ICON_LABEL)
          || !strcmp (tv->summary_mode, DETAIL_THUMB_LABEL))
      {
         gimv_thumb_view_set_widget (tv, tv->tw, tv->container, tv->summary_mode);
      }

      node = g_list_next (node);
   }
}



/******************************************************************************
 *
 *   for creating column text.
 *
 ******************************************************************************/
static gchar *
column_data_filename (GimvThumb *thumb)
{
   GimvThumbView *tv;

   if (!thumb) return NULL;
   tv = gimv_thumb_get_parent_thumbview (thumb);

   /*
   if (tv->mode == THUMB_VIEW_MODE_DIR)
      return g_strdup (g_basename (thumb->info->filename));
   else
      return g_strdup (thumb->info->filename);
   */

   {
      const gchar *filename;
      gchar *retval;

      if (tv->mode == GIMV_THUMB_VIEW_MODE_DIR)
         filename = g_basename (gimv_image_info_get_path (thumb->info));
      else
         filename = gimv_image_info_get_path (thumb->info);

      retval = gimv_image_info_get_display_name (thumb->info, filename);

      return retval;
   }
}


/*
 *  GTK4 port: without thumbnails (plain "Detail" mode, no cache) nothing
 *  loads the images, so their size was "Unknown" until one was opened.
 *  Read just the header of formats gdk-pixbuf knows (no decoding).
 */
/* width and height from the SOF marker of a JPEG file (for gdk-pixbuf
   builds without the JPEG loader) */
static gboolean
jpeg_header_size (const gchar *path, gint *width, gint *height)
{
   FILE *fp;
   guchar buf[8];
   gboolean found = FALSE;
   gint c;

   fp = fopen (path, "rb");
   if (!fp) return FALSE;

   if (fread (buf, 1, 2, fp) != 2 || buf[0] != 0xFF || buf[1] != 0xD8)
      goto END;

   for (;;) {
      guint len;

      /* next marker */
      do { c = fgetc (fp); } while (c != EOF && c != 0xFF);
      do { c = fgetc (fp); } while (c == 0xFF);
      if (c == EOF || c == 0xD9 || c == 0xDA) break;   /* EOI / SOS */
      if (c == 0x01 || (c >= 0xD0 && c <= 0xD7)) continue;   /* no length */

      if (fread (buf, 1, 2, fp) != 2) break;
      len = (buf[0] << 8) | buf[1];
      if (len < 2) break;

      /* SOF0-15 except DHT (C4), JPG (C8), DAC (CC) */
      if (c >= 0xC0 && c <= 0xCF && c != 0xC4 && c != 0xC8 && c != 0xCC) {
         if (fread (buf, 1, 5, fp) != 5) break;
         *height = (buf[1] << 8) | buf[2];
         *width  = (buf[3] << 8) | buf[4];
         found = *width > 0 && *height > 0;
         break;
      }
      if (fseek (fp, len - 2, SEEK_CUR)) break;
   }

END:
   fclose (fp);
   return found;
}


static void
probe_image_size (GimvImageInfo *info)
{
   const gchar *path;
   gint width = 0, height = 0;

   if (!info || (info->width > 0 && info->height > 0)) return;
   if (gimv_image_info_is_in_archive (info)
       || gimv_image_info_is_dir (info)
       || gimv_image_info_is_archive (info)
       || gimv_image_info_is_movie (info)
       || gimv_image_info_is_audio (info))
   {
      return;
   }

   path = gimv_image_info_get_path (info);
   if (!path || !*path) return;

   if ((gdk_pixbuf_get_file_info (path, &width, &height)
        && width > 0 && height > 0)
       || jpeg_header_size (path, &width, &height))
   {
      gimv_image_info_set_size (info, width, height);
   }
}


/*
 *  Image sizes are read from the headers in the background after the list
 *  is shown (like the thumbnail loading): an idle job walks a snapshot of
 *  the list, a few milliseconds at a time, and updates the rows it learned
 *  a size for.  It restarts when the list is rebuilt and stops when the
 *  view goes away or leaves the detail mode.
 */
#define SIZE_JOB_KEY      "detailview-size-job"
#define SIZE_JOB_SLICE_MS 15
#define SIZE_JOB_WAIT_MS  100   /* while a long job handles events */

typedef struct SizeJob_Tag
{
   GimvThumbView *tv;      /* not referenced: the job is data of the view */
   guint          idle_id;
   GList         *thumbs;  /* referenced snapshot */
   GList         *node;
   guint          snapshot_len;
} SizeJob;


static void
size_job_free (gpointer data)
{
   SizeJob *job = data;

   if (job->idle_id) g_source_remove (job->idle_id);
   g_list_free_full (job->thumbs, g_object_unref);
   g_free (job);
}


static gboolean
size_column_shown (void)
{
   GList *node;

   for (node = detailview_title_idx_list; node; node = g_list_next (node)) {
      gint idx = GPOINTER_TO_INT (node->data);
      if (detailview_columns[idx].func == column_data_image_size)
         return TRUE;
   }
   return FALSE;
}


static void
size_job_take_snapshot (SizeJob *job)
{
   g_list_free_full (job->thumbs, g_object_unref);
   job->thumbs = g_list_copy_deep (job->tv->thumblist,
                                   (GCopyFunc) g_object_ref, NULL);
   job->node = job->thumbs;
   job->snapshot_len = g_list_length (job->thumbs);
}


static gboolean idle_size_job (gpointer data);


static gboolean
timeout_size_job_resume (gpointer data)
{
   SizeJob *job = data;

   job->idle_id = g_idle_add_full (G_PRIORITY_LOW, idle_size_job, job, NULL);
   return G_SOURCE_REMOVE;
}


static gboolean
idle_size_job (gpointer data)
{
   SizeJob *job = data;
   GimvThumbView *tv = job->tv;
   gint64 end = g_get_monotonic_time () + SIZE_JOB_SLICE_MS * 1000;

   /* still a detail view of this plugin? */
   if (!g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL)
       || !tv->vfuncs || tv->vfuncs->update_thumb != detailview_update_thumbnail
       || !size_column_shown ())
   {
      job->idle_id = 0;
      g_object_set_data (G_OBJECT (tv), SIZE_JOB_KEY, NULL);   /* frees */
      return G_SOURCE_REMOVE;
   }

   /* a long job (thumbnails, an image) handles events: let it finish */
   if (gimv_flush_events_running ()) {
      job->idle_id = g_timeout_add_full (G_PRIORITY_LOW, SIZE_JOB_WAIT_MS,
                                         timeout_size_job_resume, job, NULL);
      return G_SOURCE_REMOVE;
   }

   if (!job->thumbs) size_job_take_snapshot (job);

   while (job->node && g_get_monotonic_time () < end) {
      GimvThumb *thumb = job->node->data;

      job->node = g_list_next (job->node);

      if (!thumb->info
          || (thumb->info->width > 0 && thumb->info->height > 0))
         continue;

      probe_image_size (thumb->info);
      if (thumb->info->width > 0 && thumb->info->height > 0)
         tv->vfuncs->update_thumb (tv, thumb, tv->summary_mode);
   }

   if (job->node) return G_SOURCE_CONTINUE;

   /* rows added meanwhile: once more (known sizes are skipped quickly) */
   if (g_list_length (tv->thumblist) != job->snapshot_len) {
      size_job_take_snapshot (job);
      return G_SOURCE_CONTINUE;
   }

   job->idle_id = 0;
   g_object_set_data (G_OBJECT (tv), SIZE_JOB_KEY, NULL);   /* frees */
   return G_SOURCE_REMOVE;
}


/* called for each new row; the first row of a list restarts the job */
void
detailview_size_job_start (GimvThumbView *tv, gboolean restart)
{
   SizeJob *job;

   if (!size_column_shown ()) return;

   job = g_object_get_data (G_OBJECT (tv), SIZE_JOB_KEY);
   if (job && !restart) return;   /* it picks the new row up at the end */

   job = g_new0 (SizeJob, 1);
   job->tv = tv;
   job->idle_id = g_idle_add_full (G_PRIORITY_LOW, idle_size_job, job, NULL);
   g_object_set_data_full (G_OBJECT (tv), SIZE_JOB_KEY, job, size_job_free);
}


static gchar *
column_data_image_size (GimvThumb *thumb)
{
   if (!thumb) return NULL;


   if (thumb->info->width > 0 && thumb->info->height > 0)
      return g_strdup_printf ("%d x %d", thumb->info->width, thumb->info->height);
   else
      return g_strdup (_("Unknown"));

}


static gchar *
column_data_size (GimvThumb *thumb)
{
   if (!thumb) return NULL;

   return fileutil_size2str (thumb->info->st.st_size, FALSE);
}


static gchar *
column_data_type (GimvThumb *thumb)
{
   if (!thumb) return NULL;

   return g_strdup (gimv_image_detect_type_by_ext (thumb->info->filename));
}


static gchar *
column_data_atime (GimvThumb *thumb)
{
   if (!thumb) return NULL;

   return fileutil_time2str (thumb->info->st.st_atime);
}


static gchar *
column_data_mtime (GimvThumb *thumb)
{
   if (!thumb) return NULL;

   return fileutil_time2str (thumb->info->st.st_mtime);
}


static gchar *
column_data_ctime (GimvThumb *thumb)
{
   if (!thumb) return NULL;

   return fileutil_time2str (thumb->info->st.st_ctime);
}


static gchar *
column_data_uid (GimvThumb *thumb)
{
   if (!thumb) return NULL;

   return fileutil_uid2str (thumb->info->st.st_uid);
}


static gchar *
column_data_gid (GimvThumb *thumb)
{
   if (!thumb) return NULL;

   return fileutil_gid2str (thumb->info->st.st_gid);
}


static gchar *
column_data_mode (GimvThumb *thumb)
{
   if (!thumb) return NULL;

   return fileutil_mode2str (thumb->info->st.st_mode);
}


static gchar *
column_data_subject (GimvThumb *thumb)
{
   gchar *subject = NULL;

   if (!thumb || !thumb->info) return NULL;
   gimv_comment_get_summary (thumb->info, &subject, NULL);

   return subject;
}


/* the first line of the note */
static gchar *
column_data_comment (GimvThumb *thumb)
{
   gchar *note = NULL, *line;

   if (!thumb || !thumb->info) return NULL;
   if (!gimv_comment_get_summary (thumb->info, NULL, &note)) return NULL;

   line = gimv_comment_first_line (note, 80);
   g_free (note);

   return line;
}


static gchar *
column_data_cache (GimvThumb *thumb)
{
   const gchar *cache_type;

   if (!thumb) return NULL;

   cache_type = gimv_thumb_get_cache_type (thumb);

   return (gchar *) cache_type;
}



/*
 *   For Gtk+-1.2
 */

