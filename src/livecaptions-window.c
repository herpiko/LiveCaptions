/* livecaptions-window.c
 *
 * Copyright 2022 abb128
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include "livecaptions-config.h"
#include "livecaptions-window.h"
#include "livecaptions-application.h"
#include "april_api.h"
#include "audiocap.h"
#include "window-helper.h"
#include "history.h"
#include "spectrum.h"


G_DEFINE_TYPE(LiveCaptionsWindow, livecaptions_window, GTK_TYPE_APPLICATION_WINDOW)

static void livecaptions_window_dispose(GObject *object);

static void livecaptions_window_class_init (LiveCaptionsWindowClass *klass) {
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    G_OBJECT_CLASS(klass)->dispose = livecaptions_window_dispose;

    gtk_widget_class_set_template_from_resource(widget_class, "/net/sapples/LiveCaptions/livecaptions-window.ui");
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, main);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, side_box);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, side_box_tiny);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, mic_button);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, listen_button);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, visualizer);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, label);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, too_slow_warning);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, slow_warning);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, slowest_warning);
}

static void change_button_layout(LiveCaptionsWindow *self, gint text_height){
    int button_height = 29;

    if(text_height > (2 * button_height)) {
        gtk_widget_set_visible(GTK_WIDGET(self->side_box), true);
        gtk_widget_set_visible(GTK_WIDGET(self->side_box_tiny), false);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->side_box_tiny), true);
        gtk_widget_set_visible(GTK_WIDGET(self->side_box), false);
    }
}

const char LINE_WIDTH_TEXT_TEMPLATE[] = "This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.";
static void update_line_width(LiveCaptionsWindow *self){
    if(self->font_layout != NULL) g_object_unref(self->font_layout);

    int preferred_width = g_settings_get_int(self->settings, "line-width");
    size_t text_len = sizeof(LINE_WIDTH_TEXT_TEMPLATE);
    if(preferred_width < text_len) text_len = preferred_width;

    int width, height;
    PangoLayout *layout = gtk_label_get_layout(self->label);
    layout = pango_layout_copy(layout);

    pango_layout_set_width(layout, -1);
    pango_layout_set_text(layout, LINE_WIDTH_TEXT_TEMPLATE, text_len);
    pango_layout_get_size(layout, &width, &height);

    height = (height / PANGO_SCALE) * 2 + 2;
    width  = (width / PANGO_SCALE);
    change_button_layout(self, height);

    gtk_widget_set_size_request(GTK_WIDGET(self->label), width, height);

    self->max_text_width = width;
    self->font_layout = layout;
    self->font_layout_counter++;
}

static void update_font(LiveCaptionsWindow *self) {
    PangoFontDescription *desc = pango_font_description_from_string(g_settings_get_string(self->settings, "font-name"));

    PangoAttribute *attr_font = pango_attr_font_desc_new(desc);

    PangoAttrList *attr = gtk_label_get_attributes(self->label);
    if(attr == NULL){
        attr = pango_attr_list_new();
    }
    pango_attr_list_change(attr, attr_font);

    gtk_label_set_attributes(self->label, attr);

    pango_font_description_free(desc);

    update_line_width(self);
}

static void update_window_transparency(LiveCaptionsWindow *self) {
    bool use_transparency = g_settings_get_double(self->settings, "window-transparency") > 0.01;

    if(use_transparency){
        gtk_widget_add_css_class(GTK_WIDGET(self), "transparent-mode");

        int transparency = (int)((1.0 - g_settings_get_double(self->settings, "window-transparency")) * 255.0);

        char css_data[256];
        snprintf(css_data, 256, ".transparent-mode {\nbackground-color: #000000%02X;\n}", transparency);
        gtk_css_provider_load_from_data(self->css_provider, css_data, -1);
    }else{
        gtk_widget_remove_css_class(GTK_WIDGET(self), "transparent-mode");
    }
}

static void update_keep_above(LiveCaptionsWindow *self) {
    gboolean keep_above = g_settings_get_boolean(self->settings, "keep-on-top");
    if(!set_window_keep_above(GTK_WINDOW(self), keep_above)){
        printf("Failed to set keep above\n");
    }
}

static void on_settings_change(G_GNUC_UNUSED GSettings *settings,
                               char      *key,
                               gpointer   user_data){

    LiveCaptionsWindow *self = user_data;
    if(g_str_equal(key, "font-name")) {
        update_font(self);
    }else if(g_str_equal(key, "line-width")) {
        update_line_width(self);
    }else if(g_str_equal(key, "transparent-window") || g_str_equal(key, "window-transparency")) {
        update_window_transparency(self);
    }else if(g_str_equal(key, "keep-on-top")) {
        update_keep_above(self);
    }
}


#define WINDOW_BOTTOM_MARGIN 50

// Start out centred near the bottom of the screen, like subtitles
static gboolean place_window(gpointer userdata) {
    LiveCaptionsWindow *self = userdata;
    move_window_to_bottom_center(GTK_WINDOW(self), WINDOW_BOTTOM_MARGIN);
    return G_SOURCE_REMOVE;
}

static void on_first_map(GtkWidget *widget, gpointer userdata) {
    LiveCaptionsWindow *self = userdata;
    g_signal_handlers_disconnect_by_func(widget, on_first_map, userdata);
    // Wait for the first layout so the window's size is known
    g_timeout_add(150, place_window, self);
}

static gboolean deferred_update_keep_above(void *userdata) {
    LiveCaptionsWindow *self = userdata;

    update_keep_above(self);

    return G_SOURCE_REMOVE;
}

static gboolean show_relevant_slow_warning(void *userdata) {
    LiveCaptionsWindow *self = userdata;

    GtkApplication *curr_app = gtk_window_get_application(GTK_WINDOW(self));
    LiveCaptionsApplication *app = LIVECAPTIONS_APPLICATION(curr_app);

    asr_thread asr = app->asr;

    if(asr_thread_is_errored(asr)){
        gtk_label_set_text(self->label, "[Model Error]");
        self->was_errored = true;
        return G_SOURCE_CONTINUE;
    }else if(self->was_errored) {
        self->was_errored = false;
        gtk_label_set_text(self->label, "");
    }

    // Problems worth telling the user about, e.g. a rejected OpenAI key
    g_autofree char *status = asr_thread_get_status(asr);
    if(status != NULL) {
        gtk_label_set_text(self->label, status);
        self->was_errored = true;
        return G_SOURCE_CONTINUE;
    }

    if(!asr_thread_is_loaded(asr)) return G_SOURCE_CONTINUE;

    float speedup = asr_thread_get_speedup(asr);

    if(speedup <= 1.1) {
        gtk_widget_set_visible(GTK_WIDGET(self->slow_warning), false);
        gtk_widget_set_visible(GTK_WIDGET(self->slowest_warning), false);
        gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), false);
    }else if(speedup <= 1.666) {
        gtk_widget_set_visible(GTK_WIDGET(self->slow_warning), true);
        gtk_widget_set_visible(GTK_WIDGET(self->slowest_warning), false);
        gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), false);
    }else if(speedup <= 2.33){
        gtk_widget_set_visible(GTK_WIDGET(self->slow_warning), false);
        gtk_widget_set_visible(GTK_WIDGET(self->slowest_warning), true);
        gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), false);
    }else{
        gtk_widget_set_visible(GTK_WIDGET(self->slow_warning), false);
        gtk_widget_set_visible(GTK_WIDGET(self->slowest_warning), false);
        gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), true);
    }

    return G_SOURCE_CONTINUE;
}

G_STATIC_ASSERT(SPECTRUM_BANDS == G_N_ELEMENTS(((LiveCaptionsWindow *)0)->visualizer_levels));

static void draw_visualizer(G_GNUC_UNUSED GtkDrawingArea *area, cairo_t *cr,
                            int width, int height, gpointer user_data) {
    LiveCaptionsWindow *self = user_data;

    const double gap = 2.0;
    double bar_w = (width - gap * (SPECTRUM_BANDS - 1)) / SPECTRUM_BANDS;
    if(bar_w < 1.0) bar_w = 1.0;

    for(int b = 0; b < SPECTRUM_BANDS; b++) {
        double v = self->visualizer_levels[b];
        double h = 1.0 + v * (height - 1.0);
        double x = b * (bar_w + gap);

        // Dim when quiet, brighter and greener as the band gets louder
        cairo_set_source_rgba(cr, 0.55 - 0.4 * v, 0.75 + 0.2 * v, 0.6 - 0.2 * v, 0.25 + 0.6 * v);
        cairo_rectangle(cr, x, height - h, bar_w, h);
        cairo_fill(cr);
    }
}

static gboolean tick_visualizer(gpointer user_data) {
    LiveCaptionsWindow *self = user_data;

    float levels[SPECTRUM_BANDS];
    spectrum_get(levels);

    gboolean all_zero = TRUE;
    for(int b = 0; b < SPECTRUM_BANDS; b++) {
        // Rise immediately, fall smoothly
        float decayed = self->visualizer_levels[b] * 0.82f;
        float v = levels[b] > decayed ? levels[b] : decayed;
        if(v < 0.005f) v = 0.0f;
        else all_zero = FALSE;
        self->visualizer_levels[b] = v;
    }

    // During silence, draw the flat state once and then stop redrawing
    if(all_zero && self->visualizer_idle) return G_SOURCE_CONTINUE;
    self->visualizer_idle = all_zero;

    gtk_widget_queue_draw(GTK_WIDGET(self->visualizer));
    return G_SOURCE_CONTINUE;
}

void livecaptions_window_set_listening(LiveCaptionsWindow *self, gboolean listening) {
    gtk_button_set_icon_name(GTK_BUTTON(self->listen_button),
        listening ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->listen_button),
        listening ? "Pause listening (stops audio capture to save power)"
                  : "Listening paused. Click to resume captioning");

    spectrum_reset();
    memset(self->visualizer_levels, 0, sizeof(self->visualizer_levels));
    self->visualizer_idle = FALSE;
    gtk_widget_set_visible(GTK_WIDGET(self->visualizer), listening);

    if(listening && self->visualizer_timer == 0) {
        self->visualizer_timer = g_timeout_add(33, tick_visualizer, self);
    } else if(!listening && self->visualizer_timer != 0) {
        g_source_remove(self->visualizer_timer);
        self->visualizer_timer = 0;
    }
}

static void livecaptions_window_dispose(GObject *object) {
    LiveCaptionsWindow *self = LIVECAPTIONS_WINDOW(object);
    if(self->visualizer_timer != 0) {
        g_source_remove(self->visualizer_timer);
        self->visualizer_timer = 0;
    }
    G_OBJECT_CLASS(livecaptions_window_parent_class)->dispose(object);
}

static void livecaptions_window_init(LiveCaptionsWindow *self) {
    gtk_widget_init_template(GTK_WIDGET(self));

    self->settings = g_settings_new("net.sapples.LiveCaptions");

    self->css_provider = gtk_css_provider_new();
    GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(self));
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(self->css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g_signal_connect(self->settings, "changed", G_CALLBACK(on_settings_change), self);
    
    g_settings_bind(self->settings, "microphone", self->mic_button, "active", G_SETTINGS_BIND_DEFAULT);

    self->visualizer_timer = 0;
    gtk_drawing_area_set_draw_func(self->visualizer, draw_visualizer, self, NULL);

    self->font_layout = NULL;
    self->font_layout_counter = 0;

    update_font(self);
    update_window_transparency(self);

    self->slow_warning_shown = false;
    self->was_errored = false;

    g_idle_add(deferred_update_keep_above, self);
    g_signal_connect(self, "map", G_CALLBACK(on_first_map), self);
    g_timeout_add_seconds(5, show_relevant_slow_warning, self);

    // GTK adds solid-csd class when the compositor does not support window shadows
    // This adds a very ugly thick border, so we remove it
    gtk_widget_remove_css_class(GTK_WIDGET(self), "solid-csd");
}

static gboolean hide_slow_warning_after_some_time(void *userdata) {
    LiveCaptionsWindow *self = userdata;

    time_t current_time = time(NULL);

    if(difftime(current_time, self->slow_time) > 4.0) {
        self->slow_warning_shown = false;
        gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), false);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

void livecaptions_window_warn_slow(LiveCaptionsWindow *self) {
    self->slow_time = time(NULL);
    if(self->slow_warning_shown) return;

    gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), true);
    self->slow_warning_shown = true;

    g_timeout_add_seconds(1, hide_slow_warning_after_some_time, self);
}
