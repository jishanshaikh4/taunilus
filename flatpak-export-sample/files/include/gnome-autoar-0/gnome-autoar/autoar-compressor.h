/* vim: set sw=2 ts=2 sts=2 et: */
/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * autoar-create.h
 * Automatically create archives in some GNOME programs
 *
 * Copyright (C) 2013  Ting-Wei Lan
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

#ifndef AUTOAR_COMPRESSOR_H
#define AUTOAR_COMPRESSOR_H

#include <glib-object.h>
#include <gio/gio.h>

#include "autoar-format-filter.h"

G_BEGIN_DECLS

#define AUTOAR_TYPE_COMPRESSOR autoar_compressor_get_type ()

G_DECLARE_FINAL_TYPE (AutoarCompressor, autoar_compressor, AUTOAR, COMPRESSOR, GObject)

/**
 * AUTOAR_COMPRESSOR_ERROR:
 *
 * Error domain for #AutoarCompressor. Not all error occurs in #AutoarCompressor uses
 * this domain. It is only used for error occurs in #AutoarCompressor itself.
 * See #AutoarCompressor::error signal for more information.
 **/
#define AUTOAR_COMPRESSOR_ERROR autoar_compressor_quark()

GQuark             autoar_compressor_quark                          (void);

AutoarCompressor * autoar_compressor_new                            (GList        *source_files,
                                                                     GFile        *output_file,
                                                                     AutoarFormat  format,
                                                                     AutoarFilter  filter,
                                                                     gboolean      create_top_level_directory);

void               autoar_compressor_start                          (AutoarCompressor *self,
                                                                     GCancellable     *cancellable);
void               autoar_compressor_start_async                    (AutoarCompressor *self,
                                                                     GCancellable     *cancellable);

GList *            autoar_compressor_get_source_files               (AutoarCompressor *self);
GFile *            autoar_compressor_get_output_file                (AutoarCompressor *self);
AutoarFormat       autoar_compressor_get_format                     (AutoarCompressor *self);
AutoarFilter       autoar_compressor_get_filter                     (AutoarCompressor *self);
gboolean           autoar_compressor_get_create_top_level_directory (AutoarCompressor *self);
guint64            autoar_compressor_get_size                       (AutoarCompressor *self);
guint64            autoar_compressor_get_completed_size             (AutoarCompressor *self);
guint              autoar_compressor_get_files                      (AutoarCompressor *self);
guint              autoar_compressor_get_completed_files            (AutoarCompressor *self);
gboolean           autoar_compressor_get_output_is_dest             (AutoarCompressor *self);
gint64             autoar_compressor_get_notify_interval            (AutoarCompressor *self);

void               autoar_compressor_set_output_is_dest             (AutoarCompressor *self,
                                                                     gboolean          output_is_dest);
void               autoar_compressor_set_notify_interval            (AutoarCompressor *self,
                                                                     gint64            notify_interval);
void               autoar_compressor_set_passphrase                 (AutoarCompressor *self,
                                                                     const gchar      *passphrase);
G_END_DECLS

#endif /* AUTOAR_COMPRESSOR_H */
