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
 * Author: Carlos Garnacho <carlos@lanedo.com>
 */

#include "config-miners.h"

#include <libtracker-sparql/tracker-sparql.h>
#include "libtracker-miners-common/tracker-debug.h"

#include "tracker-sparql-buffer.h"

typedef struct _TrackerSparqlBufferPrivate TrackerSparqlBufferPrivate;
typedef struct _SparqlTaskData SparqlTaskData;
typedef struct _UpdateBatchData UpdateBatchData;

enum {
	PROP_0,
	PROP_CONNECTION
};

struct _TrackerSparqlBufferPrivate
{
	TrackerSparqlConnection *connection;
	GPtrArray *tasks;
	GHashTable *file_set;
	gint n_updates;
	TrackerBatch *batch;
};

enum {
	TASK_TYPE_RESOURCE,
	TASK_TYPE_SPARQL
};

struct _SparqlTaskData
{
	guint type;

	union {
		struct {
			gchar *graph;
			TrackerResource *resource;
		} resource;

		struct {
			gchar *sparql;
		} sparql;
	} d;
};

struct _UpdateBatchData {
	TrackerSparqlBuffer *buffer;
	GPtrArray *tasks;
	TrackerBatch *batch;
	GTask *async_task;
};

G_DEFINE_TYPE_WITH_PRIVATE (TrackerSparqlBuffer, tracker_sparql_buffer, TRACKER_TYPE_TASK_POOL)

static void
tracker_sparql_buffer_finalize (GObject *object)
{
	TrackerSparqlBufferPrivate *priv;

	priv = tracker_sparql_buffer_get_instance_private (TRACKER_SPARQL_BUFFER (object));

	g_object_unref (priv->connection);

	G_OBJECT_CLASS (tracker_sparql_buffer_parent_class)->finalize (object);
}

static void
tracker_sparql_buffer_set_property (GObject      *object,
                                    guint         param_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
	TrackerSparqlBufferPrivate *priv;

	priv = tracker_sparql_buffer_get_instance_private (TRACKER_SPARQL_BUFFER (object));

	switch (param_id) {
	case PROP_CONNECTION:
		priv->connection = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	}
}

static void
tracker_sparql_buffer_get_property (GObject    *object,
                                    guint       param_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
	TrackerSparqlBufferPrivate *priv;

	priv = tracker_sparql_buffer_get_instance_private (TRACKER_SPARQL_BUFFER (object));

	switch (param_id) {
	case PROP_CONNECTION:
		g_value_set_object (value,
		                    priv->connection);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	}
}

static void
tracker_sparql_buffer_class_init (TrackerSparqlBufferClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_sparql_buffer_finalize;
	object_class->set_property = tracker_sparql_buffer_set_property;
	object_class->get_property = tracker_sparql_buffer_get_property;

	g_object_class_install_property (object_class,
	                                 PROP_CONNECTION,
	                                 g_param_spec_object ("connection",
	                                                      "sparql connection",
	                                                      "Sparql Connection",
	                                                      TRACKER_SPARQL_TYPE_CONNECTION,
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY |
	                                                      G_PARAM_STATIC_STRINGS));
}

static void
tracker_sparql_buffer_init (TrackerSparqlBuffer *buffer)
{
}

TrackerSparqlBuffer *
tracker_sparql_buffer_new (TrackerSparqlConnection *connection,
                           guint                    limit)
{
	return g_object_new (TRACKER_TYPE_SPARQL_BUFFER,
	                     "connection", connection,
	                     "limit", limit,
	                     NULL);
}

static void
remove_task_foreach (TrackerTask     *task,
                     TrackerTaskPool *pool)
{
	tracker_task_pool_remove (pool, task);
}

static void
update_batch_data_free (UpdateBatchData *batch_data)
{
	g_object_unref (batch_data->batch);

	g_ptr_array_foreach (batch_data->tasks,
	                     (GFunc) remove_task_foreach,
	                     batch_data->buffer);
	g_ptr_array_unref (batch_data->tasks);

	g_clear_object (&batch_data->async_task);

	g_slice_free (UpdateBatchData, batch_data);
}

static void
batch_execute_cb (GObject      *object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
	TrackerSparqlBufferPrivate *priv;
	TrackerSparqlBuffer *buffer;
	GError *error = NULL;
	UpdateBatchData *update_data;

	update_data = user_data;
	buffer = TRACKER_SPARQL_BUFFER (update_data->buffer);
	priv = tracker_sparql_buffer_get_instance_private (buffer);
	priv->n_updates--;

	TRACKER_NOTE (MINER_FS_EVENTS,
	              g_message ("(Sparql buffer) Finished array-update with %u tasks",
	                         update_data->tasks->len));

	if (!tracker_batch_execute_finish (TRACKER_BATCH (object),
	                                   result,
	                                   &error)) {
		g_task_set_task_data (update_data->async_task,
		                      g_ptr_array_ref (update_data->tasks),
		                      (GDestroyNotify) g_ptr_array_unref);
		g_task_return_error (update_data->async_task, error);
	} else {
		g_task_return_pointer (update_data->async_task,
		                       g_ptr_array_ref (update_data->tasks),
		                       (GDestroyNotify) g_ptr_array_unref);
	}

	update_batch_data_free (update_data);
}

gboolean
tracker_sparql_buffer_flush (TrackerSparqlBuffer *buffer,
                             const gchar         *reason,
                             GAsyncReadyCallback  cb,
                             gpointer             user_data)
{
	TrackerSparqlBufferPrivate *priv;
	UpdateBatchData *update_data;

	priv = tracker_sparql_buffer_get_instance_private (buffer);

	if (priv->n_updates > 0) {
		return FALSE;
	}

	if (!priv->tasks ||
	    priv->tasks->len == 0) {
		return FALSE;
	}

	TRACKER_NOTE (MINER_FS_EVENTS, g_message ("Flushing SPARQL buffer, reason: %s", reason));

	update_data = g_slice_new0 (UpdateBatchData);
	update_data->buffer = buffer;
	update_data->tasks = g_ptr_array_ref (priv->tasks);
	update_data->batch = g_object_ref (priv->batch);
	update_data->async_task = g_task_new (buffer, NULL, cb, user_data);

	/* Empty pool, update_data will keep
	 * references to the tasks to keep
	 * these alive.
	 */
	g_ptr_array_unref (priv->tasks);
	priv->tasks = NULL;
	g_clear_pointer (&priv->file_set, g_hash_table_unref);
	priv->n_updates++;
	g_clear_object (&priv->batch);

	tracker_batch_execute_async (update_data->batch,
	                             NULL,
	                             batch_execute_cb,
	                             update_data);
	return TRUE;
}

static void
sparql_buffer_push_to_pool (TrackerSparqlBuffer *buffer,
                            TrackerTask         *task)
{
	TrackerSparqlBufferPrivate *priv;

	priv = tracker_sparql_buffer_get_instance_private (buffer);

	/* Task pool addition increments reference */
	tracker_task_pool_add (TRACKER_TASK_POOL (buffer), task);

	if (!priv->tasks) {
		priv->tasks = g_ptr_array_new_with_free_func ((GDestroyNotify) tracker_task_unref);
		priv->file_set = g_hash_table_new (g_file_hash, (GEqualFunc) g_file_equal);
	}

	/* We add a reference here because we unref when removed from
	 * the GPtrArray. */
	g_ptr_array_add (priv->tasks, tracker_task_ref (task));
	g_hash_table_add (priv->file_set, tracker_task_get_file (task));
}

static TrackerBatch *
tracker_sparql_buffer_get_current_batch (TrackerSparqlBuffer *buffer)
{
	TrackerSparqlBufferPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer), NULL);

	priv = tracker_sparql_buffer_get_instance_private (TRACKER_SPARQL_BUFFER (buffer));

	if (!priv->batch)
		priv->batch = tracker_sparql_connection_create_batch (priv->connection);

	return priv->batch;
}

static SparqlTaskData *
sparql_task_data_new_resource (const gchar     *graph,
                               TrackerResource *resource)
{
	SparqlTaskData *task_data;

	task_data = g_slice_new0 (SparqlTaskData);
	task_data->type = TASK_TYPE_RESOURCE;
	task_data->d.resource.resource = g_object_ref (resource);
	task_data->d.resource.graph = g_strdup (graph);

	return task_data;
}

static SparqlTaskData *
sparql_task_data_new_sparql (const gchar *sparql)
{
	SparqlTaskData *task_data;

	task_data = g_slice_new0 (SparqlTaskData);
	task_data->type = TASK_TYPE_SPARQL;
	task_data->d.sparql.sparql = g_strdup (sparql);

	return task_data;
}

static void
sparql_task_data_free (SparqlTaskData *data)
{
	if (data->type == TASK_TYPE_RESOURCE) {
		g_clear_object (&data->d.resource.resource);
		g_free (data->d.resource.graph);
	} else if (data->type == TASK_TYPE_SPARQL) {
		g_free (data->d.sparql.sparql);
	}

	g_slice_free (SparqlTaskData, data);
}

void
tracker_sparql_buffer_push (TrackerSparqlBuffer *buffer,
                            GFile               *file,
                            const gchar         *graph,
                            TrackerResource     *resource)
{
	TrackerBatch *batch;
	TrackerTask *task;
	SparqlTaskData *data;

	g_return_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer));
	g_return_if_fail (G_IS_FILE (file));
	g_return_if_fail (TRACKER_IS_RESOURCE (resource));

	batch = tracker_sparql_buffer_get_current_batch (buffer);
	tracker_batch_add_resource (batch, graph, resource);

	data = sparql_task_data_new_resource (graph, resource);

	task = tracker_task_new (file, data,
	                         (GDestroyNotify) sparql_task_data_free);
	sparql_buffer_push_to_pool (buffer, task);
	tracker_task_unref (task);
}

void
tracker_sparql_buffer_push_sparql (TrackerSparqlBuffer *buffer,
                                   GFile               *file,
                                   const gchar         *sparql)
{
	TrackerBatch *batch;
	TrackerTask *task;
	SparqlTaskData *data;

	g_return_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer));
	g_return_if_fail (G_IS_FILE (file));
	g_return_if_fail (sparql != NULL);

	batch = tracker_sparql_buffer_get_current_batch (buffer);
	tracker_batch_add_sparql (batch, sparql);

	data = sparql_task_data_new_sparql (sparql);

	task = tracker_task_new (file, data,
	                         (GDestroyNotify) sparql_task_data_free);
	sparql_buffer_push_to_pool (buffer, task);
	tracker_task_unref (task);
}

gchar *
tracker_sparql_task_get_sparql (TrackerTask *task)
{
	SparqlTaskData *task_data;

	task_data = tracker_task_get_data (task);

	if (task_data->type == TASK_TYPE_RESOURCE) {
		return tracker_resource_print_sparql_update (task_data->d.resource.resource,
		                                             NULL,
		                                             task_data->d.resource.graph);
	} else if (task_data->type == TASK_TYPE_SPARQL) {
		return g_strdup (task_data->d.sparql.sparql);
	}

	return NULL;
}

GPtrArray *
tracker_sparql_buffer_flush_finish (TrackerSparqlBuffer  *buffer,
                                    GAsyncResult         *res,
                                    GError              **error)
{
	GPtrArray *tasks;

	g_return_val_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer), NULL);
	g_return_val_if_fail (G_IS_ASYNC_RESULT (res), NULL);
	g_return_val_if_fail (!error || !*error, NULL);

	tasks = g_task_propagate_pointer (G_TASK (res), error);

	if (!tasks)
		tasks = g_task_get_task_data (G_TASK (res));

	return tasks;
}

TrackerSparqlBufferState
tracker_sparql_buffer_get_state (TrackerSparqlBuffer *buffer,
                                 GFile               *file)
{
	TrackerSparqlBufferPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_SPARQL_BUFFER (buffer), TRACKER_BUFFER_STATE_UNKNOWN);
	g_return_val_if_fail (G_IS_FILE (file), TRACKER_BUFFER_STATE_UNKNOWN);

	priv = tracker_sparql_buffer_get_instance_private (TRACKER_SPARQL_BUFFER (buffer));

	if (!tracker_task_pool_find (TRACKER_TASK_POOL (buffer), file))
		return TRACKER_BUFFER_STATE_UNKNOWN;

	if (priv->file_set != NULL && g_hash_table_contains (priv->file_set, file))
		return TRACKER_BUFFER_STATE_QUEUED;

	return TRACKER_BUFFER_STATE_FLUSHING;
}
