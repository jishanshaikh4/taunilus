/*
 * Copyright (C) 2006, Jamie McCracken <jamiemcc@gnome.org>
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
 * Copyright (C) 2014, Lanedo <martyn@lanedo.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <locale.h>

#include <glib.h>
#include <glib/gi18n.h>

#include <libtracker-miners-common/tracker-common.h>
#include <libtracker-sparql/tracker-sparql.h>

#include "tracker-term-utils.h"
#include "tracker-miner-manager.h"
#include "tracker-color.h"
#include "tracker-cli-utils.h"

#define GROUP "Report"
#define KEY_URI "Uri"
#define KEY_MESSAGE "Message"
#define KEY_SPARQL "Sparql"

#define STATUS_OPTIONS_ENABLED()	  \
	(show_stat)

static gboolean show_stat;
static gchar **terms;

static GOptionEntry entries[] = {
	{ "stat", 'a', 0, G_OPTION_ARG_NONE, &show_stat,
	  N_("Show statistics for current index / data set"),
	  NULL
	},
	{ G_OPTION_REMAINING, 0, 0,
	  G_OPTION_ARG_STRING_ARRAY, &terms,
	  N_("search terms"),
	  N_("EXPRESSION") },
	{ NULL }
};

static TrackerSparqlCursor *
statistics_query (TrackerSparqlConnection  *connection,
		  GError                  **error)
{
	return tracker_sparql_connection_query (connection,
						"SELECT ?class (COUNT(?elem) AS ?count) {"
						"  ?class a rdfs:Class . "
						"  ?elem a ?class . "
						"} "
						"GROUP BY ?class "
						"ORDER BY DESC count(?elem) ",
                                               NULL, error);
}

static int
status_stat (void)
{
	TrackerSparqlConnection *connection;
	TrackerSparqlCursor *cursor;
	GError *error = NULL;

	connection = tracker_sparql_connection_bus_new ("org.freedesktop.Tracker3.Miner.Files",
	                                                NULL, NULL, &error);

	if (!connection) {
		g_printerr ("%s: %s\n",
		            _("Could not establish a connection to Tracker"),
		            error ? error->message : _("No error given"));
		g_clear_error (&error);
		return EXIT_FAILURE;
	}

	tracker_term_pipe_to_pager ();

	cursor = statistics_query (connection, &error);

	g_object_unref (connection);

	if (error) {
		g_printerr ("%s, %s\n",
		            _("Could not get Tracker statistics"),
		            error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	if (!cursor) {
		g_print ("%s\n", _("No statistics available"));
	} else {
		GString *output;

		output = g_string_new ("");

		while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
			const gchar *rdf_type;
			const gchar *rdf_type_count;

			rdf_type = tracker_sparql_cursor_get_string (cursor, 0, NULL);
			rdf_type_count = tracker_sparql_cursor_get_string (cursor, 1, NULL);

			if (terms) {
				gint i, n_terms;
				gboolean show_rdf_type = FALSE;

				n_terms = g_strv_length (terms);

				for (i = 0;
				     i < n_terms && !show_rdf_type;
				     i++) {
					show_rdf_type = g_str_match_string (terms[i], rdf_type, TRUE);
				}

				if (!show_rdf_type) {
					continue;
				}
			}

			g_string_append_printf (output,
			                        "  %s = %s\n",
			                        rdf_type,
			                        rdf_type_count);
		}

		if (output->len > 0) {
			g_string_prepend (output, "\n");
			/* To translators: This is to say there are no
			 * statistics found. We use a "Statistics:
			 * None" with multiple print statements */
			g_string_prepend (output, _("Statistics:"));
		} else {
			g_string_append_printf (output,
			                        "  %s\n", _("None"));
		}

		g_print ("%s\n", output->str);
		g_string_free (output, TRUE);

		g_object_unref (cursor);
	}

	tracker_term_pager_close ();

	return EXIT_SUCCESS;
}

static int
status_run (void)
{
	if (show_stat) {
		return status_stat ();
	}

	/* All known options have their own exit points */
	g_warn_if_reached ();

	return EXIT_FAILURE;
}

static int
get_file_and_folder_count (int *files,
                           int *folders)
{
	TrackerSparqlConnection *connection;
	TrackerSparqlCursor *cursor;
	GError *error = NULL;

	connection = tracker_sparql_connection_bus_new ("org.freedesktop.Tracker3.Miner.Files",
	                                                NULL, NULL, &error);

	if (files) {
		*files = 0;
	}

	if (folders) {
		*folders = 0;
	}

	if (!connection) {
		g_printerr ("%s: %s\n",
		            _("Could not establish a connection to Tracker"),
		            error ? error->message : _("No error given"));
		g_clear_error (&error);
		return EXIT_FAILURE;
	}

	if (files) {
		const gchar query[] =
			"SELECT COUNT(?file) "
			"WHERE { "
			"  GRAPH tracker:FileSystem {"
			"    ?file a nfo:FileDataObject ;"
			"          nie:dataSource/tracker:available true ."
			"    FILTER (! EXISTS { ?file nie:interpretedAs/rdf:type nfo:Folder }) "
			"  }"
			"}";

		cursor = tracker_sparql_connection_query (connection, query, NULL, &error);

		if (error || !tracker_sparql_cursor_next (cursor, NULL, &error)) {
			g_printerr ("%s, %s\n",
			            _("Could not get basic status for Tracker"),
			            error->message);
			g_error_free (error);
			return EXIT_FAILURE;
		}

		*files = tracker_sparql_cursor_get_integer (cursor, 0);

		g_object_unref (cursor);
	}

	if (folders) {
		const gchar query[] =
			"SELECT COUNT(?folders)"
			"WHERE { "
			"  GRAPH tracker:FileSystem {"
			"    ?folders a nfo:Folder ;"
			"             nie:isStoredAs/nie:dataSource/tracker:available true ."
			"  }"
			"}";

		cursor = tracker_sparql_connection_query (connection, query, NULL, &error);

		if (error || !tracker_sparql_cursor_next (cursor, NULL, NULL)) {
			g_printerr ("%s, %s\n",
			            _("Could not get basic status for Tracker"),
			            error ? error->message : _("No error given"));
			g_error_free (error);
			return EXIT_FAILURE;
		}

		*folders = tracker_sparql_cursor_get_integer (cursor, 0);

		g_object_unref (cursor);
	}

	g_object_unref (connection);

	return EXIT_SUCCESS;
}

static gboolean
are_miners_finished (gint *max_remaining_time)
{
	TrackerMinerManager *manager;
	GError *error = NULL;
	GSList *miners_running;
	GSList *l;
	gboolean finished = TRUE;
	gint _max_remaining_time = 0;

	/* Don't auto-start the miners here */
	manager = tracker_miner_manager_new_full (FALSE, &error);
	if (!manager) {
		g_printerr (_("Could not get status, manager could not be created, %s"),
		            error ? error->message : _("No error given"));
		g_printerr ("\n");
		g_clear_error (&error);
		return EXIT_FAILURE;
	}

	miners_running = tracker_miner_manager_get_running (manager);

	for (l = miners_running; l; l = l->next) {
		gchar *status;
		gdouble progress;
		gint remaining_time;

		if (!tracker_miner_manager_get_status (manager,
		                                       l->data,
		                                       &status,
		                                       &progress,
		                                       &remaining_time)) {
			continue;
		}

		g_free (status);

		finished &= progress == 1.0;
		_max_remaining_time = MAX(remaining_time, _max_remaining_time);
	}

	g_slist_foreach (miners_running, (GFunc) g_free, NULL);
	g_slist_free (miners_running);

	if (max_remaining_time) {
		*max_remaining_time = _max_remaining_time;
	}

	g_object_unref (manager);

	return finished;
}

static gint
print_errors (GList *keyfiles)
{
	gint cols, col_len[2];
	gchar *col_header1, *col_header2;
	GList *l;

	tracker_term_dimensions (&cols, NULL);
	col_len[0] = cols / 2;
	col_len[1] = cols / 2 - 1;

	col_header1 = tracker_term_ellipsize (_("Path"), col_len[0], TRACKER_ELLIPSIZE_END);
	col_header2 = tracker_term_ellipsize (_("Message"), col_len[1], TRACKER_ELLIPSIZE_END);

	g_print (BOLD_BEGIN "%-*s %-*s" BOLD_END "\n",
	         col_len[0], col_header1,
	         col_len[1], col_header2);
	g_free (col_header1);
	g_free (col_header2);

	for (l = keyfiles; l; l = l->next) {
		GKeyFile *keyfile = l->data;
		gchar *uri, *message, *path, *str1, *str2;
		GFile *file;

		uri = g_key_file_get_string (keyfile, GROUP, KEY_URI, NULL);
		file = g_file_new_for_uri (uri);
		path = g_file_get_path (file);
		message = g_key_file_get_string (keyfile, GROUP, KEY_MESSAGE, NULL);
		g_object_unref (file);

		str1 = tracker_term_ellipsize (path, col_len[0], TRACKER_ELLIPSIZE_START);
		str2 = tracker_term_ellipsize (message, col_len[1], TRACKER_ELLIPSIZE_END);

		g_print ("%-*s %-*s\n",
		         col_len[0], str1,
		         col_len[1], str2);
		g_free (uri);
		g_free (path);
		g_free (message);
		g_free (str1);
		g_free (str2);
	}

	return EXIT_SUCCESS;
}

static int
get_no_args (void)
{
	gchar *str;
	gchar *data_dir;
	guint64 remaining_bytes;
	gdouble remaining;
	gint remaining_time;
	gint files, folders;
	GList *keyfiles;

	tracker_term_pipe_to_pager ();

	/* How many files / folders do we have? */
	if (get_file_and_folder_count (&files, &folders) != 0) {
		return EXIT_FAILURE;
	}

	g_print (_("Currently indexed"));
	g_print (": ");
	g_print (g_dngettext (NULL,
	                      "%d file",
	                      "%d files",
	                      files),
	         files);
	g_print (", ");
	g_print (g_dngettext (NULL,
	                      "%d folder",
	                      "%d folders",
	                      folders),
	         folders);
	g_print ("\n");

	/* How much space is left? */
	data_dir = g_build_filename (g_get_user_cache_dir (), "tracker3", NULL);

	remaining_bytes = tracker_file_system_get_remaining_space (data_dir);
	str = g_format_size (remaining_bytes);

	remaining = tracker_file_system_get_remaining_space_percentage (data_dir);
	g_print ("%s: %s (%3.2lf%%)\n",
	         _("Remaining space on database partition"),
	         str,
	         remaining);
	g_free (str);
	g_free (data_dir);

	/* Are we finished indexing? */
	if (!are_miners_finished (&remaining_time)) {
		gchar *remaining_time_str;

		remaining_time_str = tracker_seconds_to_string (remaining_time, TRUE);

		g_print ("%s: ", _("Data is still being indexed"));
		g_print (_("Estimated %s left"), remaining_time_str);
		g_print ("\n");
		g_free (remaining_time_str);
	} else {
		g_print ("%s\n", _("All data miners are idle, indexing complete"));
	}

	keyfiles = tracker_cli_get_error_keyfiles ();

	if (keyfiles) {
		g_print (g_dngettext (NULL,
		                      "%d recorded failure",
		                      "%d recorded failures",
		                      g_list_length (keyfiles)),
		         g_list_length (keyfiles));

		g_print ("\n\n");
		print_errors (keyfiles);
		g_list_free_full (keyfiles, (GDestroyNotify) g_key_file_unref);
	}

	tracker_term_pager_close ();

	return EXIT_SUCCESS;
}

static int
show_errors (gchar **terms)
{
	GList *keyfiles, *l;
	GKeyFile *keyfile;
	guint i;
	gboolean found = FALSE;

	tracker_term_pipe_to_pager ();

	keyfiles = tracker_cli_get_error_keyfiles ();

	for (i = 0; terms[i] != NULL; i++) {
		for (l = keyfiles; l; l = l->next) {
			GFile *file;
			gchar *uri, *path;

			keyfile = l->data;
			uri = g_key_file_get_string (keyfile, GROUP, KEY_URI, NULL);
			file = g_file_new_for_uri (uri);
			path = g_file_get_path (file);

			if (strstr (path, terms[i])) {
				gchar *sparql = g_key_file_get_string (keyfile, GROUP, KEY_SPARQL, NULL);
				gchar *message = g_key_file_get_string (keyfile, GROUP, KEY_MESSAGE, NULL);

				found = TRUE;
				g_print (BOLD_BEGIN "URI:" BOLD_END " %s\n", uri);

				if (message)
					g_print (BOLD_BEGIN "%s:" BOLD_END " %s\n", _("Message"), message);
				if (sparql)
					g_print (BOLD_BEGIN "SPARQL:" BOLD_END " %s\n", sparql);
				g_print ("\n");

				g_free (sparql);
				g_free (message);
			}

			g_object_unref (file);
			g_free (uri);
			g_free (path);
		}
	}

	if (!found)
		g_print (BOLD_BEGIN "%s" BOLD_END "\n", _("No reports found"));

	tracker_term_pager_close ();

	return EXIT_SUCCESS;
}

static int
status_run_default (void)
{
	return get_no_args ();
}

static gboolean
status_options_enabled (void)
{
	return STATUS_OPTIONS_ENABLED ();
}

int
main (int argc, const char **argv)
{
	GOptionContext *context;
	GError *error = NULL;

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, entries, NULL);

	argv[0] = "tracker status";

	if (!g_option_context_parse (context, &argc, (char***) &argv, &error)) {
		g_printerr ("%s, %s\n", _("Unrecognized options"), error->message);
		g_error_free (error);
		g_option_context_free (context);
		return EXIT_FAILURE;
	}

	g_option_context_free (context);

	if (status_options_enabled ()) {
		return status_run ();
	}

	if (terms)
		return show_errors (terms);

	return status_run_default ();
}
