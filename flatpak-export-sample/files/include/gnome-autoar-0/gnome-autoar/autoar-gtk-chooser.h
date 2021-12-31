/* vim: set sw=2 ts=2 sts=2 et: */
/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * autoar-gtk-chooser.h
 * GTK+ widgets to choose archive format and filter
 *
 * Copyright (C) 2013, 2014  Ting-Wei Lan
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */

#ifndef AUTOAR_GTK_CHOOSER_H
#define AUTOAR_GTK_CHOOSER_H

#include <gtk/gtk.h>

#include "autoar-format-filter.h"

G_BEGIN_DECLS

GtkWidget *autoar_gtk_chooser_simple_new          (AutoarFormat default_format,
                                                   AutoarFilter default_filter);
gboolean   autoar_gtk_chooser_simple_get          (GtkWidget *simple,
                                                   int *format,
                                                   int *filter);

GtkWidget *autoar_gtk_chooser_advanced_new        (AutoarFormat default_format,
                                                   AutoarFilter default_filter);
gboolean   autoar_gtk_chooser_advanced_get        (GtkWidget *advanced,
                                                   int *format,
                                                   int *filter);

G_END_DECLS

#endif /* AUTOAR_GTK_H */
