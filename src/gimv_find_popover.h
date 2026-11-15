/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: Ctrl+F on a view of items (not a GimvListView: that has its
 * own): a search entry in a popover at its top right.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_FIND_POPOVER_H__
#define __GIMV_FIND_POPOVER_H__

#include <gtk/gtk.h>

#define GIMV_FIND_NONE G_MAXUINT

typedef struct {
   guint  (*count)    (gpointer data);
   /* the text of item pos (freed) */
   gchar *(*text)     (guint pos, gpointer data);
   /* the item chosen alone, shown */
   void   (*select)   (guint pos, gpointer data);
   /* Enter */
   void   (*activate) (guint pos, gpointer data);
   /* the item the keys are on, GIMV_FIND_NONE: none */
   guint  (*current)  (gpointer data);
} GimvFindFuncs;

/* Ctrl+F on view searches its items: the one starting with the text typed
 * (else having it) is chosen; Up and Down: the one before and the next,
 * Enter: activated, Escape (or 5 seconds without a key): closed and the
 * keys back on view.  The popover is put on anchor (NULL: the parent of
 * the view when it is first opened): a widget that may have one more
 * child. */
void gimv_find_popover_attach (GtkWidget           *view,
                               GtkWidget           *anchor,
                               const GimvFindFuncs *funcs,
                               gpointer             data);

#endif /* __GIMV_FIND_POPOVER_H__ */
