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

#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>

#if defined (__OpenBSD__) || defined (__FreeBSD__) || defined (__NetBSD__) || defined (__APPLE__)
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#define TRACKER_MONITOR_KQUEUE
#endif

#include "tracker-monitor.h"

#include "libtracker-miners-common/tracker-debug.h"

typedef struct TrackerMonitorPrivate  TrackerMonitorPrivate;

struct TrackerMonitorPrivate {
	GHashTable    *monitored_dirs;

	gboolean       enabled;

	guint          monitor_limit;
	gboolean       monitor_limit_warned;
	guint          monitors_ignored;

	/* For FAM, the _CHANGES_DONE event is not signalled, so we
	 * have to just use the _CHANGED event instead.
	 */
	gboolean       use_changed_event;

	TrackerIndexingTree *tree;

	struct {
		GMainContext *owner_context;
		GMainContext *monitor_context;
		GMainLoop *monitor_thread_loop;
		GThread *monitor_thread;
		GHashTable *cached_events;
		GHashTable *monitors;
		GMutex mutex;
		GCond cond;
		gint n_requests;
	} thread;
};

typedef struct {
	GFile    *file;
	gchar    *file_uri;
	GFile    *other_file;
	gchar    *other_file_uri;
	gboolean  is_directory;
	guint32   event_type;
	gboolean  expirable;
} EventData;

typedef struct {
	TrackerMonitor *monitor;
	GFile *file;
	GFile *other_file;
	GSource *source;
	gboolean is_directory;
	GFileMonitorEvent event_type;
} MonitorEvent;

typedef enum {
	MONITOR_REQUEST_ADD,
	MONITOR_REQUEST_REMOVE,
} MonitorRequestType;

typedef struct {
	TrackerMonitor *monitor;
	MonitorRequestType type;
	GList *files;
} MonitorRequest;

enum {
	ITEM_CREATED,
	ITEM_UPDATED,
	ITEM_ATTRIBUTE_UPDATED,
	ITEM_DELETED,
	ITEM_MOVED,
	LAST_SIGNAL
};

enum {
	PROP_0,
	PROP_ENABLED
};

static void           tracker_monitor_finalize     (GObject        *object);
static void           tracker_monitor_set_property (GObject        *object,
                                                    guint           prop_id,
                                                    const GValue   *value,
                                                    GParamSpec     *pspec);
static void           tracker_monitor_get_property (GObject        *object,
                                                    guint           prop_id,
                                                    GValue         *value,
                                                    GParamSpec     *pspec);
static guint          get_kqueue_limit             (void);
static guint          get_inotify_limit            (void);
static GFileMonitor * directory_monitor_new        (TrackerMonitor *monitor,
                                                    GFile          *file);
static void           directory_monitor_cancel     (GFileMonitor     *dir_monitor);


static gboolean       monitor_cancel_recursively   (TrackerMonitor *monitor,
                                                    GFile          *file);

static guint signals[LAST_SIGNAL] = { 0, };

static void tracker_monitor_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (TrackerMonitor, tracker_monitor, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                tracker_monitor_initable_iface_init)
                         G_ADD_PRIVATE (TrackerMonitor))

static gpointer
monitor_thread_func (gpointer user_data)
{
	TrackerMonitor *monitor = user_data;
	TrackerMonitorPrivate *priv;

	priv = tracker_monitor_get_instance_private (monitor);
	g_main_context_push_thread_default (priv->thread.monitor_context);
	g_main_loop_run (priv->thread.monitor_thread_loop);
	g_main_context_pop_thread_default (priv->thread.monitor_context);
	g_main_loop_unref (priv->thread.monitor_thread_loop);

	return NULL;
}

static gboolean
tracker_monitor_initable_init (GInitable     *initable,
                               GCancellable  *cancellable,
                               GError       **error)
{
	GType monitor_backend;
	const gchar *name;
	GError *inner_error = NULL;
	TrackerMonitorPrivate *priv;
	GFile *file;
	GFileMonitor *monitor;

	priv = tracker_monitor_get_instance_private (TRACKER_MONITOR (initable));

	/* For the first monitor we get the type and find out if we
	 * are using inotify, FAM, polling, etc.
	 */
	file = g_file_new_for_path (g_get_home_dir ());
	monitor = g_file_monitor_directory (file,
	                                    G_FILE_MONITOR_WATCH_MOVES,
	                                    NULL,
	                                    &inner_error);
	if (inner_error) {
		g_propagate_error (error, inner_error);
		return FALSE;
	}

	monitor_backend = G_OBJECT_TYPE (monitor);

	/* We use the name because the type itself is actually
	 * private and not available publically. Note this is
	 * subject to change, but unlikely of course.
	 */
	name = g_type_name (monitor_backend);

	/* Set limits based on backend... */
	if (strcmp (name, "GInotifyDirectoryMonitor") == 0 ||
	    strcmp (name, "GInotifyFileMonitor") == 0) {
		/* Using inotify */
		TRACKER_NOTE (MONITORS, g_message ("Monitor backend is Inotify"));

		/* Setting limit based on kernel
		 * settings in /proc...
		 */
		priv->monitor_limit = get_inotify_limit ();

		/* We don't use 100% of the monitors, we allow other
		 * applications to have at least 500 or so to use
		 * between them selves. This only
		 * applies to inotify because it is a
		 * user shared resource.
		 */
		priv->monitor_limit -= 500;

		/* Make sure we don't end up with a
		 * negative maximum.
		 */
		priv->monitor_limit = MAX (priv->monitor_limit, 0);
	} else if (strcmp (name, "GKqueueDirectoryMonitor") == 0 ||
	           strcmp (name, "GKqueueFileMonitor") == 0) {
		/* Using kqueue(2) */
		TRACKER_NOTE (MONITORS, g_message ("Monitor backend is kqueue"));

		priv->monitor_limit = get_kqueue_limit ();
	} else if (strcmp (name, "GFamDirectoryMonitor") == 0) {
		/* Using Fam */
		TRACKER_NOTE (MONITORS, g_message ("Monitor backend is Fam"));

		/* Setting limit to an arbitary limit
		 * based on testing
		 */
		priv->monitor_limit = 400;
		priv->use_changed_event = TRUE;
	} else if (strcmp (name, "GWin32DirectoryMonitor") == 0) {
		/* Using Windows */
		TRACKER_NOTE (MONITORS, g_message ("Monitor backend is Windows"));

		/* Guessing limit... */
		priv->monitor_limit = 8192;
	} else {
		/* Unknown */
		g_warning ("Monitor backend:'%s' is unhandled. Monitoring will be disabled",
		           name);
		priv->enabled = FALSE;
	}

	if (priv->enabled)
		TRACKER_NOTE (MONITORS, g_message ("Monitor limit is %d", priv->monitor_limit));

	g_file_monitor_cancel (monitor);
	g_object_unref (monitor);
	g_object_unref (file);

	priv->thread.owner_context = g_main_context_ref_thread_default ();
	priv->thread.monitor_context = g_main_context_new ();
	priv->thread.monitor_thread_loop = g_main_loop_new (priv->thread.monitor_context, FALSE);

	priv->thread.monitor_thread =
		g_thread_try_new ("Monitor thread",
		                  monitor_thread_func,
		                  initable,
		                  &inner_error);
	if (inner_error) {
		g_propagate_error (error, inner_error);
		return FALSE;
	}

	return TRUE;
}

static void
tracker_monitor_initable_iface_init (GInitableIface *iface)
{
	iface->init = tracker_monitor_initable_init;
}

static void
tracker_monitor_class_init (TrackerMonitorClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_monitor_finalize;
	object_class->set_property = tracker_monitor_set_property;
	object_class->get_property = tracker_monitor_get_property;

	signals[ITEM_CREATED] =
		g_signal_new ("item-created",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              2,
		              G_TYPE_OBJECT,
		              G_TYPE_BOOLEAN);
	signals[ITEM_UPDATED] =
		g_signal_new ("item-updated",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              2,
		              G_TYPE_OBJECT,
		              G_TYPE_BOOLEAN);
	signals[ITEM_ATTRIBUTE_UPDATED] =
		g_signal_new ("item-attribute-updated",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              2,
		              G_TYPE_OBJECT,
		              G_TYPE_BOOLEAN);
	signals[ITEM_DELETED] =
		g_signal_new ("item-deleted",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              2,
		              G_TYPE_OBJECT,
		              G_TYPE_BOOLEAN);
	signals[ITEM_MOVED] =
		g_signal_new ("item-moved",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              4,
		              G_TYPE_OBJECT,
		              G_TYPE_OBJECT,
		              G_TYPE_BOOLEAN,
		              G_TYPE_BOOLEAN);

	g_object_class_install_property (object_class,
	                                 PROP_ENABLED,
	                                 g_param_spec_boolean ("enabled",
	                                                       "Enabled",
	                                                       "Enabled",
	                                                       TRUE,
	                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
}

static MonitorEvent *
monitor_event_new (TrackerMonitor    *monitor,
                   GFile             *file,
                   GFile             *other_file,
                   GFileMonitorEvent  event_type,
                   gboolean           is_directory)
{
	MonitorEvent *event;

	event = g_new0 (MonitorEvent, 1);
	event->monitor = g_object_ref (monitor);
	event->file = g_object_ref (file);
	g_set_object (&event->other_file, other_file);
	event->event_type = event_type;
	event->is_directory = is_directory;

	return event;
}

static void
monitor_event_free (MonitorEvent *event)
{
	g_object_unref (event->monitor);
	g_object_unref (event->file);
	g_clear_object (&event->other_file);
	g_clear_pointer (&event->source, g_source_destroy);
	g_free (event);
}

static void
tracker_monitor_init (TrackerMonitor *object)
{
	TrackerMonitorPrivate *priv;

	priv = tracker_monitor_get_instance_private (object);

	/* By default we enable monitoring */
	priv->enabled = TRUE;

	/* Create monitors table for this module */
	priv->monitored_dirs =
		g_hash_table_new_full (g_file_hash,
		                       (GEqualFunc) g_file_equal,
		                       (GDestroyNotify) g_object_unref,
		                       NULL);

	priv->thread.cached_events =
		g_hash_table_new_full (g_file_hash,
		                       (GEqualFunc) g_file_equal,
		                       g_object_unref,
		                       (GDestroyNotify) monitor_event_free);

	priv->thread.monitors =
		g_hash_table_new_full (g_file_hash,
		                       (GEqualFunc) g_file_equal,
		                       (GDestroyNotify) g_object_unref,
		                       (GDestroyNotify) directory_monitor_cancel);

	g_mutex_init (&priv->thread.mutex);
	g_cond_init (&priv->thread.cond);
}

static gboolean
quit_thread (TrackerMonitor *monitor)
{
	TrackerMonitorPrivate *priv;

	priv = tracker_monitor_get_instance_private (monitor);
	g_main_loop_quit (priv->thread.monitor_thread_loop);

	return G_SOURCE_REMOVE;
}

static void
tracker_monitor_finalize (GObject *object)
{
	TrackerMonitorPrivate *priv;

	priv = tracker_monitor_get_instance_private (TRACKER_MONITOR (object));

	if (priv->thread.monitor_thread_loop) {
		g_main_context_invoke_full (priv->thread.monitor_context,
		                            G_PRIORITY_HIGH,
		                            (GSourceFunc) quit_thread,
		                            object, NULL);
	}

	if (priv->thread.monitor_thread)
		g_thread_join (priv->thread.monitor_thread);

	g_clear_pointer (&priv->thread.monitor_context, g_main_context_unref);
	g_clear_pointer (&priv->thread.owner_context, g_main_context_unref);
	g_clear_pointer (&priv->thread.cached_events, g_hash_table_unref);
	g_clear_pointer (&priv->thread.monitors, g_hash_table_unref);

	g_hash_table_unref (priv->monitored_dirs);

	G_OBJECT_CLASS (tracker_monitor_parent_class)->finalize (object);
}

static void
tracker_monitor_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
	switch (prop_id) {
	case PROP_ENABLED:
		tracker_monitor_set_enabled (TRACKER_MONITOR (object),
		                             g_value_get_boolean (value));
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}
}

static void
tracker_monitor_get_property (GObject      *object,
                              guint         prop_id,
                              GValue       *value,
                              GParamSpec   *pspec)
{
	TrackerMonitorPrivate *priv;

	priv = tracker_monitor_get_instance_private (TRACKER_MONITOR (object));

	switch (prop_id) {
	case PROP_ENABLED:
		g_value_set_boolean (value, priv->enabled);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}
}

static guint
get_kqueue_limit (void)
{
	guint limit = 400;

#ifdef TRACKER_MONITOR_KQUEUE
	struct rlimit rl;
	if (getrlimit (RLIMIT_NOFILE, &rl) == 0) {
		rl.rlim_cur = rl.rlim_max;
	} else {
		return limit;
	}

	if (setrlimit(RLIMIT_NOFILE, &rl) == 0)
		limit = (rl.rlim_cur * 90) / 100;
#endif /* TRACKER_MONITOR_KQUEUE */

	return limit;
}

static guint
get_inotify_limit (void)
{
	GError      *error = NULL;
	const gchar *filename;
	gchar       *contents = NULL;
	guint        limit;

	filename = "/proc/sys/fs/inotify/max_user_watches";

	if (!g_file_get_contents (filename,
	                          &contents,
	                          NULL,
	                          &error)) {
		g_warning ("Couldn't get INotify monitor limit from:'%s', %s",
		           filename,
		           error ? error->message : "no error given");
		g_clear_error (&error);

		/* Setting limit to an arbitary limit */
		limit = 8192;
	} else {
		limit = atoi (contents);
		g_free (contents);
	}

	return limit;
}

static gboolean
check_is_directory (TrackerMonitor *monitor,
                    GFile          *file)
{
	GFileType file_type;

	file_type = g_file_query_file_type (file, G_FILE_QUERY_INFO_NONE, NULL);

	if (file_type == G_FILE_TYPE_DIRECTORY)
		return TRUE;

	if (file_type == G_FILE_TYPE_UNKNOWN) {
		TrackerMonitorPrivate *priv;

		priv = tracker_monitor_get_instance_private (monitor);

		/* Whatever it was, it's gone. Check the monitors
		 * hashtable to know whether it was a directory
		 * we knew about
		 */
		if (g_hash_table_lookup (priv->thread.monitors, file) != NULL)
			return TRUE;
	}

	return FALSE;
}

/* Executed in monitor thread */
static gboolean
monitor_request_execute (MonitorRequest *request)
{
	TrackerMonitorPrivate *priv;

	priv = tracker_monitor_get_instance_private (request->monitor);

	g_mutex_lock (&priv->thread.mutex);

	while (request->files) {
		GFile *file = request->files->data;

		if (request->type == MONITOR_REQUEST_ADD) {
			GFileMonitor *monitor;

			monitor = directory_monitor_new (request->monitor,
			                                 file);
			if (monitor) {
				g_hash_table_insert (priv->thread.monitors,
				                     g_object_ref (file),
				                     monitor);
			}
		} else if (request->type == MONITOR_REQUEST_REMOVE) {
			g_hash_table_remove (priv->thread.monitors,
			                     file);
		} else {
			g_assert_not_reached ();
		}

		request->files = g_list_remove (request->files, file);
		g_object_unref (file);
	}

	if (g_atomic_int_dec_and_test (&priv->thread.n_requests))
		g_cond_signal (&priv->thread.cond);

	g_mutex_unlock (&priv->thread.mutex);

	return G_SOURCE_REMOVE;
}

/* Executed in main thread */
static void
monitor_request_queue (TrackerMonitor *monitor,
                       MonitorRequest *request)
{
	TrackerMonitorPrivate *priv;

	priv = tracker_monitor_get_instance_private (request->monitor);

	g_atomic_int_inc (&priv->thread.n_requests);
	g_main_context_invoke_full (priv->thread.monitor_context,
	                            G_PRIORITY_DEFAULT,
	                            (GSourceFunc) monitor_request_execute,
	                            request, g_free);
}

static void
block_for_requests (TrackerMonitor *monitor)
{
	TrackerMonitorPrivate *priv;

	priv = tracker_monitor_get_instance_private (monitor);

	g_mutex_lock (&priv->thread.mutex);

	while (g_atomic_int_get (&priv->thread.n_requests) != 0)
		g_cond_wait (&priv->thread.cond, &priv->thread.mutex);

	g_mutex_unlock (&priv->thread.mutex);
}

gboolean
tracker_monitor_move (TrackerMonitor *monitor,
                      GFile          *old_file,
                      GFile          *new_file)
{
	TrackerMonitorPrivate *priv;
	GHashTableIter iter;
	MonitorRequest *request;
	gchar *old_prefix;
	gpointer iter_file;
	guint items_moved = 0;

	priv = tracker_monitor_get_instance_private (monitor);

	/* So this is tricky. What we have to do is:
	 *
	 * 1) Add all monitors for the new_file directory hierarchy
	 * 2) Then remove the monitors for old_file
	 *
	 * This order is necessary because inotify can reuse watch
	 * descriptors, and libinotify will remove handles
	 * asynchronously on IN_IGNORE, so the opposite sequence
	 * may possibly remove valid, just added, monitors.
	 */
	request = g_new0 (MonitorRequest, 1);
	request->monitor = monitor;
	request->type = MONITOR_REQUEST_ADD;

	old_prefix = g_file_get_path (old_file);

	/* Find out which subdirectories should have a file monitor added */
	g_hash_table_iter_init (&iter, priv->monitored_dirs);
	while (g_hash_table_iter_next (&iter, &iter_file, NULL)) {
		GFile *f;
		gchar *old_path, *new_path;
		gchar *new_prefix;
		gchar *p;

		if (!g_file_has_prefix (iter_file, old_file) &&
		    !g_file_equal (iter_file, old_file)) {
			continue;
		}

		old_path = g_file_get_path (iter_file);
		p = strstr (old_path, old_prefix);

		if (!p || strcmp (p, old_prefix) == 0) {
			g_free (old_path);
			continue;
		}

		/* Move to end of prefix */
		p += strlen (old_prefix) + 1;

		/* Check this is not the end of the string */
		if (*p == '\0') {
			g_free (old_path);
			continue;
		}

		new_prefix = g_file_get_path (new_file);
		new_path = g_build_path (G_DIR_SEPARATOR_S, new_prefix, p, NULL);
		g_free (new_prefix);

		f = g_file_new_for_path (new_path);
		g_free (new_path);

		request->files = g_list_prepend (request->files, g_object_ref (f));

		g_object_unref (f);
		g_free (old_path);
		items_moved++;
	}

	/* Add a new monitor for the top level directory */
	tracker_monitor_add (monitor, new_file);

	/* Add new monitors for all subdirectories */
	monitor_request_queue (monitor, request);

	/* Remove the monitor for the old top level directory hierarchy */
	tracker_monitor_remove_recursively (monitor, old_file);

	g_free (old_prefix);

	block_for_requests (monitor);

	return items_moved > 0;
}

static const gchar *
monitor_event_to_string (GFileMonitorEvent event_type)
{
	switch (event_type) {
	case G_FILE_MONITOR_EVENT_CHANGED:
		return "G_FILE_MONITOR_EVENT_CHANGED";
	case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
		return "G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT";
	case G_FILE_MONITOR_EVENT_DELETED:
		return "G_FILE_MONITOR_EVENT_DELETED";
	case G_FILE_MONITOR_EVENT_CREATED:
		return "G_FILE_MONITOR_EVENT_CREATED";
	case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
		return "G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED";
	case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
		return "G_FILE_MONITOR_EVENT_PRE_UNMOUNT";
	case G_FILE_MONITOR_EVENT_UNMOUNTED:
		return "G_FILE_MONITOR_EVENT_UNMOUNTED";
	case G_FILE_MONITOR_EVENT_MOVED:
		return "G_FILE_MONITOR_EVENT_MOVED";
	case G_FILE_MONITOR_EVENT_RENAMED:
		return "G_FILE_MONITOR_EVENT_RENAMED";
	case G_FILE_MONITOR_EVENT_MOVED_IN:
		return "G_FILE_MONITOR_EVENT_MOVED_IN";
	case G_FILE_MONITOR_EVENT_MOVED_OUT:
		return "G_FILE_MONITOR_EVENT_MOVED_OUT";
		break;
	}

	return "unknown";
}

/* Executed in main thread */
static gboolean
emit_signal_for_event (MonitorEvent *event)
{
	TrackerMonitor *monitor = event->monitor;
	gboolean is_directory = event->is_directory;
	GFile *file = event->file;
	GFile *other_file = event->other_file;

	switch (event->event_type) {
	case G_FILE_MONITOR_EVENT_CREATED:
		g_signal_emit (monitor,
		               signals[ITEM_CREATED], 0,
		               file, is_directory);
		break;
	case G_FILE_MONITOR_EVENT_CHANGED:
		g_signal_emit (monitor,
		               signals[ITEM_UPDATED], 0,
		               file, is_directory);
		break;
	case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
		g_signal_emit (monitor,
		               signals[ITEM_ATTRIBUTE_UPDATED], 0,
		               file, is_directory);
		break;
	case G_FILE_MONITOR_EVENT_DELETED:
		g_signal_emit (monitor,
		               signals[ITEM_DELETED], 0,
		               file, is_directory);
		break;
	case G_FILE_MONITOR_EVENT_MOVED:
		g_signal_emit (monitor,
		               signals[ITEM_MOVED], 0,
		               file, other_file, is_directory, TRUE);
		break;
	default:
		g_warning ("Trying to emit monitor signal with unhandled event %d",
		           event->event_type);
		break;
	}

	return G_SOURCE_REMOVE;
}

/* Executed in monitor thread */
static void
queue_signal_for_event (TrackerMonitor    *monitor,
                        GFileMonitorEvent  type,
                        gboolean           is_directory,
                        GFile             *file,
                        GFile             *other_file)
{
	TrackerMonitorPrivate *priv;
	MonitorEvent *event;

	priv = tracker_monitor_get_instance_private (monitor);

	event = monitor_event_new (monitor, file, other_file,
	                           type, is_directory);

	g_main_context_invoke_full (priv->thread.owner_context,
	                            G_PRIORITY_HIGH,
	                            (GSourceFunc) emit_signal_for_event,
	                            event,
	                            (GDestroyNotify) monitor_event_free);
}

/* Executed in monitor thread */
static void
flush_cached_event (TrackerMonitor *monitor,
                    GFile          *file)
{
	TrackerMonitorPrivate *priv;
	MonitorEvent *event;

	priv = tracker_monitor_get_instance_private (monitor);

	event = g_hash_table_lookup (priv->thread.cached_events, file);

	if (event) {
		queue_signal_for_event (monitor, event->event_type,
		                        event->is_directory, event->file, NULL);
		g_hash_table_remove (priv->thread.cached_events, file);
	}
}

/* Executed in monitor thread */
static void
cache_event (TrackerMonitor    *monitor,
             GFile             *file,
             GFileMonitorEvent  event_type,
             gboolean           is_directory)
{
	TrackerMonitorPrivate *priv;
	MonitorEvent *event;

	priv = tracker_monitor_get_instance_private (monitor);
	event = g_hash_table_lookup (priv->thread.cached_events, file);

	if (!event) {
		event = monitor_event_new (monitor, file, NULL,
		                           event_type, is_directory);
		g_hash_table_insert (priv->thread.cached_events,
		                     g_object_ref (file),
		                     event);
	}
}

static gboolean
flush_event_idle_cb (gpointer user_data)
{
       MonitorEvent *event = user_data;
       TrackerMonitorPrivate *priv = tracker_monitor_get_instance_private (event->monitor);

       queue_signal_for_event (event->monitor, event->event_type,
                               event->is_directory, event->file, NULL);
       g_hash_table_remove (priv->thread.cached_events, event->file);

       return G_SOURCE_REMOVE;
}

static void
flush_event_later (TrackerMonitor *monitor,
                   GFile          *file)
{
       TrackerMonitorPrivate *priv = tracker_monitor_get_instance_private (monitor);
       MonitorEvent *event;

       event = g_hash_table_lookup (priv->thread.cached_events, file);
       if (!event)
               return;

       event->source = g_idle_source_new ();
       g_source_set_callback (event->source, flush_event_idle_cb, event, NULL);
       g_source_attach (event->source,
                        priv->thread.monitor_context);
}

/* Executed in monitor thread */
static void
monitor_event_cb (GFileMonitor      *file_monitor,
                  GFile             *file,
                  GFile             *other_file,
                  GFileMonitorEvent  event_type,
                  gpointer           user_data)
{
	TrackerMonitor *monitor;
	gchar *file_uri;
	gchar *other_file_uri;
	gboolean is_directory = FALSE;
	TrackerMonitorPrivate *priv;
	MonitorEvent *prev_event;

	monitor = user_data;
	priv = tracker_monitor_get_instance_private (monitor);
	prev_event = g_hash_table_lookup (priv->thread.cached_events, file);

	if (G_UNLIKELY (!priv->enabled)) {
		TRACKER_NOTE (MONITORS, g_message ("Silently dropping monitor event, monitor disabled for now"));
		return;
	}

	/* Get URIs as paths may not be in UTF-8 */
	file_uri = g_file_get_uri (file);

	if (!other_file) {
		is_directory = check_is_directory (monitor, file);

		other_file_uri = NULL;
		TRACKER_NOTE (MONITORS,
		              g_message ("Received monitor event:%d (%s) for %s:'%s'",
		                         event_type,
		                         monitor_event_to_string (event_type),
		                         is_directory ? "directory" : "file",
		                         file_uri));

		if (is_directory &&
		    event_type == G_FILE_MONITOR_EVENT_DELETED) {
			GFileMonitor *dir_monitor;

			dir_monitor = g_hash_table_lookup (priv->thread.monitors, file);

			/* We may get 2 DELETED events on directories, one from the
			 * directory monitor for the directory itself, and again from
			 * the parent folder.
			 *
			 * If the parent event is handled first, we cancel the monitor
			 * so the second event does not get to us. However if the
			 * order is inverted, just cancelling the directory monitor
			 * for the deleted directory will not stop the parent directory
			 * event. We must check explicitly for that case.
			 */
			if (dir_monitor &&
			    dir_monitor != file_monitor &&
			    g_file_monitor_is_cancelled (dir_monitor)) {
				g_free (file_uri);
				return;
			}
		}
	} else {
		if (event_type == G_FILE_MONITOR_EVENT_RENAMED ||
		    event_type == G_FILE_MONITOR_EVENT_MOVED_OUT) {
			is_directory = check_is_directory (monitor, other_file);
		} else if (event_type == G_FILE_MONITOR_EVENT_MOVED_IN) {
			is_directory = check_is_directory (monitor, file);
		}

		other_file_uri = g_file_get_uri (other_file);
		TRACKER_NOTE (MONITORS,
		              g_message ("Received monitor event:%d (%s) for files '%s'->'%s'",
		                         event_type,
		                         monitor_event_to_string (event_type),
		                         file_uri,
		                         other_file_uri));

		if (is_directory &&
		    (event_type == G_FILE_MONITOR_EVENT_RENAMED ||
		     event_type == G_FILE_MONITOR_EVENT_MOVED_OUT) &&
		    prev_event &&
		    prev_event->event_type == G_FILE_MONITOR_EVENT_DELETED) {
			/* If a directory is moved, there is also an EVENT_DELETED
			 * coming from the GFileMonitor on the folder itself (as the
			 * folder being monitored no longer exists). We may receive
			 * this event before this one, we should ensure it's cleared
			 * out.
			 */
			g_hash_table_remove (priv->thread.cached_events, file);
		}
	}

	/* Note that in any case we should be moving the monitors
	 * here to the new place, as the new place may be ignored.
	 * We should leave this to the upper layers. But one thing
	 * we must do is actually CANCEL all these monitors. */
	if (is_directory &&
	    (event_type == G_FILE_MONITOR_EVENT_RENAMED ||
	     event_type == G_FILE_MONITOR_EVENT_MOVED_IN ||
	     event_type == G_FILE_MONITOR_EVENT_DELETED)) {
		monitor_cancel_recursively (monitor, file);
	}

	switch (event_type) {
	case G_FILE_MONITOR_EVENT_CREATED:
	case G_FILE_MONITOR_EVENT_CHANGED:
		if (!priv->use_changed_event) {
			cache_event (monitor, file, event_type, is_directory);
		} else {
			queue_signal_for_event (monitor, event_type,
			                        is_directory, file, NULL);
		}
		break;
	case G_FILE_MONITOR_EVENT_DELETED:
		if (prev_event &&
		    prev_event->event_type == G_FILE_MONITOR_EVENT_CREATED) {
			/* Consume both the cached CREATED event and this one */
			g_hash_table_remove (priv->thread.cached_events, file);
			break;
		}

		/* In any case, cached events are stale */
		g_hash_table_remove (priv->thread.cached_events, file);

		cache_event (monitor, file, event_type, is_directory);
		flush_event_later (monitor, file);
		break;
	case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
		queue_signal_for_event (monitor, event_type,
		                        is_directory, file, NULL);
		break;
	case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
		flush_cached_event (monitor, file);
		break;
	case G_FILE_MONITOR_EVENT_MOVED_IN:
		if (other_file) {
			/* Both MOVED_IN and MOVE_OUT are fine points to emit
			 * ::item-moved when source/dest are known. We choose
			 * to emit it here, and ignore the MOVE_OUT.
			 */
			queue_signal_for_event (monitor,
			                        G_FILE_MONITOR_EVENT_MOVED,
			                        is_directory,
			                        other_file, file);
		} else {
			/* No known origin, treat as a new file */
			queue_signal_for_event (monitor,
			                        G_FILE_MONITOR_EVENT_CREATED,
			                        is_directory,
			                        file, NULL);
		}
		break;
	case G_FILE_MONITOR_EVENT_MOVED_OUT:
		if (!other_file) {
			/* No known destination. Treat as remove */
			queue_signal_for_event (monitor,
			                        G_FILE_MONITOR_EVENT_DELETED,
			                        is_directory,
			                        file, NULL);
		}
		break;
	case G_FILE_MONITOR_EVENT_RENAMED:
		queue_signal_for_event (monitor,
		                        G_FILE_MONITOR_EVENT_MOVED,
		                        is_directory, file, other_file);
		break;
	case G_FILE_MONITOR_EVENT_MOVED:
		g_warn_if_reached ();
		break;
	case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
	case G_FILE_MONITOR_EVENT_UNMOUNTED:
		break;
	}

	g_free (file_uri);
	g_free (other_file_uri);
}

static GFileMonitor *
directory_monitor_new (TrackerMonitor *monitor,
                       GFile          *file)
{
	GFileMonitor *file_monitor;
	GError *error = NULL;

	file_monitor = g_file_monitor_directory (file,
	                                         G_FILE_MONITOR_WATCH_MOVES,
	                                         NULL,
	                                         &error);

	if (error) {
		gchar *uri;

		uri = g_file_get_uri (file);
		g_warning ("Could not add monitor for path:'%s', %s",
		           uri, error->message);

		g_error_free (error);
		g_free (uri);

		return NULL;
	}

	g_signal_connect (file_monitor, "changed",
	                  G_CALLBACK (monitor_event_cb),
	                  monitor);

	return file_monitor;
}

static void
directory_monitor_cancel (GFileMonitor *monitor)
{
	if (monitor) {
		g_file_monitor_cancel (G_FILE_MONITOR (monitor));
		g_object_unref (monitor);
	}
}

TrackerMonitor *
tracker_monitor_new (void)
{
	return g_object_new (TRACKER_TYPE_MONITOR, NULL);
}

gboolean
tracker_monitor_get_enabled (TrackerMonitor *monitor)
{
	TrackerMonitorPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), FALSE);

	priv = tracker_monitor_get_instance_private (monitor);

	return priv->enabled;
}

void
tracker_monitor_set_enabled (TrackerMonitor *monitor,
                             gboolean        enabled)
{
	TrackerMonitorPrivate *priv;
	MonitorRequest *request;

	g_return_if_fail (TRACKER_IS_MONITOR (monitor));

	priv = tracker_monitor_get_instance_private (monitor);

	/* Don't replace all monitors if we are already
	 * enabled/disabled.
	 */
	if (priv->enabled == enabled) {
		return;
	}

	priv->enabled = enabled;
	g_object_notify (G_OBJECT (monitor), "enabled");

	request = g_new0 (MonitorRequest, 1);
	request->monitor = monitor;
	request->files = g_hash_table_get_keys (priv->monitored_dirs);
	g_list_foreach (request->files, (GFunc) g_object_ref, NULL);
	request->type = enabled ? MONITOR_REQUEST_ADD : MONITOR_REQUEST_REMOVE;

	monitor_request_queue (monitor, request);
	block_for_requests (monitor);
}

gboolean
tracker_monitor_add (TrackerMonitor *monitor,
                     GFile          *file)
{
	TrackerMonitorPrivate *priv;
	gchar *uri;

	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	priv = tracker_monitor_get_instance_private (monitor);

	if (g_hash_table_lookup (priv->monitored_dirs, file)) {
		return TRUE;
	}

	/* Cap the number of monitors */
	if (g_hash_table_size (priv->monitored_dirs) >= priv->monitor_limit) {
		priv->monitors_ignored++;

		if (!priv->monitor_limit_warned) {
			g_warning ("The maximum number of monitors to set (%d) "
			           "has been reached, not adding any new ones",
			           priv->monitor_limit);
			priv->monitor_limit_warned = TRUE;
		}

		return FALSE;
	}

	uri = g_file_get_uri (file);

	if (priv->enabled) {
		/* We don't check if a file exists or not since we might want
		 * to monitor locations which don't exist yet.
		 *
		 * Also, we assume ALL paths passed are directories.
		 */
		MonitorRequest *request;

		request = g_new0 (MonitorRequest, 1);
		request->monitor = monitor;
		request->files = g_list_prepend (NULL, g_object_ref (file));
		request->type = MONITOR_REQUEST_ADD;

		monitor_request_queue (monitor, request);
		block_for_requests (monitor);
	}

	g_hash_table_add (priv->monitored_dirs, g_object_ref (file));

	TRACKER_NOTE (MONITORS, g_message ("Added monitor for path:'%s', total monitors:%d",
	                                   uri,
	                                   g_hash_table_size (priv->monitored_dirs)));

	g_free (uri);

	return TRUE;
}

gboolean
tracker_monitor_remove (TrackerMonitor *monitor,
                        GFile          *file)
{
	TrackerMonitorPrivate *priv;
	gboolean removed;

	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	priv = tracker_monitor_get_instance_private (monitor);
	removed = g_hash_table_remove (priv->monitored_dirs, file);

	if (removed) {
		MonitorRequest *request;
		gchar *uri;

		request = g_new0 (MonitorRequest, 1);
		request->monitor = monitor;
		request->files = g_list_prepend (NULL, g_object_ref (file));
		request->type = MONITOR_REQUEST_REMOVE;

		monitor_request_queue (monitor, request);
		block_for_requests (monitor);

		uri = g_file_get_uri (file);
		TRACKER_NOTE (MONITORS, g_message ("Removed monitor for path:'%s', total monitors:%d",
		                                   uri,
		                                   g_hash_table_size (priv->monitored_dirs)));

		g_free (uri);
	}

	return removed;
}

/* If @is_strict is %TRUE, return %TRUE iff @file is a child of @prefix.
 * If @is_strict is %FALSE, additionally return %TRUE if @file equals @prefix.
 */
static gboolean
file_has_maybe_strict_prefix (GFile    *file,
                              GFile    *prefix,
                              gboolean  is_strict)
{
	return (g_file_has_prefix (file, prefix) ||
	        (!is_strict && g_file_equal (file, prefix)));
}

static gboolean
remove_recursively (TrackerMonitor *monitor,
                    GFile          *file,
                    gboolean        remove_top_level)
{
	TrackerMonitorPrivate *priv;
	GHashTableIter iter;
	MonitorRequest *request;
	gpointer iter_file;
	guint items_removed = 0;
	gchar *uri;

	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	priv = tracker_monitor_get_instance_private (monitor);

	request = g_new0 (MonitorRequest, 1);
	request->monitor = monitor;
	request->type = MONITOR_REQUEST_REMOVE;

	g_hash_table_iter_init (&iter, priv->monitored_dirs);
	while (g_hash_table_iter_next (&iter, &iter_file, NULL)) {
		if (!file_has_maybe_strict_prefix (iter_file, file,
		                                   !remove_top_level)) {
			continue;
		}

		request->files = g_list_prepend (request->files, g_object_ref (file));
		g_hash_table_iter_remove (&iter);
		items_removed++;
	}

	uri = g_file_get_uri (file);
	TRACKER_NOTE (MONITORS,
	              g_message ("Removed all monitors %srecursively for path:'%s', )"
	                         "total monitors:%d",
	                         !remove_top_level ? "(except top level) " : "",
	                         uri, g_hash_table_size (priv->monitored_dirs)));
	g_free (uri);

	monitor_request_queue (monitor, request);
	block_for_requests (monitor);

	if (items_removed > 0) {
		/* We reset this because now it is possible we have limit - 1 */
		priv->monitor_limit_warned = FALSE;
		return TRUE;
	}

	return FALSE;
}

gboolean
tracker_monitor_remove_recursively (TrackerMonitor *monitor,
                                    GFile          *file)
{
	return remove_recursively (monitor, file, TRUE);
}

gboolean
tracker_monitor_remove_children_recursively (TrackerMonitor *monitor,
                                             GFile          *file)
{
	return remove_recursively (monitor, file, FALSE);
}

/* Runs in the monitor thread */
static gboolean
monitor_cancel_recursively (TrackerMonitor *monitor,
                            GFile          *file)
{
	TrackerMonitorPrivate *priv;
	GHashTableIter iter;
	gpointer iter_file, iter_file_monitor;
	guint items_cancelled = 0;

	priv = tracker_monitor_get_instance_private (monitor);

	g_hash_table_iter_init (&iter, priv->thread.monitors);
	while (g_hash_table_iter_next (&iter, &iter_file, &iter_file_monitor)) {
		gchar *uri;

		if (!g_file_has_prefix (iter_file, file) &&
		    !g_file_equal (iter_file, file)) {
			continue;
		}

		uri = g_file_get_uri (iter_file);
		g_file_monitor_cancel (G_FILE_MONITOR (iter_file_monitor));
		TRACKER_NOTE (MONITORS, g_message ("Cancelled monitor for path:'%s'", uri));
		g_free (uri);

		items_cancelled++;
	}

	return items_cancelled > 0;
}

gboolean
tracker_monitor_is_watched (TrackerMonitor *monitor,
                            GFile          *file)
{
	TrackerMonitorPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	priv = tracker_monitor_get_instance_private (monitor);

	if (!priv->enabled)
		return FALSE;

	return g_hash_table_contains (priv->monitored_dirs, file);
}

guint
tracker_monitor_get_count (TrackerMonitor *monitor)
{
	TrackerMonitorPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), 0);

	priv = tracker_monitor_get_instance_private (monitor);

	return g_hash_table_size (priv->monitored_dirs);
}

guint
tracker_monitor_get_ignored (TrackerMonitor *monitor)
{
	TrackerMonitorPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), 0);

	priv = tracker_monitor_get_instance_private (monitor);

	return priv->monitors_ignored;
}

guint
tracker_monitor_get_limit (TrackerMonitor *monitor)
{
	TrackerMonitorPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), 0);

	priv = tracker_monitor_get_instance_private (monitor);

	return priv->monitor_limit;
}
