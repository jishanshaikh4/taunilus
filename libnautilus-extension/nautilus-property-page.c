/*
 *  nautilus-property-page.h - Property pages exported by
 *                             NautilusPropertyProvider objects.
 *
 *  Copyright (C) 2003 Novell, Inc.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 *  Author:  Dave Camp <dave@ximian.com>
 *
 */

#include <config.h>
#include "nautilus-property-page.h"

#include <glib/gi18n-lib.h>

enum
{
    PROP_0,
    PROP_NAME,
    PROP_LABEL,
    PROP_PAGE,
    LAST_PROP
};

struct _NautilusPropertyPage
{
    GObject parent_instance;

    char *name;
    GtkWidget *label;
    GtkWidget *page;
};

G_DEFINE_TYPE (NautilusPropertyPage, nautilus_property_page, G_TYPE_OBJECT)

NautilusPropertyPage *
nautilus_property_page_new (const char *name,
                            GtkWidget  *label,
                            GtkWidget  *page_widget)
{
    NautilusPropertyPage *page;

    g_return_val_if_fail (name != NULL, NULL);
    g_return_val_if_fail (GTK_IS_WIDGET (label), NULL);
    g_return_val_if_fail (GTK_IS_WIDGET (page_widget), NULL);

    page = g_object_new (NAUTILUS_TYPE_PROPERTY_PAGE,
                         "name", name,
                         "label", label,
                         "page", page_widget,
                         NULL);

    return page;
}

static void
nautilus_property_page_get_property (GObject    *object,
                                     guint       param_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
    NautilusPropertyPage *page;

    page = NAUTILUS_PROPERTY_PAGE (object);

    switch (param_id)
    {
        case PROP_NAME:
        {
            g_value_set_string (value, page->name);
        }
        break;

        case PROP_LABEL:
        {
            g_value_set_object (value, page->label);
        }
        break;

        case PROP_PAGE:
        {
            g_value_set_object (value, page->page);
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
        }
        break;
    }
}

static void
nautilus_property_page_set_property (GObject      *object,
                                     guint         param_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
    NautilusPropertyPage *page;

    page = NAUTILUS_PROPERTY_PAGE (object);

    switch (param_id)
    {
        case PROP_NAME:
        {
            g_free (page->name);
            page->name = g_strdup (g_value_get_string (value));
            g_object_notify (object, "name");
        }
        break;

        case PROP_LABEL:
        {
            if (page->label)
            {
                g_object_unref (page->label);
            }

            page->label = g_object_ref (g_value_get_object (value));
            g_object_notify (object, "label");
        }
        break;

        case PROP_PAGE:
        {
            if (page->page)
            {
                g_object_unref (page->page);
            }

            page->page = g_object_ref (g_value_get_object (value));
            g_object_notify (object, "page");
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
        }
        break;
    }
}

static void
nautilus_property_page_dispose (GObject *object)
{
    NautilusPropertyPage *page;

    page = NAUTILUS_PROPERTY_PAGE (object);

    if (page->label)
    {
        g_object_unref (page->label);
        page->label = NULL;
    }
    if (page->page)
    {
        g_object_unref (page->page);
        page->page = NULL;
    }
}

static void
nautilus_property_page_finalize (GObject *object)
{
    NautilusPropertyPage *page;

    page = NAUTILUS_PROPERTY_PAGE (object);

    g_free (page->name);

    G_OBJECT_CLASS (nautilus_property_page_parent_class)->finalize (object);
}

static void
nautilus_property_page_init (NautilusPropertyPage *page)
{
}

static void
nautilus_property_page_class_init (NautilusPropertyPageClass *class)
{
    G_OBJECT_CLASS (class)->finalize = nautilus_property_page_finalize;
    G_OBJECT_CLASS (class)->dispose = nautilus_property_page_dispose;
    G_OBJECT_CLASS (class)->get_property = nautilus_property_page_get_property;
    G_OBJECT_CLASS (class)->set_property = nautilus_property_page_set_property;

    g_object_class_install_property (G_OBJECT_CLASS (class),
                                     PROP_NAME,
                                     g_param_spec_string ("name",
                                                          "Name",
                                                          "Name of the page",
                                                          NULL,
                                                          G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_READABLE));
    g_object_class_install_property (G_OBJECT_CLASS (class),
                                     PROP_LABEL,
                                     g_param_spec_object ("label",
                                                          "Label",
                                                          "Label widget to display in the notebook tab",
                                                          GTK_TYPE_WIDGET,
                                                          G_PARAM_READWRITE));
    g_object_class_install_property (G_OBJECT_CLASS (class),
                                     PROP_PAGE,
                                     g_param_spec_object ("page",
                                                          "Page",
                                                          "Widget for the property page",
                                                          GTK_TYPE_WIDGET,
                                                          G_PARAM_READWRITE));
}
