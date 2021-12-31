/*
 * Copyright (C) 2020, Red Hat Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 * Author: Carlos Garnacho <carlosg@gnome.org>
 */

#include "tracker-error-report.h"

#include <glib/gstdio.h>
#include <errno.h>

#define GROUP "Report"
#define KEY_URI "Uri"
#define KEY_MESSAGE "Message"
#define KEY_SPARQL "Sparql"

static gchar *report_dir = NULL;

void
tracker_error_report_init (GFile *cache_dir)
{
	GFile *report_file;

	report_file = g_file_get_child (cache_dir, "errors");
	report_dir = g_file_get_path (report_file);
	if (g_mkdir_with_parents (report_dir, 0700) < 0)
		g_warning ("Failed to create location for error reports: %m");
	g_object_unref (report_file);
}

static gchar *
get_report_file (const gchar *uri)
{
	gchar *md5, *report_file;

	md5 = g_compute_checksum_for_string (G_CHECKSUM_MD5, uri, -1);
	report_file = g_build_filename (report_dir, md5, NULL);
	g_free (md5);

	return report_file;
}

void
tracker_error_report (GFile       *file,
                      const gchar *error_message,
                      const gchar *sparql)
{
	GKeyFile *key_file;
	gchar *report_path, *uri;
	GError *error = NULL;

	if (!report_dir)
		return;

	uri = g_file_get_uri (file);
	report_path = get_report_file (uri);
	key_file = g_key_file_new ();
	g_key_file_set_string (key_file, GROUP, KEY_URI, uri);

	if (error_message)
		g_key_file_set_string (key_file, GROUP, KEY_MESSAGE, error_message);

	if (sparql)
		g_key_file_set_string (key_file, GROUP, KEY_SPARQL, sparql);

	if (!g_key_file_save_to_file (key_file, report_path, &error)) {
		g_warning ("Could not save error report: %s\n", error->message);
		g_error_free (error);
	}

	g_key_file_unref (key_file);
	g_free (report_path);
	g_free (uri);
}

void
tracker_error_report_delete (GFile *file)
{
	gchar *uri, *report_path;

	if (!report_dir)
		return;

	uri = g_file_get_uri (file);
	report_path = get_report_file (uri);
	if (g_remove (report_path) < 0) {
		if (errno != ENOENT) {
			g_warning ("Error removing path '%s': %m",
			           report_path);
		}
	}

	g_free (report_path);
	g_free (uri);
}
