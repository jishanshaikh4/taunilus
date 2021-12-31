/* vim: set sw=2 ts=2 sts=2 et: */
/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * autoar-misc.c
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

#include "config.h"
#include "autoar-misc.h"

#include <glib.h>

/**
 * SECTION:autoar-misc
 * @Short_description: Miscellaneous functions and shared data types used
 *  by gnome-autoar
 * @Title: autoar-misc
 * @Include: gnome-autoar/autoar.h
 *
 * Public utility functions and data types used by gnome-autoar;
 **/

/**
 * autoar_libarchive_quark:
 *
 * Gets the libarchive Error Quark.
 *
 * Returns: a #GQuark.
 **/
G_DEFINE_QUARK (libarchive-quark, autoar_libarchive)
