/* vim: set sw=2 ts=2 sts=2 et: */
/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * autoar-format-filter.h
 * Functions related to archive formats and filters
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

#ifndef AUTOAR_FORMAT_H
#define AUTOAR_FORMAT_H

#include <archive.h>
#include <glib.h>

G_BEGIN_DECLS

/**
 * AutoarFormat:
 * @AUTOAR_FORMAT_ZIP: %ARCHIVE_FORMAT_ZIP: Zip archive
 * @AUTOAR_FORMAT_TAR: %ARCHIVE_FORMAT_TAR_PAX_RESTRICTED: Tar archive, use
 *   ustar format is possible. If there are extended headers which cannot be
 *   represented in the ustar format, libarchive will use pax interchage format
 *   instead.
 * @AUTOAR_FORMAT_CPIO: %ARCHIVE_FORMAT_CPIO_POSIX: CPIO archive, POSIX
 *   standard cpio interchage format.
 * @AUTOAR_FORMAT_7ZIP: %ARCHIVE_FORMAT_7ZIP: 7-zip archive
 * @AUTOAR_FORMAT_AR_BSD: %ARCHIVE_FORMAT_AR_BSD: BSD variant of Unix archive
 *   format. This format does not support storing directories.
 * @AUTOAR_FORMAT_AR_SVR4: %ARCHIVE_FORMAT_AR_GNU: GNU/SVR4 variant of Unix
 *   archive format. This format does not support storing directories.
 * @AUTOAR_FORMAT_CPIO_NEWC: %ARCHIVE_FORMAT_CPIO_SVR4_NOCRC: CPIO archive,
 *   SVR4 non-CRC variant
 * @AUTOAR_FORMAT_GNUTAR: %ARCHIVE_FORMAT_TAR_GNUTAR: Tar archive, support
 *   most popular GNU extensions.
 * @AUTOAR_FORMAT_ISO9660: %ARCHIVE_FORMAT_ISO9660: Raw CD image
 * @AUTOAR_FORMAT_PAX: %ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE: Tar archive, use
 *   pax interchage format
 * @AUTOAR_FORMAT_USTAR: %ARCHIVE_FORMAT_TAR_USTAR: Tar archive, use old
 *   ustar format
 * @AUTOAR_FORMAT_XAR: %ARCHIVE_FORMAT_XAR: Xar archive
 *
 * This is a non-negative number which represents formats supported by
 * libarchive. A libarchive format is a file format which can store many
 * files as a archive file.
 **/
typedef enum {
  /*< private >*/
  AUTOAR_FORMAT_0, /*< skip >*/
  /*< public >*/
  AUTOAR_FORMAT_ZIP = 1,   /* .zip */
  AUTOAR_FORMAT_TAR,       /* .tar, pax_restricted */
  AUTOAR_FORMAT_CPIO,      /* .cpio, odc */
  AUTOAR_FORMAT_7ZIP,      /* .7z */
  AUTOAR_FORMAT_AR_BSD,    /* .a */
  AUTOAR_FORMAT_AR_SVR4,   /* .a */
  AUTOAR_FORMAT_CPIO_NEWC, /* .cpio, newc */
  AUTOAR_FORMAT_GNUTAR,    /* .tar, gnutar */
  AUTOAR_FORMAT_ISO9660,   /* .iso */
  AUTOAR_FORMAT_PAX,       /* .tar, pax */
  AUTOAR_FORMAT_USTAR,     /* .tar, ustar */
  AUTOAR_FORMAT_XAR,       /* .xar, xar */
  /*< private >*/
  AUTOAR_FORMAT_LAST /*< skip >*/
} AutoarFormat;

/**
 * AutoarFilter:
 * @AUTOAR_FILTER_NONE: %ARCHIVE_FILTER_NONE: No filter
 * @AUTOAR_FILTER_COMPRESS: %ARCHIVE_FILTER_COMPRESS: UNIX-compressed
 * @AUTOAR_FILTER_GZIP: %ARCHIVE_FILTER_GZIP: Gzip
 * @AUTOAR_FILTER_BZIP2: %ARCHIVE_FILTER_BZIP2: Bzip2
 * @AUTOAR_FILTER_XZ: %ARCHIVE_FILTER_XZ: XZ
 * @AUTOAR_FILTER_LZMA: %ARCHIVE_FILTER_LZMA: LZMA
 * @AUTOAR_FILTER_LZIP: %ARCHIVE_FILTER_LZIP: Lzip
 * @AUTOAR_FILTER_LZOP: %ARCHIVE_FILTER_LZOP: LZO
 * @AUTOAR_FILTER_GRZIP: %ARCHIVE_FILTER_GRZIP: GRZip
 * @AUTOAR_FILTER_LRZIP: %ARCHIVE_FILTER_LRZIP: Long Range ZIP (lrzip)
 *
 * This is a non-negative number which represents filters supported by
 * libarchive. A libarchive filter is a filter which can convert a
 * regular file into a compressed file.
 **/
typedef enum {
  /*< private >*/
  AUTOAR_FILTER_0, /*< skip >*/
  /*< public >*/
  AUTOAR_FILTER_NONE = 1,
  AUTOAR_FILTER_COMPRESS,  /* .Z */
  AUTOAR_FILTER_GZIP,      /* .gz */
  AUTOAR_FILTER_BZIP2,     /* .bz2 */
  AUTOAR_FILTER_XZ,        /* .xz */
  AUTOAR_FILTER_LZMA,      /* .lzma */
  AUTOAR_FILTER_LZIP,      /* .lz */
  AUTOAR_FILTER_LZOP,      /* .lzo */
  AUTOAR_FILTER_GRZIP,     /* .grz */
  AUTOAR_FILTER_LRZIP,     /* .lrz */
  /*< private >*/
  AUTOAR_FILTER_LAST /*< skip >*/
} AutoarFilter;

typedef int (*AutoarFormatFunc) (struct archive *a);
typedef int (*AutoarFilterFunc) (struct archive *a);

int           autoar_format_last                        (void);
gboolean      autoar_format_is_valid                    (AutoarFormat format);
const char   *autoar_format_get_mime_type               (AutoarFormat format);
const char   *autoar_format_get_extension               (AutoarFormat format);
const char   *autoar_format_get_description             (AutoarFormat format);
gchar        *autoar_format_get_description_libarchive  (AutoarFormat format);
int           autoar_format_get_format_libarchive       (AutoarFormat format);
AutoarFormatFunc autoar_format_get_libarchive_read      (AutoarFormat format);
AutoarFormatFunc autoar_format_get_libarchive_write     (AutoarFormat format);

int           autoar_filter_last                        (void);
gboolean      autoar_filter_is_valid                    (AutoarFilter filter);
const char   *autoar_filter_get_mime_type               (AutoarFilter filter);
const char   *autoar_filter_get_extension               (AutoarFilter filter);
const char   *autoar_filter_get_description             (AutoarFilter filter);
gchar        *autoar_filter_get_description_libarchive  (AutoarFilter filter);
int           autoar_filter_get_filter_libarchive       (AutoarFilter filter);
AutoarFilterFunc autoar_filter_get_libarchive_read      (AutoarFilter filter);
AutoarFilterFunc autoar_filter_get_libarchive_write     (AutoarFilter filter);

gchar        *autoar_format_filter_get_mime_type        (AutoarFormat format,
                                                         AutoarFilter filter);
gchar        *autoar_format_filter_get_extension        (AutoarFormat format,
                                                         AutoarFilter filter);
gchar        *autoar_format_filter_get_description      (AutoarFormat format,
                                                         AutoarFilter filter);

G_END_DECLS

#endif /* AUTOAR_PREF_H */
