/*
 * Copyright (C) 2011, Nokia <ivan.frade@nokia.com>
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
 * Author: Carlos Garnacho  <carlos@lanedo.com>
 */

#include "config-miners.h"

#include <libtracker-miners-common/tracker-common.h>
#include <libtracker-extract/tracker-extract.h>
#include <libtracker-sparql/tracker-sparql.h>

#include "tracker-file-notifier.h"
#include "tracker-crawler.h"
#include "tracker-monitor.h"

enum {
	PROP_0,
	PROP_INDEXING_TREE,
	PROP_DATA_PROVIDER,
	PROP_CONNECTION,
	PROP_FILE_ATTRIBUTES,
};

enum {
	FILE_CREATED,
	FILE_UPDATED,
	FILE_DELETED,
	FILE_MOVED,
	DIRECTORY_STARTED,
	DIRECTORY_FINISHED,
	FINISHED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

enum {
	FILE_STATE_NONE,
	FILE_STATE_CREATE,
	FILE_STATE_UPDATE,
	FILE_STATE_DELETE,
};

typedef struct {
	GList *node;
	GFile *file;
	guint in_disk : 1;
	guint in_store : 1;
	guint is_dir_in_disk : 1;
	guint is_dir_in_store : 1;
	guint state : 2;
	guint64 store_mtime;
	guint64 disk_mtime;
	gchar *extractor_hash;
	gchar *mimetype;
} TrackerFileData;

typedef struct {
	GFile *root;
	GFile *current_dir;
	GQueue *pending_dirs;
	guint flags;
	guint directories_found;
	guint directories_ignored;
	guint files_found;
	guint files_ignored;
	guint current_dir_content_filtered : 1;
	guint ignore_root                  : 1;
} RootData;

typedef struct {
	TrackerIndexingTree *indexing_tree;

	TrackerSparqlConnection *connection;
	GCancellable *cancellable;

	TrackerCrawler *crawler;
	TrackerMonitor *monitor;
	TrackerDataProvider *data_provider;
	GHashTable *cache;
	GQueue queue;

	TrackerSparqlStatement *content_query;
	TrackerSparqlStatement *deleted_query;

	GTimer *timer;
	gchar *file_attributes;

	/* List of pending directory
	 * trees to get data from
	 */
	GList *pending_index_roots;
	RootData *current_index_root;

	guint stopped : 1;
} TrackerFileNotifierPrivate;

static gboolean notifier_query_root_contents (TrackerFileNotifier *notifier);
static gboolean crawl_directory_in_current_root (TrackerFileNotifier *notifier);
static void finish_current_directory (TrackerFileNotifier *notifier,
                                      gboolean             interrupted);

G_DEFINE_TYPE_WITH_PRIVATE (TrackerFileNotifier, tracker_file_notifier, G_TYPE_OBJECT)

static void
tracker_file_notifier_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (TRACKER_FILE_NOTIFIER (object));

	switch (prop_id) {
	case PROP_INDEXING_TREE:
		priv->indexing_tree = g_value_dup_object (value);
		break;
	case PROP_DATA_PROVIDER:
		priv->data_provider = g_value_dup_object (value);
		break;
	case PROP_CONNECTION:
		priv->connection = g_value_dup_object (value);
		break;
	case PROP_FILE_ATTRIBUTES:
		priv->file_attributes = g_value_dup_string (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
tracker_file_notifier_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (TRACKER_FILE_NOTIFIER (object));

	switch (prop_id) {
	case PROP_INDEXING_TREE:
		g_value_set_object (value, priv->indexing_tree);
		break;
	case PROP_DATA_PROVIDER:
		g_value_set_object (value, priv->data_provider);
		break;
	case PROP_CONNECTION:
		g_value_set_object (value, priv->connection);
		break;
	case PROP_FILE_ATTRIBUTES:
		g_value_set_string (value, priv->file_attributes);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static RootData *
root_data_new (TrackerFileNotifier *notifier,
               GFile               *file,
               guint                flags,
               gboolean             ignore_root)
{
	RootData *data;

	data = g_new0 (RootData, 1);
	data->root = g_object_ref (file);
	data->pending_dirs = g_queue_new ();
	data->flags = flags;
	data->ignore_root = ignore_root;

	g_queue_push_tail (data->pending_dirs, g_object_ref (file));

	return data;
}

static void
root_data_free (RootData *data)
{
	g_queue_free_full (data->pending_dirs, (GDestroyNotify) g_object_unref);
	if (data->current_dir) {
		g_object_unref (data->current_dir);
	}
	g_object_unref (data->root);
	g_free (data);
}

/* Crawler signal handlers */
static gboolean
check_file (TrackerFileNotifier *notifier,
            GFile               *file,
            GFileInfo           *info)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	return tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
	                                                file, info);
}

static gboolean
check_directory (TrackerFileNotifier *notifier,
                 GFile               *directory,
                 GFileInfo           *info)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);
	g_assert (priv->current_index_root != NULL);

	/* If it's a config root itself, other than the one
	 * currently processed, bypass it, it will be processed
	 * when the time arrives.
	 */
	if (tracker_indexing_tree_file_is_root (priv->indexing_tree, directory) &&
	    !g_file_equal (directory, priv->current_index_root->root)) {
		return FALSE;
	}

	return tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
	                                                directory, info);
}

static gboolean
check_directory_contents (TrackerFileNotifier *notifier,
                          GFile               *parent,
                          const GList         *children)
{
	TrackerFileNotifierPrivate *priv;
	gboolean process = TRUE;

	priv = tracker_file_notifier_get_instance_private (notifier);

	/* Do not let content filter apply to configured roots themselves. This
	 * is a measure to trim undesired portions of the filesystem, and if
	 * the folder is configured to be indexed, it's clearly not undesired.
	 */
	if (!tracker_indexing_tree_file_is_root (priv->indexing_tree, parent)) {
		process = tracker_indexing_tree_parent_is_indexable (priv->indexing_tree,
		                                                     parent, (GList*) children);
	}

	if (!process) {
		priv->current_index_root->current_dir_content_filtered = TRUE;
		tracker_monitor_remove (priv->monitor, parent);
	}

	return process;
}

static gboolean
file_notifier_notify (GFile           *file,
                      TrackerFileData *file_data,
                      gpointer         user_data)
{
	TrackerFileNotifier *notifier;
	TrackerFileNotifierPrivate *priv;
	gboolean stop = FALSE;

	notifier = user_data;
	priv = tracker_file_notifier_get_instance_private (notifier);

	if (file_data->state == FILE_STATE_DELETE) {
		/* In store but not in disk, delete */
		g_signal_emit (notifier, signals[FILE_DELETED], 0, file,
		               file_data->is_dir_in_store);
		stop = TRUE;
	} else if (file_data->state == FILE_STATE_CREATE) {
		/* In disk but not in store, create */
		g_signal_emit (notifier, signals[FILE_CREATED], 0, file,
		               tracker_crawler_get_file_info (priv->crawler, file));
	} else if (file_data->state == FILE_STATE_UPDATE) {
		/* File changed, update */
		g_signal_emit (notifier, signals[FILE_UPDATED], 0, file,
		               tracker_crawler_get_file_info (priv->crawler, file),
		               FALSE);
	}

	return stop;
}

static gboolean
notifier_check_next_root (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);
	g_assert (priv->current_index_root == NULL);

	if (priv->pending_index_roots) {
		return notifier_query_root_contents (notifier);
	} else {
		g_signal_emit (notifier, signals[FINISHED], 0);
		return FALSE;
	}
}

static void
file_notifier_traverse_tree (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;
	TrackerFileData *data;

	priv = tracker_file_notifier_get_instance_private (notifier);
	g_assert (priv->current_index_root != NULL);

	while ((data = g_queue_pop_tail (&priv->queue)) != NULL) {
		file_notifier_notify (data->file, data, notifier);
		g_hash_table_remove (priv->cache, data->file);
	}
}

static void
update_state (TrackerFileData *data)
{
	data->state = FILE_STATE_NONE;

	if (data->in_disk) {
		if (data->in_store) {
			if (data->store_mtime != data->disk_mtime) {
				data->state = FILE_STATE_UPDATE;
			} else if (data->mimetype) {
				const gchar *current_hash;

				current_hash = tracker_extract_module_manager_get_hash (data->mimetype);

				if (g_strcmp0 (data->extractor_hash, current_hash) != 0) {
					data->state = FILE_STATE_UPDATE;
				}
			}
		} else {
			data->state = FILE_STATE_CREATE;
		}
	} else {
		if (data->in_store) {
			data->state = FILE_STATE_DELETE;
		}
	}
}

static void
file_data_free (TrackerFileData *file_data)
{
	g_object_unref (file_data->file);
	g_slice_free (TrackerFileData, file_data);
}

static TrackerFileData *
ensure_file_data (TrackerFileNotifier *notifier,
                  GFile               *file)
{
	TrackerFileNotifierPrivate *priv;
	TrackerFileData *file_data;

	priv = tracker_file_notifier_get_instance_private (notifier);

	file_data = g_hash_table_lookup (priv->cache, file);
	if (!file_data) {
		file_data = g_slice_new0 (TrackerFileData);
		file_data->file = g_object_ref (file);
		g_hash_table_insert (priv->cache, file_data->file, file_data);
		file_data->node = g_list_alloc ();
		file_data->node->data = file_data;
		g_queue_push_head_link (&priv->queue, file_data->node);
	}

	return file_data;
}

static TrackerFileData *
_insert_disk_info (TrackerFileNotifier *notifier,
                   GFile               *file,
                   GFileType            file_type,
                   guint64              _time)
{
	TrackerFileData *file_data;

	file_data = ensure_file_data (notifier, file);
	file_data->in_disk = TRUE;
	file_data->is_dir_in_disk = file_type == G_FILE_TYPE_DIRECTORY;
	file_data->disk_mtime = _time;
	update_state (file_data);

	return file_data;
}

static gboolean
file_notifier_add_node_foreach (GNode    *node,
                                gpointer  user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerFileNotifierPrivate *priv;
	GFileInfo *file_info;
	GFile *file;

	priv = tracker_file_notifier_get_instance_private (notifier);
	file = node->data;

	if (G_NODE_IS_ROOT (node) &&
	    (file != priv->current_index_root->root ||
	     priv->current_index_root->ignore_root))
		return FALSE;

	file_info = tracker_crawler_get_file_info (priv->crawler, file);

	if (file_info) {
		TrackerFileData *file_data;
		GFileType file_type;
		guint64 _time;

		file_type = g_file_info_get_file_type (file_info);
		_time = g_file_info_get_attribute_uint64 (file_info,
		                                          G_FILE_ATTRIBUTE_TIME_MODIFIED);

		file_data = _insert_disk_info (notifier,
		                               file,
		                               file_type,
		                               _time);

		if (file_data->state == FILE_STATE_NONE) {
			/* If at this point the file has no assigned event,
			 * it didn't get changed, and can be ignored.
			 */
			g_queue_delete_link (&priv->queue, file_data->node);
			g_hash_table_remove (priv->cache, file);
		}

		if (file_type == G_FILE_TYPE_DIRECTORY &&
		    (priv->current_index_root->flags & TRACKER_DIRECTORY_FLAG_RECURSE) != 0 &&
		    !G_NODE_IS_ROOT (node)) {
			/* Queue child dirs for later processing */
			g_assert (node->children == NULL);
			g_queue_push_tail (priv->current_index_root->pending_dirs,
			                   g_object_ref (file));
		}
	}

	return FALSE;
}

static void
crawler_get_cb (TrackerCrawler *crawler,
                GAsyncResult   *result,
                gpointer        user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerFileNotifierPrivate *priv =
		tracker_file_notifier_get_instance_private (notifier);
	guint directories_found, directories_ignored;
	guint files_found, files_ignored;
	GFile *directory;
	GNode *tree;
	GError *error = NULL;

	if (!tracker_crawler_get_finish (crawler,
	                                 result,
	                                 &directory,
	                                 &tree,
	                                 &directories_found,
	                                 &directories_ignored,
	                                 &files_found,
	                                 &files_ignored,
	                                 &error)) {
		gboolean interrupted;

		interrupted = error &&
			g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

		if (error &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
			gchar *uri;

			uri = g_file_get_uri (directory);
			g_warning ("Got error crawling '%s': %s\n",
			           uri, error->message);
			g_free (uri);
		}
		tracker_monitor_remove (priv->monitor, directory);

		if (interrupted || !crawl_directory_in_current_root (notifier))
			finish_current_directory (notifier, interrupted);

		g_clear_error (&error);
		return;
	}

	g_node_traverse (tree,
	                 G_PRE_ORDER,
	                 G_TRAVERSE_ALL,
	                 -1,
	                 file_notifier_add_node_foreach,
	                 notifier);

	priv->current_index_root->directories_found += directories_found;
	priv->current_index_root->directories_ignored += directories_ignored;
	priv->current_index_root->files_found += files_found;
	priv->current_index_root->files_ignored += files_ignored;

	if (!crawl_directory_in_current_root (notifier))
		finish_current_directory (notifier, FALSE);
}

static void
_insert_store_info (TrackerFileNotifier *notifier,
                    GFile               *file,
                    GFileType            file_type,
                    const gchar         *extractor_hash,
                    const gchar         *mimetype,
                    guint64              _time)
{
	TrackerFileData *file_data;

	file_data = ensure_file_data (notifier, file);
	file_data->in_store = TRUE;
	file_data->is_dir_in_store = file_type == G_FILE_TYPE_DIRECTORY;
	file_data->extractor_hash = g_strdup (extractor_hash);
	file_data->mimetype = g_strdup (mimetype);
	file_data->store_mtime = _time;
	update_state (file_data);
}

static gboolean
crawl_directory_in_current_root (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;
	GFile *directory;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (!priv->current_index_root)
		return FALSE;

	while (!g_queue_is_empty (priv->current_index_root->pending_dirs)) {
		TrackerDirectoryFlags flags;

		directory = g_queue_pop_head (priv->current_index_root->pending_dirs);
		g_set_object (&priv->current_index_root->current_dir, directory);

		tracker_indexing_tree_get_root (priv->indexing_tree,
		                                directory, &flags);

		if ((flags & TRACKER_DIRECTORY_FLAG_MONITOR) != 0)
			tracker_monitor_add (priv->monitor, directory);

		/* Begin crawling the directory non-recursively. */
		tracker_crawler_get (priv->crawler,
		                     directory,
		                     priv->current_index_root->flags,
		                     priv->cancellable,
		                     (GAsyncReadyCallback) crawler_get_cb,
		                     notifier);
		g_object_unref (directory);
		return TRUE;
	}

	return FALSE;
}

static void
finish_current_directory (TrackerFileNotifier *notifier,
                          gboolean             interrupted)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (interrupted) {
		g_queue_clear (&priv->queue);
		g_hash_table_remove_all (priv->cache);
	} else {
		file_notifier_traverse_tree (notifier);
	}


	if (interrupted || !crawl_directory_in_current_root (notifier)) {
		/* No more directories left to be crawled in the current
		 * root, jump to the next one.
		 */
		g_signal_emit (notifier, signals[DIRECTORY_FINISHED], 0,
		               priv->current_index_root->root,
		               priv->current_index_root->directories_found,
		               priv->current_index_root->directories_ignored,
		               priv->current_index_root->files_found,
		               priv->current_index_root->files_ignored);

		TRACKER_NOTE (STATISTICS,
		              g_message ("  Notified files after %2.2f seconds",
		                         g_timer_elapsed (priv->timer, NULL)));
		TRACKER_NOTE (STATISTICS,
		              g_message ("  Found %d directories, ignored %d directories",
		                        priv->current_index_root->directories_found,
		                        priv->current_index_root->directories_ignored));
		TRACKER_NOTE (STATISTICS,
		              g_message ("  Found %d files, ignored %d files",
		                         priv->current_index_root->files_found,
		                         priv->current_index_root->files_ignored));

		if (!interrupted) {
			g_clear_pointer (&priv->current_index_root, root_data_free);
			notifier_check_next_root (notifier);
		}
	}
}

static gboolean
root_data_remove_directory (RootData *data,
			    GFile    *directory)
{
	GList *l = data->pending_dirs->head, *next;
	GFile *file;

	while (l) {
		file = l->data;
		next = l->next;

		if (g_file_equal (file, directory) ||
		    g_file_has_prefix (file, directory)) {
			g_queue_remove (data->pending_dirs, file);
			g_object_unref (file);
		}

		l = next;
	}

	return (g_file_equal (data->current_dir, directory) ||
		g_file_has_prefix (data->current_dir, directory));
}

static void
file_notifier_current_root_check_remove_directory (TrackerFileNotifier *notifier,
						   GFile               *file)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (priv->current_index_root &&
	    root_data_remove_directory (priv->current_index_root, file)) {
		g_cancellable_cancel (priv->cancellable);

		if (!crawl_directory_in_current_root (notifier)) {
			g_clear_pointer (&priv->current_index_root, root_data_free);
			notifier_check_next_root (notifier);
		}
	}
}

static TrackerSparqlStatement *
sparql_contents_ensure_statement (TrackerFileNotifier  *notifier,
                                  GError              **error)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (priv->content_query)
		return priv->content_query;

	priv->content_query =
		tracker_sparql_connection_query_statement (priv->connection,
		                                           "SELECT ?uri ?folderUrn ?lastModified ?hash nie:mimeType(?ie) "
		                                           "{"
		                                           "  GRAPH tracker:FileSystem {"
		                                           "    ?uri a nfo:FileDataObject ;"
		                                           "         nfo:fileLastModified ?lastModified ;"
		                                           "         nie:dataSource ?s ."
		                                           "    ~root nie:interpretedAs /"
		                                           "          nie:rootElementOf ?s ."
		                                           "    OPTIONAL {"
		                                           "      ?uri nie:interpretedAs ?folderUrn ."
		                                           "      ?folderUrn a nfo:Folder "
		                                           "    }"
		                                           "    OPTIONAL {"
		                                           "      ?uri tracker:extractorHash ?hash "
		                                           "    }"
		                                           "  }"
		                                           "  OPTIONAL {"
		                                           "    ?uri nie:interpretedAs ?ie "
		                                           "  }"
		                                           "}"
		                                           "ORDER BY ?uri",
		                                           priv->cancellable,
		                                           error);
	return priv->content_query;
}

static TrackerSparqlStatement *
sparql_deleted_ensure_statement (TrackerFileNotifier  *notifier,
                                 GError              **error)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (priv->deleted_query)
		return priv->deleted_query;

	priv->deleted_query =
		tracker_sparql_connection_query_statement (priv->connection,
		                                           "SELECT ?mimeType "
		                                           "{"
		                                           "  GRAPH tracker:FileSystem {"
		                                           "  ?ie nie:mimeType ?mimeType ; "
		                                           "      nie:isStoredAs ~uri . "
		                                           "  }"
		                                           "}"
		                                           "ORDER BY ?uri",
		                                           priv->cancellable,
		                                           error);
	return priv->deleted_query;
}

static void
query_execute_cb (TrackerSparqlStatement *statement,
                  GAsyncResult           *res,
                  TrackerFileNotifier    *notifier)
{
	TrackerFileNotifierPrivate *priv;
	TrackerSparqlCursor *cursor;
	GError *error = NULL;

	priv = tracker_file_notifier_get_instance_private (notifier);

	cursor = tracker_sparql_statement_execute_finish (statement, res, &error);

	if (!cursor) {
		gchar *uri;

		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			uri = g_file_get_uri (priv->current_index_root->root);
			g_critical ("Could not query contents for indexed folder '%s': %s",
				    uri, error->message);
			g_free (uri);
			g_error_free (error);
		}

		/* Move on to next root */
		finish_current_directory (notifier, TRUE);
		return;
	}

	while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
		const gchar *time_str, *folder_urn, *uri;
		GFileType file_type;
		GFile *file;
		guint64 _time;

		uri = tracker_sparql_cursor_get_string (cursor, 0, NULL);
		folder_urn = tracker_sparql_cursor_get_string (cursor, 1, NULL);
		time_str = tracker_sparql_cursor_get_string (cursor, 2, NULL);

		file = g_file_new_for_uri (uri);
		_time = tracker_string_to_date (time_str, NULL, &error);
		file_type = folder_urn != NULL ? G_FILE_TYPE_DIRECTORY : G_FILE_TYPE_UNKNOWN;

		_insert_store_info (notifier,
		                    file,
		                    file_type,
		                    tracker_sparql_cursor_get_string (cursor, 3, NULL),
		                    tracker_sparql_cursor_get_string (cursor, 4, NULL),
		                    _time);

		g_object_unref (file);
	}

	g_object_unref (cursor);

	if (!crawl_directory_in_current_root (notifier)) {
		finish_current_directory (notifier, FALSE);
	}
}

static gboolean
notifier_query_root_contents (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;
	TrackerDirectoryFlags flags;
	GFile *directory;
	gchar *uri;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (priv->current_index_root) {
		return FALSE;
	}

	if (!priv->pending_index_roots) {
		return FALSE;
	}

	if (priv->stopped) {
		return FALSE;
	}

	if (!sparql_contents_ensure_statement (notifier, NULL)) {
		return FALSE;
	}

	if (priv->cancellable)
		g_object_unref (priv->cancellable);
	priv->cancellable = g_cancellable_new ();

	priv->current_index_root = priv->pending_index_roots->data;
	priv->pending_index_roots = g_list_delete_link (priv->pending_index_roots,
	                                                priv->pending_index_roots);
	directory = priv->current_index_root->root;
	flags = priv->current_index_root->flags;
	uri = g_file_get_uri (directory);

	if ((flags & TRACKER_DIRECTORY_FLAG_IGNORE) != 0) {
		if ((flags & TRACKER_DIRECTORY_FLAG_PRESERVE) == 0) {
			g_signal_emit (notifier, signals[FILE_DELETED], 0, directory, TRUE);
		}

		/* Move on to next root */
		g_clear_pointer (&priv->current_index_root, root_data_free);
		notifier_check_next_root (notifier);
		return TRUE;
	}

	g_timer_reset (priv->timer);
	g_signal_emit (notifier, signals[DIRECTORY_STARTED], 0, directory);

	priv = tracker_file_notifier_get_instance_private (notifier);
	tracker_sparql_statement_bind_string (priv->content_query, "root", uri);
	g_free (uri);

	tracker_sparql_statement_execute_async (priv->content_query,
	                                        priv->cancellable,
	                                        (GAsyncReadyCallback) query_execute_cb,
	                                        notifier);
	return TRUE;
}

static gint
find_directory_root (RootData *data,
                     GFile    *file)
{
	if (g_file_equal (data->root, file))
		return 0;

	return -1;
}

static void
notifier_queue_root (TrackerFileNotifier   *notifier,
                     GFile                 *file,
                     TrackerDirectoryFlags  flags,
                     gboolean               ignore_root)
{
	TrackerFileNotifierPrivate *priv;
	RootData *data;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (priv->current_index_root &&
	    g_file_equal (priv->current_index_root->root, file))
		return;

	if (g_list_find_custom (priv->pending_index_roots, file,
	                        (GCompareFunc) find_directory_root))
		return;

	data = root_data_new (notifier, file, flags, ignore_root);

	if (flags & TRACKER_DIRECTORY_FLAG_PRIORITY) {
		priv->pending_index_roots = g_list_prepend (priv->pending_index_roots, data);
	} else {
		priv->pending_index_roots = g_list_append (priv->pending_index_roots, data);
	}

	if (!priv->current_index_root)
		notifier_check_next_root (notifier);
}

static GFileInfo *
create_shallow_file_info (GFile    *file,
                          gboolean  is_directory)
{
	GFileInfo *file_info;
	gchar *basename;

	file_info = g_file_info_new ();
	g_file_info_set_file_type (file_info,
	                           is_directory ?
	                           G_FILE_TYPE_DIRECTORY : G_FILE_TYPE_REGULAR);
	basename = g_file_get_basename (file);
	g_file_info_set_is_hidden (file_info, basename[0] == '.');
	g_free (basename);

	return file_info;
}

/* Monitor signal handlers */
static void
monitor_item_created_cb (TrackerMonitor *monitor,
                         GFile          *file,
                         gboolean        is_directory,
                         gpointer        user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerFileNotifierPrivate *priv;
	gboolean indexable;

	priv = tracker_file_notifier_get_instance_private (notifier);

	indexable = tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
	                                                     file, NULL);

	if (!is_directory) {
		gboolean parent_indexable;
		GList *children;
		GFile *parent;

		parent = g_file_get_parent (file);

		if (parent) {
			children = g_list_prepend (NULL, file);
			parent_indexable = tracker_indexing_tree_parent_is_indexable (priv->indexing_tree,
			                                                              parent,
			                                                              children);
			g_list_free (children);

			if (!parent_indexable) {
				/* New file triggered a directory content
				 * filter, remove parent directory altogether
				 */
				g_signal_emit (notifier, signals[FILE_DELETED], 0, parent, TRUE);
				file_notifier_current_root_check_remove_directory (notifier, parent);

				tracker_monitor_remove_recursively (priv->monitor, parent);
				return;
			}

			g_object_unref (parent);
		}

		if (!indexable)
			return;
	} else {
		TrackerDirectoryFlags flags;

		if (!indexable)
			return;

		/* If config for the directory is recursive,
		 * Crawl new entire directory and add monitors
		 */
		tracker_indexing_tree_get_root (priv->indexing_tree,
		                                file, &flags);

		if (flags & TRACKER_DIRECTORY_FLAG_RECURSE) {
			notifier_queue_root (notifier, file, flags, TRUE);

			/* Fall though, we want ::file-created to be emitted
			 * ASAP so it is ensured to be processed before any
			 * possible monitor events we might get afterwards.
			 */
		}
	}

	g_signal_emit (notifier, signals[FILE_CREATED], 0, file, NULL);
}

static void
monitor_item_updated_cb (TrackerMonitor *monitor,
                         GFile          *file,
                         gboolean        is_directory,
                         gpointer        user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (!tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
	                                              file, NULL)) {
		/* File should not be indexed */
		return;
	}

	g_signal_emit (notifier, signals[FILE_UPDATED], 0, file, NULL, FALSE);
}

static void
monitor_item_attribute_updated_cb (TrackerMonitor *monitor,
                                   GFile          *file,
                                   gboolean        is_directory,
                                   gpointer        user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (!tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
	                                              file, NULL)) {
		/* File should not be indexed */
		return;
	}

	g_signal_emit (notifier, signals[FILE_UPDATED], 0, file, NULL, TRUE);
}

static void
monitor_item_deleted_cb (TrackerMonitor *monitor,
                         GFile          *file,
                         gboolean        is_directory,
                         gpointer        user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	/* Remove monitors if any */
	if (is_directory &&
	    tracker_indexing_tree_file_is_root (priv->indexing_tree, file)) {
		tracker_monitor_remove_children_recursively (priv->monitor,
		                                             file);
	} else if (is_directory) {
		tracker_monitor_remove_recursively (priv->monitor, file);
	}

	if (!is_directory) {
		TrackerSparqlStatement *stmt;
		TrackerSparqlCursor *cursor = NULL;
		const gchar *mimetype;
		gchar *uri;

		/* TrackerMonitor only knows about monitored folders,
		 * query the data if we don't know that much.
		 */
		stmt = sparql_deleted_ensure_statement (notifier, NULL);

		if (stmt) {
			uri = g_file_get_uri (file);
			tracker_sparql_statement_bind_string (stmt, "uri", uri);
			cursor = tracker_sparql_statement_execute (stmt, NULL, NULL);
			g_free (uri);
		}

		if (cursor && tracker_sparql_cursor_next (cursor, NULL, NULL)) {
			mimetype = tracker_sparql_cursor_get_string (cursor, 0, NULL);
			is_directory = g_strcmp0 (mimetype, "inode/directory") == 0;
		}

		g_clear_object (&cursor);
	}

	if (!is_directory) {
		TrackerDirectoryFlags flags;
		gboolean indexable;
		GList *children;
		GFile *parent;

		children = g_list_prepend (NULL, file);
		parent = g_file_get_parent (file);

		indexable = tracker_indexing_tree_parent_is_indexable (priv->indexing_tree,
		                                                       parent, children);
		g_list_free (children);

		/* note: This supposedly works, but in practice
		 * won't ever happen as we don't get monitor events
		 * from directories triggering a filter of type
		 * TRACKER_FILTER_PARENT_DIRECTORY.
		 */
		if (!indexable) {
			/* New file was triggering a directory content
			 * filter, reindex parent directory altogether
			 */
			tracker_indexing_tree_get_root (priv->indexing_tree,
							parent, &flags);
			notifier_queue_root (notifier, parent, flags, FALSE);
			return;
		}

		g_object_unref (parent);
	}

	if (!tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
	                                              file, NULL)) {
		/* File was not indexed */
		return ;
	}

	g_signal_emit (notifier, signals[FILE_DELETED], 0, file, is_directory);

	file_notifier_current_root_check_remove_directory (notifier, file);
}

static gboolean
extension_changed (GFile *file1,
                   GFile *file2)
{
	gchar *basename1, *basename2;
	const gchar *ext1, *ext2;
	gboolean changed;

	basename1 = g_file_get_basename (file1);
	basename2 = g_file_get_basename (file2);

	ext1 = strrchr (basename1, '.');
	ext2 = strrchr (basename2, '.');

	changed = g_strcmp0 (ext1, ext2) != 0;

	g_free (basename1);
	g_free (basename2);

	return changed;
}

static void
monitor_item_moved_cb (TrackerMonitor *monitor,
                       GFile          *file,
                       GFile          *other_file,
                       gboolean        is_directory,
                       gboolean        is_source_monitored,
                       gpointer        user_data)
{
	TrackerFileNotifier *notifier;
	TrackerFileNotifierPrivate *priv;
	TrackerDirectoryFlags flags;

	notifier = user_data;
	priv = tracker_file_notifier_get_instance_private (notifier);
	tracker_indexing_tree_get_root (priv->indexing_tree, other_file, &flags);

	if (!is_source_monitored) {
		if (is_directory) {
			/* Remove monitors if any */
			tracker_monitor_remove_recursively (priv->monitor, file);
			notifier_queue_root (notifier, other_file, flags, FALSE);
		}
		/* else, file, do nothing */
	} else {
		gboolean should_process, should_process_other;
		GFileInfo *file_info, *other_file_info;
		GFile *check_file;

		if (is_directory) {
			check_file = g_object_ref (file);
		} else {
			check_file = g_file_get_parent (file);
		}

		file_info = create_shallow_file_info (file, is_directory);
		other_file_info = create_shallow_file_info (other_file, is_directory);

		/* If the (parent) directory is in
		 * the filesystem, file is stored
		 */
		should_process = tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
		                                                          file, file_info);
		should_process_other = tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
		                                                                other_file, other_file_info);
		g_object_unref (check_file);
		g_object_unref (file_info);
		g_object_unref (other_file_info);

		/* Ref those so they are safe to use after signal emission */
		g_object_ref (file);
		g_object_ref (other_file);

		if (!should_process) {
			/* The source was not an indexable file, the destination
			 * could be though, it should be indexed as if new, then */

			/* Remove monitors if any */
			if (is_directory) {
				tracker_monitor_remove_recursively (priv->monitor,
				                                    file);
			}

			if (should_process_other) {
				gboolean dest_is_recursive;
				TrackerDirectoryFlags flags;

				tracker_indexing_tree_get_root (priv->indexing_tree, other_file, &flags);
				dest_is_recursive = (flags & TRACKER_DIRECTORY_FLAG_RECURSE) != 0;

				/* Source file was not stored, check dest file as new */
				if (!is_directory || !dest_is_recursive) {
					g_signal_emit (notifier, signals[FILE_CREATED], 0, other_file, NULL);
				} else if (is_directory) {
					/* Crawl dest directory */
					notifier_queue_root (notifier, other_file, flags, FALSE);
				}
			}
			/* Else, do nothing else */
		} else if (!should_process_other) {
			/* Delete original location as it moves to be non indexable */
			if (is_directory) {
				tracker_monitor_remove_recursively (priv->monitor,
				                                    file);
			}

			g_signal_emit (notifier, signals[FILE_DELETED], 0, file, is_directory);
			file_notifier_current_root_check_remove_directory (notifier, file);
		} else {
			/* Handle move */
			if (is_directory) {
				gboolean dest_is_recursive, source_is_recursive;
				TrackerDirectoryFlags source_flags;

				tracker_monitor_move (priv->monitor,
				                      file, other_file);

				tracker_indexing_tree_get_root (priv->indexing_tree,
				                                file, &source_flags);
				source_is_recursive = (source_flags & TRACKER_DIRECTORY_FLAG_RECURSE) != 0;
				dest_is_recursive = (flags & TRACKER_DIRECTORY_FLAG_RECURSE) != 0;

				if (source_is_recursive && !dest_is_recursive) {
					/* A directory is being moved from a
					 * recursive location to a non-recursive
					 * one, don't do anything here, and let
					 * TrackerMinerFS handle it, see item_move().
					 */
				} else if (!source_is_recursive && dest_is_recursive) {
					/* crawl the folder */
					notifier_queue_root (notifier, other_file, flags, TRUE);
				}
			}

			g_signal_emit (notifier, signals[FILE_MOVED], 0, file, other_file, is_directory);

			if (extension_changed (file, other_file))
				g_signal_emit (notifier, signals[FILE_UPDATED], 0, other_file, NULL, FALSE);
		}

		g_object_unref (other_file);
		g_object_unref (file);
	}
}

/* Indexing tree signal handlers */
static void
indexing_tree_directory_added (TrackerIndexingTree *indexing_tree,
                               GFile               *directory,
                               gpointer             user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerDirectoryFlags flags;

	tracker_indexing_tree_get_root (indexing_tree, directory, &flags);
	notifier_queue_root (notifier, directory, flags, FALSE);
}

static void
indexing_tree_directory_updated (TrackerIndexingTree *indexing_tree,
                                 GFile               *directory,
                                 gpointer             user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerDirectoryFlags flags;

	tracker_indexing_tree_get_root (indexing_tree, directory, &flags);
	flags |= TRACKER_DIRECTORY_FLAG_CHECK_DELETED;
	notifier_queue_root (notifier, directory, flags, FALSE);
}

static void
indexing_tree_directory_removed (TrackerIndexingTree *indexing_tree,
                                 GFile               *directory,
                                 gpointer             user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerFileNotifierPrivate *priv;
	TrackerDirectoryFlags flags;
	GList *elem;

	priv = tracker_file_notifier_get_instance_private (notifier);

	/* Flags are still valid at the moment of deletion */
	tracker_indexing_tree_get_root (indexing_tree, directory, &flags);

	/* If the folder was being ignored, index/crawl it from scratch */
	if (flags & TRACKER_DIRECTORY_FLAG_IGNORE) {
		GFile *parent;

		parent = g_file_get_parent (directory);

		if (parent) {
			TrackerDirectoryFlags parent_flags;

			tracker_indexing_tree_get_root (indexing_tree,
			                                parent,
			                                &parent_flags);

			if (parent_flags & TRACKER_DIRECTORY_FLAG_RECURSE) {
				notifier_queue_root (notifier, directory, parent_flags, FALSE);
			} else if (tracker_indexing_tree_file_is_root (indexing_tree,
			                                               parent)) {
				g_signal_emit (notifier, signals[FILE_CREATED],
				               0, directory, NULL);
			}

			g_object_unref (parent);
		}
		return;
	}

	if ((flags & TRACKER_DIRECTORY_FLAG_PRESERVE) == 0) {
		/* Directory needs to be deleted from the store too */
		g_signal_emit (notifier, signals[FILE_DELETED], 0, directory, TRUE);
	}

	elem = g_list_find_custom (priv->pending_index_roots, directory,
	                           (GCompareFunc) find_directory_root);

	if (elem) {
		root_data_free (elem->data);
		priv->pending_index_roots =
			g_list_delete_link (priv->pending_index_roots, elem);
	}

	if (priv->current_index_root &&
	    g_file_equal (directory, priv->current_index_root->root)) {
		/* Directory being currently processed */
		g_cancellable_cancel (priv->cancellable);

		/* If the crawler was already stopped (eg. we're at the querying
		 * phase), the current index root won't be cleared.
		 */
		g_clear_pointer (&priv->current_index_root, root_data_free);
		notifier_check_next_root (notifier);
	}

	/* Remove monitors if any */
	/* FIXME: How do we handle this with 3rd party data_providers? */
	tracker_monitor_remove_recursively (priv->monitor, directory);
}

static void
indexing_tree_child_updated (TrackerIndexingTree *indexing_tree,
                             GFile               *root,
                             GFile               *child,
                             gpointer             user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerFileNotifierPrivate *priv;
	TrackerDirectoryFlags flags;
	GFileInfo *child_info;
	GFileType child_type;

	priv = tracker_file_notifier_get_instance_private (notifier);

	child_info = g_file_query_info (child,
	                                G_FILE_ATTRIBUTE_STANDARD_TYPE ","
	                                G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
	                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                                NULL, NULL);
	if (!child_info)
		return;

	child_type = g_file_info_get_file_type (child_info);
	tracker_indexing_tree_get_root (indexing_tree, child, &flags);

	if (child_type == G_FILE_TYPE_DIRECTORY &&
	    (flags & TRACKER_DIRECTORY_FLAG_RECURSE)) {
		flags |= TRACKER_DIRECTORY_FLAG_CHECK_DELETED;

		notifier_queue_root (notifier, child, flags, FALSE);
	} else if (tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
	                                                    child, child_info)) {
		g_signal_emit (notifier, signals[FILE_UPDATED], 0,
		               child, child_info, FALSE);
	}
}

static void
tracker_file_notifier_finalize (GObject *object)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (TRACKER_FILE_NOTIFIER (object));

	g_queue_clear (&priv->queue);
	g_hash_table_destroy (priv->cache);
	g_free (priv->file_attributes);

	if (priv->indexing_tree) {
		g_object_unref (priv->indexing_tree);
	}

	if (priv->data_provider) {
		g_object_unref (priv->data_provider);
	}

	if (priv->cancellable) {
		g_cancellable_cancel (priv->cancellable);
		g_object_unref (priv->cancellable);
	}

	g_clear_object (&priv->content_query);
	g_clear_object (&priv->deleted_query);

	tracker_monitor_set_enabled (priv->monitor, FALSE);
	g_signal_handlers_disconnect_by_data (priv->monitor, object);

	g_object_unref (priv->crawler);
	g_object_unref (priv->monitor);
	g_clear_object (&priv->connection);

	g_clear_pointer (&priv->current_index_root, root_data_free);

	g_list_foreach (priv->pending_index_roots, (GFunc) root_data_free, NULL);
	g_list_free (priv->pending_index_roots);
	g_timer_destroy (priv->timer);

	G_OBJECT_CLASS (tracker_file_notifier_parent_class)->finalize (object);
}

static void
check_disable_monitor (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;
	TrackerSparqlCursor *cursor;
	gint64 folder_count = 0;
	GError *error = NULL;

	priv = tracker_file_notifier_get_instance_private (notifier);
	cursor = tracker_sparql_connection_query (priv->connection,
	                                          "SELECT COUNT(?f) { ?f a nfo:Folder }",
	                                          NULL, &error);

	if (!error && tracker_sparql_cursor_next (cursor, NULL, &error)) {
		folder_count = tracker_sparql_cursor_get_integer (cursor, 0);
		tracker_sparql_cursor_close (cursor);
	}

	if (error) {
		g_warning ("Could not get folder count: %s\n", error->message);
		g_error_free (error);
	} else if (folder_count > tracker_monitor_get_limit (priv->monitor)) {
		/* If the folder count exceeds the monitor limit, there's
		 * nothing we can do anyway to prevent possibly out of date
		 * content. As it is the case no matter what we try, fully
		 * embrace it instead, and disable monitors until after crawling
		 * has been performed. This dramatically improves crawling time
		 * as monitors are inherently expensive.
		 */
		g_info ("Temporarily disabling monitors until crawling is "
		        "completed. Too many folders to monitor anyway");
		tracker_monitor_set_enabled (priv->monitor, FALSE);
	}

	g_clear_object (&cursor);
}

static gboolean
crawler_check_func (TrackerCrawler           *crawler,
                    TrackerCrawlerCheckFlags  flags,
                    GFile                    *file,
                    GFileInfo                *file_info,
                    const GList              *children,
                    gpointer                  user_data)
{
	TrackerFileNotifier *notifier = user_data;

	if (flags & TRACKER_CRAWLER_CHECK_FILE) {
		if (!check_file (notifier, file, file_info))
			return FALSE;
	}

	if (flags & TRACKER_CRAWLER_CHECK_DIRECTORY) {
		if (!check_directory (notifier, file, file_info))
			return FALSE;
	}

	if (flags & TRACKER_CRAWLER_CHECK_CONTENT) {
		if (!check_directory_contents (notifier, file, children))
			return FALSE;
	}

	return TRUE;
}

static void
tracker_file_notifier_constructed (GObject *object)
{
	TrackerFileNotifierPrivate *priv;

	G_OBJECT_CLASS (tracker_file_notifier_parent_class)->constructed (object);

	priv = tracker_file_notifier_get_instance_private (TRACKER_FILE_NOTIFIER (object));
	g_assert (priv->indexing_tree);

	g_signal_connect (priv->indexing_tree, "directory-added",
	                  G_CALLBACK (indexing_tree_directory_added), object);
	g_signal_connect (priv->indexing_tree, "directory-updated",
	                  G_CALLBACK (indexing_tree_directory_updated), object);
	g_signal_connect (priv->indexing_tree, "directory-removed",
	                  G_CALLBACK (indexing_tree_directory_removed), object);
	g_signal_connect (priv->indexing_tree, "child-updated",
	                  G_CALLBACK (indexing_tree_child_updated), object);

	/* Set up crawler */
	priv->crawler = tracker_crawler_new (priv->data_provider);
	tracker_crawler_set_check_func (priv->crawler,
	                                crawler_check_func,
	                                object, NULL);
	tracker_crawler_set_file_attributes (priv->crawler, priv->file_attributes);

	check_disable_monitor (TRACKER_FILE_NOTIFIER (object));
}

static void
tracker_file_notifier_real_finished (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (!tracker_monitor_get_enabled (priv->monitor)) {
		/* If the monitor was disabled on ::constructed (see
		 * check_disable_monitor()), enable it back again.
		 * This will lazily create all missing directory
		 * monitors.
		 */
		g_info ("Re-enabling directory monitors");
		tracker_monitor_set_enabled (priv->monitor, TRUE);
	}
}

static void
tracker_file_notifier_class_init (TrackerFileNotifierClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_file_notifier_finalize;
	object_class->set_property = tracker_file_notifier_set_property;
	object_class->get_property = tracker_file_notifier_get_property;
	object_class->constructed = tracker_file_notifier_constructed;

	klass->finished = tracker_file_notifier_real_finished;

	signals[FILE_CREATED] =
		g_signal_new ("file-created",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerFileNotifierClass,
		                               file_created),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              2, G_TYPE_FILE, G_TYPE_FILE_INFO);
	signals[FILE_UPDATED] =
		g_signal_new ("file-updated",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerFileNotifierClass,
		                               file_updated),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              3, G_TYPE_FILE, G_TYPE_FILE_INFO, G_TYPE_BOOLEAN);
	signals[FILE_DELETED] =
		g_signal_new ("file-deleted",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerFileNotifierClass,
		                               file_deleted),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              2, G_TYPE_FILE, G_TYPE_BOOLEAN);
	signals[FILE_MOVED] =
		g_signal_new ("file-moved",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerFileNotifierClass,
		                               file_moved),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              3, G_TYPE_FILE, G_TYPE_FILE, G_TYPE_BOOLEAN);
	signals[DIRECTORY_STARTED] =
		g_signal_new ("directory-started",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerFileNotifierClass,
		                               directory_started),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              1, G_TYPE_FILE);
	signals[DIRECTORY_FINISHED] =
		g_signal_new ("directory-finished",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerFileNotifierClass,
		                               directory_finished),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              5, G_TYPE_FILE, G_TYPE_UINT,
		              G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT);
	signals[FINISHED] =
		g_signal_new ("finished",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerFileNotifierClass,
		                               finished),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE, 0, G_TYPE_NONE);

	g_object_class_install_property (object_class,
	                                 PROP_INDEXING_TREE,
	                                 g_param_spec_object ("indexing-tree",
	                                                      "Indexing tree",
	                                                      "Indexing tree",
	                                                      TRACKER_TYPE_INDEXING_TREE,
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY |
	                                                      G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
	                                 PROP_DATA_PROVIDER,
	                                 g_param_spec_object ("data-provider",
	                                                      "Data provider",
	                                                      "Data provider to use to crawl structures populating data, e.g. like GFileEnumerator",
	                                                      TRACKER_TYPE_DATA_PROVIDER,
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY |
	                                                      G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
	                                 PROP_CONNECTION,
	                                 g_param_spec_object ("connection",
	                                                      "Connection",
	                                                      "Connection to use for queries",
	                                                      TRACKER_SPARQL_TYPE_CONNECTION,
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY |
	                                                      G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
	                                 PROP_FILE_ATTRIBUTES,
	                                 g_param_spec_string ("file-attributes",
	                                                      "File attributes",
	                                                      "File attributes",
	                                                      NULL,
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY |
	                                                      G_PARAM_STATIC_STRINGS));
}

static void
tracker_file_notifier_init (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;
	GError *error = NULL;

	priv = tracker_file_notifier_get_instance_private (notifier);
	priv->timer = g_timer_new ();
	priv->stopped = TRUE;

	/* Set up monitor */
	priv->monitor = tracker_monitor_new ();

	if (!g_initable_init (G_INITABLE (priv->monitor), NULL, &error)) {
		g_warning ("Could not init monitor: %s", error->message);
		g_error_free (error);
	} else {
		g_signal_connect (priv->monitor, "item-created",
		                  G_CALLBACK (monitor_item_created_cb),
		                  notifier);
		g_signal_connect (priv->monitor, "item-updated",
		                  G_CALLBACK (monitor_item_updated_cb),
		                  notifier);
		g_signal_connect (priv->monitor, "item-attribute-updated",
		                  G_CALLBACK (monitor_item_attribute_updated_cb),
		                  notifier);
		g_signal_connect (priv->monitor, "item-deleted",
		                  G_CALLBACK (monitor_item_deleted_cb),
		                  notifier);
		g_signal_connect (priv->monitor, "item-moved",
		                  G_CALLBACK (monitor_item_moved_cb),
		                  notifier);
	}

	g_queue_init (&priv->queue);
	priv->cache = g_hash_table_new_full (g_file_hash,
	                                     (GEqualFunc) g_file_equal,
	                                     NULL,
	                                     (GDestroyNotify) file_data_free);
}

TrackerFileNotifier *
tracker_file_notifier_new (TrackerIndexingTree     *indexing_tree,
                           TrackerDataProvider     *data_provider,
                           TrackerSparqlConnection *connection,
                           const gchar             *file_attributes)
{
	g_return_val_if_fail (TRACKER_IS_INDEXING_TREE (indexing_tree), NULL);

	return g_object_new (TRACKER_TYPE_FILE_NOTIFIER,
	                     "indexing-tree", indexing_tree,
	                     "data-provider", data_provider,
	                     "connection", connection,
	                     "file-attributes", file_attributes,
	                     NULL);
}

gboolean
tracker_file_notifier_start (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_FILE_NOTIFIER (notifier), FALSE);

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (priv->stopped) {
		priv->stopped = FALSE;
		notifier_check_next_root (notifier);
	}

	return TRUE;
}

void
tracker_file_notifier_stop (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;

	g_return_if_fail (TRACKER_IS_FILE_NOTIFIER (notifier));

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (!priv->stopped) {
		g_clear_pointer (&priv->current_index_root, root_data_free);
		g_cancellable_cancel (priv->cancellable);
		priv->stopped = TRUE;
	}
}

gboolean
tracker_file_notifier_is_active (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_FILE_NOTIFIER (notifier), FALSE);

	priv = tracker_file_notifier_get_instance_private (notifier);
	return priv->pending_index_roots || priv->current_index_root;
}
