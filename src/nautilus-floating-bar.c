/* Nautilus - Floating status bar.
 *
 * Copyright (C) 2011 Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Cosimo Cecchi <cosimoc@redhat.com>
 *
 */

#include <config.h>

#include <string.h>

#include "nautilus-floating-bar.h"
#include "nautilus-gtk4-helpers.h"

#define HOVER_HIDE_TIMEOUT_INTERVAL 100

struct _NautilusFloatingBar {
    GtkBox parent;

    gchar *primary_label;
    gchar *details_label;

    GtkWidget *primary_label_widget;
    GtkWidget *details_label_widget;
    GtkWidget *spinner;
    gboolean show_spinner;
    GtkWidget *stop_button;
    gboolean show_stop;
    gboolean is_interactive;
    guint hover_timeout_id;

    GtkEventController *motion_controller;
    double pointer_y_in_parent_coordinates;
};

enum {
    PROP_PRIMARY_LABEL = 1,
    PROP_DETAILS_LABEL,
    PROP_SHOW_SPINNER,
    PROP_SHOW_STOP,
    NUM_PROPERTIES
};

enum { STOP, NUM_SIGNALS };

static GParamSpec *properties[NUM_PROPERTIES] = {
    NULL,
};
static guint signals[NUM_SIGNALS] = {
    0,
};

G_DEFINE_TYPE(NautilusFloatingBar, nautilus_floating_bar, GTK_TYPE_BOX);

static void stop_button_clicked_cb(GtkButton *button,
                                   NautilusFloatingBar *self) {
    g_signal_emit(self, signals[STOP], 0);
}

static void nautilus_floating_bar_finalize(GObject *obj) {
    NautilusFloatingBar *self = NAUTILUS_FLOATING_BAR(obj);

    nautilus_floating_bar_remove_hover_timeout(self);
    g_free(self->primary_label);
    g_free(self->details_label);
    g_clear_object(&self->motion_controller);

    G_OBJECT_CLASS(nautilus_floating_bar_parent_class)->finalize(obj);
}

static void nautilus_floating_bar_get_property(GObject *object,
        guint property_id, GValue *value,
        GParamSpec *pspec) {
    NautilusFloatingBar *self = NAUTILUS_FLOATING_BAR(object);

    switch (property_id) {
    case PROP_PRIMARY_LABEL: {
        g_value_set_string(value, self->primary_label);
    }
    break;

    case PROP_DETAILS_LABEL: {
        g_value_set_string(value, self->details_label);
    }
    break;

    case PROP_SHOW_SPINNER: {
        g_value_set_boolean(value, self->show_spinner);
    }
    break;

    case PROP_SHOW_STOP: {
        g_value_set_boolean(value, self->show_stop);
    }
    break;

    default: {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
    break;
    }
}

static void nautilus_floating_bar_set_property(GObject *object,
        guint property_id,
        const GValue *value,
        GParamSpec *pspec) {
    NautilusFloatingBar *self = NAUTILUS_FLOATING_BAR(object);

    switch (property_id) {
    case PROP_PRIMARY_LABEL: {
        nautilus_floating_bar_set_primary_label(self, g_value_get_string(value));
    }
    break;

    case PROP_DETAILS_LABEL: {
        nautilus_floating_bar_set_details_label(self, g_value_get_string(value));
    }
    break;

    case PROP_SHOW_SPINNER: {
        nautilus_floating_bar_set_show_spinner(self, g_value_get_boolean(value));
    }
    break;

    case PROP_SHOW_STOP: {
        nautilus_floating_bar_set_show_stop(self, g_value_get_boolean(value));
    }
    break;

    default: {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
    break;
    }
}

static void update_labels(NautilusFloatingBar *self) {
    gboolean primary_visible, details_visible;

    primary_visible =
        (self->primary_label != NULL) && (strlen(self->primary_label) > 0);
    details_visible =
        (self->details_label != NULL) && (strlen(self->details_label) > 0);

    gtk_label_set_text(GTK_LABEL(self->primary_label_widget),
                       self->primary_label);
    gtk_widget_set_visible(self->primary_label_widget, primary_visible);

    gtk_label_set_text(GTK_LABEL(self->details_label_widget),
                       self->details_label);
    gtk_widget_set_visible(self->details_label_widget, details_visible);
}

void nautilus_floating_bar_remove_hover_timeout(NautilusFloatingBar *self) {
    if (self->hover_timeout_id != 0) {
        g_source_remove(self->hover_timeout_id);
        self->hover_timeout_id = 0;
    }
}

typedef struct {
    NautilusFloatingBar *floating_bar;
    gint y_down_limit;
    gint y_upper_limit;
} CheckPointerData;

static void check_pointer_data_free(gpointer data) {
    g_slice_free(CheckPointerData, data);
}

static gboolean check_pointer_timeout(gpointer user_data) {
    CheckPointerData *data = user_data;
    NautilusFloatingBar *self = data->floating_bar;
    double pointer_y = self->pointer_y_in_parent_coordinates;

    if (pointer_y == -1 || pointer_y < data->y_down_limit ||
            pointer_y > data->y_upper_limit) {
        gtk_widget_show(GTK_WIDGET(self));
        self->hover_timeout_id = 0;

        return G_SOURCE_REMOVE;
    } else {
        gtk_widget_hide(GTK_WIDGET(self));
    }

    return G_SOURCE_CONTINUE;
}

static void
on_event_controller_motion_enter(GtkEventControllerMotion *controller, double x,
                                 double y, gpointer user_data) {
    NautilusFloatingBar *self = NAUTILUS_FLOATING_BAR(user_data);
    GtkWidget *parent;
    CheckPointerData *data;
    gint y_pos;

    self->pointer_y_in_parent_coordinates = y;

    if (self->is_interactive || !gtk_widget_is_visible(GTK_WIDGET(self))) {
        return;
    }

    parent = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    gtk_widget_translate_coordinates(GTK_WIDGET(self), parent, 0, 0, NULL,
                                     &y_pos);

    if (y < y_pos) {
        return;
    }

    if (self->hover_timeout_id != 0) {
        g_source_remove(self->hover_timeout_id);
    }

    data = g_slice_new(CheckPointerData);
    data->floating_bar = self;
    data->y_down_limit = y_pos;
    data->y_upper_limit =
        y_pos + gtk_widget_get_allocated_height(GTK_WIDGET(self));

    self->hover_timeout_id =
        g_timeout_add_full(G_PRIORITY_DEFAULT, HOVER_HIDE_TIMEOUT_INTERVAL,
                           check_pointer_timeout, data, check_pointer_data_free);

    g_source_set_name_by_id(
        self->hover_timeout_id,
        "[nautilus-floating-bar] on_event_controller_motion_enter");
}

static void
on_event_controller_motion_leave(GtkEventControllerMotion *controller,
                                 gpointer user_data) {
    NautilusFloatingBar *self = NAUTILUS_FLOATING_BAR(user_data);

    self->pointer_y_in_parent_coordinates = -1;
}

static void
on_event_controller_motion_motion(GtkEventControllerMotion *controller,
                                  double x, double y, gpointer user_data) {
    NautilusFloatingBar *self = NAUTILUS_FLOATING_BAR(user_data);

    self->pointer_y_in_parent_coordinates = y;
}

static void on_parent_changed(GObject *object, GParamSpec *pspec,
                              gpointer user_data) {
    NautilusFloatingBar *self = NAUTILUS_FLOATING_BAR(object);
    GtkWidget *parent;

    parent = gtk_widget_get_parent(GTK_WIDGET(object));

    g_clear_object(&self->motion_controller);

    if (parent != NULL) {
        self->motion_controller = gtk_event_controller_motion_new(parent);

        gtk_event_controller_set_propagation_phase(self->motion_controller,
                GTK_PHASE_CAPTURE);
        g_signal_connect(self->motion_controller, "enter",
                         G_CALLBACK(on_event_controller_motion_enter), self);
        g_signal_connect(self->motion_controller, "leave",
                         G_CALLBACK(on_event_controller_motion_leave), self);
        g_signal_connect(self->motion_controller, "motion",
                         G_CALLBACK(on_event_controller_motion_motion), self);
    }
}

static void get_padding_and_border(GtkWidget *widget, GtkBorder *border) {
    GtkStyleContext *context;
    GtkStateFlags state;
    GtkBorder tmp;

    context = gtk_widget_get_style_context(widget);
    state = gtk_style_context_get_state(context);

    gtk_style_context_get_padding(context, state, border);
    gtk_style_context_get_border(context, state, &tmp);
    border->top += tmp.top;
    border->right += tmp.right;
    border->bottom += tmp.bottom;
    border->left += tmp.left;
}

static void nautilus_floating_bar_get_preferred_width(GtkWidget *widget,
        gint *minimum_size,
        gint *natural_size) {
    GtkBorder border;

    get_padding_and_border(widget, &border);

    GTK_WIDGET_CLASS(nautilus_floating_bar_parent_class)
    ->get_preferred_width(widget, minimum_size, natural_size);

    *minimum_size += border.left + border.right;
    *natural_size += border.left + border.right;
}

static void nautilus_floating_bar_get_preferred_width_for_height(
    GtkWidget *widget, gint height, gint *minimum_size, gint *natural_size) {
    GtkBorder border;

    get_padding_and_border(widget, &border);

    GTK_WIDGET_CLASS(nautilus_floating_bar_parent_class)
    ->get_preferred_width_for_height(widget, height, minimum_size,
                                     natural_size);

    *minimum_size += border.left + border.right;
    *natural_size += border.left + border.right;
}

static void nautilus_floating_bar_get_preferred_height(GtkWidget *widget,
        gint *minimum_size,
        gint *natural_size) {
    GtkBorder border;

    get_padding_and_border(widget, &border);

    GTK_WIDGET_CLASS(nautilus_floating_bar_parent_class)
    ->get_preferred_height(widget, minimum_size, natural_size);

    *minimum_size += border.top + border.bottom;
    *natural_size += border.top + border.bottom;
}

static void nautilus_floating_bar_get_preferred_height_for_width(
    GtkWidget *widget, gint width, gint *minimum_size, gint *natural_size) {
    GtkBorder border;

    get_padding_and_border(widget, &border);

    GTK_WIDGET_CLASS(nautilus_floating_bar_parent_class)
    ->get_preferred_height_for_width(widget, width, minimum_size,
                                     natural_size);

    *minimum_size += border.top + border.bottom;
    *natural_size += border.top + border.bottom;
}

static void nautilus_floating_bar_constructed(GObject *obj) {
    NautilusFloatingBar *self = NAUTILUS_FLOATING_BAR(obj);
    GtkWidget *w, *box, *labels_box;
    GtkStyleContext *context;

    G_OBJECT_CLASS(nautilus_floating_bar_parent_class)->constructed(obj);

    box = GTK_WIDGET(obj);

    w = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(box), w, FALSE, FALSE, 0);
    gtk_widget_set_visible(w, self->show_spinner);
    /* As a workaround for https://gitlab.gnome.org/GNOME/gtk/-/issues/1025,
     * ensure the spinner animates if and only if it's visible, to reduce CPU
     * usage. */
    g_object_bind_property(obj, "show-spinner", w, "active",
                           G_BINDING_SYNC_CREATE);
    self->spinner = w;

    gtk_widget_set_size_request(w, 16, 16);
    gtk_widget_set_margin_start(w, 8);

    labels_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(box), labels_box, FALSE, TRUE, 0);
    g_object_set(labels_box, "hexpand", TRUE, "margin-top", 2, "margin-bottom", 2,
                 "margin-start", 12, "margin-end", 12, NULL);
    gtk_widget_show(labels_box);

    w = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(w), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_single_line_mode(GTK_LABEL(w), TRUE);
    gtk_box_append(GTK_BOX(labels_box), w);
    self->primary_label_widget = w;
    gtk_widget_show(w);

    w = gtk_label_new(NULL);
    gtk_label_set_single_line_mode(GTK_LABEL(w), TRUE);
    gtk_box_append(GTK_BOX(labels_box), w);
    self->details_label_widget = w;
    gtk_widget_show(w);

    w = gtk_button_new_from_icon_name("process-stop-symbolic",
                                      GTK_ICON_SIZE_MENU);
    context = gtk_widget_get_style_context(w);
    gtk_style_context_add_class(context, "circular");
    gtk_style_context_add_class(context, "flat");
    gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(self), w);
    self->stop_button = w;
    gtk_widget_set_visible(w, FALSE);
    gtk_widget_set_no_show_all(w, TRUE);

    g_signal_connect(self->stop_button, "clicked",
                     G_CALLBACK(stop_button_clicked_cb), self);
}

static void nautilus_floating_bar_init(NautilusFloatingBar *self) {
    GtkStyleContext *context;

    context = gtk_widget_get_style_context(GTK_WIDGET(self));
    gtk_style_context_add_class(context, "floating-bar");

    self->motion_controller = NULL;
    self->pointer_y_in_parent_coordinates = -1;

    g_signal_connect(self, "notify::parent", G_CALLBACK(on_parent_changed), NULL);
}

static void nautilus_floating_bar_class_init(NautilusFloatingBarClass *klass) {
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);

    oclass->constructed = nautilus_floating_bar_constructed;
    oclass->set_property = nautilus_floating_bar_set_property;
    oclass->get_property = nautilus_floating_bar_get_property;
    oclass->finalize = nautilus_floating_bar_finalize;

    wclass->get_preferred_width = nautilus_floating_bar_get_preferred_width;
    wclass->get_preferred_width_for_height =
        nautilus_floating_bar_get_preferred_width_for_height;
    wclass->get_preferred_height = nautilus_floating_bar_get_preferred_height;
    wclass->get_preferred_height_for_width =
        nautilus_floating_bar_get_preferred_height_for_width;

    properties[PROP_PRIMARY_LABEL] =
        g_param_spec_string("primary-label", "Bar's primary label",
                            "Primary label displayed by the bar", NULL,
                            G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);
    properties[PROP_DETAILS_LABEL] =
        g_param_spec_string("details-label", "Bar's details label",
                            "Details label displayed by the bar", NULL,
                            G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);
    properties[PROP_SHOW_SPINNER] = g_param_spec_boolean(
                                        "show-spinner", "Show spinner",
                                        "Whether a spinner should be shown in the floating bar", FALSE,
                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_SHOW_STOP] = g_param_spec_boolean(
                                     "show-stop", "Show stop button",
                                     "Whether a stop button should be shown in the floating bar", FALSE,
                                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    signals[STOP] =
        g_signal_new("stop", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                     NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

    g_object_class_install_properties(oclass, NUM_PROPERTIES, properties);
}

void nautilus_floating_bar_set_primary_label(NautilusFloatingBar *self,
        const gchar *label) {
    if (g_strcmp0(self->primary_label, label) != 0) {
        g_free(self->primary_label);
        self->primary_label = g_strdup(label);

        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PRIMARY_LABEL]);

        update_labels(self);
    }
}

void nautilus_floating_bar_set_details_label(NautilusFloatingBar *self,
        const gchar *label) {
    if (g_strcmp0(self->details_label, label) != 0) {
        g_free(self->details_label);
        self->details_label = g_strdup(label);

        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_DETAILS_LABEL]);

        update_labels(self);
    }
}

void nautilus_floating_bar_set_labels(NautilusFloatingBar *self,
                                      const gchar *primary_label,
                                      const gchar *details_label) {
    nautilus_floating_bar_set_primary_label(self, primary_label);
    nautilus_floating_bar_set_details_label(self, details_label);
}

void nautilus_floating_bar_set_show_spinner(NautilusFloatingBar *self,
        gboolean show_spinner) {
    if (self->show_spinner != show_spinner) {
        self->show_spinner = show_spinner;
        gtk_widget_set_visible(self->spinner, show_spinner);

        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SHOW_SPINNER]);
    }
}

void nautilus_floating_bar_set_show_stop(NautilusFloatingBar *self,
        gboolean show_stop) {
    if (self->show_stop != show_stop) {
        self->show_stop = show_stop;
        gtk_widget_set_visible(self->stop_button, show_stop);
        self->is_interactive = show_stop;

        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SHOW_STOP]);
    }
}

GtkWidget *nautilus_floating_bar_new(const gchar *primary_label,
                                     const gchar *details_label,
                                     gboolean show_spinner) {
    return g_object_new(NAUTILUS_TYPE_FLOATING_BAR, "primary-label",
                        primary_label, "details-label", details_label,
                        "show-spinner", show_spinner, "orientation",
                        GTK_ORIENTATION_HORIZONTAL, "spacing", 8, NULL);
}
