/* vim: set sw=2 ts=2 sts=2 et: */
/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * autoar-misc.h
 * Miscellaneous functions and shared data types used by gnome-autoar
 *
 * Copyright (C) 2014  Ting-Wei Lan
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

#ifndef AUTOAR_MISC_H
#define AUTOAR_MISC_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * AUTOAR_LIBARCHIVE_ERROR:
 *
 * Error domain for libarchive. Error returned by functions in libarchive uses
 * this domain. Error code and messages are got using archive_errno() and
 * archive_error_string() functions provided by libarchive.
 **/

#define AUTOAR_LIBARCHIVE_ERROR autoar_libarchive_quark()

GQuark    autoar_libarchive_quark                      (void);

G_END_DECLS

#endif /* AUTOAR_COMMON_H */
