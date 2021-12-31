/*
 *
 * Copyright (C) 2021, Nishit Patel <nishitlimbani130@gmail.com>
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
 */


#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "tracker-cli-utils.h"


static gint
sort_by_date (gconstpointer a,
              gconstpointer b)
{
	GFileInfo *info_a = (GFileInfo *) a, *info_b = (GFileInfo *) b;
	gint64 time_a, time_b;

	time_a = g_file_info_get_attribute_uint64 (info_a, G_FILE_ATTRIBUTE_TIME_CREATED);
	time_b = g_file_info_get_attribute_uint64 (info_b, G_FILE_ATTRIBUTE_TIME_CREATED);

	if (time_a < time_b)
		return -1;
	else if (time_a > time_b)
		return 1;
	return 0;
}


GList *
tracker_cli_get_error_keyfiles (void)
{
	GFile *file;
	GFileEnumerator *enumerator;
	GList *infos = NULL, *keyfiles = NULL, *l;
	gchar *path;

	path = g_build_filename (g_get_user_cache_dir (),
	                         "tracker3",
	                         "files",
	                         "errors",
	                         NULL);
	file = g_file_new_for_path (path);
	g_free (path);

	enumerator = g_file_enumerate_children (file,
	                                        G_FILE_ATTRIBUTE_STANDARD_NAME ","
	                                        G_FILE_ATTRIBUTE_TIME_CHANGED,
	                                        G_FILE_QUERY_INFO_NONE,
	                                        NULL,
	                                        NULL);
	while (TRUE) {
		GFileInfo *info;

		if (!g_file_enumerator_iterate (enumerator, &info, NULL, NULL, NULL))
			break;
		if (!info)
			break;

		infos = g_list_prepend (infos, g_object_ref (info));
	}

	infos = g_list_sort (infos, sort_by_date);

	for (l = infos; l; l = l->next) {
		GKeyFile *keyfile;
		GFile *child;
		GError *error = NULL;

		child = g_file_get_child (file, g_file_info_get_name (l->data));
		path = g_file_get_path (child);
		keyfile = g_key_file_new ();

		if (g_key_file_load_from_file (keyfile, path, 0, &error)) {
			keyfiles = g_list_prepend (keyfiles, keyfile);

		} else {
			g_warning ("Error retrieving keyfiles: %s", error->message);
			g_error_free (error);
			g_key_file_free (keyfile);
		}

		g_object_unref (child);
	}

	g_object_unref (enumerator);
	g_list_free_full (infos, g_object_unref);

	return keyfiles;
}