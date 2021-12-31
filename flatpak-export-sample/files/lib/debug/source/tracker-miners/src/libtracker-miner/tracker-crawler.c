/*
 * Copyright (C) 2009, Nokia <ivan.frade@nokia.com>
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
 */

#include "config-miners.h"

#include "tracker-crawler.h"
#include "tracker-file-data-provider.h"
#include "tracker-miner-enums.h"
#include "tracker-miner-enum-types.h"
#include "tracker-utils.h"

#define FILE_ATTRIBUTES	  \
	G_FILE_ATTRIBUTE_STANDARD_NAME "," \
	G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
	G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN

#define MAX_SIMULTANEOUS_ITEMS       64

typedef struct TrackerCrawlerPrivate  TrackerCrawlerPrivate;
typedef struct DirectoryChildData DirectoryChildData;
typedef struct DirectoryProcessingData DirectoryProcessingData;
typedef struct DirectoryRootInfo DirectoryRootInfo;

typedef struct {
	TrackerCrawler *crawler;
	GFileEnumerator *enumerator;
	DirectoryRootInfo  *root_info;
	DirectoryProcessingData *dir_info;
	GFile *dir_file;
	GList *files;
} DataProviderData;

struct DirectoryChildData {
	GFile          *child;
	gboolean        is_dir;
};

struct DirectoryProcessingData {
	GNode *node;
	GSList *children;
	guint was_inspected : 1;
	guint ignored_by_content : 1;
};

struct DirectoryRootInfo {
	TrackerCrawler *crawler;
	GTask *task;
	GFile *directory;
	GNode *tree;

	GQueue *directory_processing_queue;

	TrackerDirectoryFlags flags;

	DataProviderData *dpd;

	guint idle_id;

	/* Directory stats */
	guint directories_found;
	guint directories_ignored;
	guint files_found;
	guint files_ignored;
};

struct TrackerCrawlerPrivate {
	TrackerDataProvider *data_provider;

	/* Idle handler for processing found data */
	guint           idle_id;

	gchar          *file_attributes;

	/* Check func */
	TrackerCrawlerCheckFunc check_func;
	gpointer check_func_data;
	GDestroyNotify check_func_destroy;
};

enum {
	PROP_0,
	PROP_DATA_PROVIDER,
};

static void     crawler_get_property     (GObject         *object,
                                          guint            prop_id,
                                          GValue          *value,
                                          GParamSpec      *pspec);
static void     crawler_set_property     (GObject         *object,
                                          guint            prop_id,
                                          const GValue    *value,
                                          GParamSpec      *pspec);
static void     crawler_finalize         (GObject         *object);
static void     data_provider_data_free  (DataProviderData        *dpd);

static void     data_provider_begin      (TrackerCrawler          *crawler,
					  DirectoryRootInfo       *info,
					  DirectoryProcessingData *dir_data);
static void     data_provider_end        (TrackerCrawler          *crawler,
                                          DirectoryRootInfo       *info);
static void     directory_root_info_free (DirectoryRootInfo *info);

static GQuark file_info_quark = 0;

G_DEFINE_TYPE_WITH_PRIVATE (TrackerCrawler, tracker_crawler, G_TYPE_OBJECT)

static void
tracker_crawler_class_init (TrackerCrawlerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->set_property = crawler_set_property;
	object_class->get_property = crawler_get_property;
	object_class->finalize = crawler_finalize;

	g_object_class_install_property (object_class,
	                                 PROP_DATA_PROVIDER,
	                                 g_param_spec_object ("data-provider",
	                                                      "Data provider",
	                                                      "Data provider to use to crawl structures populating data, e.g. like GFileEnumerator",
	                                                      TRACKER_TYPE_DATA_PROVIDER,
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY |
	                                                      G_PARAM_STATIC_STRINGS));

	file_info_quark = g_quark_from_static_string ("tracker-crawler-file-info");
}

static void
tracker_crawler_init (TrackerCrawler *object)
{
}

static void
crawler_set_property (GObject      *object,
                      guint         prop_id,
                      const GValue *value,
                      GParamSpec   *pspec)
{
	TrackerCrawlerPrivate *priv;

	priv = tracker_crawler_get_instance_private (TRACKER_CRAWLER (object));

	switch (prop_id) {
	case PROP_DATA_PROVIDER:
		priv->data_provider = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
crawler_get_property (GObject    *object,
                      guint       prop_id,
                      GValue     *value,
                      GParamSpec *pspec)
{
	TrackerCrawlerPrivate *priv;

	priv = tracker_crawler_get_instance_private (TRACKER_CRAWLER (object));

	switch (prop_id) {
	case PROP_DATA_PROVIDER:
		g_value_set_object (value, priv->data_provider);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
crawler_finalize (GObject *object)
{
	TrackerCrawlerPrivate *priv;

	priv = tracker_crawler_get_instance_private (TRACKER_CRAWLER (object));

	if (priv->check_func_data && priv->check_func_destroy) {
		priv->check_func_destroy (priv->check_func_data);
	}

	g_free (priv->file_attributes);

	if (priv->data_provider) {
		g_object_unref (priv->data_provider);
	}

	G_OBJECT_CLASS (tracker_crawler_parent_class)->finalize (object);
}

TrackerCrawler *
tracker_crawler_new (TrackerDataProvider *data_provider)
{
	TrackerCrawler *crawler;
	TrackerDataProvider *default_data_provider = NULL;

	if (G_LIKELY (!data_provider)) {
		/* Default to the file data_provider if none is passed */
		data_provider = default_data_provider = tracker_file_data_provider_new ();
	}

	crawler = g_object_new (TRACKER_TYPE_CRAWLER,
	                        "data-provider", data_provider,
	                        NULL);

	/* When a data provider is passed to us, we add a reference in
	 * the set_properties() function for this class, however, if
	 * we create the data provider, we also have the original
	 * reference for the created object which needs to be cleared
	 * up here.
	 */
	if (default_data_provider) {
		g_object_unref (default_data_provider);
	}

	return crawler;
}

void
tracker_crawler_set_check_func (TrackerCrawler          *crawler,
                                TrackerCrawlerCheckFunc  func,
                                gpointer                 user_data,
                                GDestroyNotify           destroy_notify)
{
	TrackerCrawlerPrivate *priv;

	g_return_if_fail (TRACKER_IS_CRAWLER (crawler));

	priv = tracker_crawler_get_instance_private (crawler);
	priv->check_func = func;
	priv->check_func_data = user_data;
	priv->check_func_destroy = destroy_notify;
}

static gboolean
invoke_check (TrackerCrawler           *crawler,
              TrackerCrawlerCheckFlags  flags,
              GFile                    *file,
              GFileInfo                *file_info,
              GList                    *children)
{
	TrackerCrawlerPrivate *priv;

	priv = tracker_crawler_get_instance_private (crawler);

	if (!priv->check_func)
		return TRUE;

	return priv->check_func (crawler, flags,
	                         file, file_info, children,
	                         priv->check_func_data);
}

static gboolean
check_file (TrackerCrawler    *crawler,
	    DirectoryRootInfo *info,
            GFile             *file)
{
	GFileInfo *file_info;
	gboolean use = FALSE;

	file_info = g_object_get_qdata (G_OBJECT (file), file_info_quark);
	use = invoke_check (crawler, TRACKER_CRAWLER_CHECK_FILE,
	                    file, file_info, NULL);

	info->files_found++;

	if (!use) {
		info->files_ignored++;
	}

	return use;
}

static gboolean
check_directory (TrackerCrawler    *crawler,
		 DirectoryRootInfo *info,
		 GFile             *file)
{
	GFileInfo *file_info;
	gboolean use = FALSE;

	file_info = g_object_get_qdata (G_OBJECT (file), file_info_quark);
	use = invoke_check (crawler, TRACKER_CRAWLER_CHECK_DIRECTORY,
	                    file, file_info, NULL);

	info->directories_found++;

	if (!use) {
		info->directories_ignored++;
	}

	return use;
}

static DirectoryChildData *
directory_child_data_new (GFile    *child,
			  gboolean  is_dir)
{
	DirectoryChildData *child_data;

	child_data = g_slice_new (DirectoryChildData);
	child_data->child = g_object_ref (child);
	child_data->is_dir = is_dir;

	return child_data;
}

static void
directory_child_data_free (DirectoryChildData *child_data)
{
	g_object_unref (child_data->child);
	g_slice_free (DirectoryChildData, child_data);
}

static DirectoryProcessingData *
directory_processing_data_new (GNode *node)
{
	DirectoryProcessingData *data;

	data = g_slice_new0 (DirectoryProcessingData);
	data->node = node;

	return data;
}

static void
directory_processing_data_free (DirectoryProcessingData *data)
{
	g_slist_foreach (data->children, (GFunc) directory_child_data_free, NULL);
	g_slist_free (data->children);

	g_slice_free (DirectoryProcessingData, data);
}

static void
directory_processing_data_add_child (DirectoryProcessingData *data,
				     GFile                   *child,
				     gboolean                 is_dir)
{
	DirectoryChildData *child_data;

	child_data = directory_child_data_new (child, is_dir);
	data->children = g_slist_prepend (data->children, child_data);
}

static DirectoryRootInfo *
directory_root_info_new (GFile                 *file,
                         GFileInfo             *file_info,
                         gchar                 *file_attributes,
                         TrackerDirectoryFlags  flags)
{
	DirectoryRootInfo *info;
	DirectoryProcessingData *dir_info;
	gboolean allow_stat = TRUE;

	info = g_slice_new0 (DirectoryRootInfo);

	info->directory = g_object_ref (file);
	info->directory_processing_queue = g_queue_new ();

	info->tree = g_node_new (g_object_ref (file));

	info->flags = flags;

	if ((info->flags & TRACKER_DIRECTORY_FLAG_NO_STAT) != 0) {
		allow_stat = FALSE;
	}

	if (!file_info && allow_stat && file_attributes) {
		GFileQueryInfoFlags file_flags;

		file_flags = G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS;

		file_info = g_file_query_info (file,
		                               file_attributes,
		                               file_flags,
		                               NULL,
		                               NULL);
		g_object_set_qdata_full (G_OBJECT (file),
		                         file_info_quark,
		                         file_info,
		                         (GDestroyNotify) g_object_unref);
	} else if (!file_info) {
		gchar *basename;

		file_info = g_file_info_new ();
		g_file_info_set_file_type (file_info, G_FILE_TYPE_DIRECTORY);

		basename = g_file_get_basename (file);
		g_file_info_set_name (file_info, basename);
		g_free (basename);

		/* Only thing missing is mtime, we can't know this.
		 * Not setting it means 0 is assumed, but if we set it
		 * to 'now' then the state machines above us will
		 * assume the directory is always newer when it may
		 * not be.
		 */

		g_file_info_set_content_type (file_info, "inode/directory");

		g_object_set_qdata_full (G_OBJECT (file),
		                         file_info_quark,
		                         file_info,
		                         (GDestroyNotify) g_object_unref);
	}

	/* Fill in the processing info for the root node */
	dir_info = directory_processing_data_new (info->tree);
	g_queue_push_tail (info->directory_processing_queue, dir_info);

	return info;
}

static gboolean
directory_tree_free_foreach (GNode    *node,
			     gpointer  user_data)
{
	g_object_unref (node->data);
	return FALSE;
}

static void
directory_root_info_free (DirectoryRootInfo *info)
{
	if (info->idle_id) {
		g_source_remove (info->idle_id);
	}

	if (info->dpd)  {
		data_provider_end (info->dpd->crawler, info);
	}

	g_object_unref (info->directory);

	g_node_traverse (info->tree,
			 G_PRE_ORDER,
			 G_TRAVERSE_ALL,
			 -1,
			 directory_tree_free_foreach,
			 NULL);
	g_node_destroy (info->tree);

	g_queue_foreach (info->directory_processing_queue,
			 (GFunc) directory_processing_data_free,
			 NULL);
	g_queue_free (info->directory_processing_queue);

	g_slice_free (DirectoryRootInfo, info);
}

static gboolean
process_next (DirectoryRootInfo *info)
{
	DirectoryProcessingData *dir_data;
	GTask *task = info->task;

	if (g_task_return_error_if_cancelled (task)) {
		g_object_unref (task);
		return G_SOURCE_REMOVE;
	}

	dir_data = g_queue_peek_head (info->directory_processing_queue);

	if (dir_data) {
		/* One directory inside the tree hierarchy is being inspected */
		if (!dir_data->ignored_by_content &&
		    dir_data->children != NULL) {
			DirectoryChildData *child_data;
			GNode *child_node = NULL;

			/* Directory has been already inspected, take children
			 * one by one and check whether they should be incorporated
			 * to the tree.
			 */
			child_data = dir_data->children->data;
			dir_data->children = g_slist_remove (dir_data->children, child_data);

			if (((child_data->is_dir &&
			      check_directory (info->crawler, info, child_data->child)) ||
			     (!child_data->is_dir &&
			      check_file (info->crawler, info, child_data->child)))) {
				child_node = g_node_prepend_data (dir_data->node,
								  g_object_ref (child_data->child));
			}

			if (G_NODE_IS_ROOT (dir_data->node) &&
			    child_node && child_data->is_dir) {
				DirectoryProcessingData *child_dir_data;

				child_dir_data = directory_processing_data_new (child_node);
				g_queue_push_tail (info->directory_processing_queue, child_dir_data);
			}

			directory_child_data_free (child_data);

			return G_SOURCE_CONTINUE;
		} else {
			/* No (more) children, or directory ignored. stop processing. */
			g_queue_pop_head (info->directory_processing_queue);
			g_task_return_boolean (task, !dir_data->ignored_by_content);
			directory_processing_data_free (dir_data);
			g_object_unref (task);
		}
	} else {
		/* Current directory being crawled doesn't have anything else
		 * to process.
		 */
		g_task_return_boolean (task, TRUE);
		g_object_unref (task);

		data_provider_end (info->crawler, info);
	}

	/* There's nothing else to process */
	return G_SOURCE_REMOVE;
}

static gboolean
process_func (gpointer data)
{
	DirectoryRootInfo *info = data;
	gboolean retval = FALSE;
	gint i;

	for (i = 0; i < MAX_SIMULTANEOUS_ITEMS; i++) {
		retval = process_next (info);
		if (retval == FALSE)
			break;
	}

	return retval;
}

static gboolean
process_func_start (DirectoryRootInfo *info)
{
	if (info->idle_id == 0) {
		info->idle_id = g_idle_add (process_func, info);
	}

	return TRUE;
}

static DataProviderData *
data_provider_data_new (TrackerCrawler          *crawler,
                        DirectoryRootInfo       *root_info,
                        DirectoryProcessingData *dir_info)
{
	DataProviderData *dpd;

	dpd = g_slice_new0 (DataProviderData);

	dpd->crawler = g_object_ref (crawler);
	dpd->root_info = root_info;
	dpd->dir_info = dir_info;
	/* Make sure there's always a ref of the GFile while we're
	 * iterating it */
	dpd->dir_file = g_object_ref (G_FILE (dir_info->node->data));

	return dpd;
}

static void
data_provider_data_process (DataProviderData *dpd)
{
	TrackerCrawler *crawler;
	GSList *l;
	GList *children = NULL;
	GFileInfo *file_info;
	gboolean use;

	crawler = dpd->crawler;

	for (l = dpd->dir_info->children; l; l = l->next) {
		DirectoryChildData *child_data;

		child_data = l->data;
		children = g_list_prepend (children, child_data->child);
	}

	file_info = g_object_get_qdata (G_OBJECT (dpd->dir_file), file_info_quark);
	use = invoke_check (crawler, TRACKER_CRAWLER_CHECK_CONTENT,
	                    dpd->dir_file, file_info, children);
	g_list_free (children);

	if (!use) {
		dpd->dir_info->ignored_by_content = TRUE;
		/* FIXME: Update stats */
		return;
	}
}

static void
data_provider_data_add (DataProviderData *dpd)
{
	TrackerCrawlerPrivate *priv;
	TrackerCrawler *crawler;
	GFile *parent;
	GList *l;

	crawler = dpd->crawler;
	parent = dpd->dir_file;
	priv = tracker_crawler_get_instance_private (crawler);

	for (l = dpd->files; l; l = l->next) {
		GFileInfo *info;
		GFile *child;
		const gchar *child_name;
		gboolean is_dir;

		info = l->data;

		child_name = g_file_info_get_name (info);
		child = g_file_get_child (parent, child_name);
		is_dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;

		if (priv->file_attributes) {
			/* Store the file info for future retrieval */
			g_object_set_qdata_full (G_OBJECT (child),
			                         file_info_quark,
			                         g_object_ref (info),
			                         (GDestroyNotify) g_object_unref);
		}

		directory_processing_data_add_child (dpd->dir_info, child, is_dir);

		g_object_unref (child);
		g_object_unref (info);
	}

	g_list_free (dpd->files);
	dpd->files = NULL;
}

static void
data_provider_data_free (DataProviderData *dpd)
{
	g_object_unref (dpd->dir_file);
	g_object_unref (dpd->crawler);

	if (dpd->files) {
		g_list_free_full (dpd->files, g_object_unref);
	}

	if (dpd->enumerator) {
		g_object_unref (dpd->enumerator);
	}

	g_slice_free (DataProviderData, dpd);
}

static void
data_provider_end_cb (GObject      *object,
                      GAsyncResult *result,
                      gpointer      user_data)
{
	DataProviderData *dpd;
	GError *error = NULL;

	g_file_enumerator_close_finish (G_FILE_ENUMERATOR (object), result, &error);
	dpd = user_data;

	if (error) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			gchar *uri = g_file_get_uri (dpd->dir_file);
			g_warning ("Could not end data provider for container / directory '%s', %s",
			           uri, error ? error->message : "no error given");
			g_free (uri);
		}
		g_clear_error (&error);
	}

	data_provider_data_free (dpd);
}

static void
data_provider_end (TrackerCrawler    *crawler,
                   DirectoryRootInfo *info)
{
	DataProviderData *dpd;

	g_return_if_fail (info != NULL);

	if (info->dpd == NULL) {
		/* Nothing to do */
		return;
	}

	/* We detach the DataProviderData from the DirectoryRootInfo
	 * here so it's not freed early. We can't use
	 * DirectoryRootInfo as user data for the async function below
	 * because it's freed before that callback will be called.
	 */
	dpd = info->dpd;
	info->dpd = NULL;

	if (dpd->enumerator) {
		g_file_enumerator_close_async (dpd->enumerator,
		                               G_PRIORITY_LOW, NULL,
		                               data_provider_end_cb,
		                               dpd);
	} else {
		data_provider_data_free (dpd);
	}
}

static void
enumerate_next_cb (GObject      *object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
	DataProviderData *dpd;
	GList *info;
	GError *error = NULL;
	DirectoryRootInfo *root_info;

	info = g_file_enumerator_next_files_finish (G_FILE_ENUMERATOR (object), result, &error);
	dpd = user_data;
	root_info = dpd->root_info;

	if (!info) {
		/* Could be due to:
		 * a) error,
		 * b) no more items
		 */
		if (error) {
			/* We don't consider cancellation an error, so we only
			 * log errors which are not cancellations.
			 */
			g_task_return_error (root_info->task, error);
			g_object_unref (root_info->task);
			return;
		} else {
			/* Done enumerating, start processing what we got ... */
			data_provider_data_add (dpd);
			data_provider_data_process (dpd);
		}

		process_func_start (dpd->root_info);
	} else {
		DirectoryRootInfo *root_info;

		root_info = dpd->root_info;

		/* More work to do, we keep reference given to us */
		dpd->files = g_list_concat (dpd->files, info);
		g_file_enumerator_next_files_async (G_FILE_ENUMERATOR (object),
		                                    MAX_SIMULTANEOUS_ITEMS,
		                                    G_PRIORITY_LOW,
		                                    g_task_get_cancellable (root_info->task),
		                                    enumerate_next_cb,
		                                    dpd);
	}
}

static void
data_provider_begin_cb (GObject      *object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
	GFileEnumerator *enumerator;
	DirectoryRootInfo *info;
	DataProviderData *dpd;
	GError *error = NULL;

	enumerator = tracker_data_provider_begin_finish (TRACKER_DATA_PROVIDER (object), result, &error);

	info = user_data;

	if (error) {
		g_task_return_error (info->task, error);
		g_object_unref (info->task);
		return;
	}

	dpd = info->dpd;
	dpd->enumerator = enumerator;
	g_file_enumerator_next_files_async (enumerator,
	                                    MAX_SIMULTANEOUS_ITEMS,
	                                    G_PRIORITY_LOW,
	                                    g_task_get_cancellable (info->task),
	                                    enumerate_next_cb,
	                                    dpd);
}

static void
data_provider_begin (TrackerCrawler          *crawler,
                     DirectoryRootInfo       *info,
                     DirectoryProcessingData *dir_data)
{
	TrackerCrawlerPrivate *priv;
	DataProviderData *dpd;
	gchar *attrs;

	priv = tracker_crawler_get_instance_private (crawler);

	dpd = data_provider_data_new (crawler, info, dir_data);
	info->dpd = dpd;

	if (priv->file_attributes) {
		attrs = g_strconcat (FILE_ATTRIBUTES ",",
		                     priv->file_attributes,
		                     NULL);
	} else {
		attrs = g_strdup (FILE_ATTRIBUTES);
	}

	tracker_data_provider_begin_async (priv->data_provider,
	                                   dpd->dir_file,
	                                   attrs,
	                                   info->flags,
	                                   G_PRIORITY_LOW,
	                                   g_task_get_cancellable (info->task),
	                                   data_provider_begin_cb,
	                                   info);
	g_free (attrs);
}

gboolean
tracker_crawler_get_finish (TrackerCrawler  *crawler,
                            GAsyncResult    *result,
                            GFile          **directory,
                            GNode          **tree,
                            guint           *directories_found,
                            guint           *directories_ignored,
                            guint           *files_found,
                            guint           *files_ignored,
                            GError         **error)
{
	DirectoryRootInfo *info;
	gboolean retval;

	info = g_task_get_task_data (G_TASK (result));

	retval = g_task_propagate_boolean (G_TASK (result), error);
	if (retval) {
		if (tree)
			*tree = info->tree;
	}

	if (directory)
		*directory = info->directory;
	if (directories_found)
		*directories_found = info->directories_found;
	if (directories_ignored)
		*directories_ignored = info->directories_ignored;
	if (files_found)
		*files_found = info->files_found;
	if (files_ignored)
		*files_ignored = info->files_ignored;

	return retval;
}

void
tracker_crawler_get (TrackerCrawler        *crawler,
                     GFile                 *file,
                     TrackerDirectoryFlags  flags,
                     GCancellable          *cancellable,
                     GAsyncReadyCallback    callback,
                     gpointer               user_data)
{
	TrackerCrawlerPrivate *priv;
	DirectoryProcessingData *dir_data;
	DirectoryRootInfo *info;
	GFileInfo *file_info;
	GTask *task;

	g_return_if_fail (TRACKER_IS_CRAWLER (crawler));
	g_return_if_fail (G_IS_FILE (file));

	priv = tracker_crawler_get_instance_private (crawler);

	file_info = tracker_crawler_get_file_info (crawler, file);

	info = directory_root_info_new (file, file_info, priv->file_attributes, flags);
	task = g_task_new (crawler, cancellable, callback, user_data);
	g_task_set_task_data (task, info,
	                      (GDestroyNotify) directory_root_info_free);
	info->task = task;
	info->crawler = crawler;

	if (!file_info && !check_directory (crawler, info, file)) {
		g_task_return_boolean (task, FALSE);
		g_object_unref (task);
		return;
	}

	dir_data = g_queue_peek_head (info->directory_processing_queue);

	if (dir_data)
		data_provider_begin (crawler, info, dir_data);
}

/**
 * tracker_crawler_set_file_attributes:
 * @crawler: a #TrackerCrawler
 * @file_attributes: file attributes to extract
 *
 * Sets the file attributes that @crawler will fetch for every
 * file it gets, this info may be requested through
 * tracker_crawler_get_file_info() in any #TrackerCrawler callback
 **/
void
tracker_crawler_set_file_attributes (TrackerCrawler *crawler,
				     const gchar    *file_attributes)
{
	TrackerCrawlerPrivate *priv;

	g_return_if_fail (TRACKER_IS_CRAWLER (crawler));

	priv = tracker_crawler_get_instance_private (crawler);

	g_free (priv->file_attributes);
	priv->file_attributes = g_strdup (file_attributes);
}

/**
 * tracker_crawler_get_file_attributes:
 * @crawler: a #TrackerCrawler
 *
 * Returns the file attributes that @crawler will fetch
 *
 * Returns: the file attributes as a string.
 **/
const gchar *
tracker_crawler_get_file_attributes (TrackerCrawler *crawler)
{
	TrackerCrawlerPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CRAWLER (crawler), NULL);

	priv = tracker_crawler_get_instance_private (crawler);

	return priv->file_attributes;
}

/**
 * tracker_crawler_get_file_info:
 * @crawler: a #TrackerCrawler
 * @file: a #GFile returned by @crawler
 *
 * Returns a #GFileInfo with the file attributes requested through
 * tracker_crawler_set_file_attributes().
 *
 * Returns: (transfer none): a #GFileInfo with the file information
 **/
GFileInfo *
tracker_crawler_get_file_info (TrackerCrawler *crawler,
			       GFile          *file)
{
	GFileInfo *info;

	g_return_val_if_fail (TRACKER_IS_CRAWLER (crawler), NULL);
	g_return_val_if_fail (G_IS_FILE (file), NULL);

	info = g_object_get_qdata (G_OBJECT (file), file_info_quark);
	return info;
}
