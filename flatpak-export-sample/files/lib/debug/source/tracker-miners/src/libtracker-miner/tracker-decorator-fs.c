/*
 * Copyright (C) 2014 Carlos Garnacho  <carlosg@gnome.org>
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

#include <glib.h>

#include <libtracker-miners-common/tracker-common.h>
#include <libtracker-sparql/tracker-sparql.h>

#include "tracker-decorator-private.h"
#include "tracker-decorator-fs.h"
#include "tracker-miner-fs.h"

/**
 * SECTION:tracker-decorator-fs
 * @short_description: Filesystem implementation for TrackerDecorator
 * @include: libtracker-miner/tracker-miner.h
 * @title: TrackerDecoratorFS
 * @see_also: #TrackerDecorator
 *
 * #TrackerDecoratorFS is used to handle extended metadata extraction
 * for resources on file systems that are mounted or unmounted.
 **/

typedef struct _TrackerDecoratorFSPrivate TrackerDecoratorFSPrivate;

struct _TrackerDecoratorFSPrivate {
	GVolumeMonitor *volume_monitor;
};

static GInitableIface *parent_initable_iface;

static void tracker_decorator_fs_initable_iface_init (GInitableIface *iface);

G_DEFINE_ABSTRACT_TYPE_WITH_CODE (TrackerDecoratorFS, tracker_decorator_fs,
                                  TRACKER_TYPE_DECORATOR,
                                  G_ADD_PRIVATE (TrackerDecoratorFS)
                                  G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, tracker_decorator_fs_initable_iface_init))

static void
tracker_decorator_fs_finalize (GObject *object)
{
	TrackerDecoratorFSPrivate *priv;

	priv = TRACKER_DECORATOR_FS (object)->priv;

	if (priv->volume_monitor)
		g_object_unref (priv->volume_monitor);

	G_OBJECT_CLASS (tracker_decorator_fs_parent_class)->finalize (object);
}

static void
tracker_decorator_fs_class_init (TrackerDecoratorFSClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_decorator_fs_finalize;
}

static void
mount_points_changed_cb (GVolumeMonitor *monitor,
                         GMount         *mount,
                         gpointer        user_data)
{
	GDrive *drive = g_mount_get_drive (mount);

	if (drive) {
		if (g_drive_is_media_removable (drive))
			_tracker_decorator_invalidate_cache (user_data);
		g_object_unref (drive);
	}
}

static gboolean
tracker_decorator_fs_iface_init (GInitable     *initable,
                                 GCancellable  *cancellable,
                                 GError       **error)
{
	TrackerDecoratorFSPrivate *priv;

	priv = TRACKER_DECORATOR_FS (initable)->priv;

	priv->volume_monitor = g_volume_monitor_get ();
	g_signal_connect_object (priv->volume_monitor, "mount-added",
	                         G_CALLBACK (mount_points_changed_cb), initable, 0);
	g_signal_connect_object (priv->volume_monitor, "mount-pre-unmount",
	                         G_CALLBACK (mount_points_changed_cb), initable, 0);
	g_signal_connect_object (priv->volume_monitor, "mount-removed",
	                         G_CALLBACK (mount_points_changed_cb), initable, 0);

	return parent_initable_iface->init (initable, cancellable, error);
}

static void
tracker_decorator_fs_initable_iface_init (GInitableIface *iface)
{
	parent_initable_iface = g_type_interface_peek_parent (iface);
	iface->init = tracker_decorator_fs_iface_init;
}

static void
tracker_decorator_fs_init (TrackerDecoratorFS *decorator)
{
	decorator->priv = tracker_decorator_fs_get_instance_private (decorator);
}
