#include "config.h"

#include <stdbool.h>
#include <string.h>

#include <gtk/gtk.h>

typedef struct {
    GtkWidget *window;
    GtkSpinButton *pre_arm_spin;
    GtkSpinButton *animation_spin;
    GtkSpinButton *size_spin;
    GtkSpinButton *line_width_spin;
    GtkColorButton *color_button;
    GtkDrawingArea *preview;
    GtkLabel *status_label;
    TouchRMBConfig applied_config;
} SettingsUi;

static gboolean preview_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    SettingsUi *ui = user_data;
    GtkAllocation allocation;
    GdkRGBA color;
    int size;
    int line_width;
    double x;
    double y;

    gtk_widget_get_allocation(widget, &allocation);
    size = gtk_spin_button_get_value_as_int(ui->size_spin);
    line_width = gtk_spin_button_get_value_as_int(ui->line_width_spin);
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(ui->color_button), &color);

    x = ((double)allocation.width - (double)size) / 2.0;
    y = ((double)allocation.height - (double)size) / 2.0;

    cairo_set_source_rgb(cr, 0.10, 0.10, 0.10);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, color.red, color.green, color.blue);
    cairo_set_line_width(cr, (double)line_width);
    cairo_rectangle(cr, x + (double)line_width / 2.0, y + (double)line_width / 2.0, (double)size - (double)line_width, (double)size - (double)line_width);
    cairo_stroke(cr);
    return FALSE;
}

static void queue_preview(SettingsUi *ui) {
    gtk_widget_queue_draw(GTK_WIDGET(ui->preview));
}

static void on_control_changed(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    queue_preview(user_data);
}

static void set_status(SettingsUi *ui, const char *message) {
    gtk_label_set_text(ui->status_label, message);
}

static void load_to_widgets(SettingsUi *ui, const TouchRMBConfig *config) {
    GdkRGBA color;

    gtk_spin_button_set_value(ui->pre_arm_spin, config->pre_arm_ms);
    gtk_spin_button_set_value(ui->animation_spin, config->animation_ms);
    gtk_spin_button_set_value(ui->size_spin, config->square_size);
    gtk_spin_button_set_value(ui->line_width_spin, config->line_width);

    color.red = (double)config->color_r / 255.0;
    color.green = (double)config->color_g / 255.0;
    color.blue = (double)config->color_b / 255.0;
    color.alpha = 1.0;
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(ui->color_button), &color);
    queue_preview(ui);
}

static void read_from_widgets(SettingsUi *ui, TouchRMBConfig *config) {
    GdkRGBA color;

    config->pre_arm_ms = gtk_spin_button_get_value_as_int(ui->pre_arm_spin);
    config->animation_ms = gtk_spin_button_get_value_as_int(ui->animation_spin);
    config->square_size = gtk_spin_button_get_value_as_int(ui->size_spin);
    config->line_width = gtk_spin_button_get_value_as_int(ui->line_width_spin);

    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(ui->color_button), &color);
    config->color_r = (unsigned char)(color.red * 255.0 + 0.5);
    config->color_g = (unsigned char)(color.green * 255.0 + 0.5);
    config->color_b = (unsigned char)(color.blue * 255.0 + 0.5);
    touchrmb_config_clamp(config);
}

static gboolean restart_service(SettingsUi *ui) {
    gint exit_status = 0;
    GError *error = NULL;
    const gchar *argv[] = {
        "systemctl",
        "--user",
        "restart",
        "touchrmb.service",
        NULL
    };

    if (!g_spawn_sync(
        NULL,
        (gchar **)argv,
        NULL,
        G_SPAWN_SEARCH_PATH,
        NULL,
        NULL,
        NULL,
        NULL,
        &exit_status,
        &error)) {
        set_status(ui, error->message);
        g_clear_error(&error);
        return FALSE;
    }

    if (!g_spawn_check_wait_status(exit_status, &error)) {
        set_status(ui, error ? error->message : "Failed to restart touchrmb.service");
        g_clear_error(&error);
        return FALSE;
    }

    set_status(ui, "Applied. TouchRMB restarted.");
    return TRUE;
}

static void on_apply_clicked(GtkButton *button, gpointer user_data) {
    SettingsUi *ui = user_data;
    TouchRMBConfig config;

    (void)button;
    read_from_widgets(ui, &config);
    if (!touchrmb_config_save(&config)) {
        set_status(ui, "Failed to save configuration");
        return;
    }
    if (restart_service(ui)) {
        ui->applied_config = config;
    }
}

static void on_defaults_clicked(GtkButton *button, gpointer user_data) {
    SettingsUi *ui = user_data;
    TouchRMBConfig config;

    (void)button;
    touchrmb_config_defaults(&config);
    load_to_widgets(ui, &config);
    set_status(ui, "Default values loaded");
}

static void on_cancel_clicked(GtkButton *button, gpointer user_data) {
    SettingsUi *ui = user_data;

    (void)button;
    load_to_widgets(ui, &ui->applied_config);
    set_status(ui, "Unsaved changes discarded");
}

static void labeled_spin(const char *label_text, GtkWidget *grid, int row, SettingsUi *ui, GtkSpinButton **out_spin, double min, double max, double step) {
    GtkWidget *label = gtk_label_new(label_text);
    GtkWidget *spin = gtk_spin_button_new_with_range(min, max, step);

    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(spin, TRUE);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), spin, 1, row, 1, 1);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_control_changed), ui);
    *out_spin = GTK_SPIN_BUTTON(spin);
}

int main(int argc, char **argv) {
    SettingsUi ui;
    TouchRMBConfig config;
    GtkWidget *root;
    GtkWidget *grid;
    GtkWidget *buttons;
    GtkWidget *apply_button;
    GtkWidget *defaults_button;
    GtkWidget *cancel_button;
    GtkWidget *preview_frame;
    GtkWidget *color_label;

    (void)argv;
    gtk_init(&argc, &argv);
    memset(&ui, 0, sizeof(ui));

    ui.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ui.window), "TouchRMB");
    gtk_window_set_default_size(GTK_WINDOW(ui.window), 420, 460);
    gtk_container_set_border_width(GTK_CONTAINER(ui.window), 16);
    g_signal_connect(ui.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_add(GTK_CONTAINER(ui.window), root);

    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_pack_start(GTK_BOX(root), grid, FALSE, FALSE, 0);

    labeled_spin("Hold delay (ms)", grid, 0, &ui, &ui.pre_arm_spin, 50, 2000, 10);
    labeled_spin("Animation (ms)", grid, 1, &ui, &ui.animation_spin, 50, 2000, 10);
    labeled_spin("Square size (px)", grid, 2, &ui, &ui.size_spin, 12, 160, 1);
    labeled_spin("Border width (px)", grid, 3, &ui, &ui.line_width_spin, 1, 12, 1);

    color_label = gtk_label_new("Square color");
    gtk_widget_set_halign(color_label, GTK_ALIGN_START);
    ui.color_button = GTK_COLOR_BUTTON(gtk_color_button_new());
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(ui.color_button), FALSE);
    g_signal_connect(ui.color_button, "color-set", G_CALLBACK(on_control_changed), &ui);
    gtk_grid_attach(GTK_GRID(grid), color_label, 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ui.color_button), 1, 4, 1, 1);

    preview_frame = gtk_frame_new("Preview (fully expanded, 1:1 px)");
    gtk_box_pack_start(GTK_BOX(root), preview_frame, TRUE, TRUE, 0);
    ui.preview = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_widget_set_size_request(GTK_WIDGET(ui.preview), 220, 220);
    gtk_container_add(GTK_CONTAINER(preview_frame), GTK_WIDGET(ui.preview));
    g_signal_connect(ui.preview, "draw", G_CALLBACK(preview_draw), &ui);

    ui.status_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_halign(GTK_WIDGET(ui.status_label), GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(root), GTK_WIDGET(ui.status_label), FALSE, FALSE, 0);

    buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
    gtk_box_pack_end(GTK_BOX(root), buttons, FALSE, FALSE, 0);

    defaults_button = gtk_button_new_with_label("Default");
    cancel_button = gtk_button_new_with_label("Cancel");
    apply_button = gtk_button_new_with_label("Apply");
    gtk_container_add(GTK_CONTAINER(buttons), defaults_button);
    gtk_container_add(GTK_CONTAINER(buttons), cancel_button);
    gtk_container_add(GTK_CONTAINER(buttons), apply_button);
    g_signal_connect(defaults_button, "clicked", G_CALLBACK(on_defaults_clicked), &ui);
    g_signal_connect(cancel_button, "clicked", G_CALLBACK(on_cancel_clicked), &ui);
    g_signal_connect(apply_button, "clicked", G_CALLBACK(on_apply_clicked), &ui);

    touchrmb_config_defaults(&config);
    touchrmb_config_load(&config);
    ui.applied_config = config;
    load_to_widgets(&ui, &config);
    gtk_widget_show_all(ui.window);
    gtk_main();
    return 0;
}
