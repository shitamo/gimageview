/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2026 shitamo
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
 */

/*
 * GStreamer movie player (image view embedder), movie frame loader for
 * thumbnails and its preferences page.
 *
 * Playback runs a playbin in the gimv process.  The video goes to an
 * appsink (RGBA, square pixels) and every frame is shown as a GdkTexture
 * in a GtkPicture, so nothing depends on the windowing system; the audio
 * goes to the default sink (autoaudiosink).  Which formats can be played
 * depends on the installed GStreamer plugins (gstreamer1.0-plugins-good,
 * -bad, -ugly, gstreamer1.0-libav).
 */

#include "config.h"

#ifdef ENABLE_GSTREAMER

#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include "gimageview.h"
#include "gimv_gtk4_compat.h"
#include "gimv_image.h"
#include "gimv_image_info.h"
#include "gimv_image_loader.h"
#include "gimv_image_view.h"
#include "gimv_mime_types.h"
#include "gimv_plugin.h"
#include "gimv_prefs_ui_utils.h"
#include "gimv_prefs_win.h"
#include "gimv_thumb_cache.h"
#include "gtkutils.h"

#define GST_KEY "gimv-gst-player"

/* GstPlayFlags of playbin */
#define PLAY_FLAG_VIDEO (1 << 0)

#define CONF_THUMBNAIL_ENABLE_KEY "thumbnail_enable"
#define CONF_THUMBNAIL_ENABLE     "TRUE"
#define CONF_THUMBNAIL_POS_KEY    "thumbnail_pos"
#define CONF_THUMBNAIL_POS        "10.0"

#define THUMBNAIL_TIMEOUT (10 * GST_SECOND)


typedef struct GimvGstPlayer_Tag
{
   GimvImageView *iv;
   GtkWidget     *widget;     /* the draw area (event box) */
   GtkWidget     *picture;
   GtkWidget     *message;    /* error messages */

   GstElement    *playbin;
   guint          bus_watch;
   guint          timer;

   GMutex         lock;
   GstSample     *pending;    /* the newest frame, from the streaming thread */
   guint          idle_id;
   GstSample     *shown;      /* the frame on the screen */

   gboolean       playing;    /* PLAYING was requested */
   gboolean       paused;

   /* seek bar preview: a second, paused pipeline of the same file */
   GstElement    *pv_pipe;
   gchar         *pv_uri;
   guint          pv_bus_watch;
   gboolean       pv_ready;   /* prerolled once */
   gboolean       pv_failed;  /* no video / error: answer NULL */
   gboolean       pv_busy;    /* a seek is running */
   gint64         pv_want;    /* request while not ready/busy [ms], -1 */
   guint          pv_cur;     /* position of the running seek [ms] */
   GstSample     *pv_sample;  /* from the streaming thread (lock) */
   guint          pv_idle;
} GimvGstPlayer;


/******************************************************************************
 *
 *   plugin tables
 *
 ******************************************************************************/
static gboolean   imageview_gst_is_supported     (GimvImageView *iv,
                                                  GimvImageInfo *info);
static GtkWidget *imageview_gst_create           (GimvImageView *iv);
static void       imageview_gst_create_thumbnail (GimvImageView *iv,
                                                  const gchar   *type);

static gboolean   imageview_gst_is_playable      (GimvImageView *iv,
                                                  GimvImageInfo *info);
static gboolean   imageview_gst_is_seekable      (GimvImageView *iv);
static void       imageview_gst_play             (GimvImageView *iv);
static void       imageview_gst_stop             (GimvImageView *iv);
static void       imageview_gst_pause            (GimvImageView *iv);
static void       imageview_gst_seek             (GimvImageView *iv,
                                                  gfloat         pos);
static GimvImageViewPlayableStatus
                  imageview_gst_get_status       (GimvImageView *iv);
static guint      imageview_gst_get_length       (GimvImageView *iv);
static guint      imageview_gst_get_position     (GimvImageView *iv);
static void       imageview_gst_preview          (GimvImageView *iv,
                                                  guint          pos);

static GimvImage *gst_image_loader_load          (GimvImageLoader *loader,
                                                  gpointer         data);

static gboolean   prefs_gst_get_page             (guint              idx,
                                                  GimvPrefsWinPage **page,
                                                  guint             *size);


static GimvImageViewPlayableIF playable_if = {
   is_playable_fn:      imageview_gst_is_playable,
   is_seekable_fn:      imageview_gst_is_seekable,
   play_fn:             imageview_gst_play,
   stop_fn:             imageview_gst_stop,
   pause_fn:            imageview_gst_pause,
   forward_fn:          NULL,
   reverse_fn:          NULL,
   seek_fn:             imageview_gst_seek,
   eject_fn:            NULL,
   get_status_fn:       imageview_gst_get_status,
   get_length_fn:       imageview_gst_get_length,
   get_position_fn:     imageview_gst_get_position,
   preview_fn:          imageview_gst_preview,
};


static GimvImageViewPlugin imageview_gst =
{
   if_version:          GIMV_IMAGE_VIEW_IF_VERSION,
   label:               N_("Movie Player (GStreamer)"),

   /* before MPlayer and Xine: in-process and maintained */
   priority_hint:       G_PRIORITY_DEFAULT - 20,
   is_supported_fn:     imageview_gst_is_supported,
   create_fn:           imageview_gst_create,
   create_thumbnail_fn: imageview_gst_create_thumbnail,
   fullscreen_fn:       NULL,

   scalable:            NULL,
   rotatable:           NULL,
   playable:            &playable_if,
};


static GimvImageLoaderPlugin gst_image_loader =
{
   if_version:    GIMV_IMAGE_LOADER_IF_VERSION,
   id:            "GSTREAMER",
   priority_hint: GIMV_IMAGE_LOADER_PRIORITY_HIGH - 1,
   check_type:    NULL,
   get_info:      NULL,
   loader:        gst_image_loader_load,
};


static const gchar *mpeg_extensions[]      = { "mpg", "mpeg", "mpe", "mp2", "m2v", "vob" };
static const gchar *quicktime_extensions[] = { "mov", "qt", "moov", "qtvr" };
static const gchar *asf_extensions[]       = { "asf" };
static const gchar *wmv_extensions[]       = { "wmv" };
static const gchar *msvideo_extensions[]   = { "avi" };
static const gchar *au_extensions[]        = { "au", "snd" };
static const gchar *mp3_extensions[]       = { "mp3" };
static const gchar *wav_extensions[]       = { "wav", "wave" };
static const gchar *mp4_extensions[]       = { "mp4", "m4v" };
static const gchar *matroska_extensions[]  = { "mkv" };
static const gchar *webm_extensions[]      = { "webm" };
static const gchar *flv_extensions[]       = { "flv" };
static const gchar *ogg_video_extensions[] = { "ogv", "ogm" };
static const gchar *mpeg_ts_extensions[]   = { "ts", "m2ts", "mts" };
static const gchar *threegpp_extensions[]  = { "3gp" };
static const gchar *threegpp2_extensions[] = { "3g2" };
static const gchar *ogg_audio_extensions[] = { "ogg", "oga", "opus" };
static const gchar *flac_extensions[]      = { "flac" };
static const gchar *mp4_audio_extensions[] = { "m4a", "aac" };

#define MIME_ENTRY(type, desc, exts) \
   { mime_type: type, description: desc, extensions: exts, \
     extensions_len: G_N_ELEMENTS (exts), icon: NULL }

static GimvMimeTypeEntry gst_mime_types[] =
{
   MIME_ENTRY ("application/vnd.ms-asf", "ASF Media",             asf_extensions),
   MIME_ENTRY ("audio/au",               "Basic audio",           au_extensions),
   MIME_ENTRY ("audio/x-mp3",            "MPEG layer 3 Audio",    mp3_extensions),
   MIME_ENTRY ("audio/x-wav",            "WAV Audio",             wav_extensions),
   MIME_ENTRY ("audio/ogg",              "Ogg Audio",             ogg_audio_extensions),
   MIME_ENTRY ("audio/flac",             "FLAC Audio",            flac_extensions),
   MIME_ENTRY ("audio/mp4",              "MPEG-4 Audio",          mp4_audio_extensions),
   MIME_ENTRY ("video/mpeg",             "MPEG Video",            mpeg_extensions),
   MIME_ENTRY ("video/quicktime",        "Quicktime Video",       quicktime_extensions),
   MIME_ENTRY ("video/x-ms-asf",         "MS ASF Video",          asf_extensions),
   MIME_ENTRY ("video/x-ms-wmv",         "MS WMV Video",          wmv_extensions),
   MIME_ENTRY ("video/x-msvideo",        "Microsoft Video",       msvideo_extensions),
   MIME_ENTRY ("video/mp4",              "MPEG-4 Video",          mp4_extensions),
   MIME_ENTRY ("video/x-matroska",       "Matroska Video",        matroska_extensions),
   MIME_ENTRY ("video/webm",             "WebM Video",            webm_extensions),
   MIME_ENTRY ("video/x-flv",            "Flash Video",           flv_extensions),
   MIME_ENTRY ("video/ogg",              "Ogg Video",             ogg_video_extensions),
   MIME_ENTRY ("video/mp2t",             "MPEG Transport Stream", mpeg_ts_extensions),
   MIME_ENTRY ("video/3gpp",             "3GPP Video",            threegpp_extensions),
   MIME_ENTRY ("video/3gpp2",            "3GPP2 Video",           threegpp2_extensions),
};


static const gchar *
gimv_plugin_get_impl (guint idx, gpointer *impl, guint *size)
{
   g_return_val_if_fail (impl, NULL);
   *impl = NULL;
   g_return_val_if_fail (size, NULL);
   *size = 0;

   switch (idx) {
   case 0:
      *impl = &imageview_gst;
      *size = sizeof (imageview_gst);
      return GIMV_PLUGIN_IMAGEVIEW_EMBEDER;
   case 1:
      *impl = &gst_image_loader;
      *size = sizeof (gst_image_loader);
      return GIMV_PLUGIN_IMAGE_LOADER;
   default:
      return NULL;
   }
}
GIMV_PLUGIN_GET_MIME_TYPE(gst_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("GStreamer Embedder & Movie Frame Loader"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   N_("Plays movies and audio with GStreamer"),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  prefs_gst_get_page,
};


gchar *
g_module_check_init (GModule *module)
{
   GError *error = NULL;

   if (!gst_init_check (NULL, NULL, &error)) {
      gchar *msg = g_strdup_printf ("GStreamer: %s",
                                    error ? error->message : "gst_init failed");
      g_clear_error (&error);
      return msg;
   }

   /* gimv loads plugins with G_MODULE_BIND_LAZY and may unload a module;
      GStreamer keeps callbacks into this one */
   g_module_make_resident (module);

   return NULL;
}


/******************************************************************************
 *
 *   preferences
 *
 ******************************************************************************/
static gboolean
prefs_gst_get_thumb_enable (void)
{
   gboolean enable = TRUE;

   if (!gimv_plugin_prefs_load_value (gimv_plugin_info.name,
                                      GIMV_PLUGIN_IMAGE_LOADER,
                                      CONF_THUMBNAIL_ENABLE_KEY,
                                      GIMV_PLUGIN_PREFS_BOOL,
                                      (gpointer) &enable))
   {
      enable = !g_ascii_strcasecmp ("TRUE", CONF_THUMBNAIL_ENABLE);
      gimv_plugin_prefs_save_value (gimv_plugin_info.name,
                                    GIMV_PLUGIN_IMAGE_LOADER,
                                    CONF_THUMBNAIL_ENABLE_KEY,
                                    CONF_THUMBNAIL_ENABLE);
   }

   return enable;
}


static gfloat
prefs_gst_get_thumb_pos (void)
{
   gfloat pos = atof (CONF_THUMBNAIL_POS);

   if (!gimv_plugin_prefs_load_value (gimv_plugin_info.name,
                                      GIMV_PLUGIN_IMAGE_LOADER,
                                      CONF_THUMBNAIL_POS_KEY,
                                      GIMV_PLUGIN_PREFS_FLOAT,
                                      (gpointer) &pos))
   {
      pos = g_ascii_strtod (CONF_THUMBNAIL_POS, NULL);
      gimv_plugin_prefs_save_value (gimv_plugin_info.name,
                                    GIMV_PLUGIN_IMAGE_LOADER,
                                    CONF_THUMBNAIL_POS_KEY,
                                    CONF_THUMBNAIL_POS);
   }

   return CLAMP (pos, 0.0, 100.0);
}


typedef struct {
   gboolean thumb;
   gfloat   thumb_pos;
} GstConf;

static GstConf gconf, gconf_pre;


static GtkWidget *
prefs_gst_page (void)
{
   GtkWidget *main_vbox, *frame, *vbox, *hbox, *label, *spinner, *toggle;
   GtkAdjustment *adj;
   gchar *version, *text;

   gconf.thumb     = gconf_pre.thumb     = prefs_gst_get_thumb_enable ();
   gconf.thumb_pos = gconf_pre.thumb_pos = prefs_gst_get_thumb_pos ();

   main_vbox = gimv_vbox_new (FALSE, 0);

   /* GStreamer version */
   version = gst_version_string ();
   text = g_strdup_printf ("%s", version);
   label = gtk_label_new (text);
   gtk_widget_set_halign (label, GTK_ALIGN_START);
   gtk_widget_set_margin_start (label, 5);
   gtk_widget_set_margin_bottom (label, 5);
   gimv_box_pack_start (GTK_BOX (main_vbox), label, FALSE, FALSE, 0);
   g_free (text);
   g_free (version);

   /* thumbnail */
   gimv_prefs_ui_create_frame (_("Thumbnail"), frame, vbox, main_vbox, FALSE);

   toggle = gtkutil_create_check_button (_("Enable creating thumbnail of movie using GStreamer"),
                                         gconf.thumb,
                                         gtkutil_get_data_from_toggle_cb,
                                         &gconf.thumb);
   gimv_container_set_border_width (GTK_WIDGET (toggle), 5);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   hbox = gimv_hbox_new (FALSE, 5);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 5);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

   label = gtk_label_new (_("Stream position: "));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);

   adj = gtk_adjustment_new (gconf.thumb_pos, 0.0, 100.0, 0.1, 1.0, 0.0);
   spinner = gtkutil_create_spin_button (adj);
   gimv_widget_set_size (spinner, 70, -1);
   gtk_spin_button_set_digits (GTK_SPIN_BUTTON (spinner), 1);
   g_signal_connect (G_OBJECT (adj), "value_changed",
                     G_CALLBACK (gtkutil_get_data_from_adjustment_by_float_cb),
                     &gconf.thumb_pos);
   gimv_box_pack_start (GTK_BOX (hbox), spinner, FALSE, FALSE, 0);

   label = gtk_label_new (_("[%]"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);

   return main_vbox;
}


static gboolean
prefs_gst_apply (GimvPrefsWinAction action)
{
   GstConf *c = (action == GIMV_PREFS_WIN_ACTION_OK
                 || action == GIMV_PREFS_WIN_ACTION_APPLY) ? &gconf : &gconf_pre;
   gchar pos_str[G_ASCII_DTOSTR_BUF_SIZE];

   g_ascii_dtostr (pos_str, sizeof (pos_str), c->thumb_pos);

   gimv_plugin_prefs_save_value (gimv_plugin_info.name,
                                 GIMV_PLUGIN_IMAGE_LOADER,
                                 CONF_THUMBNAIL_ENABLE_KEY,
                                 c->thumb ? "TRUE" : "FALSE");
   gimv_plugin_prefs_save_value (gimv_plugin_info.name,
                                 GIMV_PLUGIN_IMAGE_LOADER,
                                 CONF_THUMBNAIL_POS_KEY,
                                 pos_str);

   return FALSE;
}


static GimvPrefsWinPage gst_prefs_page =
{
   path:           N_("/Movie and Audio/GStreamer"),
   priority_hint:  0,
   icon:           NULL,
   icon_open:      NULL,
   create_page_fn: prefs_gst_page,
   apply_fn:       prefs_gst_apply,
};


static gboolean
prefs_gst_get_page (guint idx, GimvPrefsWinPage **page, guint *size)
{
   g_return_val_if_fail (page, FALSE);
   *page = NULL;
   g_return_val_if_fail (size, FALSE);
   *size = 0;

   if (idx != 0) return FALSE;

   *page = &gst_prefs_page;
   *size = sizeof (gst_prefs_page);
   return TRUE;
}


/******************************************************************************
 *
 *   frames
 *
 ******************************************************************************/
typedef struct {
   GstVideoFrame frame;
} MappedFrame;


static void
mapped_frame_free (gpointer data)
{
   MappedFrame *m = data;

   gst_video_frame_unmap (&m->frame);
   g_free (m);
}


/* the RGBA frame of a sample as a texture (no copy) */
static GdkTexture *
texture_from_sample (GstSample *sample)
{
   GstVideoInfo info;
   GstCaps *caps = gst_sample_get_caps (sample);
   GstBuffer *buffer = gst_sample_get_buffer (sample);
   MappedFrame *m;
   GBytes *bytes;
   GdkTexture *texture;
   gsize size;

   if (!caps || !buffer || !gst_video_info_from_caps (&info, caps)) return NULL;
   if (GST_VIDEO_INFO_FORMAT (&info) != GST_VIDEO_FORMAT_RGBA) return NULL;

   m = g_new0 (MappedFrame, 1);
   if (!gst_video_frame_map (&m->frame, &info, buffer, GST_MAP_READ)) {
      g_free (m);
      return NULL;
   }

   size = (gsize) GST_VIDEO_FRAME_PLANE_STRIDE (&m->frame, 0)
      * GST_VIDEO_FRAME_HEIGHT (&m->frame);
   bytes = g_bytes_new_with_free_func (GST_VIDEO_FRAME_PLANE_DATA (&m->frame, 0),
                                       size, mapped_frame_free, m);
   texture = gdk_memory_texture_new (GST_VIDEO_FRAME_WIDTH (&m->frame),
                                     GST_VIDEO_FRAME_HEIGHT (&m->frame),
                                     GDK_MEMORY_R8G8B8A8,
                                     bytes,
                                     GST_VIDEO_FRAME_PLANE_STRIDE (&m->frame, 0));
   g_bytes_unref (bytes);

   return texture;
}


/* packed RGB (rowstride = width * 3) of an RGB or RGBA sample, for GimvImage */
static GimvImage *
image_from_sample (GstSample *sample)
{
   GstVideoInfo info;
   GstVideoFrame frame;
   GstCaps *caps = gst_sample_get_caps (sample);
   GstBuffer *buffer = gst_sample_get_buffer (sample);
   GstVideoFormat format;
   guchar *data;
   gint width, height, x, y, bpp;

   if (!caps || !buffer || !gst_video_info_from_caps (&info, caps)) return NULL;
   format = GST_VIDEO_INFO_FORMAT (&info);
   if (format == GST_VIDEO_FORMAT_RGB)       bpp = 3;
   else if (format == GST_VIDEO_FORMAT_RGBA) bpp = 4;
   else return NULL;

   if (!gst_video_frame_map (&frame, &info, buffer, GST_MAP_READ)) return NULL;

   width  = GST_VIDEO_FRAME_WIDTH (&frame);
   height = GST_VIDEO_FRAME_HEIGHT (&frame);
   data = g_try_malloc ((gsize) width * height * 3);
   if (data) {
      for (y = 0; y < height; y++) {
         const guchar *src = (const guchar *) GST_VIDEO_FRAME_PLANE_DATA (&frame, 0)
            + (gsize) y * GST_VIDEO_FRAME_PLANE_STRIDE (&frame, 0);
         guchar *dest = data + (gsize) y * width * 3;

         if (bpp == 3) {
            memcpy (dest, src, (gsize) width * 3);
         } else {
            for (x = 0; x < width; x++, src += 4, dest += 3) {
               dest[0] = src[0]; dest[1] = src[1]; dest[2] = src[2];
            }
         }
      }
   }
   gst_video_frame_unmap (&frame);

   if (!data) return NULL;

   return gimv_image_create_from_data (data, width, height, FALSE);
}


/******************************************************************************
 *
 *   player
 *
 ******************************************************************************/
static GimvGstPlayer *
get_player (GimvImageView *iv)
{
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), NULL);
   if (!iv->draw_area) return NULL;

   return g_object_get_data (G_OBJECT (iv->draw_area), GST_KEY);
}


static void
player_show_message (GimvGstPlayer *player, const gchar *message)
{
   gtk_label_set_text (GTK_LABEL (player->message), message ? message : "");
   gtk_widget_set_visible (player->message, message && *message);
}


static gboolean
idle_show_frame (gpointer data)
{
   GimvGstPlayer *player = data;
   GstSample *sample;
   GdkTexture *texture;

   g_mutex_lock (&player->lock);
   sample = player->pending;
   player->pending = NULL;
   player->idle_id = 0;
   g_mutex_unlock (&player->lock);

   if (!sample) return G_SOURCE_REMOVE;

   texture = texture_from_sample (sample);
   if (texture) {
      gtk_picture_set_paintable (GTK_PICTURE (player->picture),
                                 GDK_PAINTABLE (texture));
      g_object_unref (texture);
   }

   if (player->shown) gst_sample_unref (player->shown);
   player->shown = sample;

   return G_SOURCE_REMOVE;
}


/* streaming thread */
static GstFlowReturn
cb_new_frame (GstAppSink *sink, gpointer data, gboolean preroll)
{
   GimvGstPlayer *player = data;
   GstSample *sample;

   sample = preroll ? gst_app_sink_pull_preroll (sink)
                    : gst_app_sink_pull_sample (sink);
   if (!sample) return GST_FLOW_OK;

   g_mutex_lock (&player->lock);
   if (player->pending) gst_sample_unref (player->pending);
   player->pending = sample;             /* drop frames the GUI didn't show */
   if (!player->idle_id)
      player->idle_id = g_idle_add_full (G_PRIORITY_DEFAULT, idle_show_frame,
                                         player, NULL);
   g_mutex_unlock (&player->lock);

   return GST_FLOW_OK;
}


static GstFlowReturn
cb_new_sample (GstAppSink *sink, gpointer data)
{
   return cb_new_frame (sink, data, FALSE);
}


static GstFlowReturn
cb_new_preroll (GstAppSink *sink, gpointer data)
{
   return cb_new_frame (sink, data, TRUE);
}


static void
player_stop_timer (GimvGstPlayer *player)
{
   if (player->timer) {
      g_source_remove (player->timer);
      player->timer = 0;
   }
}


static gboolean
timeout_position (gpointer data)
{
   GimvGstPlayer *player = data;
   gint64 pos = 0, len = 0;

   if (gst_element_query_position (player->playbin, GST_FORMAT_TIME, &pos)
       && gst_element_query_duration (player->playbin, GST_FORMAT_TIME, &len)
       && len > 0)
   {
      gimv_image_view_playable_set_position (player->iv,
                                             (gfloat) pos * 100.0 / (gfloat) len);
   }

   return G_SOURCE_CONTINUE;
}


static void
player_set_stopped (GimvGstPlayer *player)
{
   player->playing = FALSE;
   player->paused  = FALSE;
   player_stop_timer (player);
   gimv_image_view_playable_set_status (player->iv, GimvImageViewPlayableStop);
}


static gboolean
cb_bus_message (GstBus *bus, GstMessage *msg, gpointer data)
{
   GimvGstPlayer *player = data;

   switch (GST_MESSAGE_TYPE (msg)) {
   case GST_MESSAGE_EOS:
   {
      gboolean next = FALSE;

      /* keep the last frame on the screen */
      gst_element_set_state (player->playbin, GST_STATE_READY);
      player_set_stopped (player);
      gimv_image_view_playable_set_position (player->iv, 100.0);

      g_object_get (G_OBJECT (player->iv), "continuance_play", &next, NULL);
      if (next)
         gimv_image_view_next (player->iv);
      break;
   }

   case GST_MESSAGE_ERROR:
   {
      GError *error = NULL;
      gchar *debug = NULL;

      gst_message_parse_error (msg, &error, &debug);
      g_warning ("GStreamer: %s\n%s", error ? error->message : "",
                 debug ? debug : "");
      if (error && (g_error_matches (error, GST_CORE_ERROR, GST_CORE_ERROR_MISSING_PLUGIN)
                    || g_error_matches (error, GST_STREAM_ERROR,
                                        GST_STREAM_ERROR_CODEC_NOT_FOUND)))
      {
         gchar *text = g_strdup_printf ("%s\n\n%s", error->message,
                                        _("A GStreamer plugin for this format may not be "
                                          "installed (gstreamer1.0-plugins-good, -bad, "
                                          "-ugly, gstreamer1.0-libav)."));
         player_show_message (player, text);
         g_free (text);
      } else {
         player_show_message (player, error ? error->message : _("Error"));
      }
      g_clear_error (&error);
      g_free (debug);

      gst_element_set_state (player->playbin, GST_STATE_NULL);
      player_set_stopped (player);
      break;
   }

   case GST_MESSAGE_STATE_CHANGED:
   {
      GstState old_state, new_state;

      if (GST_MESSAGE_SRC (msg) != GST_OBJECT (player->playbin)) break;
      gst_message_parse_state_changed (msg, &old_state, &new_state, NULL);

      if (new_state == GST_STATE_PLAYING) {
         gimv_image_view_playable_set_status (player->iv, GimvImageViewPlayablePlay);
         if (!player->timer)
            player->timer = g_timeout_add (250, timeout_position, player);
      } else if (new_state == GST_STATE_PAUSED && old_state == GST_STATE_PLAYING) {
         gimv_image_view_playable_set_status (player->iv, GimvImageViewPlayablePause);
         player_stop_timer (player);
         timeout_position (player);
      }
      break;
   }

   default:
      break;
   }

   return G_SOURCE_CONTINUE;
}


/* forget a frame that the streaming thread queued but the GUI didn't show */
static void
player_drop_pending (GimvGstPlayer *player)
{
   g_mutex_lock (&player->lock);
   if (player->idle_id) {
      g_source_remove (player->idle_id);
      player->idle_id = 0;
   }
   if (player->pending) {
      gst_sample_unref (player->pending);
      player->pending = NULL;
   }
   g_mutex_unlock (&player->lock);
}


/******************************************************************************
 *
 *   seek bar preview
 *
 ******************************************************************************/
#define PREVIEW_FRAME_WIDTH 192   /* = the popover picture */

static void preview_seek (GimvGstPlayer *player, guint ms);


static void
preview_answer (GimvGstPlayer *player, GdkTexture *texture, guint ms)
{
   gimv_image_view_playable_set_preview (player->iv, texture, ms);
}


static void
preview_shutdown (GimvGstPlayer *player)
{
   if (player->pv_pipe)
      gst_element_set_state (player->pv_pipe, GST_STATE_NULL);   /* joins threads */
   if (player->pv_bus_watch) {
      g_source_remove (player->pv_bus_watch);
      player->pv_bus_watch = 0;
   }

   g_mutex_lock (&player->lock);
   if (player->pv_idle) {
      g_source_remove (player->pv_idle);
      player->pv_idle = 0;
   }
   if (player->pv_sample) {
      gst_sample_unref (player->pv_sample);
      player->pv_sample = NULL;
   }
   g_mutex_unlock (&player->lock);

   if (player->pv_pipe) {
      gst_object_unref (player->pv_pipe);
      player->pv_pipe = NULL;
   }
   g_free (player->pv_uri);
   player->pv_uri    = NULL;
   player->pv_ready  = FALSE;
   player->pv_failed = FALSE;
   player->pv_busy   = FALSE;
   player->pv_want   = -1;
}


static gboolean
idle_preview_frame (gpointer data)
{
   GimvGstPlayer *player = data;
   GstSample *sample;
   GdkTexture *texture = NULL;

   g_mutex_lock (&player->lock);
   sample = player->pv_sample;
   player->pv_sample = NULL;
   player->pv_idle = 0;
   g_mutex_unlock (&player->lock);

   if (!sample) return G_SOURCE_REMOVE;

   if (!player->pv_ready) {
      /* the first preroll (position 0) only says that seeking works now */
      player->pv_ready = TRUE;
      gst_sample_unref (sample);
      if (player->pv_want >= 0) {
         guint ms = player->pv_want;
         player->pv_want = -1;
         preview_seek (player, ms);
      }
      return G_SOURCE_REMOVE;
   }

   texture = texture_from_sample (sample);
   gst_sample_unref (sample);
   player->pv_busy = FALSE;

   preview_answer (player, texture, player->pv_cur);   /* may ask again */
   if (texture) g_object_unref (texture);

   return G_SOURCE_REMOVE;
}


/* streaming thread */
static GstFlowReturn
cb_preview_preroll (GstAppSink *sink, gpointer data)
{
   GimvGstPlayer *player = data;
   GstSample *sample = gst_app_sink_pull_preroll (sink);

   if (!sample) return GST_FLOW_OK;

   g_mutex_lock (&player->lock);
   if (player->pv_sample) gst_sample_unref (player->pv_sample);
   player->pv_sample = sample;
   if (!player->pv_idle)
      player->pv_idle = g_idle_add_full (G_PRIORITY_DEFAULT, idle_preview_frame,
                                         player, NULL);
   g_mutex_unlock (&player->lock);

   return GST_FLOW_OK;
}


static gboolean
cb_preview_bus (GstBus *bus, GstMessage *msg, gpointer data)
{
   GimvGstPlayer *player = data;

   if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
      /* e.g. no video stream: no preview pictures for this file */
      gboolean was_busy = player->pv_busy || !player->pv_ready;
      guint ms = player->pv_want >= 0 ? (guint) player->pv_want : player->pv_cur;

      player->pv_failed = TRUE;
      player->pv_busy   = FALSE;
      player->pv_want   = -1;
      if (player->pv_pipe)
         gst_element_set_state (player->pv_pipe, GST_STATE_NULL);
      if (was_busy)
         preview_answer (player, NULL, ms);
   }

   return G_SOURCE_CONTINUE;
}


static gboolean
preview_create (GimvGstPlayer *player, const gchar *uri)
{
   GstElement *sink, *audio_sink;
   GstCaps *caps;
   GstBus *bus;
   GstAppSinkCallbacks callbacks = { NULL, cb_preview_preroll, NULL };

   player->pv_pipe = gst_element_factory_make ("playbin", NULL);
   sink            = gst_element_factory_make ("appsink", NULL);
   audio_sink      = gst_element_factory_make ("fakesink", NULL);
   if (!player->pv_pipe || !sink || !audio_sink) {
      if (sink)       gst_object_unref (gst_object_ref_sink (sink));
      if (audio_sink) gst_object_unref (gst_object_ref_sink (audio_sink));
      if (player->pv_pipe) {
         gst_object_unref (gst_object_ref_sink (player->pv_pipe));
         player->pv_pipe = NULL;
      }
      return FALSE;
   }
   gst_object_ref_sink (player->pv_pipe);

   /* small frames: the height follows the aspect ratio */
   caps = gst_caps_new_simple ("video/x-raw",
                               "format", G_TYPE_STRING, "RGBA",
                               "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
                               "width", G_TYPE_INT, PREVIEW_FRAME_WIDTH,
                               NULL);
   g_object_set (sink, "caps", caps, "sync", FALSE, "max-buffers", 1,
                 "drop", TRUE, NULL);
   gst_caps_unref (caps);
   gst_app_sink_set_callbacks (GST_APP_SINK (sink), &callbacks, player, NULL);

   g_object_set (player->pv_pipe,
                 "uri",        uri,
                 "video-sink", sink,
                 "audio-sink", audio_sink,
                 "flags",      PLAY_FLAG_VIDEO,
                 NULL);

   bus = gst_element_get_bus (player->pv_pipe);
   player->pv_bus_watch = gst_bus_add_watch (bus, cb_preview_bus, player);
   gst_object_unref (bus);

   player->pv_uri    = g_strdup (uri);
   player->pv_ready  = FALSE;
   player->pv_failed = FALSE;
   player->pv_busy   = FALSE;
   player->pv_want   = -1;

   gst_element_set_state (player->pv_pipe, GST_STATE_PAUSED);

   return TRUE;
}


static void
preview_seek (GimvGstPlayer *player, guint ms)
{
   player->pv_busy = TRUE;
   player->pv_cur  = ms;

   /* key frames only: fast; good enough for a preview */
   if (!gst_element_seek_simple (player->pv_pipe, GST_FORMAT_TIME,
                                 GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT
                                 | GST_SEEK_FLAG_SNAP_NEAREST,
                                 (gint64) ms * GST_MSECOND))
   {
      player->pv_busy = FALSE;
      preview_answer (player, NULL, ms);
   }
}


static void
imageview_gst_preview (GimvImageView *iv, guint pos)
{
   GimvGstPlayer *player = get_player (iv);
   gchar *filename;
   gchar *uri;

   if (!player || !iv->info || !gimv_image_info_is_movie (iv->info)) {
      if (iv) gimv_image_view_playable_set_preview (iv, NULL, pos);
      return;
   }

   filename = gimv_image_info_get_local_path (iv->info);
   uri = filename ? gst_filename_to_uri (filename, NULL) : NULL;
   g_free (filename);
   if (!uri) {
      gimv_image_view_playable_set_preview (iv, NULL, pos);
      return;
   }

   if (player->pv_pipe && g_strcmp0 (player->pv_uri, uri))
      preview_shutdown (player);

   if (!player->pv_pipe && !preview_create (player, uri)) {
      g_free (uri);
      gimv_image_view_playable_set_preview (iv, NULL, pos);
      return;
   }
   g_free (uri);

   if (player->pv_failed) {
      gimv_image_view_playable_set_preview (iv, NULL, pos);
      return;
   }

   if (!player->pv_ready || player->pv_busy) {
      player->pv_want = pos;   /* sought when the pipeline is ready */
      return;
   }

   preview_seek (player, pos);
}


static void
player_shutdown (GimvGstPlayer *player)
{
   if (player->playbin) {
      /* joins the streaming threads */
      gst_element_set_state (player->playbin, GST_STATE_NULL);
   }
   player_stop_timer (player);
   if (player->bus_watch) {
      g_source_remove (player->bus_watch);
      player->bus_watch = 0;
   }

   player_drop_pending (player);
   preview_shutdown (player);

   if (player->playbin) {
      gst_object_unref (player->playbin);
      player->playbin = NULL;
   }
}


static void
player_free (gpointer data)
{
   GimvGstPlayer *player = data;

   player_shutdown (player);
   if (player->shown) gst_sample_unref (player->shown);
   g_mutex_clear (&player->lock);
   g_free (player);
}


static void
cb_draw_area_destroy (GtkWidget *widget, gpointer data)
{
   player_shutdown (data);
}


static gboolean
player_create_pipeline (GimvGstPlayer *player)
{
   GstElement *sink;
   GstCaps *caps;
   GstBus *bus;
   GstAppSinkCallbacks callbacks = { NULL, cb_new_preroll, cb_new_sample };

   player->playbin = gst_element_factory_make ("playbin", NULL);
   sink = gst_element_factory_make ("appsink", NULL);
   if (!player->playbin || !sink) {
      if (sink) gst_object_unref (gst_object_ref_sink (sink));
      if (player->playbin) {
         gst_object_unref (player->playbin);
         player->playbin = NULL;
      }
      return FALSE;
   }
   gst_object_ref_sink (player->playbin);

   /* square pixels: the picture shows the texture's own aspect ratio */
   caps = gst_caps_from_string ("video/x-raw,format=RGBA,pixel-aspect-ratio=1/1");
   g_object_set (sink,
                 "caps",         caps,
                 "max-buffers",  2,
                 "drop",         TRUE,
                 NULL);
   gst_caps_unref (caps);
   gst_app_sink_set_callbacks (GST_APP_SINK (sink), &callbacks, player, NULL);

   g_object_set (player->playbin, "video-sink", sink, NULL);

   bus = gst_element_get_bus (player->playbin);
   player->bus_watch = gst_bus_add_watch (bus, cb_bus_message, player);
   gst_object_unref (bus);

   return TRUE;
}


static void
player_start (GimvGstPlayer *player)
{
   GimvImageView *iv = player->iv;
   gchar *filename;
   gchar *uri;

   if (!iv->info) return;
   if (!gimv_image_info_is_movie (iv->info) && !gimv_image_info_is_audio (iv->info))
      return;

   if (!player->playbin && !player_create_pipeline (player)) {
      player_show_message (player,
                           _("GStreamer: the \"playbin\" element is not available."));
      return;
   }

   filename = gimv_image_info_get_local_path (iv->info);
   if (!filename) {
      player_show_message (player, _("Cannot extract the file from the archive."));
      return;
   }
   uri = gst_filename_to_uri (filename, NULL);
   g_free (filename);
   if (!uri) return;

   gst_element_set_state (player->playbin, GST_STATE_NULL);
   player_drop_pending (player);
   player_show_message (player, NULL);
   gtk_picture_set_paintable (GTK_PICTURE (player->picture), NULL);
   if (player->shown) {
      gst_sample_unref (player->shown);
      player->shown = NULL;
   }

   g_object_set (player->playbin, "uri", uri, NULL);
   g_free (uri);

   player->playing = TRUE;
   player->paused  = FALSE;
   gst_element_set_state (player->playbin, GST_STATE_PLAYING);
}


/******************************************************************************
 *
 *   image view embedder
 *
 ******************************************************************************/
static gboolean
imageview_gst_is_supported (GimvImageView *iv, GimvImageInfo *info)
{
   g_return_val_if_fail (GIMV_IS_IMAGE_VIEW (iv), FALSE);
   if (!info) return FALSE;

   return gimv_image_info_is_movie (info) || gimv_image_info_is_audio (info);
}


static GtkWidget *
imageview_gst_create (GimvImageView *iv)
{
   static gboolean css_done = FALSE;
   GimvGstPlayer *player;
   GtkWidget *widget, *overlay;

   if (!css_done) {
      GtkCssProvider *provider = gtk_css_provider_new ();
      gtk_css_provider_load_from_string (provider,
         ".gimv-gst-video { background-color: black; }\n"
         ".gimv-gst-message { color: white; background-color: rgba(0,0,0,0.6);"
         " padding: 6px; }\n");
      gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                  GTK_STYLE_PROVIDER (provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      g_object_unref (provider);
      css_done = TRUE;
   }

   player = g_new0 (GimvGstPlayer, 1);
   g_mutex_init (&player->lock);
   player->iv = iv;

   widget = gimv_event_box_new ();
   player->widget = widget;

   overlay = gtk_overlay_new ();
   gtk_widget_add_css_class (overlay, "gimv-gst-video");
   gimv_container_add (GTK_WIDGET (widget), overlay);
   gtk_widget_set_hexpand (overlay, TRUE);
   gtk_widget_set_vexpand (overlay, TRUE);

   player->picture = gtk_picture_new ();
   gtk_picture_set_content_fit (GTK_PICTURE (player->picture), GTK_CONTENT_FIT_CONTAIN);
   gtk_picture_set_can_shrink (GTK_PICTURE (player->picture), TRUE);
   gtk_widget_set_hexpand (player->picture, TRUE);
   gtk_widget_set_vexpand (player->picture, TRUE);
   gtk_overlay_set_child (GTK_OVERLAY (overlay), player->picture);

   player->message = gtk_label_new (NULL);
   gtk_label_set_wrap (GTK_LABEL (player->message), TRUE);
   gtk_label_set_justify (GTK_LABEL (player->message), GTK_JUSTIFY_CENTER);
   gtk_label_set_max_width_chars (GTK_LABEL (player->message), 50);
   gtk_widget_add_css_class (player->message, "gimv-gst-message");
   gtk_widget_set_halign (player->message, GTK_ALIGN_CENTER);
   gtk_widget_set_valign (player->message, GTK_ALIGN_CENTER);
   gtk_widget_set_visible (player->message, FALSE);
   gtk_overlay_add_overlay (GTK_OVERLAY (overlay), player->message);

   g_object_set_data_full (G_OBJECT (widget), GST_KEY, player, player_free);
   g_signal_connect (widget, "destroy", G_CALLBACK (cb_draw_area_destroy), player);

   return widget;
}


static void
imageview_gst_create_thumbnail (GimvImageView *iv, const gchar *cache_write_type)
{
   GimvGstPlayer *player = get_player (iv);
   GimvImage *image, *imcache;
   gchar *filename;

   if (!player || !iv->info || !gimv_image_info_is_movie (iv->info)) return;
   if (!player->shown) return;

   image = image_from_sample (player->shown);
   if (!image) return;

   filename = gimv_image_info_get_path_with_archive (iv->info);
   imcache = gimv_thumb_cache_save (filename, cache_write_type, image, iv->info);
   if (imcache) {
      gimv_image_unref (imcache);
      g_signal_emit_by_name (G_OBJECT (iv), "thumbnail_created", iv->info);
   }

   g_free (filename);
   gimv_image_unref (image);
}


static gboolean
imageview_gst_is_playable (GimvImageView *iv, GimvImageInfo *info)
{
   return TRUE;
}


static gboolean
imageview_gst_is_seekable (GimvImageView *iv)
{
   return TRUE;
}


static void
imageview_gst_play (GimvImageView *iv)
{
   GimvGstPlayer *player = get_player (iv);

   if (!player) return;

   if (!player->playing) {
      player_start (player);
   } else if (player->paused) {
      player->paused = FALSE;
      gst_element_set_state (player->playbin, GST_STATE_PLAYING);
   } else {
      imageview_gst_pause (iv);          /* the play button toggles */
   }
}


static void
imageview_gst_pause (GimvImageView *iv)
{
   GimvGstPlayer *player = get_player (iv);

   if (!player || !player->playbin || !player->playing || player->paused) return;

   player->paused = TRUE;
   gst_element_set_state (player->playbin, GST_STATE_PAUSED);
}


static void
imageview_gst_stop (GimvImageView *iv)
{
   GimvGstPlayer *player = get_player (iv);

   if (!player || !player->playbin) return;

   gst_element_set_state (player->playbin, GST_STATE_READY);
   if (player->playing) {
      player_set_stopped (player);
      gimv_image_view_playable_set_position (iv, 0.0);
   }
}


static void
imageview_gst_seek (GimvImageView *iv, gfloat pos)
{
   GimvGstPlayer *player = get_player (iv);
   gint64 len = 0;

   if (!player || !player->playbin || !player->playing) return;
   if (!gst_element_query_duration (player->playbin, GST_FORMAT_TIME, &len) || len <= 0)
      return;

   gst_element_seek_simple (player->playbin, GST_FORMAT_TIME,
                            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                            (gint64) (len * CLAMP (pos, 0.0, 100.0) / 100.0));
}


static GimvImageViewPlayableStatus
imageview_gst_get_status (GimvImageView *iv)
{
   GimvGstPlayer *player = get_player (iv);

   if (!player || !player->playing) return GimvImageViewPlayableStop;
   if (player->paused) return GimvImageViewPlayablePause;
   return GimvImageViewPlayablePlay;
}


/* [ms] */
static guint
imageview_gst_get_length (GimvImageView *iv)
{
   GimvGstPlayer *player = get_player (iv);
   gint64 len = 0;

   if (!player || !player->playbin) return 0;
   if (!gst_element_query_duration (player->playbin, GST_FORMAT_TIME, &len) || len < 0)
      return 0;

   return len / GST_MSECOND;
}


static guint
imageview_gst_get_position (GimvImageView *iv)
{
   GimvGstPlayer *player = get_player (iv);
   gint64 pos = 0;

   if (!player || !player->playbin) return 0;
   if (!gst_element_query_position (player->playbin, GST_FORMAT_TIME, &pos) || pos < 0)
      return 0;

   return pos / GST_MSECOND;
}


/******************************************************************************
 *
 *   movie frame loader (thumbnails)
 *
 ******************************************************************************/
/*
 *  Wait for the state change; FALSE on error, timeout or cancel.  Waits in
 *  short steps and lets the loader handle events in between (the thumbnail
 *  view and the image view flush events on "progress_update"), so the
 *  windows stay responsive while a movie thumbnail is made.
 */
#define THUMBNAIL_STEP (50 * GST_MSECOND)

static gboolean
wait_for_preroll (GimvImageLoader *loader, GstElement *pipeline)
{
   GstStateChangeReturn ret;
   GstClockTime waited = 0;

   do {
      ret = gst_element_get_state (pipeline, NULL, NULL, THUMBNAIL_STEP);
      if (ret != GST_STATE_CHANGE_ASYNC) break;
      waited += THUMBNAIL_STEP;
      if (!gimv_image_loader_progress_update (loader)) return FALSE;
   } while (waited < THUMBNAIL_TIMEOUT);

   return ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_NO_PREROLL;
}


static GimvImage *
gst_image_loader_load (GimvImageLoader *loader, gpointer data)
{
   const gchar *filename;
   GstElement *playbin, *sink, *audio_sink;
   GstCaps *caps;
   GstSample *sample = NULL;
   GimvImage *image = NULL;
   gint64 len = 0;
   gchar *uri;

   g_return_val_if_fail (loader, NULL);

   filename = gimv_image_loader_get_path (loader);
   if (!filename || !*filename) return NULL;
   if (!loader->info || !gimv_image_info_is_movie (loader->info)) return NULL;
   if (!prefs_gst_get_thumb_enable ()) return NULL;

   uri = gst_filename_to_uri (filename, NULL);
   if (!uri) return NULL;

   playbin    = gst_element_factory_make ("playbin", NULL);
   sink       = gst_element_factory_make ("appsink", NULL);
   audio_sink = gst_element_factory_make ("fakesink", NULL);
   if (!playbin || !sink || !audio_sink) {
      if (playbin)    gst_object_unref (gst_object_ref_sink (playbin));
      if (sink)       gst_object_unref (gst_object_ref_sink (sink));
      if (audio_sink) gst_object_unref (gst_object_ref_sink (audio_sink));
      g_free (uri);
      return NULL;
   }
   gst_object_ref_sink (playbin);

   caps = gst_caps_from_string ("video/x-raw,format=RGB,pixel-aspect-ratio=1/1");
   g_object_set (sink, "caps", caps, "sync", FALSE, NULL);
   gst_caps_unref (caps);

   g_object_set (playbin,
                 "uri",        uri,
                 "video-sink", sink,
                 "audio-sink", audio_sink,
                 "flags",      PLAY_FLAG_VIDEO,
                 NULL);
   g_free (uri);

   gst_element_set_state (playbin, GST_STATE_PAUSED);
   if (!wait_for_preroll (loader, playbin)) goto END;

   if (gst_element_query_duration (playbin, GST_FORMAT_TIME, &len) && len > 0) {
      gint64 pos = (gint64) (len * prefs_gst_get_thumb_pos () / 100.0);

      if (pos > 0
          && gst_element_seek_simple (playbin, GST_FORMAT_TIME,
                                      GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                      pos))
      {
         if (!wait_for_preroll (loader, playbin)) goto END;
      }
   }

   /* prerolled: the frame is there (or the stream has none) */
   sample = gst_app_sink_try_pull_preroll (GST_APP_SINK (sink), THUMBNAIL_STEP);
   if (sample) {
      image = image_from_sample (sample);
      gst_sample_unref (sample);
   }

END:
   gst_element_set_state (playbin, GST_STATE_NULL);
   gst_object_unref (playbin);

   return image;
}

#endif /* ENABLE_GSTREAMER */
