/*
    Playback Buttons, a plugin for the DeaDBeeF audio player

    Based on Playback Order Plugin from Christian Boxdörfer <christian.boxdoerfer@posteo.de>

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <string.h>
#include <gtk/gtk.h>
#include "deadbeef.h"
#include "gtkui_api.h"


//#define trace(...) { fprintf(stderr, __VA_ARGS__); }
#define trace(fmt,...)

static DB_misc_t plugin;
static DB_functions_t *deadbeef = NULL;
static ddb_gtkui_t *gtkui_plugin = NULL;

typedef struct {
    ddb_gtkui_widget_t base;
    GtkWidget *playback_button;
    GtkWidget *loop_button;
} w_playback_buttons_t;

static void
playback_button_set_text (GtkWidget *widget) {
    if (!widget) {
        return;
    }

    char *text = NULL;
    int shuffle_mode = deadbeef->streamer_get_shuffle () ;
    switch (shuffle_mode) {
    case DDB_SHUFFLE_OFF:
        text = "Linear";
        break;
    case DDB_SHUFFLE_TRACKS:
        text = "Shuffle";
        break;
    case DDB_SHUFFLE_ALBUMS:
        text = "RND Album";
        break;
    case DDB_SHUFFLE_RANDOM:
        text = "Random";
        break;
    }

    const char *old = gtk_button_get_label (widget);
    if (strcmp (text, old) != 0) {
        gtk_button_set_label (widget, text);
    }
}

static void
loop_button_set_text (GtkWidget *widget) {
    if (!widget) {
        return;
    }

    char *text = NULL;
    int repeat_mode = deadbeef->streamer_get_repeat ();
    switch (repeat_mode) {
    case DDB_REPEAT_OFF:
        text = "Loop Off";
        break;
    case DDB_REPEAT_SINGLE:
        text = "Loop Track";
        break;
    case DDB_REPEAT_ALL:
        text = "Loop All";
        break;
    }

    const char *old = gtk_button_get_label (widget);
    if (strcmp (text, old) != 0) {
        gtk_button_set_label (widget, text);
    }
}

static int
playback_buttons_message (ddb_gtkui_widget_t *widget, uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    w_playback_buttons_t *w = (w_playback_buttons_t *)widget;
    
    if (id == DB_EV_CONFIGCHANGED) {
        playback_button_set_text (w->playback_button);
        loop_button_set_text (w->loop_button);
    }
    return 0;
}

static void
button_loop_clicked (GtkWidget *widget, gpointer user_data) {
    int repeat_mode_old;
    int repeat_mode = repeat_mode_old = deadbeef->streamer_get_repeat ();

    repeat_mode = (repeat_mode == DDB_REPEAT_SINGLE) ? DDB_REPEAT_ALL : DDB_REPEAT_SINGLE;
  
    if (repeat_mode != repeat_mode_old) {
        deadbeef->streamer_set_repeat (repeat_mode);
        deadbeef->sendmessage (DB_EV_CONFIGCHANGED, 0, 0, 0);
    }
}

static void
button_order_clicked (GtkWidget *widget, gpointer user_data) {
    int shuffle_mode_old;
    int shuffle_mode = shuffle_mode_old = deadbeef->streamer_get_shuffle ();
    
    shuffle_mode = (shuffle_mode == DDB_SHUFFLE_OFF) ? DDB_SHUFFLE_TRACKS : DDB_SHUFFLE_OFF;
  
    if (shuffle_mode != shuffle_mode_old) {
        deadbeef->streamer_set_shuffle (shuffle_mode);
        deadbeef->sendmessage (DB_EV_CONFIGCHANGED, 0, 0, 0);
    }
}

static void
playback_buttons_init (ddb_gtkui_widget_t *ww) {
    w_playback_buttons_t *w = (w_playback_buttons_t *)ww;

    GtkWidget *hbox = gtk_hbox_new (FALSE, 2);
    gtk_widget_show (hbox);
    gtk_container_add (GTK_CONTAINER (w->base.widget), hbox);

    w->playback_button = gtk_button_new_with_label ("");
    gtk_widget_show (w->playback_button);
    gtk_box_pack_start (GTK_BOX (hbox), w->playback_button, FALSE, TRUE, 0);
    gtk_widget_set_size_request(w->playback_button, 110, 32);
    g_signal_connect_after ((gpointer) w->playback_button, "clicked", G_CALLBACK (button_order_clicked), w);

    w->loop_button = gtk_button_new_with_label ("");
    gtk_widget_show (w->loop_button);
    gtk_box_pack_start (GTK_BOX (hbox), w->loop_button, FALSE, TRUE, 0);
    gtk_widget_set_size_request(w->loop_button, 110, 32);
    g_signal_connect_after ((gpointer) w->loop_button, "clicked", G_CALLBACK (button_loop_clicked), w);
 
    playback_button_set_text (w->playback_button);
    loop_button_set_text(w->loop_button);

    gtkui_plugin->w_override_signals (w->playback_button, w);
    gtkui_plugin->w_override_signals (w->loop_button, w);
}

static void
    playback_buttons_destroy (ddb_gtkui_widget_t *w) {
}

static ddb_gtkui_widget_t *
w_playback_buttons_create (void) {
    w_playback_buttons_t *w = malloc (sizeof (w_playback_buttons_t));
    memset (w, 0, sizeof (w_playback_buttons_t));

    w->base.widget = gtk_event_box_new ();
    w->base.init = playback_buttons_init;
    w->base.destroy  = playback_buttons_destroy;
    w->base.message = playback_buttons_message;
   
    gtkui_plugin->w_override_signals (w->base.widget, w);

    return (ddb_gtkui_widget_t *)w;
}

static int
playback_buttons_connect (void) {
    gtkui_plugin = (ddb_gtkui_t *) deadbeef->plug_get_for_id (DDB_GTKUI_PLUGIN_ID);
    if (gtkui_plugin) {
        //trace("using '%s' plugin %d.%d\n", DDB_GTKUI_PLUGIN_ID, gtkui_plugin->gui.plugin.version_major, gtkui_plugin->gui.plugin.version_minor );
        if (gtkui_plugin->gui.plugin.version_major == 2) {
            //printf ("fb api2\n");
            // 0.6+, use the new widget API
            gtkui_plugin->w_reg_widget ("Playback Buttons", DDB_WF_SINGLE_INSTANCE, w_playback_buttons_create, "shuffle_mode", NULL);
            return 0;
        }
    }
    return -1;
}

static int
playback_buttons_disconnect (void) {
    gtkui_plugin = NULL;
    return 0;
}

static int
playback_buttons_start (void) {
    //load_config ();
    return 0;
}

static int
playback_buttons_stop (void) {
    return 0;
}

// define plugin interface
static DB_misc_t plugin = {
    .plugin.api_vmajor = 1,
    .plugin.api_vminor = 5,
    .plugin.version_major = 0,
    .plugin.version_minor = 1,
    .plugin.type = DB_PLUGIN_MISC,
#if GTK_CHECK_VERSION(3,0,0)
    .plugin.id = "playback_buttons_widget-gtk3",
#else
    .plugin.id = "playback_buttons_widget",
#endif
    .plugin.name = "Playback Buttons",
    .plugin.descr = "Plugin to easily change the playback shuffle and loop.",
    .plugin.copyright =
        "Copyright (C) 2019 kpcee\n"
        "\n"
        "This program is free software; you can redistribute it and/or\n"
        "modify it under the terms of the GNU General Public License\n"
        "as published by the Free Software Foundation; either version 2\n"
        "of the License, or (at your option) any later version.\n"
        "\n"
        "This program is distributed in the hope that it will be useful,\n"
        "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
        "GNU General Public License for more details.\n"
        "\n"
        "You should have received a copy of the GNU General Public License\n"
        "along with this program; if not, write to the Free Software\n"
        "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n"
    ,
    .plugin.website = "https://github.com/kpcee/deadbeef-playback-buttons",
    .plugin.start = playback_buttons_start,
    .plugin.stop = playback_buttons_stop,
    .plugin.connect  = playback_buttons_connect,
    .plugin.disconnect  = playback_buttons_disconnect,
};

#if !GTK_CHECK_VERSION(3,0,0)
DB_plugin_t *
ddb_misc_playback_buttons_GTK2_load (DB_functions_t *ddb) {
    deadbeef = ddb;
    return &plugin.plugin;
}
#else
DB_plugin_t *
ddb_misc_playback_buttons_GTK3_load (DB_functions_t *ddb) {
    deadbeef = ddb;
    return &plugin.plugin;
}
#endif
