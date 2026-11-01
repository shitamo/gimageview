/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView preferences: image index (dates and tags).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __PREFS_UI_INDEX_H__
#define __PREFS_UI_INDEX_H__

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_prefs_win.h"

GtkWidget *prefs_ui_index         (void);
GtkWidget *prefs_ui_index_newimg  (void);
GtkWidget *prefs_ui_index_auto    (void);
GtkWidget *prefs_ui_index_faces   (void);
GtkWidget *prefs_ui_index_missing (void);
gboolean   prefs_ui_index_apply (GimvPrefsWinAction action);

#endif /* __PREFS_UI_INDEX_H__ */
