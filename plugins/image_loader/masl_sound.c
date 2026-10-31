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
 * The sound of MASL: the PCMs of a .pkn (signed 8 bit, or the 4 bit
 * ADPCM (OKI MSM6258) of the X68000) on 8 channels, mixed and played with
 * GStreamer (appsrc ! audioconvert ! audioresample ! autoaudiosink).
 *
 * The player tells what it does with the PCMs at its (virtual) times; the
 * pictures of those times are shown as the pipeline's clock goes from
 * start () on, so the sound is made at the same times: the samples of a
 * time are mixed when the sink asks for them, with the events up to it.
 * The player is read ahead a picture; a little past the time it has
 * reached (it is late) the channels go on, far past it (the animation was
 * stopped) silence is made and they wait.
 *
 * GIMV_MASL_AUDIO_SINK replaces the sink (a gst-launch description, for
 * tests: "wavenc ! filesink location=x.wav").
 */

#include "config.h"

#include <string.h>

#include "masl.h"

#ifdef ENABLE_GSTREAMER

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#define SOUND_RATE      44100
#define SOUND_CHUNK     1024              /* samples a buffer */
#define SOUND_CHANNELS  8
/* the sound goes on this far past the time the player has reached (it is
 * late), then stops (the animation was stopped) */
#define SOUND_MARGIN    2.0               /* s */

typedef struct {
   MaslSoundWhat what;
   gint          ch, pcm;
   guint         rate;
   gboolean      loop;
   gdouble       time;                    /* s */
} SoundEvent;

typedef struct {
   const gint16 *samples;                 /* NULL: not playing */
   gsize         len;
   gdouble       pos, step;
   gboolean      loop;
} SoundChannel;

struct MaslSound_Tag {
   GstElement   *pipeline;
   GstElement   *src;
   gint16      **pcms;                    /* decoded */
   gsize        *lens;
   guint         n_pcms;

   GMutex        lock;
   GQueue        events;                  /* SoundEvent, in time */
   gdouble       known;                   /* s the player has reached */
   SoundChannel  ch[SOUND_CHANNELS];
   guint64       done;                    /* samples made */
};


/* the ADPCM of the MSM6258: 4 bits a sample, the low nibble first */
static gint16 *
adpcm_decode (const guchar *data, gsize len, gsize *out_len)
{
   static const gint steps[49] = {
      16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
      73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
      337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
      1552
   };
   static const gint adjust[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
   gint16 *out = g_new (gint16, len * 2);
   gint value = 0, index = 0;
   gsize i;

   for (i = 0; i < len * 2; i++) {
      gint nib = (i & 1) ? data[i / 2] >> 4 : data[i / 2] & 0x0f;
      gint step = steps[index];
      gint diff = step / 8;
      if (nib & 1) diff += step / 4;
      if (nib & 2) diff += step / 2;
      if (nib & 4) diff += step;
      value += (nib & 8) ? -diff : diff;
      value = CLAMP (value, -2048, 2047);
      index = CLAMP (index + adjust[nib & 7], 0, 48);
      out[i] = (gint16) (value * 16);
   }
   *out_len = len * 2;
   return out;
}


static void
apply_event (MaslSound *snd, const SoundEvent *ev)
{
   gint i;

   if (ev->what == MASL_SOUND_STOP) {
      for (i = 0; i < SOUND_CHANNELS; i++)
         if (ev->ch & (1 << i)) snd->ch[i].samples = NULL;
      return;
   }
   if (ev->ch < 0 || ev->ch >= SOUND_CHANNELS || ev->pcm < 0 || ev->pcm >= (gint) snd->n_pcms)
      return;
   snd->ch[ev->ch].samples = snd->pcms[ev->pcm];
   snd->ch[ev->ch].len     = snd->lens[ev->pcm];
   snd->ch[ev->ch].pos     = 0;
   snd->ch[ev->ch].step    = (ev->rate ? ev->rate : 16000) / (gdouble) SOUND_RATE;
   snd->ch[ev->ch].loop    = ev->loop;
}


/* the next chunk of samples, with the events up to them */
static void
cb_need_data (GstAppSrc *src, guint length, MaslSound *snd)
{
   GstBuffer *buf = gst_buffer_new_allocate (NULL, SOUND_CHUNK * 2, NULL);
   GstMapInfo map;
   gint16 *out;
   guint i;

   gst_buffer_map (buf, &map, GST_MAP_WRITE);
   out = (gint16 *) map.data;

   g_mutex_lock (&snd->lock);
   for (i = 0; i < SOUND_CHUNK; i++) {
      gdouble t = (snd->done + i) / (gdouble) SOUND_RATE;
      gint mix = 0, c;
      SoundEvent *ev;

      while ((ev = g_queue_peek_head (&snd->events)) && ev->time <= t) {
         apply_event (snd, ev);
         g_free (g_queue_pop_head (&snd->events));
      }
      /* the player has not got so far: silence, the channels wait */
      if (t > snd->known + SOUND_MARGIN) {
         out[i] = 0;
         continue;
      }
      for (c = 0; c < SOUND_CHANNELS; c++) {
         SoundChannel *ch = &snd->ch[c];
         gsize p;
         if (!ch->samples) continue;
         p = (gsize) ch->pos;
         if (p >= ch->len) {
            if (!ch->loop || ch->len == 0) { ch->samples = NULL; continue; }
            ch->pos -= ch->len;
            p = (gsize) ch->pos;
         }
         mix += ch->samples[p];
         ch->pos += ch->step;
      }
      out[i] = (gint16) CLAMP (mix / 2, -32768, 32767);
   }
   GST_BUFFER_PTS (buf) = gst_util_uint64_scale (snd->done, GST_SECOND, SOUND_RATE);
   GST_BUFFER_DURATION (buf) = gst_util_uint64_scale (SOUND_CHUNK, GST_SECOND, SOUND_RATE);
   snd->done += SOUND_CHUNK;
   g_mutex_unlock (&snd->lock);

   gst_buffer_unmap (buf, &map);
   gst_app_src_push_buffer (src, buf);
}


static GstElement *
make_sink (void)
{
   const gchar *desc = g_getenv ("GIMV_MASL_AUDIO_SINK");
   GstElement *sink = NULL;

   if (desc && *desc)
      sink = gst_parse_bin_from_description (desc, TRUE, NULL);
   if (!sink)
      sink = gst_element_factory_make ("autoaudiosink", NULL);
   return sink;
}


MaslSound *
masl_sound_new (const MaslPcm *pcms, guint n_pcms)
{
   MaslSound *snd;
   GstElement *conv, *resample, *sink;
   GstCaps *caps;
   GstAppSrcCallbacks cbs = { 0 };
   guint i;

   if (n_pcms == 0) return NULL;
   if (!gst_init_check (NULL, NULL, NULL)) return NULL;

   snd = g_new0 (MaslSound, 1);
   g_mutex_init (&snd->lock);
   g_queue_init (&snd->events);

   snd->n_pcms = n_pcms;
   snd->pcms = g_new0 (gint16 *, n_pcms);
   snd->lens = g_new0 (gsize, n_pcms);
   for (i = 0; i < n_pcms; i++) {
      if (pcms[i].adpcm) {
         snd->pcms[i] = adpcm_decode (pcms[i].data, pcms[i].len, &snd->lens[i]);
      } else {
         gsize k;
         snd->pcms[i] = g_new (gint16, pcms[i].len ? pcms[i].len : 1);
         for (k = 0; k < pcms[i].len; k++)
            snd->pcms[i][k] = (gint16) ((gint8) pcms[i].data[k] * 256);
         snd->lens[i] = pcms[i].len;
      }
   }

   snd->pipeline = gst_pipeline_new ("masl-sound");
   snd->src = gst_element_factory_make ("appsrc", NULL);
   conv = gst_element_factory_make ("audioconvert", NULL);
   resample = gst_element_factory_make ("audioresample", NULL);
   sink = make_sink ();
   if (!snd->pipeline || !snd->src || !conv || !resample || !sink) {
      if (snd->src) gst_object_unref (snd->src);
      if (conv) gst_object_unref (conv);
      if (resample) gst_object_unref (resample);
      if (sink) gst_object_unref (sink);
      if (snd->pipeline) gst_object_unref (snd->pipeline);
      snd->pipeline = snd->src = NULL;
      masl_sound_free (snd);
      return NULL;
   }
   gst_bin_add_many (GST_BIN (snd->pipeline), snd->src, conv, resample, sink, NULL);
   if (!gst_element_link_many (snd->src, conv, resample, sink, NULL)) {
      masl_sound_free (snd);
      return NULL;
   }

   caps = gst_caps_new_simple ("audio/x-raw",
                               "format",   G_TYPE_STRING, "S16LE",
                               "layout",   G_TYPE_STRING, "interleaved",
                               "rate",     G_TYPE_INT,    SOUND_RATE,
                               "channels", G_TYPE_INT,    1,
                               NULL);
   g_object_set (snd->src,
                 "caps",        caps,
                 "format",      GST_FORMAT_TIME,
                 "stream-type", GST_APP_STREAM_TYPE_STREAM,
                 "is-live",     FALSE,
                 "max-bytes",   (guint64) SOUND_CHUNK * 2 * 4,
                 NULL);
   gst_caps_unref (caps);
   cbs.need_data = (void (*) (GstAppSrc *, guint, gpointer)) cb_need_data;
   gst_app_src_set_callbacks (GST_APP_SRC (snd->src), &cbs, snd, NULL);

   return snd;
}


void
masl_sound_free (MaslSound *snd)
{
   guint i;

   if (!snd) return;
   if (snd->pipeline) {
      gst_element_set_state (snd->pipeline, GST_STATE_NULL);
      gst_object_unref (snd->pipeline);
   }
   g_queue_clear_full (&snd->events, g_free);
   for (i = 0; i < snd->n_pcms; i++) g_free (snd->pcms[i]);
   g_free (snd->pcms);
   g_free (snd->lens);
   g_mutex_clear (&snd->lock);
   g_free (snd);
}


void
masl_sound_start (MaslSound *snd)
{
   if (!snd) return;

   gst_element_set_state (snd->pipeline, GST_STATE_NULL);

   g_mutex_lock (&snd->lock);
   g_queue_clear_full (&snd->events, g_free);
   memset (snd->ch, 0, sizeof (snd->ch));
   snd->done = 0;
   snd->known = 0;
   g_mutex_unlock (&snd->lock);

   gst_element_set_state (snd->pipeline, GST_STATE_PLAYING);
}


void
masl_sound_set_time (MaslSound *snd, gdouble time_ms)
{
   if (!snd) return;
   g_mutex_lock (&snd->lock);
   snd->known = MAX (snd->known, time_ms / 1000.0);
   g_mutex_unlock (&snd->lock);
}


void
masl_sound_event (MaslSoundWhat what, gint ch, gint pcm, guint rate,
                  gboolean loop, gdouble time_ms, gpointer data)
{
   MaslSound *snd = data;
   SoundEvent *ev;

   if (!snd) return;
   ev = g_new0 (SoundEvent, 1);
   ev->what = what;
   ev->ch   = ch;
   ev->pcm  = pcm;
   ev->rate = rate;
   ev->loop = loop;
   ev->time = time_ms / 1000.0;
   g_mutex_lock (&snd->lock);
   g_queue_push_tail (&snd->events, ev);
   g_mutex_unlock (&snd->lock);
}

#else /* ENABLE_GSTREAMER */

MaslSound *masl_sound_new (const MaslPcm *pcms, guint n_pcms) { return NULL; }
void masl_sound_free (MaslSound *sound) {}
void masl_sound_start (MaslSound *sound) {}
void masl_sound_set_time (MaslSound *sound, gdouble time_ms) {}
void masl_sound_event (MaslSoundWhat what, gint ch, gint pcm, guint rate,
                       gboolean loop, gdouble time_ms, gpointer sound) {}

#endif /* ENABLE_GSTREAMER */
