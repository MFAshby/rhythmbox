#include "rb-deezer-source.h"

#include <string.h>
#include <gtk/gtk.h>
#include <glib/gi18n-lib.h>
#include <deezer-track.h>
#include <json-glib/json-glib.h>
#include <webkit2/webkit2.h>

#include "rb-deezer-plugin.h"
#include "rb-debug.h"
#include "rb-shell.h"
#include "rb-shell-player.h"
#include "rb-util.h"
#include "rb-source.h"
#include "rb-file-helpers.h"
#include "rb-property-view.h"
#include "rhythmdb-property-model.h"
#include "rb-deezer-login.h"

enum {
    RB_DEEZER_SOURCE_PROP_PLUGIN = 1,
};

enum {
    RB_DEEZER_SOURCE_ARTIST_ID_COL = 0,
    RB_DEEZER_SOURCE_ARTIST_NAME_COL,
    RB_DEEZER_SOURCE_NUM_ARTIST_COLS,
};

enum {
    RB_DEEZER_SOURCE_ALBUM_ID_COL = 0,
    RB_DEEZER_SOURCE_ALBUM_TITLE_COL,
    RB_DEEZER_SOURCE_NUM_ALBUM_COLS,
};

struct _RBDeezerSource {
    RBSource parent;
    RBDeezerPlugin* plugin;

    // Search page
    RBSearchEntry* search_entry;
    RBEntryView* entry_view;
    GtkTreeView* artist_tree_view;
    GtkTreeView* album_tree_view;
    GtkListStore* artist_list_store;
    GtkListStore* album_list_store;
    GtkPaned* paned; 

    // Login page
    WebKitWebView* login_web_view;
};

/// Hoist the function definitions me hearties
RBEntryView* rb_deezer_source_get_entry_view (RBSource *source);
static void rb_deezer_source_clear(RBDeezerSource* deezer_source);
static void rb_deezer_source_set_track_component(JsonNode* node,
                                                guint rhythmdb_prop,
                                                RhythmDB* db,
                                                RhythmDBEntry* entry);
static void rb_deezer_source_add_track(RBDeezerSource* deezer_source, 
                                       RhythmDB* db, 
                                       RhythmDBEntryType* entry_type,
                                       RhythmDBQueryModel* model,
                                       JsonNode* track_json);
static void rb_deezer_source_set_tracks(RBDeezerSource* deezer_source, 
                                        JsonArray* tracks);
static void rb_deezer_source_link_signals(RBDeezerSource* deezer_source);
static void rb_deezer_source_search_entry_cb(RBSearchEntry* entry,
                                             gchar* text,
                                             gpointer user_data);

RBEntryView* rb_deezer_source_get_entry_view (RBSource *source) {
    return RB_DEEZER_SOURCE(source)->entry_view;
}

/**
 * Clear down all the widgets
 */ 
static void rb_deezer_source_clear(RBDeezerSource* deezer_source) {
    gtk_container_foreach(
        GTK_CONTAINER(deezer_source), 
        (GtkCallback)gtk_widget_destroy, 
        NULL
    );
}

/**
 * Copy a value onto a RhythmDBEntry from a JsonNode
 */ 
static void rb_deezer_source_set_track_component(JsonNode* node,
                                                 guint rhythmdb_prop,
                                                 RhythmDB* db,
                                                 RhythmDBEntry* entry) {
    GValue val = G_VALUE_INIT;
    json_node_get_value(node, &val);
    rhythmdb_entry_set(db, entry, rhythmdb_prop, &val);
    g_value_unset(&val);
}

/**
 * Adds a track to the given model.
 * Creates the track in the database if required.
 */ 
static void rb_deezer_source_add_track(RBDeezerSource* deezer_source, 
                                       RhythmDB* db, 
                                       RhythmDBEntryType* entry_type,
                                       RhythmDBQueryModel* model,
                                       JsonNode* track_json) {
    // Needs to be a track 
    JsonObject* track_json_obj = json_node_get_object(track_json);                                          
    const char* type = json_object_get_string_member(track_json_obj, "type");
    if (strcmp(type, "track") != 0) {
        return;
    }

    // Needs to be readable
    if (!json_object_get_boolean_member(track_json_obj, "readable")) {
        return;
    }

    // Build the URL from the track ID
    int id = json_object_get_int_member(track_json_obj, "id");
    char* stream_url = g_strdup_printf("dzmedia:///track/%d", id);

    JsonObject* artist_obj = json_object_get_object_member(track_json_obj, "artist");
    JsonObject* album_obj = json_object_get_object_member(track_json_obj, "album");

    // Already got this one
    RhythmDBEntry* entry = rhythmdb_entry_lookup_by_location(db, stream_url);
    if (entry == NULL) {
        entry = rhythmdb_entry_new(db, entry_type, stream_url);
        rb_deezer_source_set_track_component(
            json_object_get_member(artist_obj, "name"),
            RHYTHMDB_PROP_ARTIST, 
            db, entry
        );

        rb_deezer_source_set_track_component(
            json_object_get_member(track_json_obj, "title"),
            RHYTHMDB_PROP_TITLE,
            db, entry
        );

        // Deezer returns us, Rhythmbox expects ns
        rb_deezer_source_set_track_component(
            json_object_get_member(track_json_obj, "duration"),
            RHYTHMDB_PROP_DURATION,
            db, entry
        );

        rb_deezer_source_set_track_component(
            json_object_get_member(album_obj, "cover"),
            RHYTHMDB_PROP_MUSICBRAINZ_ALBUMID,
            db, entry
        );

        rb_deezer_source_set_track_component(
            json_object_get_member(album_obj, "title"), 
            RHYTHMDB_PROP_ALBUM, 
            db, entry
        );

        if (json_object_has_member(track_json_obj, "track_position")) {
            rb_deezer_source_set_track_component(
                json_object_get_member(track_json_obj, "track_position"), 
                RHYTHMDB_PROP_TRACK_NUMBER, 
                db, entry
            );
        }
        
        rhythmdb_commit(db);
    }
    
    rhythmdb_query_model_add_entry(model, entry, -1);

    g_free(stream_url);
}

/**
 * Traverse Deezer API response and add to given query model
 */
static void rb_deezer_source_set_tracks(RBDeezerSource* deezer_source, 
                                        JsonArray* tracks) { 
    RBShell* shell;
    RhythmDB* db;
    RhythmDBEntryType* entry_type;
    g_object_get(deezer_source, 
        "shell", &shell,
        "entry-type", &entry_type,
        NULL);
    g_object_get(shell, 
        "db", &db, 
        NULL);

    RhythmDBQueryModel* model = rhythmdb_query_model_new_empty(db);
    for (uint i = 0; i<json_array_get_length(tracks); i++) {
        JsonNode* data_element = json_array_get_element(tracks, i);
        rb_deezer_source_add_track(deezer_source, 
            db, 
            entry_type, 
            model, 
            data_element
        );
    }

    rb_entry_view_set_model(deezer_source->entry_view, model);

    g_object_unref(shell);
    g_object_unref(db);
}

/**
 * Notebook with 2 tabs (not user selectable)
 * 
 */ 
static void rb_deezer_source_create_ui(RBDeezerSource* deezer_source) {
    RBShell* shell;
    RBShellPlayer* shell_player;
    RhythmDB* db;
    g_object_get(deezer_source, 
        "shell", &shell,
        NULL);
    g_object_get(shell, 
        "db", &db, 
        "shell_player", &shell_player,
        NULL);

    // Search bar
    RBSearchEntry* se = rb_search_entry_new(false);

    // EntryView shows track listings
    RBEntryView* ev = rb_entry_view_new(
        db,
        G_OBJECT(shell_player),
        true, 
        false 
    );
    rb_entry_view_append_column(ev, RB_ENTRY_VIEW_COL_TRACK_NUMBER, true);
    rb_entry_view_append_column(ev, RB_ENTRY_VIEW_COL_TITLE, true);
    rb_entry_view_append_column(ev, RB_ENTRY_VIEW_COL_ARTIST, true);
    rb_entry_view_append_column(ev, RB_ENTRY_VIEW_COL_ALBUM, true);
    rb_entry_view_append_column(ev, RB_ENTRY_VIEW_COL_DURATION, true);

    // Show artist and album search results
    GtkTreeView* artist_tree_view = GTK_TREE_VIEW(gtk_tree_view_new());
    gtk_tree_view_insert_column_with_attributes(
        artist_tree_view, 
        -1,
        _("Artist"), 
        gtk_cell_renderer_text_new(),
        "text", RB_DEEZER_SOURCE_ARTIST_NAME_COL,
        NULL
    );

    GtkListStore* artist_list_store  = gtk_list_store_new(RB_DEEZER_SOURCE_NUM_ARTIST_COLS, G_TYPE_INT64, G_TYPE_STRING);
    gtk_tree_view_set_model(artist_tree_view, GTK_TREE_MODEL(artist_list_store));

    // Got to be in a scroll window
    GtkScrolledWindow* artist_scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new(NULL, NULL));
    gtk_container_add(GTK_CONTAINER(artist_scroll), GTK_WIDGET(artist_tree_view));

    GtkTreeView* album_tree_view = GTK_TREE_VIEW(gtk_tree_view_new());
    gtk_tree_view_insert_column_with_attributes(
        album_tree_view, 
        -1, 
        _("Album"),
        gtk_cell_renderer_text_new(), 
        "text", RB_DEEZER_SOURCE_ALBUM_TITLE_COL,
        NULL
    );

    GtkScrolledWindow* album_scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new(NULL, NULL));
    gtk_container_add(GTK_CONTAINER(album_scroll), GTK_WIDGET(album_tree_view));

    GtkListStore* album_list_store = gtk_list_store_new(RB_DEEZER_SOURCE_NUM_ALBUM_COLS, G_TYPE_INT64, G_TYPE_STRING);
    gtk_tree_view_set_model(album_tree_view, GTK_TREE_MODEL(album_list_store));

    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(artist_tree_view), GTK_SELECTION_SINGLE);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(album_tree_view), GTK_SELECTION_SINGLE);

    // WebView for login
    WebKitWebView* login_web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());

    // Put the property views in a H box 
    GtkBox* box_prop_views = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
    gtk_box_set_homogeneous(box_prop_views, true);
    gtk_box_pack_start(box_prop_views, GTK_WIDGET(artist_scroll), true, true, 0);
    gtk_box_pack_start(box_prop_views, GTK_WIDGET(album_scroll), true, true, 0);
    
    // Put the property views box and the entry view in a split pane
    GtkPaned* paned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_VERTICAL));
    gtk_paned_set_position(paned, 250);
    gtk_paned_add1(paned, GTK_WIDGET(box_prop_views));
    gtk_paned_add2(paned, GTK_WIDGET(ev));

    // Put everything on the search page into a v box
    GtkBox* box_search = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_box_pack_end(box_search, GTK_WIDGET(paned), true, true, 0);
    gtk_box_pack_end(box_search, GTK_WIDGET(se), false, true, 0);

    // Put the search and login on tabs of a notebook, but hide the tabs
    // as we'll be switching programatically
    GtkNotebook* notebook = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_notebook_append_page(notebook, GTK_WIDGET(box_search), NULL);
    gtk_notebook_append_page(notebook, GTK_WIDGET(login_web_view), NULL);
    gtk_notebook_set_show_tabs(notebook, false);

    // Put the notebook on the source page
    gtk_box_pack_end(GTK_BOX(deezer_source), GTK_WIDGET(notebook), true, true, 0);

    // Finito
    gtk_widget_show_all(GTK_WIDGET(deezer_source));

    // Hold onto references that we need later
    deezer_source->entry_view = ev;
    deezer_source->search_entry = se;
    deezer_source->album_tree_view = album_tree_view;
    deezer_source->artist_tree_view = artist_tree_view;
    deezer_source->login_web_view = login_web_view;
    deezer_source->album_list_store = album_list_store;
    deezer_source->artist_list_store = artist_list_store;


    g_object_unref(shell);
    g_object_unref(db);
}

static void rb_deezer_source_search_tracks_cb(JsonNode* node, void* user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
    // Root element is an object, search has a 'data' member which is an array of tracks
    JsonObject* obj = json_node_get_object(node);
    JsonArray* tracks = json_object_get_array_member(obj, "data");
    rb_deezer_source_set_tracks(deezer_source, tracks);
}

static void rb_deezer_source_show_album_tracks_cb(JsonNode* node, void* user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);

    // Root element is an Album object, which has a member 'tracks' which is an array of the tracks..
    JsonObject* album_obj = json_node_get_object(node);
    JsonObject* tracks_obj = json_object_get_object_member(album_obj, "tracks");
    JsonArray* tracks = json_object_get_array_member(tracks_obj, "data");

    // These tracks DON't have the album or position set on them as a child, so set those here
    for (uint i = 0; i<json_array_get_length(tracks); i++) {
        JsonObject* track = json_array_get_object_element(tracks, i);
        json_object_set_object_member(track, "album", album_obj);
        json_object_set_int_member(track, "track_position", i+1);
    }

    rb_deezer_source_set_tracks(deezer_source, tracks);
}

static void rb_deezer_source_search_artists_cb(JsonNode* node, void* user_data) {
    // "id": "13",
    //   "name": "Eminem",
    //   "link": "https://www.deezer.com/artist/13",
    //   "picture": "https://api.deezer.com/artist/13/image",
    //   "picture_small": "https://e-cdns-images.dzcdn.net/images/artist/0707267475580b1b82f4da20a1b295c6/56x56-000000-80-0-0.jpg",
    //   "picture_medium": "https://e-cdns-images.dzcdn.net/images/artist/0707267475580b1b82f4da20a1b295c6/250x250-000000-80-0-0.jpg",
    //   "picture_big": "https://e-cdns-images.dzcdn.net/images/artist/0707267475580b1b82f4da20a1b295c6/500x500-000000-80-0-0.jpg",
    //   "picture_xl": "https://e-cdns-images.dzcdn.net/images/artist/0707267475580b1b82f4da20a1b295c6/1000x1000-000000-80-0-0.jpg",
    //   "nb_album": 42,
    //   "nb_fan": 11233526,
    //   "radio": true,
    //   "tracklist": "https://api.deezer.com/artist/13/top?limit=50",
    //   "type": "artist"
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);

    JsonObject* obj = json_node_get_object(node);
    JsonArray* artists = json_object_get_array_member(obj, "data");
    gtk_list_store_clear(deezer_source->artist_list_store);
    GtkTreeIter iter;
    for (uint i = 0; i<json_array_get_length(artists); i++) {
        JsonObject* artist_obj = json_array_get_object_element(artists, i);
        const gchar* name = json_object_get_string_member(artist_obj, "name");
        gint64 id = json_object_get_int_member(artist_obj, "id");
        gtk_list_store_append(deezer_source->artist_list_store, &iter);
        gtk_list_store_set(deezer_source->artist_list_store, &iter, 
            RB_DEEZER_SOURCE_ARTIST_ID_COL, id, 
            RB_DEEZER_SOURCE_ARTIST_NAME_COL, name, 
            -1);
    }
}

static void rb_deezer_source_search_albums_cb(JsonNode* node, void* user_data) {
    // "id": "53227232",
    //   "title": "Revival",
    //   "link": "https://www.deezer.com/album/53227232",
    //   "cover": "https://api.deezer.com/album/53227232/image",
    //   "cover_small": "https://e-cdns-images.dzcdn.net/images/cover/516a8154a838930774a98a1f5cf92f1a/56x56-000000-80-0-0.jpg",
    //   "cover_medium": "https://e-cdns-images.dzcdn.net/images/cover/516a8154a838930774a98a1f5cf92f1a/250x250-000000-80-0-0.jpg",
    //   "cover_big": "https://e-cdns-images.dzcdn.net/images/cover/516a8154a838930774a98a1f5cf92f1a/500x500-000000-80-0-0.jpg",
    //   "cover_xl": "https://e-cdns-images.dzcdn.net/images/cover/516a8154a838930774a98a1f5cf92f1a/1000x1000-000000-80-0-0.jpg",
    //   "genre_id": 116,
    //   "nb_tracks": 19,
    //   "record_type": "album",
    //   "tracklist": "https://api.deezer.com/album/53227232/tracks",
    //   "explicit_lyrics": true,
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);

    JsonObject* obj = json_node_get_object(node);
    JsonArray* albums = json_object_get_array_member(obj, "data");
    
    GtkTreeIter iter;
    gtk_list_store_clear(deezer_source->album_list_store);
    for (uint i = 0; i<json_array_get_length(albums); i++) {
        JsonObject* artist_obj = json_array_get_object_element(albums, i);
        const gchar* title = json_object_get_string_member(artist_obj, "title");
        gint64 id = json_object_get_int_member(artist_obj, "id");
        gtk_list_store_append(deezer_source->album_list_store, &iter);
        gtk_list_store_set(deezer_source->album_list_store, &iter, 
            RB_DEEZER_SOURCE_ALBUM_ID_COL, id, 
            RB_DEEZER_SOURCE_ALBUM_TITLE_COL, title, 
            -1);
    }
}

/**
 * Search Deezer and update all views when the search bar is changed
 */ 
static void rb_deezer_source_search_entry_cb(RBSearchEntry* entry,
                                             gchar* text,
                                             gpointer user_data) {
    if (text == NULL) {
        return;
    }

    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
    RBDeezerPlugin* plugin;
    g_object_get(deezer_source, "plugin", &plugin, NULL);
    // http://api.deezer.com/search/artist?q=eminem
    // http://api.deezer.com/search/album?q=eminem
    // http://api.deezer.com/search/track?q=eminem

    rb_deezer_plugin_api_call(
        plugin, 
        rb_deezer_source_search_tracks_cb, 
        deezer_source, 
        "search/track", 
        "q", text,
        NULL
    );

    rb_deezer_plugin_api_call(
        plugin, 
        rb_deezer_source_search_artists_cb, 
        deezer_source, 
        "search/artist", 
        "q", text,
        NULL
    );

    rb_deezer_plugin_api_call(
        plugin, 
        rb_deezer_source_search_albums_cb, 
        deezer_source, 
        "search/album", 
        "q", text,
        NULL
    );

    g_object_unref(plugin);
}

static void rb_deezer_source_artist_selected_cb(GtkTreeView* artist_tree_view, gpointer user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
    
    // http://api.deezer.com/search/artist?q=eminem
    // http://api.deezer.com/search/album?q=eminem
    // http://api.deezer.com/search/track?q=eminem
    
    GtkTreeIter iter;
    GtkTreeModel* model;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(deezer_source->artist_tree_view), &model, &iter)) {
        return;
    }

    gint64 id;
    gtk_tree_model_get(model, &iter, RB_DEEZER_SOURCE_ARTIST_ID_COL, &id, -1);

    RBDeezerPlugin* plugin;
    g_object_get(deezer_source, "plugin", &plugin, NULL);

    // Populate the albums view with all the artist's albums
    rb_deezer_plugin_api_call(
        plugin, 
        rb_deezer_source_search_albums_cb, 
        deezer_source, 
        g_strdup_printf("artist/%ld/albums", id),
        NULL
    );

    // Populate the tracks view with top tracks
    rb_deezer_plugin_api_call(
        plugin, 
        rb_deezer_source_search_tracks_cb, 
        deezer_source, 
        g_strdup_printf("artist/%ld/top", id),
        NULL
    );

    g_object_unref(plugin);
}

static void rb_deezer_source_album_selected_cb(GtkTreeView* album_tree_view, gpointer user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
    
    GtkTreeIter iter;
    GtkTreeModel* model;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(deezer_source->album_tree_view), &model, &iter)) {
        return;
    }

    gint64 id;
    gtk_tree_model_get(model, &iter, RB_DEEZER_SOURCE_ALBUM_ID_COL, &id, -1);

    RBDeezerPlugin* plugin;
    g_object_get(deezer_source, "plugin", &plugin, NULL);

    // Populate the tracks view with the album contents
    rb_deezer_plugin_api_call(
        plugin, 
        rb_deezer_source_show_album_tracks_cb, 
        deezer_source, 
        g_strdup_printf("album/%ld", id),
        NULL
    );

    g_object_unref(plugin);
}

/**
 * Connect up the UI elements to things they need to do
 */ 
static void rb_deezer_source_link_signals(RBDeezerSource* deezer_source) {
    g_signal_connect(deezer_source->search_entry, 
        "search", 
        (GCallback)rb_deezer_source_search_entry_cb, 
        deezer_source
    );

    g_signal_connect(deezer_source->artist_tree_view, 
        "cursor-changed", 
        (GCallback)rb_deezer_source_artist_selected_cb, 
        deezer_source
    ); 

    g_signal_connect(deezer_source->album_tree_view, 
        "cursor-changed", 
        (GCallback)rb_deezer_source_album_selected_cb, 
        deezer_source
    ); 
}

/*************** Boilerplatey class stuff ********************/
static void rb_deezer_source_get_property(GObject* object, 
                                        guint property_id, 
                                        GValue* value, 
                                        GParamSpec* pspec) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(object);
    switch (property_id) {
        case RB_DEEZER_SOURCE_PROP_PLUGIN:
            g_value_set_object(value, deezer_source->plugin);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            break;
    }
}

static void rb_deezer_source_set_property(GObject* object, 
                                        guint property_id, 
                                        const GValue* value, 
                                        GParamSpec* pspec) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(object);
    switch (property_id) {
        case RB_DEEZER_SOURCE_PROP_PLUGIN:
            deezer_source->plugin = g_value_get_object(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            break;
    }
}

static void rb_deezer_source_init(RBDeezerSource* src) {
    // Most things need to happen after construction properties are set (i.e. in rb_deezer_source_constructed)
}

static void rb_deezer_source_constructed(GObject* src) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(src);
    rb_deezer_source_create_ui(deezer_source);
    rb_deezer_source_link_signals(deezer_source);
}

static void rb_deezer_source_class_init(RBDeezerSourceClass* cls) {
    GObjectClass* obj_class = G_OBJECT_CLASS(cls);
    obj_class->set_property = rb_deezer_source_set_property;
    obj_class->get_property = rb_deezer_source_get_property;
    obj_class->constructed = rb_deezer_source_constructed;

    // Override methods
    RBSourceClass* src_class = RB_SOURCE_CLASS(cls);
    src_class->get_entry_view = rb_deezer_source_get_entry_view;    

    g_object_class_install_property(
        obj_class, 
        RB_DEEZER_SOURCE_PROP_PLUGIN, 
        g_param_spec_object(
            "plugin", "Plugin", "plugin", 
            rb_deezer_plugin_get_type(), 
            G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE
        )
    );
}

static void rb_deezer_source_class_finalize(RBDeezerSourceClass* cls) {}

G_DEFINE_DYNAMIC_TYPE(
    RBDeezerSource, 
    rb_deezer_source, 
    RB_TYPE_SOURCE
);

// Required for PEAS plugin system
void _rb_deezer_source_register_type(GTypeModule* type_module) {
    rb_deezer_source_register_type(type_module);
}
