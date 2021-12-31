/*
 * Copyright (C) 2020, Red Hat Inc.
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
 * Author: Carlos Garnacho  <carlosg@gnome.org>
 */
#ifndef __TRACKER_LRU_H__
#define __TRACKER_LRU_H__

#include <glib.h>

typedef struct _TrackerLRU TrackerLRU;

TrackerLRU * tracker_lru_new (guint                size,
                              GHashFunc            elem_hash_func,
                              GEqualFunc           elem_equal_func,
                              GDestroyNotify       elem_destroy,
                              GDestroyNotify       data_destroy);

TrackerLRU *tracker_lru_ref (TrackerLRU *lru);
void tracker_lru_unref (TrackerLRU *lru);

gboolean tracker_lru_find (TrackerLRU *lru,
                           gpointer    elem,
                           gpointer   *data);

void tracker_lru_add (TrackerLRU *lru,
                      gpointer    elem,
                      gpointer    data);
void tracker_lru_remove (TrackerLRU *lru,
                         gpointer    elem);

void tracker_lru_remove_foreach (TrackerLRU *lru,
                                 GEqualFunc  compare_func,
                                 gpointer    elem);

#endif /* __TRACKER_LRU_H__ */
