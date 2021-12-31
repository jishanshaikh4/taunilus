/* vim: set sw=2 ts=2 sts=2 et: */
/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * autoar-extractor.h
 * Automatically extract archives in some GNOME programs
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

#ifndef AUTOAR_EXTRACTOR_H
#define AUTOAR_EXTRACTOR_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define AUTOAR_TYPE_EXTRACTOR autoar_extractor_get_type ()

G_DECLARE_FINAL_TYPE (AutoarExtractor, autoar_extractor, AUTOAR, EXTRACTOR, GObject)

/**
 * AUTOAR_EXTRACT_ERROR:
 *
 * Error domain for #AutoarExtractor. Not all error occurs in #AutoarExtractor uses
 * this domain. It is only used for error occurs in #AutoarExtractor itself.
 * See #AutoarExtractor::error signal for more information.
 **/
#define AUTOAR_EXTRACTOR_ERROR autoar_extractor_quark()

GQuark           autoar_extractor_quark                       (void);

AutoarExtractor *autoar_extractor_new                         (GFile *source_file,
                                                               GFile *output_file);

void             autoar_extractor_start                       (AutoarExtractor *self,
                                                               GCancellable    *cancellable);
void             autoar_extractor_start_async                 (AutoarExtractor *self,
                                                               GCancellable    *cancellable);

GFile           *autoar_extractor_get_source_file             (AutoarExtractor *self);
GFile           *autoar_extractor_get_output_file             (AutoarExtractor *self);
guint64          autoar_extractor_get_total_size              (AutoarExtractor *self);
guint64          autoar_extractor_get_completed_size          (AutoarExtractor *self);
guint            autoar_extractor_get_total_files             (AutoarExtractor *self);
guint            autoar_extractor_get_completed_files         (AutoarExtractor *self);
gboolean         autoar_extractor_get_output_is_dest          (AutoarExtractor *self);
gboolean         autoar_extractor_get_delete_after_extraction (AutoarExtractor *self);
gint64           autoar_extractor_get_notify_interval         (AutoarExtractor *self);

void             autoar_extractor_set_output_is_dest          (AutoarExtractor *self,
                                                               gboolean         output_is_dest);
void             autoar_extractor_set_delete_after_extraction (AutoarExtractor *self,
                                                               gboolean         delete_after_extraction);
void             autoar_extractor_set_notify_interval         (AutoarExtractor *self,
                                                               gint64           notify_interval);

typedef enum {
    AUTOAR_CONFLICT_UNHANDLED = 0,
    AUTOAR_CONFLICT_SKIP,
    AUTOAR_CONFLICT_OVERWRITE,
    AUTOAR_CONFLICT_CHANGE_DESTINATION
} AutoarConflictAction;

G_END_DECLS

#endif /* AUTOAR_EXTRACTOR_H */
