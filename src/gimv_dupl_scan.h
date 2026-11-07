/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: finding the duplicate images of a list, in one pass sorted
 * into kinds: the same file, the same picture in another size, similar.
 * No GTK: it runs in a thread and in the tests.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_DUPL_SCAN_H__
#define __GIMV_DUPL_SCAN_H__

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

typedef enum {
   GIMV_DUPL_SAME,      /* the same bytes */
   GIMV_DUPL_RESIZED,   /* the same picture, other sizes (and copies) */
   GIMV_DUPL_SIMILAR    /* alike: edited, saved again, ... */
} GimvDuplKind;

#define GIMV_DUPL_SIG_SIZE (32 * 32 * 3)

typedef struct {
   gchar    *path;
   goffset   size;
   gint64    mtime;
   gpointer  user;            /* the caller's (a GimvThumb) */

   /* filled by the scan */
   gint      width, height;   /* as shown (EXIF orientation); 0: unknown */
   gchar     md5[33];         /* "" when not needed (no other of its size) */
   guint8   *sig;             /* GIMV_DUPL_SIG_SIZE: 32 x 32 RGB means */
   gint64    sig_sum;
   gint      index;           /* in the scan */
} GimvDuplItem;

typedef struct {
   GimvDuplKind  kind;
   GPtrArray    *members;     /* GimvDuplItem * of the scan, the keep first */
   GArray       *sim;         /* gfloat: of each member to members[0] */
   gboolean      suggested;   /* members[0] is the one to keep, the others
                                 to delete (the same, resized) */
} GimvDuplGroup;

typedef enum {
   GIMV_DUPL_PHASE_SAME,      /* comparing the files of the same size */
   GIMV_DUPL_PHASE_READ,      /* reading the images (small) */
   GIMV_DUPL_PHASE_COMPARE,   /* comparing the pictures */
   GIMV_DUPL_PHASE_DONE
} GimvDuplPhase;

typedef struct GimvDuplScan_Tag GimvDuplScan;

/* called from the thread of the scan: now and then (not too often) */
typedef void (*GimvDuplProgressFunc) (GimvDuplPhase phase,
                                      guint         done,
                                      guint         total,
                                      gpointer      data);

GimvDuplScan *gimv_dupl_scan_new        (gfloat accuracy);
void          gimv_dupl_scan_free       (GimvDuplScan *scan);
/* path, size and mtime given; the item is the scan's */
GimvDuplItem *gimv_dupl_scan_add        (GimvDuplScan *scan,
                                         const gchar  *path,
                                         goffset       size,
                                         gint64        mtime,
                                         gpointer      user);
/* the work, in a thread: may be cut short by cancel (the groups then are
 * those found so far).  FALSE when cancelled. */
gboolean      gimv_dupl_scan_run        (GimvDuplScan         *scan,
                                         GCancellable         *cancel,
                                         GimvDuplProgressFunc  progress,
                                         gpointer              data);
/* the groups found (GimvDuplGroup *, owned by the scan), in their order:
 * the kind, then the path of the first; after gimv_dupl_scan_run (), or
 * after its first phase from the progress function */
GPtrArray    *gimv_dupl_scan_get_groups (GimvDuplScan *scan);
/* the groups of the same files only (made after the first phase): a new
 * array for the caller, of groups the caller frees */
GPtrArray    *gimv_dupl_scan_same_groups (GimvDuplScan *scan);
void          gimv_dupl_group_free      (GimvDuplGroup *group);

/* the signature of a picture: 32 x 32 RGB means (for the tests) */
void          gimv_dupl_sig_fill        (guint8        sig[GIMV_DUPL_SIG_SIZE],
                                         GdkPixbuf    *pixbuf);
gfloat        gimv_dupl_sig_compare     (const guint8 *a,
                                         const guint8 *b);

#endif /* __GIMV_DUPL_SCAN_H__ */
