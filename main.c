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
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include "deadbeef.h"
#include "gtkui_api.h"

typedef struct {
    GtkWidget *widget;
    char *text;
    int button_type; // 0 = shuffle, 1 = repeat
    int combo_index;
    int combo_active;
} ui_update_data_t;

static void free_ui_update_data(ui_update_data_t *data) {
    if (data) {
        free(data->text);
        free(data);
    }
}

static gboolean update_shuffle_button_ui(gpointer user_data) {
    ui_update_data_t *data = (ui_update_data_t *)user_data;
    
    if (data && data->widget && data->text) {
        const char *old = gtk_button_get_label(GTK_BUTTON(data->widget));
        if (strcmp(data->text, old) != 0) {
            gtk_button_set_label(GTK_BUTTON(data->widget), data->text);
        }
    }
    
    free_ui_update_data(data);
    return G_SOURCE_REMOVE;
}

static gboolean update_repeat_button_ui(gpointer user_data) {
    ui_update_data_t *data = (ui_update_data_t *)user_data;
    
    if (data && data->widget && data->text) {
        const char *old = gtk_button_get_label(GTK_BUTTON(data->widget));
        if (strcmp(data->text, old) != 0) {
            gtk_button_set_label(GTK_BUTTON(data->widget), data->text);
        }
    }
    
    free_ui_update_data(data);
    return G_SOURCE_REMOVE;
}

static gboolean update_combobox_ui(gpointer user_data) {
    ui_update_data_t *data = (ui_update_data_t *)user_data;
    
    if (data && data->widget) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(data->widget), data->combo_active);
    }
    
    free_ui_update_data(data);
    return G_SOURCE_REMOVE;
}

static void safe_shuffle_button_set_text(GtkWidget *widget, const char *text) {
    if (!widget || !text) return;
    
    ui_update_data_t *data = malloc(sizeof(ui_update_data_t));
    if (!data) return;
    
    data->widget = widget;
    data->text = malloc(strlen(text) + 1);
    if (!data->text) {
        free(data);
        return;
    }
    strcpy(data->text, text);
    data->button_type = 0;
    
    g_idle_add(update_shuffle_button_ui, data);
}

static void safe_repeat_button_set_text(GtkWidget *widget, const char *text) {
    if (!widget || !text) return;
    
    ui_update_data_t *data = malloc(sizeof(ui_update_data_t));
    if (!data) return;
    
    data->widget = widget;
    data->text = malloc(strlen(text) + 1);
    if (!data->text) {
        free(data);
        return;
    }
    strcpy(data->text, text);
    data->button_type = 1; // Repeat
    
    g_idle_add(update_repeat_button_ui, data);
}

static void safe_combo_box_set_active(GtkWidget *widget, int active) {
    if (!widget) return;
    
    ui_update_data_t *data = malloc(sizeof(ui_update_data_t));
    if (!data) return;
    
    data->widget = widget;
    data->text = NULL;
    data->combo_active = active;
    
    g_idle_add(update_combobox_ui, data);
}

// Constants
#define INITIAL_ARRAY_SIZE 1
#define MAX_METADATA_LENGTH 2048
#define BUTTON_WIDTH 110
#define COMBOBOX_WIDTH 140
#define TRACE_PREFIX "PlaybackButtons: "

static pthread_mutex_t playlist_mutex;

// Enhanced trace macro with function and line number
#define trace(fmt, ...) fprintf(stderr, TRACE_PREFIX "%s:%d: " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define CHECK_NULL(ptr, msg) if (!(ptr)) { trace(msg "\n"); return; }
#define CHECK_NULL_RET(ptr, msg, ret) if (!(ptr)) { trace(msg "\n"); return ret; }


static void safe_strncpy(char *dest, const char *src, size_t dest_size) {
    if (!dest || dest_size == 0) {
        return;
    }
    
    dest[0] = '\0';
    
    if (src) {
        strncpy(dest, src, dest_size - 1);
        dest[dest_size - 1] = '\0';
    }
}

// Data structures
typedef struct {
    ddb_gtkui_widget_t base;
    GtkWidget *shuffle_button;
    GtkWidget *repeat_button;
    GtkWidget *play_combobox;
} w_playback_buttons_t;

typedef struct {
    int *array;
    size_t used;
    size_t size;
} Array;

typedef enum {
    PLAYLIST = 0,       // Plays tracks in original playlist order
    KEEP_ALBUM,         // Restricts playback to current album
    KEEP_ARTIST,        // Restricts playback to current artist
    TOP_RATED_SONGS,    // Plays tracks with high ratings
    SELECTION,          // Plays currently selected tracks
    PURE_RANDOM,        // Completely random track selection
    SMART_RANDOM        // Random selection weighted by ratings
} PlayModes;

typedef struct {
    Array playlist;
    int current_played_item;
    PlayModes play_mode;
    int is_enabled;
} PluginState;

typedef struct {
    int plt_id;
    Array playlist;
    PlayModes play_mode;
} SavedPlaylist;

static DB_misc_t plugin;
static DB_functions_t *deadbeef        = NULL;
static ddb_gtkui_t *gtkui_plugin       = NULL;
static w_playback_buttons_t *p_buttons = NULL;
static PluginState state = { .current_played_item = 0, .play_mode = PLAYLIST, .is_enabled = 0 };
static SavedPlaylist *saved_playlists = NULL;
static size_t saved_playlists_count = 0;

// Thread-lokale Variable für Race-Condition-Prävention
static __thread DB_playItem_t *thread_last_played = NULL;

// Locks mutex with error handling
static int lock_mutex(pthread_mutex_t *mutex, const char *func_name) {
    if (!mutex) {
        trace("NULL mutex in %s\n", func_name);
        return EINVAL;
    }
    
    int result = pthread_mutex_lock(mutex);
    if (result != 0) {
        const char *error_str = "Unknown error";
        switch (result) {
            case EINVAL: error_str = "Invalid mutex"; break;
            case EDEADLK: error_str = "Deadlock detected"; break;
            case EAGAIN: error_str = "Maximum recursion exceeded"; break;
        }
        trace("Failed to lock mutex in %s: %s (%d)\n", func_name, error_str, result);
    }
    return result;
}

// Unlocks mutex with error handling
static int unlock_mutex(pthread_mutex_t *mutex, const char *func_name) {
    if (!mutex) {
        trace("NULL mutex in %s\n", func_name);
        return EINVAL;
    }
    
    int result = pthread_mutex_unlock(mutex);
    if (result != 0) {
        const char *error_str = "Unknown error";
        switch (result) {
            case EINVAL: error_str = "Invalid mutex"; break;
            case EPERM: error_str = "Thread doesn't own mutex"; break;
        }
        trace("Failed to unlock mutex in %s: %s (%d)\n", func_name, error_str, result);
    }
    return result;
}

// Frees the array memory in a thread-safe manner
static int freeArray(Array *a) {
    CHECK_NULL_RET(a, "Null pointer passed to freeArray", -1);
    if (lock_mutex(&playlist_mutex, "freeArray") != 0) {
        return -1;
    }
    if (a->array) {
        free(a->array);
        a->array = NULL;
        a->used = a->size = 0;
    }
    return unlock_mutex(&playlist_mutex, "freeArray");
}

// Initializes a dynamic array with thread-safe memory allocation
static int initArray(Array *a, size_t initialSize) {
    CHECK_NULL_RET(a, "Null pointer passed to initArray", -1);
    if (lock_mutex(&playlist_mutex, "initArray") != 0) {
        return -1;
    }
    a->array = malloc(initialSize * sizeof(int));
    if (!a->array) {
        trace("Memory allocation failed in initArray\n");
        unlock_mutex(&playlist_mutex, "initArray");
        return -1;
    }
    memset(a->array, 0, initialSize * sizeof(int));
    a->used = 0;
    a->size = initialSize;
    return unlock_mutex(&playlist_mutex, "initArray");
}

// Inserts an element into the dynamic array with optimized resizing
static int insertArray(Array *a, int element) {
    CHECK_NULL_RET(a, "Null pointer passed to insertArray", -1);
    
    if (lock_mutex(&playlist_mutex, "insertArray") != 0) {
        return -1;
    }
    
    int result = 0;
    if (a->used == a->size) {
        size_t growth_factor = (a->size < 1000) ? 2 : 1.5;
        size_t new_size = a->size * growth_factor + 1;
        if (new_size > SIZE_MAX / sizeof(int)) {
            trace("Array size overflow in insertArray\n");
            result = -1;
        } else {
            int *newArray = realloc(a->array, new_size * sizeof(int));
            if (!newArray) {
                trace("Memory reallocation failed in insertArray\n");
                result = -1;
            } else {
                a->array = newArray;
                a->size = new_size;
            }
        }
    }
    
    if (result == 0) {
        a->array[a->used++] = element;
    }
    
    unlock_mutex(&playlist_mutex, "insertArray");
    return result;
}

// Performs a playlist operation in a single critical section
static int performPlaylistOperation(Array *a, int (*operation)(Array *, void *), void *data) {
    CHECK_NULL_RET(a, "Null array in performPlaylistOperation", -1);
    if (lock_mutex(&playlist_mutex, "performPlaylistOperation") != 0) {
        return -1;
    }
    int result = operation(a, data);
    unlock_mutex(&playlist_mutex, "performPlaylistOperation");
    return result;
}

// Shuffles the array using Fisher-Yates algorithm
static int shuffleArrayOperation(Array *a, void *unused) {
    if (!a->array || a->used == 0) {
        trace("Empty or invalid array in shuffleArray\n");
        return -1;
    }
    if (a->used > a->size) {
        trace("Array inconsistency detected: used (%zu) > size (%zu)\n", a->used, a->size);
        return -1;
    }
    if (a->used <= 1) return 0;

    for (size_t i = a->used - 1; i > 0; i--) {
#ifdef _POSIX_C_SOURCE
        size_t j = random() % (i + 1);
#else
        size_t j = rand() % (i + 1);
#endif
        int temp = a->array[i];
        a->array[i] = a->array[j];
        a->array[j] = temp;
    }
    return 0;
}

// Resets the playlist to initial state with pre-allocation
static int resetPlaylist(Array *a) {
    if (freeArray(a) != 0) {
        trace("Failed to free playlist array\n");
        return -1;
    }
    size_t initialSize = INITIAL_ARRAY_SIZE;
    if (deadbeef) {
        int count = deadbeef->pl_getcount(PL_MAIN);
        initialSize = (count > 0) ? count : INITIAL_ARRAY_SIZE;
    }
    return initArray(a, initialSize);
}

// Applies shuffle based on mode
static void applyShuffle(Array *a, int shuffle_mode, PlayModes play_mode, int *currentItem) {
    CHECK_NULL(a, "Invalid array in applyShuffle");
    CHECK_NULL(currentItem, "Invalid currentItem in applyShuffle");
    
    if (a->used <= 1) return;
    
    if (*currentItem < 0 || *currentItem >= (int)a->used) {
        trace("Invalid currentItem index %d in applyShuffle\n", *currentItem);
        *currentItem = 0;
        return;
    }
    
    if (shuffle_mode != DDB_SHUFFLE_OFF || play_mode == PURE_RANDOM || play_mode == SMART_RANDOM) {
        int value = a->array[*currentItem];
        performPlaylistOperation(a, shuffleArrayOperation, NULL);
        for (size_t i = 0; i < a->used; ++i) {
            if (a->array[i] == value) {
                *currentItem = i;
                break;
            }
        }
    }
}

// Seeds the random number generator once
static void init_random_seed(void) {
    static int initialized = 0;
    if (!initialized) {
#ifdef _POSIX_C_SOURCE
        srandom(time(NULL));
#else
        srand(time(NULL));
#endif
        initialized = 1;
    }
}

// Cleans up global resources
static void cleanup(void) {
    int lock_result = pthread_mutex_trylock(&playlist_mutex);
    int was_locked = (lock_result == 0);
    
    // Free saved playlists
    for (size_t i = 0; i < saved_playlists_count; i++) {
        freeArray(&saved_playlists[i].playlist);
    }
    free(saved_playlists);
    saved_playlists = NULL;
    saved_playlists_count = 0;
    
    // State cleanup
    freeArray(&state.playlist);
    
    if (was_locked) {
        unlock_mutex(&playlist_mutex, "cleanup");
    }
    
    pthread_mutex_destroy(&playlist_mutex);
    
    trace("Cleanup completed (mutex %s)\n", was_locked ? "locked" : "not locked");
}

// Comparison function for sorting array
static int sortArray(const void *a, const void *b) {
    int int_a = *(const int *)a;
    int int_b = *(const int *)b;
    return (int_a > int_b) - (int_a < int_b);
}

// Sets the currentPlayedItem based on the currently playing or marked track
static void syncCurrentPlayedItem(void) {
    DB_playItem_t *playing = deadbeef->streamer_get_playing_track_safe();
    if (!playing) {
        trace("No track currently playing\n");
        return;
    }

    int idx = deadbeef->pl_get_idx_of(playing);
    deadbeef->pl_item_unref(playing);

    for (size_t i = 0; i < state.playlist.used; i++) {
        if (state.playlist.array[i] == idx) {
            state.current_played_item = i;
            trace("Current position updated to: %zu (track index %d)\n", i, idx);
            return;
        }
    }

    if (state.playlist.used > 0) {
        state.current_played_item = 0;
        trace("Track not found, resetting to first position\n");
    } else {
        trace("Playlist empty, can't sync position\n");
    }
}

// Updates shuffle button text based on current mode
static void shuffle_button_set_text(GtkWidget *widget) {
    CHECK_NULL(widget, "Invalid widget in shuffle_button_set_text");
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in shuffle_button_set_text");

    const char *text;
    int shuffle_mode = deadbeef->streamer_get_shuffle();
    switch (shuffle_mode) {
        case DDB_SHUFFLE_OFF:    text = "Linear"; break;
        case DDB_SHUFFLE_TRACKS: text = "Shuffle"; break;
        case DDB_SHUFFLE_ALBUMS: text = "Album"; break;
        case DDB_SHUFFLE_RANDOM: text = "Random"; break;
        default: return;
    }

    const char *old = gtk_button_get_label(GTK_BUTTON(widget));
    if (strcmp(text, old) != 0) {
        safe_shuffle_button_set_text(widget, text);
        
        if (state.playlist.used > 1) {
            int value = state.playlist.array[state.current_played_item];
            if (shuffle_mode == DDB_SHUFFLE_OFF) {
                qsort(state.playlist.array, state.playlist.used, sizeof(int), sortArray);
            } else {
                performPlaylistOperation(&state.playlist, shuffleArrayOperation, NULL);
            }
            for (size_t i = 0; i < state.playlist.used; i++) {
                if (state.playlist.array[i] == value) {
                    state.current_played_item = i;
                    break;
                }
            }
        }
    }
}

// Updates repeat button text based on current mode
static void repeat_button_set_text(GtkWidget *widget) {
    CHECK_NULL(widget, "Invalid widget in repeat_button_set_text");
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in repeat_button_set_text");

    const char *text;
    int repeat_mode = deadbeef->streamer_get_repeat();
    switch (repeat_mode) {
        case DDB_REPEAT_OFF:   text = "Loop Off"; break;
        case DDB_REPEAT_SINGLE: text = "Loop Track"; break;
        case DDB_REPEAT_ALL:   text = "Loop All"; break;
        default: return;
    }

    const char *old = gtk_button_get_label(GTK_BUTTON(widget));
    if (strcmp(text, old) != 0) {
        safe_repeat_button_set_text(widget, text);
    }
}

// Checks if playback is active
static int isPlaybackActive(void) {
    CHECK_NULL_RET(deadbeef, "Deadbeef API not initialized in isPlaybackActive", 0);
    return (deadbeef->pl_getcount(PL_MAIN) > 0 && 
            deadbeef->get_output() != NULL && 
            deadbeef->get_output()->state() == DDB_PLAYBACK_STATE_PLAYING);
}

// Updates combobox to default state if playlist is empty
static void updateComboboxOnEmpty(w_playback_buttons_t *w) {
    if (state.playlist.used <= 0 && w && w->play_combobox) {
        state.play_mode = PLAYLIST;
        safe_combo_box_set_active(w->play_combobox, PLAYLIST);
    }
}

// Adds top-rated songs to playlist based on rating
static void createTopRatedSongs(DB_playItem_t *it, int index) {
    CHECK_NULL(it, "Invalid play item in createTopRatedSongs");
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in createTopRatedSongs");
    int rating = deadbeef->pl_find_meta_int(it, "rating", 0);
    if (rating >= 4) {
        insertArray(&state.playlist, index);
    }
}

// Adds songs by the same artist to playlist
static void createKeepArtistSongs(const char *artist, DB_playItem_t *it, int index) {
    CHECK_NULL(artist, "Invalid artist in createKeepArtistSongs");
    CHECK_NULL(it, "Invalid play item in createKeepArtistSongs");
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in createKeepArtistSongs");
    
    const char *track_artist = deadbeef->pl_find_meta_raw(it, "artist");
    if (!track_artist) {
        return;
    }
    
    if (strstr(track_artist, artist) != NULL) {
        insertArray(&state.playlist, index);
    }
}

// Adds songs from the same album to playlist
static void createKeepAlbumSongs(const char *folder_uri, DB_playItem_t *it, int index) {
    CHECK_NULL(folder_uri, "Invalid folder URI in createKeepAlbumSongs");
    CHECK_NULL(it, "Invalid play item in createKeepAlbumSongs");
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in createKeepAlbumSongs");
    
    const char *track_uri = deadbeef->pl_find_meta(it, ":URI");
    if (!track_uri) {
        return;
    }
    
    if (strstr(track_uri, folder_uri) != NULL) {
        insertArray(&state.playlist, index);
    }
}

// Adds selected songs to playlist
static void createSelectionSongs(DB_playItem_t *it, int index) {
    CHECK_NULL(it, "Invalid play item in createSelectionSongs");
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in createSelectionSongs");
    if (deadbeef->pl_is_selected(it)) {
        insertArray(&state.playlist, index);
    }
}

// Extracts artist name from track metadata (sicher)
static void extractArtistFromTrack(DB_playItem_t *track, char *artist, size_t size) {
    CHECK_NULL(track, "Invalid track in extractArtistFromTrack");
    CHECK_NULL(artist, "Invalid artist buffer in extractArtistFromTrack");
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in extractArtistFromTrack");
    
    artist[0] = '\0';
    
    const char *meta = deadbeef->pl_find_meta_raw(track, "artist");
    if (meta) {
        safe_strncpy(artist, meta, size);
        
        char *feat_ptr = strstr(artist, " feat");
        if (!feat_ptr) feat_ptr = strstr(artist, " feat.");
        if (!feat_ptr) feat_ptr = strstr(artist, " featuring");
        
        if (feat_ptr) {
            *feat_ptr = '\0';
            
            size_t len = strlen(artist);
            while (len > 0 && artist[len-1] == ' ') {
                artist[len-1] = '\0';
                len--;
            }
        }
    }
}

// Extracts folder URI from track metadata
static void extractFolderUriFromTrack(DB_playItem_t *track, char *folder_uri, size_t size) {
    CHECK_NULL(track, "Invalid track in extractFolderUriFromTrack");
    CHECK_NULL(folder_uri, "Invalid folder URI buffer in extractFolderUriFromTrack");
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in extractFolderUriFromTrack");
    
    folder_uri[0] = '\0';
    
    const char *uri = deadbeef->pl_find_meta(track, ":URI");
    if (uri) {
        safe_strncpy(folder_uri, uri, size);
        
        char *last_slash = strrchr(folder_uri, '/');
        if (last_slash) {
            *last_slash = '\0';
            
            char *cd_dir = strstr(folder_uri, "/CD");
            if (cd_dir) {
                *cd_dir = '\0';
                
                size_t len = strlen(folder_uri);
                if (len > 0 && folder_uri[len-1] == '/') {
                    folder_uri[len-1] = '\0';
                }
            }
        }
    }
}

// Processes tracks based on specified criteria (mit Parameter-Validierung)
static void processTrackForCriteria(int criteria, DB_playItem_t *it, int index, const char *artist, const char *folder_uri) {
    CHECK_NULL(it, "Invalid play item in processTrackForCriteria");
    
    if ((criteria == KEEP_ARTIST && (!artist || artist[0] == '\0')) ||
        (criteria == KEEP_ALBUM && (!folder_uri || folder_uri[0] == '\0'))) {
        trace("Invalid parameters for criteria %d\n", criteria);
        return;
    }
    
    switch (criteria) {
        case TOP_RATED_SONGS: 
            createTopRatedSongs(it, index); 
            break;
        case KEEP_ARTIST:     
            if (artist && artist[0] != '\0') {
                createKeepArtistSongs(artist, it, index); 
            }
            break;
        case KEEP_ALBUM:      
            if (folder_uri && folder_uri[0] != '\0') {
                createKeepAlbumSongs(folder_uri, it, index); 
            }
            break;
        case SELECTION:       
            createSelectionSongs(it, index); 
            break;
        default:
            trace("Unknown criteria type: %d\n", criteria);
            break;
    }
}

// Creates a playlist based on specified criteria
static void createPlaylistByCriteria(int criteriaType) {
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in createPlaylistByCriteria");
    
    ddb_playlist_t *plt = deadbeef->plt_get_curr();
    if (!plt) {
        trace("No current playlist found\n");
        return;
    }
    
    DB_playItem_t *playedSong = deadbeef->streamer_get_playing_track_safe();
    if (!playedSong) {
        trace("No playing track found\n");
        deadbeef->plt_unref(plt);
        return;
    }
    
    deadbeef->pl_lock();
    
    char artist[MAX_METADATA_LENGTH] = {0};
    char folder_uri[MAX_METADATA_LENGTH] = {0};
    
    if (criteriaType == KEEP_ARTIST) {
        extractArtistFromTrack(playedSong, artist, sizeof(artist));
    } else if (criteriaType == KEEP_ALBUM) {
        extractFolderUriFromTrack(playedSong, folder_uri, sizeof(folder_uri));
    }
    
    state.current_played_item = 0;
    int index = 0;
    DB_playItem_t *it = deadbeef->plt_get_first(plt, PL_MAIN);
    
    while (it) {
        DB_playItem_t *next = deadbeef->pl_get_next(it, PL_MAIN);
        processTrackForCriteria(criteriaType, it, index, artist, folder_uri);
        
        if (it == playedSong) {
            state.current_played_item = state.playlist.used - 1;
        }
        
        deadbeef->pl_item_unref(it);
        it = next;
        ++index;
    }
    
    deadbeef->pl_item_unref(playedSong);
    deadbeef->plt_unref(plt);
    deadbeef->pl_unlock();
}

// Creates a pure random playlist
static void createPureRandomList(void) {
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in createPureRandomList");
    
    ddb_playlist_t *plt = deadbeef->plt_get_curr();
    if (!plt) {
        trace("No current playlist found\n");
        return;
    }
    
    DB_playItem_t *playedSong = deadbeef->streamer_get_playing_track_safe();
    
    deadbeef->pl_lock();
    
    state.current_played_item = 0;
    int index = 0;
    
    DB_playItem_t *it = deadbeef->plt_get_first(plt, PL_MAIN);
    
    while (it) {
        DB_playItem_t *next = deadbeef->pl_get_next(it, PL_MAIN);
        
        if (playedSong && it == playedSong) {
            state.current_played_item = index;
        }
        
        if (insertArray(&state.playlist, index) != 0) {
            trace("Failed to insert index into playlist\n");
            deadbeef->pl_item_unref(it);
            if (next) deadbeef->pl_item_unref(next);
            break;
        }
        
        deadbeef->pl_item_unref(it);
        it = next;
        ++index;
    }
    
    if (playedSong) {
        deadbeef->pl_item_unref(playedSong);
    }
    deadbeef->plt_unref(plt);
    deadbeef->pl_unlock();
    
    if (state.playlist.used > 1) {
        performPlaylistOperation(&state.playlist, shuffleArrayOperation, NULL);
    }
}

// Creates a smart random playlist with rating-based weighting
static void createSmartRandomList(void) {
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in createSmartRandomList");
    
    ddb_playlist_t *plt = deadbeef->plt_get_curr();
    if (!plt) {
        trace("No current playlist found\n");
        return;
    }
    
    if (resetPlaylist(&state.playlist) != 0) {
        deadbeef->plt_unref(plt);
        return;
    }
    
    DB_playItem_t *playedSong = deadbeef->streamer_get_playing_track_safe();
    if (!playedSong) {
        trace("No currently playing track found\n");
        deadbeef->plt_unref(plt);
        return;
    }
    
    deadbeef->pl_lock();
    
    Array tempList;
    if (initArray(&tempList, deadbeef->pl_getcount(PL_MAIN)) != 0) {
        deadbeef->pl_item_unref(playedSong);
        deadbeef->plt_unref(plt);
        deadbeef->pl_unlock();
        return;
    }
    
    int index = 0;
    DB_playItem_t *it = deadbeef->plt_get_first(plt, PL_MAIN);
    
    while (it) {
        int rating = deadbeef->pl_find_meta_int(it, "rating", 0);
        int weight = rating + 1;
        
        for (int i = 0; i < weight; i++) {
            if (insertArray(&tempList, index) != 0) {
                trace("Failed to insert weighted index into temp playlist\n");
                freeArray(&tempList);
                deadbeef->pl_item_unref(it);
                deadbeef->pl_item_unref(playedSong);
                deadbeef->plt_unref(plt);
                deadbeef->pl_unlock();
                return;
            }
        }
        
        if (it == playedSong) {
            state.current_played_item = tempList.used - 1;
        }
        
        DB_playItem_t *next = deadbeef->pl_get_next(it, PL_MAIN);
        deadbeef->pl_item_unref(it);
        it = next;
        ++index;
    }
    
    for (size_t i = 0; i < tempList.used; i++) {
        if (insertArray(&state.playlist, tempList.array[i]) != 0) {
            trace("Failed to copy index to main playlist\n");
            break;
        }
    }
    
    freeArray(&tempList);
    
    if (state.playlist.used > 1) {
        performPlaylistOperation(&state.playlist, shuffleArrayOperation, NULL);
    }
    
    deadbeef->pl_item_unref(playedSong);
    deadbeef->plt_unref(plt);
    deadbeef->pl_unlock();
}

// Creates a default playlist
static void createDefaultList(void) {
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in createDefaultList");
    
    ddb_playlist_t *plt = deadbeef->plt_get_curr();
    if (!plt) {
        trace("No current playlist found\n");
        return;
    }
    
    deadbeef->pl_lock();
    
    state.current_played_item = 0;
    int index = 0;
    
    DB_playItem_t *playedSong = deadbeef->streamer_get_playing_track_safe();
    DB_playItem_t *it = deadbeef->plt_get_first(plt, PL_MAIN);
    
    while (it) {
        DB_playItem_t *next = deadbeef->pl_get_next(it, PL_MAIN);
        
        if (playedSong && it == playedSong) {
            state.current_played_item = index;
        }
        
        if (insertArray(&state.playlist, index) != 0) {
            trace("Failed to insert index into playlist\n");
            deadbeef->pl_item_unref(it);
            if (next) deadbeef->pl_item_unref(next);
            break;
        }
        
        deadbeef->pl_item_unref(it);
        it = next;
        ++index;
    }
    
    if (playedSong) {
        deadbeef->pl_item_unref(playedSong);
    }
    deadbeef->plt_unref(plt);
    deadbeef->pl_unlock();
}

// Finds a saved playlist by ID
static SavedPlaylist* find_saved_playlist(int plt_id) {
    for (size_t i = 0; i < saved_playlists_count; i++) {
        if (saved_playlists[i].plt_id == plt_id) {
            return &saved_playlists[i];
        }
    }
    return NULL;
}

// Saves the current playlist state
static void save_current_playlist(int plt_id) {
    SavedPlaylist *sp = find_saved_playlist(plt_id);
    if (!sp) {
        // Create new saved playlist
        saved_playlists = realloc(saved_playlists, (saved_playlists_count + 1) * sizeof(SavedPlaylist));
        sp = &saved_playlists[saved_playlists_count++];
        sp->plt_id = plt_id;
        initArray(&sp->playlist, state.playlist.size);
    }
    
    // Copy current playlist
    freeArray(&sp->playlist);
    initArray(&sp->playlist, state.playlist.size);
    for (size_t i = 0; i < state.playlist.used; i++) {
        insertArray(&sp->playlist, state.playlist.array[i]);
    }
    sp->play_mode = state.play_mode;
}

// Loads a saved playlist
static int load_saved_playlist(int plt_id) {
    SavedPlaylist *sp = find_saved_playlist(plt_id);
    if (!sp) return 0;
    
    freeArray(&state.playlist);
    initArray(&state.playlist, sp->playlist.size);
    for (size_t i = 0; i < sp->playlist.used; i++) {
        insertArray(&state.playlist, sp->playlist.array[i]);
    }
    state.play_mode = sp->play_mode;
    return 1;
}

// Generates the current playlist based on selected mode
static void createSongList(void) {
    static time_t last_generation = 0;
    time_t now = time(NULL);

    // Rate-Limiting
    if ((now - last_generation) < 2) {
        trace("Playlist generation throttled (last: %ld, now: %ld)\n", last_generation, now);
        return;
    }
    last_generation = now;

    if (!isPlaybackActive()) {
        updateComboboxOnEmpty(p_buttons);
        return;
    }

    int plt_id = deadbeef->plt_get_curr_idx();
    SavedPlaylist *sp = find_saved_playlist(plt_id);

    if (!sp || sp->play_mode != state.play_mode) {
        trace("Generating new playlist for mode: %d\n", state.play_mode);
        
        if (resetPlaylist(&state.playlist) != 0) {
            trace("Failed to reset playlist array\n");
            return;
        }

        switch (state.play_mode) {
            case PLAYLIST:
                createDefaultList();
                break;
            case KEEP_ALBUM:
                createPlaylistByCriteria(KEEP_ALBUM);
                break;
            case KEEP_ARTIST:
                createPlaylistByCriteria(KEEP_ARTIST);
                break;
            case TOP_RATED_SONGS:
                createPlaylistByCriteria(TOP_RATED_SONGS);
                break;
            case SELECTION:
                createPlaylistByCriteria(SELECTION);
                break;
            case PURE_RANDOM:
                createPureRandomList();
                break;
            case SMART_RANDOM:
                createSmartRandomList();
                break;
        }

        int shuffle_mode = deadbeef->streamer_get_shuffle();
        applyShuffle(&state.playlist, shuffle_mode, state.play_mode, &state.current_played_item);

        if (lock_mutex(&playlist_mutex, "createSongList_sync") == 0) {
            syncCurrentPlayedItem();
            unlock_mutex(&playlist_mutex, "createSongList_sync");
        }
        
        save_current_playlist(plt_id);
        
        trace("Generated playlist with %zu items\n", state.playlist.used);
    }

    updateComboboxOnEmpty(p_buttons);
}

// Saves the current playback button state
static void save_playback_button_state(void) {
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in save_playback_button_state");
    char key[64];
    snprintf(key, sizeof(key), "Playback_Buttons_State_playlist_%i", deadbeef->plt_get_curr_idx());
    deadbeef->conf_set_int(key, state.play_mode);
}

// Restores the saved playback button state
static void restore_playback_button_state(void) {
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in restore_playback_button_state");
    CHECK_NULL(p_buttons, "Invalid p_buttons in restore_playback_button_state");
    CHECK_NULL(p_buttons->play_combobox, "Invalid play_combobox in restore_playback_button_state");
    
    char key[64];
    snprintf(key, sizeof(key), "Playback_Buttons_State_playlist_%i", deadbeef->plt_get_curr_idx());
    int mode = deadbeef->conf_get_int(key, PLAYLIST);
    
    // Only update if mode changed
    if (mode != state.play_mode) {
        state.play_mode = mode;
        
        // Update combobox
        safe_combo_box_set_active(p_buttons->play_combobox, mode);
        
        // Force playlist regeneration
        int plt_id = deadbeef->plt_get_curr_idx();
        SavedPlaylist *sp = find_saved_playlist(plt_id);
        if (sp) {
            freeArray(&sp->playlist);
        }
        createSongList();
    }
}

// Handles combobox changes for playback mode
static void play_ComboBox_changed(GtkWidget *widget, gpointer user_data) {
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in play_ComboBox_changed");
    CHECK_NULL(widget, "Invalid widget in play_ComboBox_changed");
    
    // Get new mode from combobox
    PlayModes new_mode = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
    
    // Only proceed if mode actually changed
    if (new_mode == state.play_mode) {
        return;
    }
    
    state.play_mode = new_mode;

    // Handle special cases for random modes
    if ((state.play_mode == PURE_RANDOM || state.play_mode == SMART_RANDOM) 
        && deadbeef->streamer_get_shuffle() != DDB_SHUFFLE_TRACKS) {
        deadbeef->streamer_set_shuffle(DDB_SHUFFLE_TRACKS);
        deadbeef->sendmessage(DB_EV_CONFIGCHANGED, 0, 0, 0);
    }

    // Force new playlist generation
    int plt_id = deadbeef->plt_get_curr_idx();
    SavedPlaylist *sp = find_saved_playlist(plt_id);
    if (sp) {
        freeArray(&sp->playlist);
    }
    
    // Create new playlist for current mode
    createSongList();
    
    // Save the new state
    save_playback_button_state();
}

// Handles widget messages, particularly config changes
static int playback_buttons_message(ddb_gtkui_widget_t *widget, uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    CHECK_NULL_RET(widget, "Invalid widget in playback_buttons_message", -1);
    CHECK_NULL_RET(deadbeef, "Deadbeef API not initialized in playback_buttons_message", -1);
    w_playback_buttons_t *w = (w_playback_buttons_t *)widget;

    if (id == DB_EV_CONFIGCHANGED) {
        if ((state.play_mode == PURE_RANDOM || state.play_mode == SMART_RANDOM) 
            && deadbeef->streamer_get_shuffle() != DDB_SHUFFLE_TRACKS) {
            state.play_mode = PLAYLIST;
            if (w->play_combobox) {
                safe_combo_box_set_active(w->play_combobox, PLAYLIST);
                trace("Reset play mode to PLAYLIST due to incompatible shuffle mode\n");
            }
        }
        shuffle_button_set_text(w->shuffle_button);
        repeat_button_set_text(w->repeat_button);
    }
    return 0;
}

// Toggles repeat mode on button click
static void repeat_button_clicked(GtkWidget *widget, gpointer user_data) {
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in repeat_button_clicked");
    int repeat_mode_old = deadbeef->streamer_get_repeat();
    int repeat_mode = (repeat_mode_old == DDB_REPEAT_SINGLE) ? DDB_REPEAT_ALL : DDB_REPEAT_SINGLE;

    if (repeat_mode != repeat_mode_old) {
        deadbeef->streamer_set_repeat(repeat_mode);
        deadbeef->sendmessage(DB_EV_CONFIGCHANGED, 0, 0, 0);
    }
}

// Toggles shuffle mode on button click
static void shuffle_button_clicked(GtkWidget *widget, gpointer user_data) {
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in shuffle_button_clicked");
    int shuffle_mode = deadbeef->streamer_get_shuffle();
    shuffle_mode = (shuffle_mode == DDB_SHUFFLE_OFF) ? DDB_SHUFFLE_TRACKS : DDB_SHUFFLE_OFF;

    deadbeef->streamer_set_shuffle(shuffle_mode);
    deadbeef->sendmessage(DB_EV_CONFIGCHANGED, 0, 0, 0);
}

// Creates the play combobox
static GtkWidget *create_combobox(w_playback_buttons_t *w) {
    GtkWidget *combobox = gtk_combo_box_text_new();
    CHECK_NULL_RET(combobox, "Failed to create play combobox", NULL);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combobox), "Playlist");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combobox), "Keep Album");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combobox), "Keep Artist");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combobox), "Top Rated");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combobox), "Selection");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combobox), "Pure Random");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combobox), "Smart Random");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combobox), 0);
    gtk_widget_show(combobox);
    gtk_widget_set_size_request(combobox, COMBOBOX_WIDTH, 32);
    g_signal_connect((gpointer)combobox, "changed", G_CALLBACK(play_ComboBox_changed), w);
    return combobox;
}

// Creates a button with specified label and callback
static GtkWidget *create_button(const char *label, GCallback callback, w_playback_buttons_t *w) {
    GtkWidget *button = gtk_button_new_with_label(label);
    CHECK_NULL_RET(button, "Failed to create button", NULL);
    gtk_widget_show(button);
    gtk_widget_set_size_request(button, BUTTON_WIDTH, 32);
    g_signal_connect((gpointer)button, "clicked", callback, w);
    return button;
}

// Disconnects the plugin from GTKUI
static int __attribute__((used)) playback_buttons_disconnect(void) {
    gtkui_plugin = NULL;
    return 0;
}

// Initializes the plugin
static int playback_buttons_start(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    
    if (pthread_mutex_init(&playlist_mutex, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        trace("Failed to initialize recursive mutex\n");
        return -1;
    }
    pthread_mutexattr_destroy(&attr);
    
    init_random_seed();
    if (initArray(&state.playlist, INITIAL_ARRAY_SIZE) != 0) {
        cleanup();
        return -1;
    }

    createSongList();
    
    if (lock_mutex(&playlist_mutex, "playback_buttons_start") == 0) {
        syncCurrentPlayedItem();
        unlock_mutex(&playlist_mutex, "playback_buttons_start");
    }
    
    trace("Player started with song index: %d\n", state.current_played_item);
    return 0;
}

// Stops the plugin and cleans up
static int __attribute__((used)) playback_buttons_stop(void) {
    cleanup();
    return 0;
}

// Initializes the playback buttons widget
static void playback_buttons_init(ddb_gtkui_widget_t *ww) {
    CHECK_NULL(ww, "Invalid widget in playback_buttons_init");
    CHECK_NULL(gtkui_plugin, "Invalid gtkui_plugin in playback_buttons_init");
    w_playback_buttons_t *w = (w_playback_buttons_t *)ww;
    p_buttons = w;

    // Create horizontal box
    GtkWidget *hbox = gtk_hbox_new(FALSE, 2);
    CHECK_NULL(hbox, "Failed to create hbox in playback_buttons_init");
    gtk_widget_show(hbox);
    gtk_container_add(GTK_CONTAINER(w->base.widget), hbox);

    // Create play combobox
    w->play_combobox = create_combobox(w);
    CHECK_NULL(w->play_combobox, "Failed to create play combobox in playback_buttons_init");
    gtk_box_pack_start(GTK_BOX(hbox), w->play_combobox, FALSE, TRUE, 0);

    // Create shuffle button
    w->shuffle_button = create_button("", G_CALLBACK(shuffle_button_clicked), w);
    CHECK_NULL(w->shuffle_button, "Failed to create shuffle button in playback_buttons_init");
    gtk_box_pack_start(GTK_BOX(hbox), w->shuffle_button, FALSE, TRUE, 0);

    // Create repeat button
    w->repeat_button = create_button("", G_CALLBACK(repeat_button_clicked), w);
    CHECK_NULL(w->repeat_button, "Failed to create repeat button in playback_buttons_init");
    gtk_box_pack_start(GTK_BOX(hbox), w->repeat_button, FALSE, TRUE, 0);

    // Update button labels
    shuffle_button_set_text(w->shuffle_button);
    repeat_button_set_text(w->repeat_button);

    // Register widget signals (avoid overriding tray signals)
    gtkui_plugin->w_override_signals(w->base.widget, w);

    // Restore saved state
    restore_playback_button_state();
}

// Destroys the playback buttons widget
static void playback_buttons_destroy(ddb_gtkui_widget_t *w) {
    if (freeArray(&state.playlist) != 0) {
        trace("Failed to free playlist array during destroy\n");
    }
}

// Creates a new playback buttons widget instance
static ddb_gtkui_widget_t *w_playback_buttons_create(void) {
    w_playback_buttons_t *w = malloc(sizeof(w_playback_buttons_t));
    CHECK_NULL_RET(w, "Failed to allocate memory for playback buttons widget", NULL);
    memset(w, 0, sizeof(w_playback_buttons_t));

    w->base.widget = gtk_event_box_new();
    CHECK_NULL_RET(w->base.widget, "Failed to create event box for widget", NULL);
    w->base.init = playback_buttons_init;
    w->base.destroy = playback_buttons_destroy;
    w->base.message = playback_buttons_message;

    return (ddb_gtkui_widget_t *)w;
}

// Connects the plugin to the GTKUI system
static int __attribute__((used)) playback_buttons_connect(void) {
    CHECK_NULL_RET(deadbeef, "Deadbeef API not initialized in playback_buttons_connect", -1);
    gtkui_plugin = (ddb_gtkui_t *)deadbeef->plug_get_for_id(DDB_GTKUI_PLUGIN_ID);
    if (!gtkui_plugin) {
        trace("Failed to get gtkui_plugin in playback_buttons_connect\n");
        return -1;
    }

    // Register widget (void return, no error checking possible)
    gtkui_plugin->w_reg_widget("Playback Buttons", DDB_WF_SINGLE_INSTANCE, w_playback_buttons_create, "shuffle_mode", NULL);
    trace("Successfully registered Playback Buttons widget\n");
    return 0;
}

// Gets the configuration key for playback mode
static void get_playback_mode_key(char *buffer, size_t buffer_size) {
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in get_playback_mode_key");
    snprintf(buffer, buffer_size, "Saved_playback_mode_playlist_%i", deadbeef->plt_get_curr_idx());
}

// Retrieves the saved playback mode
static int get_playback_mode(void) {
    CHECK_NULL_RET(deadbeef, "Deadbeef API not initialized in get_playback_mode", -1);
    char key[64];
    get_playback_mode_key(key, sizeof(key));
    return deadbeef->conf_get_int(key, -1);
}

// Applies the saved playback mode
static void change_playback_mode(void) {
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in change_playback_mode");
    int saved_mode = get_playback_mode();
    int shuffle_mode = deadbeef->streamer_get_shuffle();

    if (saved_mode != -1 && saved_mode != shuffle_mode) {
        deadbeef->streamer_set_shuffle(saved_mode);
        deadbeef->sendmessage(DB_EV_CONFIGCHANGED, 0, 0, 0);
    }
}

// Gets the configuration key for repeat mode
static void get_repeat_mode_key(char *buffer, size_t buffer_size) {
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in get_repeat_mode_key");
    snprintf(buffer, buffer_size, "Saved_repeat_mode_playlist_%i", deadbeef->plt_get_curr_idx());
}

// Retrieves the saved repeat mode
static ddb_repeat_t get_repeat_mode(void) {
    CHECK_NULL_RET(deadbeef, "Deadbeef API not initialized in get_repeat_mode", DDB_REPEAT_OFF);
    char key[64];
    get_repeat_mode_key(key, sizeof(key));
    return (ddb_repeat_t)deadbeef->conf_get_int(key, DDB_REPEAT_OFF);
}

// Applies the saved repeat mode
static void change_repeat_mode(void) {
    CHECK_NULL(deadbeef, "Deadbeef API not initialized in change_repeat_mode");
    ddb_repeat_t saved_mode = get_repeat_mode();
    ddb_repeat_t current_mode = deadbeef->streamer_get_repeat();

    if (saved_mode != current_mode) {
        deadbeef->streamer_set_repeat(saved_mode);
        deadbeef->sendmessage(DB_EV_CONFIGCHANGED, 0, 0, 0);
    }
}

// Handles DeaDBeeF events
static int handle_event(uint32_t current_event, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    CHECK_NULL_RET(deadbeef, "Deadbeef API not initialized in handle_event", -1);
    
    if (current_event == DB_EV_PLAYLISTSWITCHED) {
        int plt_id = deadbeef->plt_get_curr_idx();
        if (!load_saved_playlist(plt_id)) {
            if (state.is_enabled) {
                change_playback_mode();
                change_repeat_mode();
                restore_playback_button_state();
            }
            createSongList();
        }
        
        if (lock_mutex(&playlist_mutex, "handle_event_playlistswitched") == 0) {
            syncCurrentPlayedItem();
            unlock_mutex(&playlist_mutex, "handle_event_playlistswitched");
        }
        return 0;
    }
    else if (current_event == DB_EV_PLAYLISTCHANGED) {
        int plt_id = deadbeef->plt_get_curr_idx();
        if ((int)p1 == plt_id) {
            save_current_playlist(plt_id);
            createSongList();
        }
        return 0;
    }
   else if (current_event == DB_EV_SONGCHANGED || current_event == DB_EV_TRACKINFOCHANGED) {
    DB_playItem_t *playing = deadbeef->streamer_get_playing_track_safe();
    if (!playing) return 0;

    if (playing != thread_last_played) {
        if (lock_mutex(&playlist_mutex, "handle_event_songchanged") == 0) {
            syncCurrentPlayedItem();
            unlock_mutex(&playlist_mutex, "handle_event_songchanged");
        }
        
        if (thread_last_played) {
            deadbeef->pl_item_unref(thread_last_played);
        }
        thread_last_played = playing;
    } else {
        deadbeef->pl_item_unref(playing);
    }
    return 0;
}
    else if (current_event == DB_EV_CONFIGCHANGED) {
        state.is_enabled = deadbeef->conf_get_int("Remember_Playback_Mode_Enabled", 0);
        if (!state.is_enabled) return 0;

        int old_mode = get_playback_mode();
        int shuffle_mode = deadbeef->streamer_get_shuffle();
        ddb_repeat_t old_repeat_mode = get_repeat_mode();
        ddb_repeat_t current_repeat_mode = deadbeef->streamer_get_repeat();

        if (shuffle_mode != old_mode) {
            char key[64];
            get_playback_mode_key(key, sizeof(key));
            deadbeef->conf_set_int(key, shuffle_mode);
        }

        if (current_repeat_mode != old_repeat_mode) {
            char key[64];
            get_repeat_mode_key(key, sizeof(key));
            deadbeef->conf_set_int(key, current_repeat_mode);
        }
        return 0;
    }

    if (state.play_mode == PLAYLIST || deadbeef->playqueue_get_count() != 0) return 0;

    if (current_event == DB_EV_NEXT || current_event == DB_EV_PREV) {
        if (state.playlist.used == 0) {
            createSongList();
            if (lock_mutex(&playlist_mutex, "handle_event_navigation") == 0) {
                syncCurrentPlayedItem();
                unlock_mutex(&playlist_mutex, "handle_event_navigation");
            }
        }

        if (state.playlist.used == 0) {
            trace("Playlist still empty after generation, aborting navigation\n");
            return 0;
        }

        deadbeef->sendmessage(DB_EV_STOP, 0, 0, 0);

        if (deadbeef->streamer_get_shuffle() == DDB_SHUFFLE_RANDOM) {
            deadbeef->sendmessage(DB_EV_PLAY_NUM, 0, state.playlist.array[rand() % state.playlist.used], 0);
        } else {
            if (current_event == DB_EV_NEXT) {
                state.current_played_item++;
                if (state.current_played_item >= (int)state.playlist.used) state.current_played_item = 0;
            } else {
                state.current_played_item--;
                if (state.current_played_item < 0) state.current_played_item = state.playlist.used - 1;
            }
            deadbeef->sendmessage(DB_EV_PLAY_NUM, 0, state.playlist.array[state.current_played_item], 0);
        }
    }
    return 0;
}

// Helper for context menu actions
static int context_action_helper(PlayModes new_play_mode) {
    state.play_mode = new_play_mode;
    if (p_buttons && p_buttons->play_combobox) {
        safe_combo_box_set_active(p_buttons->play_combobox, state.play_mode);
    }
    createSongList();
    return 0;
}

// Context menu action definitions
static int setPureRandom_action(DB_plugin_action_t *action, ddb_action_context_t ctx) { return context_action_helper(PURE_RANDOM); }
static int setSmartRandom_action(DB_plugin_action_t *action, ddb_action_context_t ctx) { return context_action_helper(SMART_RANDOM); }
static int setSelection_action(DB_plugin_action_t *action, ddb_action_context_t ctx) { return context_action_helper(SELECTION); }
static int TopRated_action(DB_plugin_action_t *action, ddb_action_context_t ctx) { return context_action_helper(TOP_RATED_SONGS); }
static int setAlbum_action(DB_plugin_action_t *action, ddb_action_context_t ctx) { return context_action_helper(KEEP_ALBUM); }
static int setArtist_action(DB_plugin_action_t *action, ddb_action_context_t ctx) { return context_action_helper(KEEP_ARTIST); }
static int setDisabled(DB_plugin_action_t *action, ddb_action_context_t ctx) { return context_action_helper(PLAYLIST); }

static DB_plugin_action_t context7_action = {
    .title = "Custom Playlist/Set Smart Random",
    .name = "custom_playlist7",
    .flags = DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = setSmartRandom_action,
    .next = NULL
};

static DB_plugin_action_t context6_action = {
    .title = "Custom Playlist/Set Pure Random",
    .name = "custom_playlist6",
    .flags = DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = setPureRandom_action,
    .next = &context7_action
};

static DB_plugin_action_t context5_action = {
    .title = "Custom Playlist/Set Selection",
    .name = "custom_playlist5",
    .flags = DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = setSelection_action,
    .next = &context6_action
};

static DB_plugin_action_t context4_action = {
    .title = "Custom Playlist/Set TopRated",
    .name = "custom_playlist4",
    .flags = DB_ACTION_SINGLE_TRACK | DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = TopRated_action,
    .next = &context5_action
};

static DB_plugin_action_t context3_action = {
    .title = "Custom Playlist/Set Artist",
    .name = "custom_playlist3",
    .flags = DB_ACTION_SINGLE_TRACK | DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = setArtist_action,
    .next = &context4_action
};

static DB_plugin_action_t context2_action = {
    .title = "Custom Playlist/Set Album",
    .name = "custom_playlist2",
    .flags = DB_ACTION_SINGLE_TRACK | DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = setAlbum_action,
    .next = &context3_action
};

static DB_plugin_action_t context1_action = {
    .title = "Custom Playlist/Disable",
    .name = "custom_playlist1",
    .flags = DB_ACTION_SINGLE_TRACK | DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = setDisabled,
    .next = &context2_action
};

// Returns the context menu actions
static DB_plugin_action_t * __attribute__((used)) context_actions(DB_playItem_t *it) {
    return &context1_action;
}

static DB_misc_t plugin = {
    .plugin.api_vmajor = 1,
    .plugin.api_vminor = 5,
    .plugin.version_major = 1,
    .plugin.version_minor = 0,
    .plugin.type = DB_PLUGIN_MISC,
#if GTK_CHECK_VERSION(3, 0, 0)
    .plugin.id = "playback_buttons_widget-gtk3",
#else
    .plugin.id = "playback_buttons_widget",
#endif
    .plugin.name = "Playback Buttons",
    .plugin.descr = "Plugin to easily change the playback shuffle and repeat.",
    .plugin.copyright =
        "Copyright (C) 2020-2026 kpcee\n"
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
    .plugin.website = "https://github.com/kpcee/deadbeef-playback-buttons",
    .plugin.start = playback_buttons_start,
    .plugin.stop = playback_buttons_stop,
    .plugin.connect = playback_buttons_connect,
    .plugin.disconnect = playback_buttons_disconnect,
    .plugin.message = handle_event,
    .plugin.configdialog = "property \"Enable saving play modes per playlist.\" checkbox Remember_Playback_Mode_Enabled 0 ;\n",
    .plugin.get_actions = context_actions,
};

#if !GTK_CHECK_VERSION(3, 0, 0)
DB_plugin_t *ddb_misc_playback_buttons_GTK2_load(DB_functions_t *ddb) {
    deadbeef = ddb;
    return &plugin.plugin;
}
#else
DB_plugin_t *ddb_misc_playback_buttons_GTK3_load(DB_functions_t *ddb) {
    deadbeef = ddb;
    return &plugin.plugin;
}
#endif