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
#define trace( fmt, ... )

typedef struct
{
    ddb_gtkui_widget_t base;
    GtkWidget *play_button;
    GtkWidget *shuffle_button;
    GtkWidget *repeat_button;
} w_playback_buttons_t;

typedef struct
{
    int *array;
    size_t used;
    size_t size;
} Array;

static void initArray( Array *a, size_t initialSize )
{
    a->array = malloc( initialSize * sizeof( int ) );
    a->used  = 0;
    a->size  = initialSize;
}

static void insertArray( Array *a, int element )
{
    // a->used is the number of used entries, because a->array[a->used++] updates a->used only *after* the array has been accessed.
    // Therefore a->used can go up to a->size
    if ( a->used == a->size )
    {
        a->size *= 2;
        a->array = realloc( a->array, a->size * sizeof( int ) );
    }

    a->array[a->used++] = element;
}

static void shuffleArray( Array *a )
{
    srand( time( NULL ) );

    int size = a->used - 1;
    for ( int i = size; i >= 0; i-- )
    {
        int j = rand() % (i+1);

        int t       = a->array[i];
        a->array[i] = a->array[j];
        a->array[j] = t;
    }

    // for ( int i = 0; i <= size; i++ ) printf("%d ", a->array[i]);
    // printf("\n");
}

static int sortArray( const void* a, const void* b)
{
   int int_a = * ( (int*) a );
   int int_b = * ( (int*) b );

   return (int_a > int_b) - (int_a < int_b);
}

static void freeArray( Array *a )
{
    free( a->array );
    a->array = NULL;
    a->used = a->size = 0;
}

enum PLAYMODES { PLAYLIST = 0, KEEP_ALBUM, KEEP_ARTIST, TOP_RATED_SONGS, SELECTION };
static DB_misc_t plugin;
static DB_functions_t *deadbeef        = NULL;
static ddb_gtkui_t *gtkui_plugin       = NULL;
static w_playback_buttons_t *p_buttons = NULL;

static Array PlayList;
static int currentPlayedItem = 0;   // Index of the current item in the internal PlayList that is being played
static int shuffle_mode      = 0;   // Playback mode from deadbeef (linear, shuffle, random , album)
static int playlist_items    = 0;   // Number of items in deadbeefs playlist is used to detect changes
static int plugin_play_mode  = PLAYLIST; // Plugin Abspielmodus

static void shuffle_button_set_text( GtkWidget *widget )
{
    if ( !widget )
    {
        return;
    }

    char *text       = NULL;
    int shuffle_mode = deadbeef->streamer_get_shuffle();
    switch ( shuffle_mode )
    {
        case DDB_SHUFFLE_OFF:
            text = "Off";
            break;
        case DDB_SHUFFLE_TRACKS:
            text = "Shuffle";
            break;
        case DDB_SHUFFLE_ALBUMS:
            text = "Album";
            break;
        case DDB_SHUFFLE_RANDOM:
            text = "Random";
            break;
    }

    const char *old = gtk_button_get_label( GTK_BUTTON( widget ) );
    if ( strcmp( text, old ) != 0 )
    {
        gtk_button_set_label( GTK_BUTTON( widget ), text );

        // Instead of re-creating the playlist (which would fail with Selection because the selections are deleted on song
        // change), sort or shuffle the array manually
        if (PlayList.used > 1) {
            int value = PlayList.array[currentPlayedItem];

            if (shuffle_mode == DDB_SHUFFLE_OFF) {
                qsort( PlayList.array, PlayList.used, sizeof(int), sortArray );
                // printf( "Playlist re-sorted!\n\n" );
            }
            else {
                shuffleArray( &PlayList );
                // printf( "Playlist shuffled!\n\n" );
            }

            // Find the currently played song in the changed playlist
            for (int i = 0; i < PlayList.used; i++)
            {
                if (PlayList.array[i] == value) {
                    currentPlayedItem = i;
                    break;
                }
            }
        }
    }
}

static void repeat_button_set_text( GtkWidget *widget )
{
    if ( !widget )
    {
        return;
    }

    char *text      = NULL;
    int repeat_mode = deadbeef->streamer_get_repeat();
    switch ( repeat_mode )
    {
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

    const char *old = gtk_button_get_label( GTK_BUTTON( widget ) );
    if ( strcmp( text, old ) != 0 )
    {
        gtk_button_set_label( GTK_BUTTON( widget ), text );
    }
}

static void play_button_set_text( GtkWidget *widget )
{
    if ( !widget )
    {
        return;
    }

    char *text = NULL;

    switch ( plugin_play_mode )
    {
        case PLAYLIST:
            text = "Playlist";
            break;
        case KEEP_ALBUM:
            text = "Keep Album";
            break;
        case KEEP_ARTIST:
            text = "Keep Artist";
            break;
        case TOP_RATED_SONGS:
            text = "Top Rated";
            break;
        case SELECTION:
            text = "Selection";
            break;
    }

    const char *old = gtk_button_get_label( GTK_BUTTON( widget ) );
    if ( strcmp( text, old ) != 0 )
    {
        gtk_button_set_label( GTK_BUTTON( widget ), text );
    }
}

static void createTopRatedSongList()
{
    ddb_playlist_t *plt = deadbeef->plt_get_curr();
    if ( !plt )
    {
        return;
    }

    playlist_items = deadbeef->plt_get_item_count( plt, PL_MAIN );

    currentPlayedItem         = 0;
    int index                 = 0;
    DB_playItem_t *playedSong = deadbeef->streamer_get_playing_track();

    DB_playItem_t *it = deadbeef->plt_get_first( plt, PL_MAIN );
    while ( it )
    {
        DB_playItem_t *next = deadbeef->pl_get_next( it, PL_MAIN );
        int rating          = deadbeef->pl_find_meta_int( it, "rating", 0 );

        if ( rating >= 4 )
        {
            if ( playedSong && it == playedSong ) currentPlayedItem = PlayList.used;

            insertArray( &PlayList, index );
        }

        deadbeef->pl_item_unref( it );
        it = next;
        ++index;
    }

    // printf( "TopRatedSongList items: %d\n", PlayList.used );

    deadbeef->pl_item_unref( playedSong );
    deadbeef->plt_unref( plt );
}

static void createKeepArtistList()
{
    ddb_playlist_t *plt = deadbeef->plt_get_curr();
    if ( !plt )
    {
        return;
    }

    currentPlayedItem         = 0;
    int index                 = 0;
    DB_playItem_t *playedSong = deadbeef->streamer_get_playing_track();

    // Extract artist from the currently playing song
    const char *meta = deadbeef->pl_find_meta_raw( playedSong, "artist" );
    if ( meta == NULL )
    {
        return;
    }

    // Make a copy of the string
    char *tmp;
    char artist[2048];
    strcpy( artist, deadbeef->pl_find_meta_raw( playedSong, "artist" ) );

    // Remove "feat", "feat." "feature"
    tmp = strstr( artist, " feat" );
    if ( tmp ) *tmp = '\0';

    playlist_items = deadbeef->plt_get_item_count( plt, PL_MAIN );

    // printf( "extracted artist: %s\n", artist );

    DB_playItem_t *it = deadbeef->plt_get_first( plt, PL_MAIN );
    while ( it )
    {
        DB_playItem_t *next = deadbeef->pl_get_next( it, PL_MAIN );

        if ( strstr( deadbeef->pl_find_meta_raw( it, "artist" ), artist ) != NULL )
        {
            if ( it == playedSong ) currentPlayedItem = PlayList.used;

            insertArray( &PlayList, index );
        }

        deadbeef->pl_item_unref( it );
        it = next;
        ++index;
    }

    // printf( "createKeepArtistList items: %d\n", PlayList.used );

    deadbeef->pl_item_unref( playedSong );
    deadbeef->plt_unref( plt );
}

static void createKeepAlbumList()
{
    ddb_playlist_t *plt = deadbeef->plt_get_curr();
    if ( !plt )
    {
        return;
    }

    currentPlayedItem         = 0;
    int index                 = 0;
    DB_playItem_t *playedSong = deadbeef->streamer_get_playing_track();

    playlist_items = deadbeef->plt_get_item_count( plt, PL_MAIN );

    // Make a copy of the string
    char *tmp;
    char folder_uri[2048];
    strcpy( folder_uri, deadbeef->pl_find_meta( playedSong, ":URI" ) );

    // All files in the same folder are considered as the same album
    tmp  = strrchr( folder_uri, '/' );
    *tmp = '\0';

    // Cutting out /CD1, /CD2 …
    tmp = strstr( folder_uri, "/CD" );
    if ( tmp ) *tmp = '\0';

    // printf( "Extracted folder: %s\n", folder_uri );

    DB_playItem_t *it = deadbeef->plt_get_first( plt, PL_MAIN );
    while ( it )
    {
        DB_playItem_t *next = deadbeef->pl_get_next( it, PL_MAIN );

        if ( strstr( deadbeef->pl_find_meta( it, ":URI" ), folder_uri ) != NULL )
        {
            if ( it == playedSong ) currentPlayedItem = PlayList.used;

            insertArray( &PlayList, index );
        }

        deadbeef->pl_item_unref( it );
        it = next;
        ++index;
    }

    // printf( "createKeepAlbumList items: %d\n", PlayList.used );

    deadbeef->pl_item_unref( playedSong );
    deadbeef->plt_unref( plt );
}

static void createSelectionSongList()
{
    ddb_playlist_t *plt = deadbeef->plt_get_curr();
    if ( !plt )
    {
        return;
    }

    currentPlayedItem         = 0;
    int index                 = 0;
    DB_playItem_t *playedSong = deadbeef->streamer_get_playing_track();

    DB_playItem_t *it = deadbeef->plt_get_first( plt, PL_MAIN );
    while ( it )
    {
        DB_playItem_t *next = deadbeef->pl_get_next( it, PL_MAIN );

        if ( deadbeef->pl_is_selected( it ) )
        {
            if ( it == playedSong ) currentPlayedItem = PlayList.used;

            insertArray( &PlayList, index );
        }

        deadbeef->pl_item_unref( it );
        it = next;
        ++index;
    }

    // printf( "createSelectionSongList items: %d\n", PlayList.used );

    deadbeef->pl_item_unref( playedSong );
    deadbeef->plt_unref( plt );
}

static void createSongList()
{
    // Deadbeefs playlist must be locked by the caller

    if ( deadbeef->pl_getcount( PL_MAIN ) <= 0 ) return;
    if ( deadbeef->get_output()->state() != OUTPUT_STATE_PLAYING ) return;

    freeArray( &PlayList );
    initArray( &PlayList, 1 );

    switch ( plugin_play_mode )
    {
        case KEEP_ALBUM:
            createKeepAlbumList();
            break;
        case KEEP_ARTIST:
            createKeepArtistList();
            break;
        case TOP_RATED_SONGS:
            createTopRatedSongList();
            break;
        case SELECTION:
            createSelectionSongList();
            break;

        default:
            return;
    }

    // Shuffle if needed
    if ( PlayList.used > 1 && deadbeef->streamer_get_shuffle() != DDB_SHUFFLE_OFF )
    {
        shuffleArray( &PlayList );
        // printf( "Array shuffled!\n\n" );
    }

    // Is the playlist empty? Then we switch back to the standard list
    if (PlayList.used <= 0) {
        plugin_play_mode  = PLAYLIST;
        play_button_set_text( p_buttons->play_button );
    }
}

static int playback_buttons_message( ddb_gtkui_widget_t *widget, uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2 )
{
    w_playback_buttons_t *w = (w_playback_buttons_t *) widget;

    if ( id == DB_EV_CONFIGCHANGED )
    {
        shuffle_button_set_text( w->shuffle_button );
        repeat_button_set_text( w->repeat_button );
    }
    return 0;
}

static void repeat_button_clicked( GtkWidget *widget, gpointer user_data )
{
    int repeat_mode_old;
    int repeat_mode = repeat_mode_old = deadbeef->streamer_get_repeat();

    repeat_mode = ( repeat_mode == DDB_REPEAT_SINGLE ) ? DDB_REPEAT_ALL : DDB_REPEAT_SINGLE;

    if ( repeat_mode != repeat_mode_old )
    {
        deadbeef->streamer_set_repeat( repeat_mode );
        deadbeef->sendmessage( DB_EV_CONFIGCHANGED, 0, 0, 0 );
    }
}

static void shuffle_button_clicked( GtkWidget *widget, gpointer user_data )
{
    int shuffle_mode_old;
    int shuffle_mode = shuffle_mode_old = deadbeef->streamer_get_shuffle();

    shuffle_mode = ( shuffle_mode == DDB_SHUFFLE_OFF ) ? DDB_SHUFFLE_TRACKS : DDB_SHUFFLE_OFF;

    if ( shuffle_mode != shuffle_mode_old )
    {
        deadbeef->streamer_set_shuffle( shuffle_mode );
        deadbeef->sendmessage( DB_EV_CONFIGCHANGED, 0, 0, 0 );
    }
}

static void play_button_clicked( GtkWidget *widget, gpointer user_data )
{
    deadbeef->pl_lock();
    plugin_play_mode++;
    if ( plugin_play_mode > SELECTION ) plugin_play_mode = PLAYLIST;

    play_button_set_text( widget );
    createSongList();
    deadbeef->pl_unlock();
}

static void playback_buttons_init( ddb_gtkui_widget_t *ww )
{
    w_playback_buttons_t *w = (w_playback_buttons_t *) ww;
    p_buttons               = w;

    GtkWidget *hbox = gtk_hbox_new( FALSE, 2 );
    gtk_widget_show( hbox );
    gtk_container_add( GTK_CONTAINER( w->base.widget ), hbox );

    w->play_button = gtk_button_new_with_label( "" );
    gtk_widget_show( w->play_button );
    gtk_box_pack_start( GTK_BOX( hbox ), w->play_button, FALSE, TRUE, 0 );
    gtk_widget_set_size_request( w->play_button, 110, 32 );
    g_signal_connect_after( (gpointer) w->play_button, "clicked", G_CALLBACK( play_button_clicked ), w );

    w->shuffle_button = gtk_button_new_with_label( "" );
    gtk_widget_show( w->shuffle_button );
    gtk_box_pack_start( GTK_BOX( hbox ), w->shuffle_button, FALSE, TRUE, 0 );
    gtk_widget_set_size_request( w->shuffle_button, 110, 32 );
    g_signal_connect_after( (gpointer) w->shuffle_button, "clicked", G_CALLBACK( shuffle_button_clicked ), w );

    w->repeat_button = gtk_button_new_with_label( "" );
    gtk_widget_show( w->repeat_button );
    gtk_box_pack_start( GTK_BOX( hbox ), w->repeat_button, FALSE, TRUE, 0 );
    gtk_widget_set_size_request( w->repeat_button, 110, 32 );
    g_signal_connect_after( (gpointer) w->repeat_button, "clicked", G_CALLBACK( repeat_button_clicked ), w );

    play_button_set_text( w->play_button );
    shuffle_button_set_text( w->shuffle_button );
    repeat_button_set_text( w->repeat_button );

    gtkui_plugin->w_override_signals( w->play_button, w );
    gtkui_plugin->w_override_signals( w->shuffle_button, w );
    gtkui_plugin->w_override_signals( w->repeat_button, w );
}

static void playback_buttons_destroy( ddb_gtkui_widget_t *w ) { freeArray( &PlayList ); }

static ddb_gtkui_widget_t *w_playback_buttons_create( void )
{
    w_playback_buttons_t *w = malloc( sizeof( w_playback_buttons_t ) );
    memset( w, 0, sizeof( w_playback_buttons_t ) );

    w->base.widget  = gtk_event_box_new();
    w->base.init    = playback_buttons_init;
    w->base.destroy = playback_buttons_destroy;
    w->base.message = playback_buttons_message;

    gtkui_plugin->w_override_signals( w->base.widget, w );

    return (ddb_gtkui_widget_t *) w;
}

static int playback_buttons_connect( void )
{
    gtkui_plugin = (ddb_gtkui_t *) deadbeef->plug_get_for_id( DDB_GTKUI_PLUGIN_ID );
    if ( gtkui_plugin )
    {
        // trace("using '%s' plugin %d.%d\n", DDB_GTKUI_PLUGIN_ID, gtkui_plugin->gui.plugin.version_major, gtkui_plugin->gui.plugin.version_minor );
        if ( gtkui_plugin->gui.plugin.version_major == 2 )
        {
            // printf ("fb api2\n");
            // 0.6+, use the new widget API
            gtkui_plugin->w_reg_widget( "Playback Buttons", DDB_WF_SINGLE_INSTANCE, w_playback_buttons_create, "shuffle_mode", NULL );
            return 0;
        }
    }
    return -1;
}

static int playback_buttons_disconnect( void )
{
    gtkui_plugin = NULL;
    return 0;
}

static int playback_buttons_start( void )
{
    return 0;
}

static int playback_buttons_stop( void ) { return 0; }

static int recount_playlist_items()
{
    // Playlist must be locked by the caller
    ddb_playlist_t *plt = deadbeef->plt_get_curr();
    if ( !plt )
    {
        return 0;
    }

    int items = deadbeef->plt_get_item_count( plt, PL_MAIN );
    deadbeef->plt_unref( plt );

    return items;
}

static int handle_event( uint32_t current_event, uintptr_t ctx, uint32_t p1, uint32_t p2 )
{
    // Plugin is offline
    if ( plugin_play_mode == PLAYLIST ) return 0;

    if ( current_event == DB_EV_NEXT )
    {
        deadbeef->pl_lock();
        deadbeef->sendmessage( DB_EV_STOP, 0, 0, 0 );

        // printf( "\n+++DB_EV_NEXT +++\n" );

        // If the playlist has changed we will rebuild our internal playlist to have no dead items.
        // The signals DB_EV_TRACKINFOCHANGED and DB_EV_PLAYLISTCHANGED are unfortunately not suitable for this purpose, because they are not used in
        // be triggered several times each time the track changes!
        int counter = recount_playlist_items();
        if ( counter != playlist_items )
        {
            createSongList();
            playlist_items = counter;
        }

        if ( shuffle_mode == DDB_SHUFFLE_RANDOM )
        {
            deadbeef->sendmessage( DB_EV_PLAY_NUM, 0, PlayList.array[rand() % PlayList.used - 1], 0 );
        } else
        {
            currentPlayedItem++;
            if ( currentPlayedItem >= PlayList.used ) currentPlayedItem = 0;

            deadbeef->sendmessage( DB_EV_PLAY_NUM, 0, PlayList.array[currentPlayedItem], 0 );
        }

        deadbeef->pl_unlock();
    } else if ( current_event == DB_EV_PREV )
    {
        deadbeef->pl_lock();
        deadbeef->sendmessage( DB_EV_STOP, 0, 0, 0 );

        // printf( "\n+++ DB_EV_PREV +++\n" );

        // If the playlist has changed we will rebuild our internal playlist to have no dead items.
        // The signals DB_EV_TRACKINFOCHANGED and DB_EV_PLAYLISTCHANGED are unfortunately not suitable for this purpose, because they are not used in
        // be triggered several times each time the track changes!
        int counter = recount_playlist_items();
        if ( counter != playlist_items )
        {
            createSongList();
            playlist_items = counter;
        }

        if ( shuffle_mode == DDB_SHUFFLE_RANDOM )
        {
            deadbeef->sendmessage( DB_EV_PLAY_NUM, 0, PlayList.array[rand() % PlayList.used - 1], 0 );
        } else
        {
            currentPlayedItem--;
            if ( currentPlayedItem < 0 ) currentPlayedItem = PlayList.used - 1;

            deadbeef->sendmessage( DB_EV_PLAY_NUM, 0, PlayList.array[currentPlayedItem], 0 );
        }

        deadbeef->pl_unlock();
    }

    else if ( current_event == DB_EV_CONFIGCHANGED )
    {
        shuffle_mode = deadbeef->streamer_get_shuffle();
    }

    return 0;
}

static int context_action_helper( int new_plugin_play_mode )
{
    deadbeef->pl_lock();
    plugin_play_mode = new_plugin_play_mode;
    play_button_set_text( p_buttons->play_button );
    createSongList();
    deadbeef->pl_unlock();
    return 0;
}

static int setSelection_action( DB_plugin_action_t *action, int ctx ) { return context_action_helper( SELECTION ); }

static int TopRated_action( DB_plugin_action_t *action, int ctx ) { return context_action_helper( TOP_RATED_SONGS ); }

static int setAlbum_action( DB_plugin_action_t *action, int ctx ) { return context_action_helper( KEEP_ALBUM ); }

static int setArtist_action( DB_plugin_action_t *action, int ctx ) { return context_action_helper( KEEP_ARTIST ); }

static int setDisabled( DB_plugin_action_t *action, int ctx ) { return context_action_helper( PLAYLIST ); }

static DB_plugin_action_t context5_action = { .title     = "Custom Playlist/Set Selection",
                                              .name      = "custom_playlist5",
                                              .flags     = DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
                                              .callback2 = setSelection_action,
                                              .next      = NULL };

static DB_plugin_action_t context4_action = { .title     = "Custom Playlist/Set TopRated",
                                              .name      = "custom_playlist4",
                                              .flags     = DB_ACTION_SINGLE_TRACK | DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
                                              .callback2 = TopRated_action,
                                              .next      = &context5_action };

static DB_plugin_action_t context3_action = { .title     = "Custom Playlist/Set Artist",
                                              .name      = "custom_playlist3",
                                              .flags     = DB_ACTION_SINGLE_TRACK | DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
                                              .callback2 = setArtist_action,
                                              .next      = &context4_action };

static DB_plugin_action_t context2_action = { .title     = "Custom Playlist/Set Album",
                                              .name      = "custom_playlist2",
                                              .flags     = DB_ACTION_SINGLE_TRACK | DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
                                              .callback2 = setAlbum_action,
                                              .next      = &context3_action };

static DB_plugin_action_t context1_action = { .title     = "Custom Playlist/Disable",
                                              .name      = "custom_playlist1",
                                              .flags     = DB_ACTION_SINGLE_TRACK | DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
                                              .callback2 = setDisabled,
                                              .next      = &context2_action };

static DB_plugin_action_t *context_actions( DB_playItem_t *it ) { return &context1_action; }

// define plugin interface
static DB_misc_t plugin = {
    .plugin.api_vmajor    = 1,
    .plugin.api_vminor    = 5,
    .plugin.version_major = 1,
    .plugin.version_minor = 0,
    .plugin.type          = DB_PLUGIN_MISC,
#if GTK_CHECK_VERSION( 3, 0, 0 )
    .plugin.id = "playback_buttons_widget-gtk3",
#else
    .plugin.id = "playback_buttons_widget",
#endif
    .plugin.name      = "Playback Buttons",
    .plugin.descr     = "Plugin to easily change the playback shuffle and repeat.",
    .plugin.copyright = "Copyright (C) 2020 kpcee\n"
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
                        "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n",
    .plugin.website     = "https://github.com/kpcee/deadbeef-playback-buttons",
    .plugin.start       = playback_buttons_start,
    .plugin.stop        = playback_buttons_stop,
    .plugin.connect     = playback_buttons_connect,
    .plugin.disconnect  = playback_buttons_disconnect,
    .plugin.message     = handle_event,
    .plugin.get_actions = context_actions,
};

#if !GTK_CHECK_VERSION( 3, 0, 0 )
DB_plugin_t *ddb_misc_playback_buttons_GTK2_load( DB_functions_t *ddb )
{
    deadbeef = ddb;
    return &plugin.plugin;
}
#else
DB_plugin_t *ddb_misc_playback_buttons_GTK3_load( DB_functions_t *ddb )
{
    deadbeef = ddb;
    return &plugin.plugin;
}
#endif