/* vim: set sw=2 ts=2 sts=2 et: */
/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * autoar-private.c
 * Some common functions used in several classes of gnome-autoar
 * This file does NOT declare any new classes and it should NOT
 * be used outside the library itself!
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

#include "config.h"
#include "autoar-private.h"

#include "autoar-misc.h"

#include <glib.h>
#include <gobject/gvaluecollector.h>
#include <string.h>

/**
 * SECTION:autoar-common
 * @Short_description: Miscellaneous functions used by gnome-autoar
 * @Title: autoar-common
 * @Include: gnome-autoar/autoar.h
 *
 * Public utility functions used internally by other gnome-autoar functions.
 **/

typedef struct _AutoarCommonSignalData AutoarCommonSignalData;

struct _AutoarCommonSignalData
{
  GValue instance_and_params[3]; /* Maximum number of parameters + 1 */
  gssize used_values; /* Number of GValues to be unset */
  guint signal_id;
  GQuark detail;
};

/**
 * autoar_common_get_filename_extension:
 * @filename: a filename
 *
 * Gets the extension of a filename.
 *
 * Returns: (transfer none): a pointer to the extension of the filename
 **/
G_GNUC_INTERNAL char*
autoar_common_get_filename_extension (const char *filename)
{
  char *dot_location;

  dot_location = strrchr (filename, '.');
  if (dot_location == NULL || dot_location == filename) {
    return (char*)filename;
  }

  if (dot_location - 4 > filename && strncmp (dot_location - 4, ".tar", 4) == 0)
    dot_location -= 4;
  else if (dot_location - 5 > filename && strncmp (dot_location - 5, ".cpio", 5) == 0)
    dot_location -= 5;

  return dot_location;
}

/**
 * autoar_common_get_basename_remove_extension:
 * @filename: a filename
 *
 * Gets the basename of a path without its file name extension.
 *
 * Returns: (transfer full): a new filename without extension. Free the
 * returned string with g_free().
 **/
G_GNUC_INTERNAL char*
autoar_common_get_basename_remove_extension (const char *filename)
{
  char *dot_location;
  char *basename;

  if (filename == NULL) {
    return NULL;
  }

  /* filename must not be directory, so we do not get a bad basename. */
  basename = g_path_get_basename (filename);

  dot_location = autoar_common_get_filename_extension (basename);
  if (dot_location != basename)
    *dot_location = '\0';

  g_debug ("autoar_common_get_basename_remove_extension: %s => %s",
           filename,
           basename);
  return basename;
}

static void
autoar_common_signal_data_free (AutoarCommonSignalData *signal_data)
{
  int i;

  for (i = 0; i < signal_data->used_values; i++)
    g_value_unset (signal_data->instance_and_params + i);

  g_free (signal_data);
}

static gboolean
autoar_common_g_signal_emit_main_context (void *data)
{
  AutoarCommonSignalData *signal_data = data;
  g_signal_emitv (signal_data->instance_and_params,
                  signal_data->signal_id,
                  signal_data->detail,
                  NULL);
  autoar_common_signal_data_free (signal_data);
  return FALSE;
}

/**
 * autoar_common_g_signal_emit:
 * @instance: the instance the signal is being emitted on.
 * @in_thread: %TRUE if you are not call this function inside the main thread.
 * @signal_id: the signal id
 * @detail: the detail
 * @...: parameters to be passed to the signal.
 *
 * This is a wrapper for g_signal_emit(). If @in_thread is %FALSE, this
 * function is the same as g_signal_emit(). If @in_thread is %TRUE, the
 * signal will be emitted from the main thread. This function will send
 * the signal emission job via g_main_context_invoke(), but it does not
 * wait for the signal emission job to be completed. Hence, the signal
 * may emitted after autoar_common_g_signal_emit() is returned.
 **/
G_GNUC_INTERNAL void
autoar_common_g_signal_emit (gpointer instance,
                             gboolean in_thread,
                             guint signal_id,
                             GQuark detail,
                             ...)
{
  va_list ap;

  va_start (ap, detail);
  if (in_thread) {
    int i;
    gchar *error;
    GSignalQuery query;
    AutoarCommonSignalData *data;

    error = NULL;
    data = g_new0 (AutoarCommonSignalData, 1);
    data->signal_id = signal_id;
    data->detail = detail;
    data->used_values = 1;
    g_value_init (data->instance_and_params, G_TYPE_FROM_INSTANCE (instance));
    g_value_set_instance (data->instance_and_params, instance);

    g_signal_query (signal_id, &query);
    if (query.signal_id == 0) {
      autoar_common_signal_data_free (data);
      va_end (ap);
      return;
    }

    for (i = 0; i < query.n_params; i++) {
      G_VALUE_COLLECT_INIT (data->instance_and_params + i + 1,
                            query.param_types[i],
                            ap,
                            0,
                            &error);
      if (error != NULL)
        break;
      data->used_values++;
    }

    if (error == NULL) {
      g_main_context_invoke (NULL, autoar_common_g_signal_emit_main_context, data);
    } else {
      autoar_common_signal_data_free (data);
      g_debug ("G_VALUE_COLLECT_INIT: Error: %s", error);
      g_free (error);
      va_end (ap);
      return;
    }
  } else {
    g_signal_emit_valist (instance, signal_id, detail, ap);
  }
  va_end (ap);
}

/**
 * autoar_common_g_object_unref:
 * @object: a #GObject
 *
 * This is a wrapper for g_object_unref(). If @object is %NULL, this function
 * does nothing. Otherwise, it will call g_object_unref() on the @object.
 **/
G_GNUC_INTERNAL void
autoar_common_g_object_unref (gpointer object)
{
  if (object != NULL)
    g_object_unref (object);
}

/**
 * autoar_common_g_error_new_a:
 * @a: a archive object
 * @pathname: the file which causes error, or %NULL
 *
 * Creates a new #GError with error messages got from libarchive.
 *
 * Returns: (transfer full): a #GError. Free with g_error_free().
 **/
G_GNUC_INTERNAL GError*
autoar_common_g_error_new_a (struct archive *a,
                             const char *pathname)
{
  GError *newerror;
  newerror = g_error_new (AUTOAR_LIBARCHIVE_ERROR,
                          archive_errno (a),
                          "%s%s%s%s",
                          pathname != NULL ? "\'" : "",
                          pathname != NULL ? pathname : "",
                          pathname != NULL ? "\': " : "",
                          archive_error_string (a));
  return newerror;
}

/**
 * autoar_common_g_error_new_a_entry:
 * @a: a archive object
 * @entry: a archive_entry object
 *
 * Gets pathname from @entry and call autoar_common_g_error_new_a().
 *
 * Returns: (transfer full): a #GError. Free with g_error_free().
 **/
G_GNUC_INTERNAL GError*
autoar_common_g_error_new_a_entry (struct archive *a,
                                   struct archive_entry *entry)
{
  return autoar_common_g_error_new_a (a, archive_entry_pathname (entry));
}

/**
 * autoar_common_g_file_get_name:
 * @file: a #GFile
 *
 * Gets a string represents the @file. It will be the path of @file if
 * available. Otherwise, it will be the URI of @file.
 *
 * Returns: (transfer full): a string represents the file. Free the string
 * with g_free().
 **/
G_GNUC_INTERNAL char*
autoar_common_g_file_get_name (GFile *file)
{
  char *name;
  name = g_file_get_path (file);
  if (name == NULL)
    name = g_file_get_uri (file);
  return name;
}

/**
 * autoar_common_get_utf8_pathname:
 * @pathname: a pathname with an unspecified encoding
 *
 * Transforms pathname into a UTF-8 filename from a variety of common
 * legacy encodings.
 *
 * Returns: (transfer full): a UTF-8 filename, or %NULL if the filename
 * could not be converted or is already in UTF-8. Free the string with
 * g_free().
 **/
G_GNUC_INTERNAL char*
autoar_common_get_utf8_pathname (const char *pathname)
{
  char *utf8_pathname;
  static const char *try_charsets[] = { "CSPC8CODEPAGE437", "ISO-8859-1", "WINDOWS-1252" };
  guint i;

  if (g_utf8_validate (pathname, -1, NULL))
    return NULL;
  /* If pathname is not in UTF-8 encoding already, try
   * to convert it using commonly used encoding in various archive types.
   * See also https://git.gnome.org//browse/file-roller/tree/src/fr-process.c#n245 */
  for (i = 0; i < G_N_ELEMENTS (try_charsets); i++) {
    utf8_pathname = g_convert (pathname, -1, "UTF-8",
                               try_charsets[i], NULL, NULL, NULL);
    if (utf8_pathname != NULL)
      break;
  }

  return utf8_pathname;
}
