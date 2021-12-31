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

#include "config-miners.h"

#include "tracker-lru.h"

typedef struct _TrackerLRUElement TrackerLRUElement;

struct _TrackerLRUElement {
	gpointer element;
	gpointer data;
	GList *link;
};

struct _TrackerLRU {
	GQueue queue;
	GHashTable *items;
	GDestroyNotify elem_destroy;
	GDestroyNotify data_destroy;
	guint max_size;
	gint ref_count;
};

static void
free_node (TrackerLRUElement *node,
           TrackerLRU        *lru)
{
	g_hash_table_remove (lru->items, node->element);
	lru->elem_destroy (node->element);
	lru->data_destroy (node->data);
	g_slice_free (TrackerLRUElement, node);
}

TrackerLRU *
tracker_lru_new (guint                size,
                 GHashFunc            elem_hash_func,
                 GEqualFunc           elem_equal_func,
                 GDestroyNotify       elem_destroy,
                 GDestroyNotify       data_destroy)
{
	TrackerLRU *lru;

	lru = g_new0 (TrackerLRU, 1);
	g_queue_init (&lru->queue);
	lru->max_size = size;
	lru->elem_destroy = elem_destroy;
	lru->data_destroy = data_destroy;
	lru->items = g_hash_table_new (elem_hash_func,
	                               elem_equal_func);
	lru->ref_count = 1;

	return lru;
}

TrackerLRU *
tracker_lru_ref (TrackerLRU *lru)
{
	g_atomic_int_inc (&lru->ref_count);
	return lru;
}

void
tracker_lru_unref (TrackerLRU *lru)
{
	if (g_atomic_int_dec_and_test (&lru->ref_count)) {
		TrackerLRUElement *node;
		GHashTableIter iter;

		g_hash_table_iter_init (&iter, lru->items);

		while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &node)) {
			g_hash_table_iter_remove (&iter);
			free_node (node, lru);
		}

		g_hash_table_unref (lru->items);
		g_queue_clear (&lru->queue);
		g_free (lru);
	}
}

gboolean
tracker_lru_find (TrackerLRU *lru,
                  gpointer    elem,
                  gpointer   *data)
{
	TrackerLRUElement *node;

	node = g_hash_table_lookup (lru->items, elem);
	if (!node)
		return FALSE;

	if (data)
		*data = node->data;

	if (node->link != lru->queue.head) {
		/* Push to head */
		g_queue_unlink (&lru->queue, node->link);
		g_queue_push_head_link (&lru->queue, node->link);
	}

	return TRUE;
}

void
tracker_lru_add (TrackerLRU *lru,
                 gpointer    elem,
                 gpointer    data)
{
	TrackerLRUElement *node, *last;

	node = g_slice_new0 (TrackerLRUElement);
	node->element = elem;
	node->data = data;
	node->link = g_list_alloc ();
	node->link->data = node;

	g_queue_push_head_link (&lru->queue, node->link);

	g_hash_table_insert (lru->items, elem, node);

	if (g_hash_table_size (lru->items) > lru->max_size) {
		/* Remove last element */
		last = g_queue_pop_tail (&lru->queue);
		free_node (last, lru);
	}
}

void
tracker_lru_remove (TrackerLRU *lru,
                    gpointer    elem)
{
	TrackerLRUElement *node;

	node = g_hash_table_lookup (lru->items, elem);
	if (!node)
		return;

	g_queue_remove (&lru->queue, node);
	free_node (node, lru);
}

void
tracker_lru_remove_foreach (TrackerLRU *lru,
                            GEqualFunc  equal_func,
                            gpointer    elem)
{
	TrackerLRUElement *node;
	GList *next, *link = lru->queue.head;

	while (link) {
		node = link->data;
		next = link->next;

		if (equal_func (node->element, elem) == TRUE) {
			g_queue_unlink (&lru->queue, node->link);
			free_node (node, lru);
		}

		link = next;
	}
}
