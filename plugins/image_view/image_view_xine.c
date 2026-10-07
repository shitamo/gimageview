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
 * $Id: image_view_xine.c,v 1.8 2004/10/03 19:18:57 makeinu Exp $
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif /* HAVE_CONFIG_H */

#ifdef ENABLE_XINE

#include <stdlib.h>

#include "gimv_image_view.h"
#include "gimv_plugin.h"
#include "gimv_thumb.h"
#include "gimv_thumb_cache.h"
#include "gimv_xine.h"
#include "gtkutils.h"
#include "prefs_xine.h"

/* callback functions */
static void         cb_playback_finished             (GimvXine  *gtx,
                                                      GimvImageView *iv);
static void         cb_destroy                       (GtkWidget *widget,
                                                      GimvImageView *iv);

/* other private fiunctions */
static gboolean     timeout_movie_status_listener    (gpointer   data);
static gint         timeout_create_thumbnail         (gpointer   data);
static void         install_movie_listener           (GimvImageView *iv);
static void         remove_movie_listener            (GimvImageView *iv);
static void         install_create_thumbnail_timer   (GimvImageView *iv);
static void         remove_create_thumbnail_timer    (GimvImageView *iv);
static void         imageview_xine_real_play         (GimvImageView *iv,
                                                      guint      pos);
static gboolean     imageview_xine_is_playing        (GimvImageView *iv);

/* vertual functions */
static gboolean     imageview_xine_is_supported      (GimvImageView *iv,
                                                      GimvImageInfo *info);
static GtkWidget   *imageview_xine_create            (GimvImageView *iv);
static void         imageview_xine_create_thumbnail  (GimvImageView *iv,
                                                      const gchar *type);

static gboolean     imageview_xine_is_playable       (GimvImageView *iv,
                                                      GimvImageInfo *info);
static gboolean     imageview_xine_is_seekable       (GimvImageView *iv);
static guint        imageview_xine_get_position      (GimvImageView *iv);
static guint        imageview_xine_get_length        (GimvImageView *iv);
static void         imageview_xine_play              (GimvImageView *iv);
static void         imageview_xine_stop              (GimvImageView *iv);
static void         imageview_xine_pause             (GimvImageView *iv);
static void         imageview_xine_forward           (GimvImageView *iv);
static void         imageview_xine_seek              (GimvImageView *iv,
                                                      gfloat     pos);
static void         imageview_xine_eject             (GimvImageView *iv);
static GimvImageViewPlayableStatus
                    imageview_xine_get_status        (GimvImageView *iv);
static guint        imageview_xine_get_length        (GimvImageView *iv);
static guint        imageview_xine_get_position      (GimvImageView *iv);


static GimvImageViewPlayableIF playable_if = {
   is_playable_fn:      imageview_xine_is_playable,
   is_seekable_fn:      imageview_xine_is_seekable,
   play_fn:             imageview_xine_play,
   stop_fn:             imageview_xine_stop,
   pause_fn:            imageview_xine_pause,
   forward_fn:          imageview_xine_forward,
   reverse_fn:          NULL,
   seek_fn:             imageview_xine_seek,
   eject_fn:            imageview_xine_eject,
   get_status_fn:       imageview_xine_get_status,
   get_length_fn:       imageview_xine_get_length,
   get_position_fn:     imageview_xine_get_position,
};


static GimvImageViewPlugin imageview_xine[] =
{
   {
      if_version:          GIMV_IMAGE_VIEW_IF_VERSION,
      label:               N_("Movie Player (Xine)"),
      priority_hint:       G_PRIORITY_DEFAULT,
      is_supported_fn:     imageview_xine_is_supported,
      create_fn:           imageview_xine_create,
      create_thumbnail_fn: imageview_xine_create_thumbnail,
      fullscreen_fn:       NULL,

      scalable:            NULL,
      rotatable:           NULL,
      playable:            &playable_if,
   }
};

static const gchar *mpeg_extensions[] =
{
   "mpg", "mpeg", "mpe", "mp2", "m2v", "vob",
};
static const gchar *quicktime_extensions[] =
{
   "mov", "qt", "moov", "qtvr",
};
static const gchar *asf_extensions[] =
{
   "asf",
};
static const gchar *wmv_extensions[] =
{
   "wmv",
};
static const gchar *msvideo_extensions[] =
{
   "avi",
};
static const gchar *audio_extensions[] =
{
   "au", "snd",
};
static const gchar *mp3_extensions[] =
{
   "mp3",
};
static const gchar *realmedia_extensions[] =
{
   "ra", "ram", "rm", "sml",
};
static const gchar *wav_extensions[] =
{
   "wav", "wave",
};
/* formats that did not exist or were rare in 2004 */
static const gchar *mp4_extensions[] =
{
   "mp4", "m4v",
};
static const gchar *matroska_extensions[] =
{
   "mkv",
};
static const gchar *webm_extensions[] =
{
   "webm",
};
static const gchar *flv_extensions[] =
{
   "flv",
};
static const gchar *ogg_video_extensions[] =
{
   "ogv", "ogm",
};
static const gchar *mpeg_ts_extensions[] =
{
   "ts", "m2ts", "mts",
};
static const gchar *threegpp_extensions[] =
{
   "3gp",
};
static const gchar *threegpp2_extensions[] =
{
   "3g2",
};
static const gchar *ogg_audio_extensions[] =
{
   "ogg", "oga", "opus",
};
static const gchar *flac_extensions[] =
{
   "flac",
};
static const gchar *mp4_audio_extensions[] =
{
   "m4a", "aac",
};

static GimvMimeTypeEntry xine_mime_types[] =
{
   {
      mime_type:      "application/vnd.ms-asf",
      description:    "ASF Media",
      extensions:     asf_extensions,
      extensions_len: sizeof (asf_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "audio/au",
      description:    "Basic audio",
      extensions:     audio_extensions,
      extensions_len: sizeof (audio_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "audio/x-mp3",
      description:    "MPEG layer 3 Audio",
      extensions:     mp3_extensions,
      extensions_len: sizeof (mp3_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "audio/x-pn-realaudio",
      description:    "RealAudio/Video",
      extensions:     realmedia_extensions,
      extensions_len: sizeof (realmedia_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "audio/x-real-audio",
      description:    "RealAudio/Video",
      extensions:     realmedia_extensions,
      extensions_len: sizeof (realmedia_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "audio/x-wav",
      description:    "WAV Audio",
      extensions:     wav_extensions,
      extensions_len: sizeof (wav_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "video/mpeg",
      description:    "MPEG Video",
      extensions:     mpeg_extensions,
      extensions_len: sizeof (mpeg_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "video/quicktime",
      description:    "Quicktime Video",
      extensions:     quicktime_extensions,
      extensions_len: sizeof (quicktime_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "video/x-ms-asf",
      description:    "MS ASF Video",
      extensions:     asf_extensions,
      extensions_len: sizeof (asf_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "video/x-ms-wmv",
      description:    "MS WMV Video",
      extensions:     wmv_extensions,
      extensions_len: sizeof (wmv_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "video/x-msvideo",
      description:    "Microsoft Video",
      extensions:     msvideo_extensions,
      extensions_len: sizeof (msvideo_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "audio/ogg",
      description:    "Ogg Audio",
      extensions:     ogg_audio_extensions,
      extensions_len: sizeof (ogg_audio_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "audio/flac",
      description:    "FLAC Audio",
      extensions:     flac_extensions,
      extensions_len: sizeof (flac_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "audio/mp4",
      description:    "MPEG-4 Audio",
      extensions:     mp4_audio_extensions,
      extensions_len: sizeof (mp4_audio_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "video/mp4",
      description:    "MPEG-4 Video",
      extensions:     mp4_extensions,
      extensions_len: sizeof (mp4_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "video/x-matroska",
      description:    "Matroska Video",
      extensions:     matroska_extensions,
      extensions_len: sizeof (matroska_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "video/webm",
      description:    "WebM Video",
      extensions:     webm_extensions,
      extensions_len: sizeof (webm_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "video/x-flv",
      description:    "Flash Video",
      extensions:     flv_extensions,
      extensions_len: sizeof (flv_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "video/ogg",
      description:    "Ogg Video",
      extensions:     ogg_video_extensions,
      extensions_len: sizeof (ogg_video_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "video/mp2t",
      description:    "MPEG Transport Stream",
      extensions:     mpeg_ts_extensions,
      extensions_len: sizeof (mpeg_ts_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "video/3gpp",
      description:    "3GPP Video",
      extensions:     threegpp_extensions,
      extensions_len: sizeof (threegpp_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "video/3gpp2",
      description:    "3GPP2 Video",
      extensions:     threegpp2_extensions,
      extensions_len: sizeof (threegpp2_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
};

/* FIXME */
#include <xine_image_loader.h>
static const gchar *
gimv_plugin_get_impl (guint idx, gpointer *impl, guint *size)
{
   g_return_val_if_fail(impl, NULL);
   *impl = NULL;
   g_return_val_if_fail(size, NULL);
   *size = 0;

   switch (idx) {
   case 0:
      *impl = &imageview_xine;
      *size = sizeof(imageview_xine);
      return GIMV_PLUGIN_IMAGEVIEW_EMBEDER;
   case 1:
      gimv_xine_image_loader_get_impl(impl, size);
      return GIMV_PLUGIN_IMAGE_LOADER;
   default:
      return NULL;
   }

   return NULL;
}
/*GIMV_PLUGIN_GET_IMPL(plugin_impl, GIMV_PLUGIN_IMAGEVIEW_EMBEDER)*/
GIMV_PLUGIN_GET_MIME_TYPE(xine_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("Xine Embedder & Movie Frame Loader"),
   version:       "0.3.0",
   author:        N_("Takuro Ashie, shitamo"),
   description:   N_("Plays movies and audio with xine-lib 1.2"),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  gimv_prefs_ui_xine_get_page,
};
static GimvPluginInfo *this;


static GHashTable *movie_listener_id_table   = NULL;
static GHashTable *create_thumbnail_id_table = NULL;


gchar *
g_module_check_init (GModule *module)
{
   g_module_symbol (module, "gimv_plugin_info", (gpointer) &this);
   return NULL;
}

GimvPluginInfo *
gimv_xine_plugin_get_info (void)
{
   return this;
}


/*****************************************************************************
 *
 *   callback functions
 *
 *****************************************************************************/
static void
cb_playback_finished (GimvXine *gtx, GimvImageView *iv)
{
   gboolean next;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   g_object_get (G_OBJECT (iv),
                   "continuance_play", &next,
                   NULL);

   if (next)
      gimv_image_view_next (iv);

   remove_create_thumbnail_timer (iv);
   remove_movie_listener (iv);

   gimv_image_view_playable_set_status (iv, GimvImageViewPlayableStop);
}


static void
cb_destroy (GtkWidget *widget, GimvImageView *iv)
{
   g_return_if_fail (iv);

   remove_create_thumbnail_timer (iv);
   remove_movie_listener (iv);
}



/*****************************************************************************
 *
 *   other private functions
 *
 *****************************************************************************/
static gboolean
timeout_movie_status_listener (gpointer data)
{
   GimvImageView   *iv = data;
   gint now, len;
   gdouble pos;

   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);

   g_return_val_if_fail (iv, FALSE);

   if (!iv->info || (!gimv_image_info_is_movie (iv->info) && !gimv_image_info_is_audio (iv->info))) {
      if (movie_listener_id_table)
         g_hash_table_remove (movie_listener_id_table, iv);
      gimv_image_view_playable_set_status (iv, GimvImageViewPlayableStop);
      return FALSE;
   }

   if (!imageview_xine_is_playing (iv)) {
      if (movie_listener_id_table)
         g_hash_table_remove (movie_listener_id_table, iv);
      gimv_image_view_playable_set_status (iv, GimvImageViewPlayableStop);

      return FALSE;
   }

   now = imageview_xine_get_position (iv);
   len = imageview_xine_get_length (iv);
   if (len > 0)
      pos = (gdouble) now / (gdouble) len * 100.0;
   else
      pos = 0.0;

   gimv_image_view_playable_set_position (iv, pos);

   return TRUE;
}


static gint
timeout_create_thumbnail (gpointer data)
{
   GList *node;
   GimvImageView *iv = data;

   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);

   node = g_list_find (gimv_image_view_get_list(), iv);
   if (!node) goto ERROR;

   gimv_image_view_create_thumbnail (iv);

ERROR:
   g_hash_table_remove (create_thumbnail_id_table, iv);
   return FALSE;
}


static void
install_movie_listener (GimvImageView *iv)
{
   guint timer;

   remove_movie_listener (iv);

   timer = g_timeout_add (300, timeout_movie_status_listener, iv);
   g_hash_table_insert (movie_listener_id_table, iv, GUINT_TO_POINTER (timer));
}


static void
remove_movie_listener (GimvImageView *iv)
{
   guint timer;
   gpointer data;

   if (!movie_listener_id_table) return;

   data = g_hash_table_lookup (movie_listener_id_table, iv);
   timer = GPOINTER_TO_UINT (data);

   if (timer > 0)
      g_source_remove (timer);

   g_hash_table_remove (movie_listener_id_table, iv);

   gimv_image_view_playable_set_status (iv, GimvImageViewPlayableStop);
}


static void
install_create_thumbnail_timer (GimvImageView *iv)
{
   gpointer data = g_hash_table_lookup (create_thumbnail_id_table, iv);
   guint timer = GPOINTER_TO_UINT (data);

   remove_create_thumbnail_timer (iv);

   timer = g_timeout_add (gimv_prefs_xine_get_delay(this) * 1000,
                            (GSourceFunc) timeout_create_thumbnail,
                            (gpointer) iv);
   g_hash_table_insert (create_thumbnail_id_table,
                        iv, GUINT_TO_POINTER (timer));
}


static void
remove_create_thumbnail_timer (GimvImageView *iv)
{
   gpointer id_p;
   guint id;

   id_p = g_hash_table_lookup (create_thumbnail_id_table, iv);
   id = GPOINTER_TO_UINT (id_p);
   if (id > 0)
      g_source_remove (id);
   g_hash_table_remove (create_thumbnail_id_table, iv);
}


static void
imageview_xine_real_play (GimvImageView *iv, guint pos)
{
   GimvXine *gtx;
   gchar *filename, *cache_file;

   g_return_if_fail (iv);
   if (!iv->info) return;
   if (!gimv_image_info_is_movie (iv->info) && !gimv_image_info_is_audio (iv->info)) return;
   g_return_if_fail (GTK_IS_WIDGET (iv->draw_area));

   gtx = GIMV_XINE (gimv_bin_get_child (GTK_WIDGET (iv->draw_area)));
   if (!GIMV_IS_XINE (gtx)) return;

   {
      gchar *local = gimv_image_info_get_local_path (iv->info);
      if (!local) return;
      gimv_xine_set_mrl (gtx, local);   /* copies it */
      g_free (local);
   }
   gimv_xine_play (gtx, 0, pos);

   filename = gimv_image_info_get_path_with_archive (iv->info);
   cache_file = gimv_thumb_find_thumbcache (filename, NULL);

   if (!cache_file && !gimv_image_info_is_url (iv->info)) {
      install_create_thumbnail_timer (iv);
   }

   g_free (filename);
   g_free (cache_file);

   install_movie_listener (iv);
   gimv_image_view_playable_set_status (iv, GimvImageViewPlayablePlay);
}


static gboolean
imageview_xine_is_playing (GimvImageView *iv)
{
   GimvXine *gtx;

   g_return_val_if_fail (iv, FALSE);
   g_return_val_if_fail (iv->info, FALSE);
   g_return_val_if_fail (gimv_image_info_is_movie (iv->info)
                         || gimv_image_info_is_audio (iv->info),
                         FALSE);
   g_return_val_if_fail (GTK_IS_WIDGET (iv->draw_area), FALSE);

   gtx = GIMV_XINE (gimv_bin_get_child (GTK_WIDGET (iv->draw_area)));
   g_return_val_if_fail (GIMV_IS_XINE (gtx), FALSE);

   return gimv_xine_is_playing (GIMV_XINE (gtx));
}



/*****************************************************************************
 *
 *   Virtual functions
 *
 *****************************************************************************/
static gboolean
imageview_xine_is_supported (GimvImageView *iv, GimvImageInfo *info)
{
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);
   if (!info) return FALSE;

   return gimv_image_info_is_movie (info) || gimv_image_info_is_audio (info);
}


static GtkWidget *
imageview_xine_create (GimvImageView *iv)
{
   GtkWidget *gxine, *widget;

   if (!movie_listener_id_table)
      movie_listener_id_table
         = g_hash_table_new (g_direct_hash, g_direct_equal);

   if (!create_thumbnail_id_table)
      create_thumbnail_id_table
         = g_hash_table_new (g_direct_hash, g_direct_equal);

   widget = gimv_event_box_new ();
   gxine = gimv_xine_new (NULL, NULL);
   gimv_container_add (GTK_WIDGET (widget), gxine);
   /* GTK4: after packing (which resets them): the GTK2 event box gave its
      child the whole area, otherwise the video window is 0x0 */
   gtk_widget_set_hexpand (gxine, TRUE);
   gtk_widget_set_vexpand (gxine, TRUE);
   gtk_widget_show (gxine);

   g_signal_connect (G_OBJECT (gxine), "playback_finished",
                       G_CALLBACK (cb_playback_finished), iv);
   g_signal_connect (G_OBJECT (gxine), "destroy",
                       G_CALLBACK (cb_destroy), iv);

   return widget;
}


static void
imageview_xine_create_thumbnail (GimvImageView *iv, const gchar *cache_write_type)
{
   GimvImage *imcache;
   GtkWidget *gtkxine;
   gint width, height;
   guchar *rgb;
   GimvImage *image;
   gchar *filename;

   if (!gimv_image_info_is_movie (iv->info))
      return;
   if (!GTK_IS_WIDGET (iv->draw_area)) return;

   gtkxine = gimv_bin_get_child (GTK_WIDGET (iv->draw_area));

   if (!GIMV_IS_XINE (gtkxine)) return;
   if (!gimv_xine_is_playing (GIMV_XINE (gtkxine))) return;

   rgb = gimv_xine_get_current_frame_rgb (GIMV_XINE (gtkxine),
                                          &width, &height);
   if (!rgb) return;

   image = gimv_image_create_from_data (rgb, width, height, FALSE);
   if (!image) {
      g_free (rgb);
      return;
   }

   filename = gimv_image_info_get_path_with_archive (iv->info);

   imcache = gimv_thumb_cache_save (filename,
                                    cache_write_type,
                                    image,
                                    iv->info);

   g_free (filename);
   gimv_image_unref (image);

   if (imcache) {
      gimv_image_unref (imcache);
      g_signal_emit_by_name (G_OBJECT (iv),
                               "thumbnail_created",
                               iv->info);
   }
}


static gboolean
imageview_xine_is_playable (GimvImageView *iv, GimvImageInfo *info)
{
   return TRUE;
}


static gboolean
imageview_xine_is_seekable (GimvImageView *iv)
{
   return TRUE;
}


static void
imageview_xine_play (GimvImageView *iv)
{
   GimvXine *gtx;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   gtx = GIMV_XINE (gimv_bin_get_child (GTK_WIDGET (iv->draw_area)));
   if (!GIMV_IS_XINE (gtx)) return;

   if (!imageview_xine_is_playing (iv)) {
      imageview_xine_real_play (iv, 0);
   } else if (gimv_xine_get_speed (gtx) != GIMV_XINE_SPEED_NORMAL) {
      gimv_xine_set_speed (gtx, GIMV_XINE_SPEED_NORMAL);
      gimv_image_view_playable_set_status (iv, GimvImageViewPlayablePlay);
   } else {
      imageview_xine_pause (iv);
   }
}


static void
imageview_xine_stop (GimvImageView *iv)
{
   GimvXine *gtx;

   g_return_if_fail (iv);
   if (!iv->info) return;
   if (!gimv_image_info_is_movie (iv->info) && !gimv_image_info_is_audio (iv->info))
      return;
   g_return_if_fail (GTK_IS_WIDGET (iv->draw_area));

   gtx = GIMV_XINE (gimv_bin_get_child (GTK_WIDGET (iv->draw_area)));
   if (!GIMV_IS_XINE (gtx)) return;

   remove_create_thumbnail_timer (iv);
   remove_movie_listener (iv);

   gimv_xine_stop (gtx);

   gimv_image_view_playable_set_status (iv, GimvImageViewPlayableStop);
}


static void
imageview_xine_pause (GimvImageView *iv)
{
   GimvXine *gtx;

   g_return_if_fail (iv);
   if (!iv->info) return;
   if (!gimv_image_info_is_movie (iv->info) && !gimv_image_info_is_audio (iv->info))
      return;
   g_return_if_fail (GTK_IS_WIDGET (iv->draw_area));

   gtx = GIMV_XINE (gimv_bin_get_child (GTK_WIDGET (iv->draw_area)));
   if (!GIMV_IS_XINE (gtx)) return;

   gimv_xine_set_speed (gtx, GIMV_XINE_SPEED_PAUSE);

   gimv_image_view_playable_set_status (iv, GimvImageViewPlayablePause);
}


static void
imageview_xine_forward (GimvImageView *iv)
{
   GimvXine *gtx;

   g_return_if_fail (iv);
   if (!iv->info) return;
   if (!gimv_image_info_is_movie (iv->info) && !gimv_image_info_is_audio (iv->info))
      return;
   g_return_if_fail (GTK_IS_WIDGET (iv->draw_area));

   gtx = GIMV_XINE (gimv_bin_get_child (GTK_WIDGET (iv->draw_area)));
   if (!GIMV_IS_XINE (gtx)) return;

   gimv_xine_set_speed (gtx, 12);

   gimv_image_view_playable_set_status (iv, GimvImageViewPlayableForward);
}


static void
imageview_xine_seek (GimvImageView *iv, gfloat pos)
{
   GimvXine *gtx;
   gfloat ppos;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   gtx = GIMV_XINE (gimv_bin_get_child (GTK_WIDGET (iv->draw_area)));
   if (!GIMV_IS_XINE (gtx)) return;

   ppos = (gfloat) pos * (gfloat) gimv_xine_get_stream_length (gtx) / 100.0;

   imageview_xine_real_play (iv, ppos);
}


static void
imageview_xine_eject (GimvImageView *iv)
{
   GList *plugin_list = NULL, *mrl_list = NULL;
   GimvXine *gtx;
   gchar **plugins, *str, **mrls;
   gint i, num;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   gtx = GIMV_XINE (gimv_bin_get_child (GTK_WIDGET (iv->draw_area)));
   if (!GIMV_IS_XINE (gtx)) return;

   if (gimv_xine_is_playing (gtx)) {
      gtkutil_message_dialog (_("Error!"), _("Please stop first."),
                              GTK_WINDOW (gimv_widget_get_toplevel (GTK_WIDGET (iv))));
      return;
   }

   plugins = gimv_xine_get_autoplay_input_plugin_ids (gtx);

   for (i = 0; plugins[i]; i++) {
      plugin_list = g_list_append (plugin_list, plugins[i]);
   }

   if (!plugin_list) {
      gtkutil_message_dialog (_("Error!"), _("No available plugin found."),
                             GTK_WINDOW (gimv_widget_get_toplevel (GTK_WIDGET (iv))));
      return;
   }

   str = gtkutil_popup_textentry (_("Available plugins"), _("Plugin: "),
                                  plugin_list->data,
                                  plugin_list, -1,
                                  TEXT_ENTRY_NO_EDITABLE,
                                  GTK_WINDOW (gimv_widget_get_toplevel (GTK_WIDGET (iv))));

   if (!str) goto ERROR;

   mrls = gimv_xine_get_autoplay_mrls (gtx, str, &num);
   for (i = 0; i < num; i++) {
      GimvImageInfo *info;
      info = gimv_image_info_get_url (mrls[i]);
      info->flags |= GIMV_IMAGE_INFO_MOVIE_FLAG;
      info->flags |= GIMV_IMAGE_INFO_MRL_FLAG;
      mrl_list = g_list_append (mrl_list, info);
   }

   if (mrl_list) {
      gimv_image_view_set_list_self (iv, mrl_list, mrl_list);
      gimv_image_view_change_image (iv, mrl_list->data);
   } else {
      gtkutil_message_dialog (_("Error!"), _("No available MRL found."),
                              GTK_WINDOW (gimv_widget_get_toplevel (GTK_WIDGET (iv))));
   }

   g_free (str);
ERROR:
   g_list_free (plugin_list);
}


static GimvImageViewPlayableStatus
imageview_xine_get_status (GimvImageView *iv)
{
   GimvXine *gtx;
   GimvXineSpeed speed;

   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), GimvImageViewPlayableStop);

   gtx = GIMV_XINE (gimv_bin_get_child (GTK_WIDGET (iv->draw_area)));
   g_return_val_if_fail (GIMV_IS_XINE (gtx), GimvImageViewPlayableStop);

   if (!imageview_xine_is_playing (iv))
      return GimvImageViewPlayableStop;

   speed = gimv_xine_get_speed (gtx);
   if (speed > GIMV_XINE_SPEED_NORMAL) {
      return GimvImageViewPlayableForward;
   } else if (speed == GIMV_XINE_SPEED_PAUSE) {
      return GimvImageViewPlayablePause;
   } else if (speed == GIMV_XINE_SPEED_NORMAL) {
      return GimvImageViewPlayablePlay;
   }

   return GimvImageViewPlayableDisable;
}


static guint
imageview_xine_get_position (GimvImageView *iv)
{
   GimvXine *gtx;

   g_return_val_if_fail (iv, 0);
   g_return_val_if_fail (iv->info, 0);
   g_return_val_if_fail (gimv_image_info_is_movie (iv->info)
                         || gimv_image_info_is_audio (iv->info),
                         0);
   g_return_val_if_fail (GTK_IS_WIDGET (iv->draw_area), 0);

   gtx = GIMV_XINE (gimv_bin_get_child (GTK_WIDGET (iv->draw_area)));
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   return gimv_xine_get_current_time (GIMV_XINE (gtx));
}


static guint
imageview_xine_get_length (GimvImageView *iv)
{
   GimvXine *gtx;

   g_return_val_if_fail (iv, 0);
   g_return_val_if_fail (iv->info, 0);
   g_return_val_if_fail (gimv_image_info_is_movie (iv->info)
                         || gimv_image_info_is_audio (iv->info),
                         0);
   g_return_val_if_fail (GTK_IS_WIDGET (iv->draw_area), FALSE);

   gtx = GIMV_XINE (gimv_bin_get_child (GTK_WIDGET (iv->draw_area)));
   g_return_val_if_fail (GIMV_IS_XINE (gtx), 0);

   return gimv_xine_get_stream_length (GIMV_XINE (gtx));
}

#endif /* ENABLE_XINE */
