/*
 * Copyright (C) 2014, Lanedo <martyn@lanedo.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "config-miners.h"

#include <stdlib.h>
#include <errno.h>

#ifdef __sun
#include <procfs.h>
#endif

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <locale.h>

#include <libtracker-sparql/tracker-sparql.h>
#include <libtracker-miners-common/tracker-common.h>

#include "tracker-color.h"
#include "tracker-dbus.h"

static gboolean opt_add;
static gboolean opt_remove;
static gboolean opt_recursive;
static gchar **filenames;

#define INDEX_OPTIONS_ENABLED()	  \
	(opt_add || opt_remove || opt_recursive)

static GOptionEntry entries[] = {
	{ "add", 'a', 0, G_OPTION_ARG_NONE, &opt_add,
	  N_("Adds FILE as an indexed location"),
	  NULL },
	{ "remove", 'd', 0, G_OPTION_ARG_NONE, &opt_remove,
	  N_("Removes FILE from indexed locations"),
	  NULL },
	{ "recursive", 'r', 0, G_OPTION_ARG_NONE, &opt_recursive,
	  N_("Makes indexing recursive"),
	  NULL },
	{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &filenames,
	  N_("FILE"),
	  N_("FILE") },
	{ NULL }
};

const struct {
	const gchar *symbol;
	GUserDirectory user_dir;
} special_dirs[] = {
	{ "&DESKTOP",      G_USER_DIRECTORY_DESKTOP },
	{ "&DOCUMENTS",    G_USER_DIRECTORY_DOCUMENTS },
	{ "&DOWNLOAD",     G_USER_DIRECTORY_DOWNLOAD },
	{ "&MUSIC",        G_USER_DIRECTORY_MUSIC },
	{ "&PICTURES",     G_USER_DIRECTORY_PICTURES },
	{ "&PUBLIC_SHARE", G_USER_DIRECTORY_PUBLIC_SHARE },
	{ "&TEMPLATES",    G_USER_DIRECTORY_TEMPLATES },
	{ "&VIDEOS",       G_USER_DIRECTORY_VIDEOS }
};

static const gchar *
alias_to_path (const gchar *alias)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS (special_dirs); i++) {
		if (g_strcmp0 (special_dirs[i].symbol, alias) == 0)
			return g_get_user_special_dir (special_dirs[i].user_dir);
	}

	return NULL;
}

static const gchar *
path_to_alias (const gchar *path)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS (special_dirs); i++) {
		if (g_strcmp0 (path,
		               g_get_user_special_dir (special_dirs[i].user_dir)) == 0)
			return special_dirs[i].symbol;
	}

	return NULL;
}

static const gchar *
envvar_to_path (const gchar *envvar)
{
	const gchar *path;

	path = g_getenv (&envvar[1]);
	if (g_file_test (path, G_FILE_TEST_EXISTS))
		return path;

	return NULL;
}

static GStrv
strv_add (GStrv        strv,
          const gchar *elem)
{
	GArray *array;
	gchar *str;

	array = g_array_new (TRUE, TRUE, sizeof (char *));
	g_array_append_vals (array, strv, g_strv_length (strv));
	g_free (strv);

	str = g_strdup (elem);
	g_array_append_val (array, str);

	return (GStrv) g_array_free (array, FALSE);
}

static GStrv
strv_remove (GStrv        strv,
             const gchar *elem)
{
	GArray *array;
	guint i;

	array = g_array_new (TRUE, TRUE, sizeof (char *));
	g_array_append_vals (array, strv, g_strv_length (strv));

	for (i = 0; i < array->len; i++) {
		gchar *str = g_array_index (array, gchar *, i);

		if (g_strcmp0 (str, elem) != 0)
			continue;

		g_array_remove_index (array, i);
		g_free (str);
	}

	g_free (strv);

	return (GStrv) g_array_free (array, FALSE);
}

static int
index_add (void)
{
	gboolean handled = FALSE;
	GSettings *settings;
	guint i;

	settings = g_settings_new ("org.freedesktop.Tracker3.Miner.Files");

	for (i = 0; filenames[i]; i++) {
		GFile *file;
		gchar *path;
		const gchar *alias;
		GStrv dirs, rec_dirs;

		dirs = g_settings_get_strv (settings, "index-single-directories");
		rec_dirs = g_settings_get_strv (settings, "index-recursive-directories");

		file = g_file_new_for_commandline_arg (filenames[i]);
		path = g_file_get_path (file);
		alias = path_to_alias (path);

		if (g_strv_contains ((const gchar * const *) dirs, path) ||
		    (alias && g_strv_contains ((const gchar * const *) dirs, alias)) ||
		    g_strv_contains ((const gchar * const *) rec_dirs, path) ||
		    (alias && g_strv_contains ((const gchar * const *) rec_dirs, alias))) {
			g_strfreev (dirs);
			g_strfreev (rec_dirs);
			handled = TRUE;
			continue;
		}

		if (!g_file_test (path, G_FILE_TEST_IS_DIR)) {
			g_printerr (_("“%s” is not a directory"),
			            path);
			g_printerr ("\n");
			g_strfreev (dirs);
			g_strfreev (rec_dirs);
			continue;
		}

		handled = TRUE;

		if (opt_recursive) {
			rec_dirs = strv_add (rec_dirs, path);
			g_settings_set_strv (settings, "index-recursive-directories",
					     (const gchar * const *) rec_dirs);
		} else {
			dirs = strv_add (dirs, path);
			g_settings_set_strv (settings, "index-single-directories",
					     (const gchar * const *) dirs);
		}

		g_object_unref (file);
		g_strfreev (dirs);
		g_strfreev (rec_dirs);
		g_free (path);
	}

	g_settings_sync ();
	g_object_unref (settings);

	return handled ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void
index_remove_setting (GSettings   *settings,
                      const gchar *setting_path,
                      const gchar *path)
{
	GStrv dirs;
	const gchar *alias;

	dirs = g_settings_get_strv (settings, setting_path);
	alias = path_to_alias (path);

	if (g_strv_contains ((const gchar * const *) dirs, path))
		dirs = strv_remove (dirs, path);
	if (alias && g_strv_contains ((const gchar * const *) dirs, alias))
		dirs = strv_remove (dirs, path_to_alias (path));

	g_settings_set_strv (settings, setting_path,
	                     (const gchar * const *) dirs);
	g_strfreev (dirs);
}

static int
index_remove (void)
{
	GSettings *settings;
	guint i;

	settings = g_settings_new ("org.freedesktop.Tracker3.Miner.Files");

	for (i = 0; filenames[i]; i++) {
		GFile *file;
		gchar *path;

		file = g_file_new_for_commandline_arg (filenames[i]);
		path = g_file_get_path (file);

		index_remove_setting (settings,
		                      "index-recursive-directories",
		                      path);
		index_remove_setting (settings,
		                      "index-single-directories",
		                      path);

		g_object_unref (file);
		g_free (path);
	}

	g_settings_sync ();
	g_object_unref (settings);

	return EXIT_SUCCESS;
}

static int
index_run (void)
{
	if (!opt_add && !opt_remove) {
		/* TRANSLATORS: These are commandline options */
		g_printerr ("%s\n", _("Either --add or --remove must be provided"));
		return EXIT_FAILURE;
	} else if (opt_add && opt_remove) {
		/* TRANSLATORS: These are commandline options */
		g_printerr ("%s\n", _("--add and --remove are mutually exclusive"));
		return EXIT_FAILURE;
	}

	if (opt_add) {
		return index_add ();
	}

	if (opt_recursive) {
		/* TRANSLATORS: These are commandline options */
		g_printerr ("%s\n", _("--recursive requires --add"));
		return EXIT_FAILURE;
	}

	if (opt_remove) {
		return index_remove ();
	}

	return EXIT_FAILURE;
}

static void
print_list (GStrv    list,
            gint     len,
            gboolean recursive)
{
	guint i;

	for (i = 0; list[i]; i++) {
		const gchar *path;
		gchar *str;

		if (list[i][0] == '&')
			path = alias_to_path (list[i]);
		else if (list[i][0] == '$')
			path = envvar_to_path (list[i]);
		else if (list[i][0] == '/')
			path = list[i];
		else
			continue;

		if (path) {
			str = tracker_term_ellipsize (path, len, TRACKER_ELLIPSIZE_START);
			g_print ("%-*s " BOLD_BEGIN "%s" BOLD_END "\n",
		    	     len, str,
		    	     recursive ? "*" : "-");
			g_free (str);
		}
		else {
			g_warning ("Could not expand XDG user directory %s", list[i]);
		}
	}
}

static int
list_index_roots (void)
{
	GSettings *settings;
	GStrv recursive, non_recursive;
	gint cols, col_len[2];
	gchar *col_header1, *col_header2;

	settings = g_settings_new ("org.freedesktop.Tracker3.Miner.Files");
	recursive = g_settings_get_strv (settings, "index-recursive-directories");
	non_recursive = g_settings_get_strv (settings, "index-single-directories");

	tracker_term_dimensions (&cols, NULL);
	col_len[0] = cols * 3 / 4;
	col_len[1] = cols / 4 - 1;

	col_header1 = tracker_term_ellipsize (_("Path"), col_len[0], TRACKER_ELLIPSIZE_END);
	col_header2 = tracker_term_ellipsize (_("Recursive"), col_len[1], TRACKER_ELLIPSIZE_END);

	g_print (BOLD_BEGIN "%-*s %-*s" BOLD_END "\n",
	         col_len[0], col_header1,
	         col_len[1], col_header2);
	g_free (col_header1);
	g_free (col_header2);

	print_list (recursive, col_len[0], TRUE);
	print_list (non_recursive, col_len[0], FALSE);

	g_strfreev (recursive);
	g_strfreev (non_recursive);
	g_object_unref (settings);

	return EXIT_SUCCESS;
}

int
main (int argc, const char **argv)
{
	GOptionContext *context;
	GError *error = NULL;
	const gchar *failed;

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, entries, NULL);

	argv[0] = "tracker index";

	if (!g_option_context_parse (context, &argc, (char***) &argv, &error)) {
		g_printerr ("%s, %s\n", _("Unrecognized options"), error->message);
		g_error_free (error);
		g_option_context_free (context);
		return EXIT_FAILURE;
	}

	g_option_context_free (context);

	if (!filenames && !INDEX_OPTIONS_ENABLED ()) {
		return list_index_roots ();
	}

	if (!filenames || g_strv_length (filenames) < 1) {
		failed = _("Please specify one or more locations to index.");
		g_printerr ("%s\n", failed);
		return EXIT_FAILURE;
	}

	return index_run ();
}
