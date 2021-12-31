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

#ifndef __LIBTRACKER_MINER_CRAWLER_H__
#define __LIBTRACKER_MINER_CRAWLER_H__

#if !defined (__LIBTRACKER_MINER_H_INSIDE__) && !defined (TRACKER_COMPILATION)
#error "Only <libtracker-miner/tracker-miner.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "tracker-data-provider.h"

G_BEGIN_DECLS

#define TRACKER_TYPE_CRAWLER            (tracker_crawler_get_type ())
#define TRACKER_CRAWLER(object)         (G_TYPE_CHECK_INSTANCE_CAST ((object), TRACKER_TYPE_CRAWLER, TrackerCrawler))
#define TRACKER_CRAWLER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), TRACKER_TYPE_CRAWLER, TrackerCrawlerClass))
#define TRACKER_IS_CRAWLER(object)      (G_TYPE_CHECK_INSTANCE_TYPE ((object), TRACKER_TYPE_CRAWLER))
#define TRACKER_IS_CRAWLER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), TRACKER_TYPE_CRAWLER))
#define TRACKER_CRAWLER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), TRACKER_TYPE_CRAWLER, TrackerCrawlerClass))

/* Max timeouts time (in msec) */
#define TRACKER_CRAWLER_MAX_TIMEOUT_INTERVAL 1000

typedef struct TrackerCrawler         TrackerCrawler;
typedef struct TrackerCrawlerClass    TrackerCrawlerClass;
typedef struct TrackerCrawlerPrivate  TrackerCrawlerPrivate;

typedef enum {
	TRACKER_CRAWLER_CHECK_FILE      = 1 << 0,
	TRACKER_CRAWLER_CHECK_DIRECTORY = 1 << 1,
	TRACKER_CRAWLER_CHECK_CONTENT   = 1 << 2,
} TrackerCrawlerCheckFlags;

typedef gboolean (*TrackerCrawlerCheckFunc) (TrackerCrawler           *crawler,
                                             TrackerCrawlerCheckFlags  flags,
                                             GFile                    *file,
                                             GFileInfo                *file_info,
                                             const GList              *children,
                                             gpointer                  user_data);

struct TrackerCrawler {
	GObject parent;
};

struct TrackerCrawlerClass {
	GObjectClass parent;
};

GType           tracker_crawler_get_type     (void);
TrackerCrawler *tracker_crawler_new          (TrackerDataProvider *data_provider);

gboolean tracker_crawler_get_finish (TrackerCrawler  *crawler,
                                     GAsyncResult    *result,
                                     GFile          **directory,
                                     GNode          **tree,
                                     guint           *directories_found,
                                     guint           *directories_ignored,
                                     guint           *files_found,
                                     guint           *files_ignored,
                                     GError         **error);
void tracker_crawler_get (TrackerCrawler        *crawler,
                          GFile                 *file,
                          TrackerDirectoryFlags  flags,
                          GCancellable          *cancellable,
                          GAsyncReadyCallback    callback,
                          gpointer               user_data);

void            tracker_crawler_set_file_attributes (TrackerCrawler *crawler,
						     const gchar    *file_attributes);
const gchar *   tracker_crawler_get_file_attributes (TrackerCrawler *crawler);

GFileInfo *     tracker_crawler_get_file_info       (TrackerCrawler *crawler,
						     GFile          *file);

void            tracker_crawler_set_check_func (TrackerCrawler          *crawler,
                                                TrackerCrawlerCheckFunc  func,
                                                gpointer                 user_data,
                                                GDestroyNotify           destroy_notify);

G_END_DECLS

#endif /* __LIBTRACKER_MINER_CRAWLER_H__ */
