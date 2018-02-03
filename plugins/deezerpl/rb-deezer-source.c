#include "rb-deezer-source.h"

#include <string.h>
#include <gtk/gtk.h>
#include <glib/gi18n-lib.h>
#include <deezer-track.h>
#include <deezer-offline.h>
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
#include "rb-play-order.h"
#include "rb-display-page.h"
#include "yuarel.h"
#include "rb-builder-helpers.h"

#define OAUTH_URL "https://connect.deezer.com/oauth/auth.php?"\
                "app_id=" APP_ID\
                "&redirect_uri=http://127.0.0.1:8080"\
                "&perms=basic_access,offline_access"\
                "&response_type=token"\

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

    GSettings* settings;

    // Notebook for switching between login & search
    GtkNotebook* notebook;
    gint notebook_tab_login;
    gint notebook_tab_search;

    // Search page
    RBSearchEntry* search_entry;
    GtkSwitch* offline_mode_switch;
    RBEntryView* entry_view;
    GtkTreeView* artist_tree_view;
    GtkTreeView* album_tree_view;
    GtkListStore* artist_list_store;
    GtkListStore* album_list_store;
    GtkTreeSelection* artist_tree_selection;
    GtkTreeSelection* album_tree_selection;
    GtkPaned* paned; 

    // Signal handlers 
    gulong signal_handler_artist_selection;
    gulong signal_handler_album_selection;
    gulong signal_handler_search;
    gulong signal_handler_artist_activated;
    gulong signal_handler_album_activated;
    gulong signal_handler_access_token_change;
    gulong signal_handler_offline_mode_switch;

    // Login page
    WebKitWebView* login_web_view;

    // Popup menu
    GMenuModel* popup_menu_model;
};

/// Hoist the function definitions me hearties
RBEntryView* rb_deezer_source_get_entry_view (RBSource *source);
static void rb_deezer_source_set_track_component(JsonNode* node,
                                                guint rhythmdb_prop,
                                                RhythmDB* db,
                                                RhythmDBEntry* entry);
static void rb_deezer_source_add_track(RBDeezerSource* deezer_source, 
                                       RhythmDB* db, 
                                       RhythmDBEntryType* entry_type,
                                       RhythmDBQueryModel* model,
                                       JsonNode* track_json,
                                       gboolean recreate);
static void rb_deezer_source_set_tracks(RBDeezerSource* deezer_source, 
                                        JsonArray* tracks, 
                                        gboolean recreate);
static void rb_deezer_source_link_signals(RBDeezerSource* deezer_source);
static void rb_deezer_source_search_entry_cb(RBSearchEntry* entry,
                                             gchar* text,
                                             gpointer user_data);
static void rb_deezer_source_web_view_load_changed(WebKitWebView* wv, 
                                                   WebKitLoadEvent evt, 
                                                   gpointer user_data);


RBEntryView* rb_deezer_source_get_entry_view (RBSource *source) {
    return RB_DEEZER_SOURCE(source)->entry_view;
}

/**
 * Copy a value onto a RhythmDBEntry from a JsonNode
 */ 
static void rb_deezer_source_set_track_component(JsonNode* node,
                                                 RhythmDBPropType rhythmdb_prop,
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
                                       JsonNode* track_json,
                                       gboolean recreate) {
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

    RhythmDBEntry* entry = rhythmdb_entry_lookup_by_location(db, stream_url);
    
    if (recreate && entry != NULL) {
        rhythmdb_entry_delete(db, entry);
        entry = NULL;
    }

    if (entry == NULL) {
        entry = rhythmdb_entry_new(db, entry_type, stream_url);

        {
            // I'm abusing the musicbrainz ID to store the Deezer ID
            // this code could probably be shorter somehow
            char buf[100];
            snprintf(buf, 100, "%d", id);
            GValue val = G_VALUE_INIT;
            g_value_init(&val, G_TYPE_STRING);
            g_value_set_string(&val, buf);
            rhythmdb_entry_set(db, entry, RHYTHMDB_PROP_MUSICBRAINZ_TRACKID, &val);
            g_value_unset(&val);
        }
        

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
                                        JsonArray* tracks, 
                                        gboolean recreate) { 
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
            data_element,
            recreate
        );
    }

    // Set the model as the property on RBSource so that auto moving to next works
    g_object_set(RB_SOURCE(deezer_source), "query-model", model, NULL);

    rb_entry_view_set_model(deezer_source->entry_view, model);

    g_object_unref(shell);
    g_object_unref(db);
}

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

    // Offline mode switch
    GtkSwitch* offline_mode_switch = GTK_SWITCH(gtk_switch_new());
    GtkLabel* offline_mode_label = GTK_LABEL(gtk_label_new(_("Offline mode")));

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
    webkit_web_view_load_uri(login_web_view, OAUTH_URL);

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

    // Ptu search bar and offline switch in h box
    GtkBox* search_and_offline = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
    gtk_box_pack_start(search_and_offline, GTK_WIDGET(se), false, true, 0);
    gtk_box_pack_start(search_and_offline, GTK_WIDGET(offline_mode_label), false, false, 0);
    gtk_box_pack_start(search_and_offline, GTK_WIDGET(offline_mode_switch), false, false, 0);

    // Put everything on the search page into a v box
    GtkBox* box_search = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_box_pack_end(box_search, GTK_WIDGET(paned), true, true, 0);
    gtk_box_pack_end(box_search, GTK_WIDGET(search_and_offline), false, true, 0);

    // Put the search and login on tabs of a notebook, but hide the tabs
    // as we'll be switching programatically
    GtkNotebook* notebook = GTK_NOTEBOOK(gtk_notebook_new());
    gint notebook_tab_search = gtk_notebook_append_page(notebook, GTK_WIDGET(box_search), NULL);
    gint notebook_tab_login = gtk_notebook_append_page(notebook, GTK_WIDGET(login_web_view), NULL);
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
    deezer_source->album_list_store = album_list_store;
    deezer_source->artist_list_store = artist_list_store;
    deezer_source->artist_tree_selection = gtk_tree_view_get_selection(artist_tree_view);
    deezer_source->album_tree_selection = gtk_tree_view_get_selection(album_tree_view);
    deezer_source->login_web_view = login_web_view;
    deezer_source->notebook = notebook;
    deezer_source->notebook_tab_login = notebook_tab_login;
    deezer_source->notebook_tab_search = notebook_tab_search;
    deezer_source->offline_mode_switch = offline_mode_switch;

    g_object_unref(shell);
    g_object_unref(db);
}

static void rb_deezer_source_search_tracks_cb(JsonNode* node, void* user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
    // Root element is an object, search has a 'data' member which is an array of tracks
    JsonObject* obj = json_node_get_object(node);
    JsonArray* tracks = json_object_get_array_member(obj, "data");
    rb_deezer_source_set_tracks(deezer_source, tracks, false);
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

    // Need to recreate the tracks in the database since they only have track numbers via this query
    rb_deezer_source_set_tracks(deezer_source, tracks, true);
}

static void rb_deezer_source_search_artists_cb(JsonNode* node, void* user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);

    JsonObject* obj = json_node_get_object(node);
    JsonArray* artists = json_object_get_array_member(obj, "data");

    // Don't try to deal with selectin while we're rebuilding the list
    g_signal_handler_block(deezer_source->artist_tree_selection, deezer_source->signal_handler_artist_selection);

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

    g_signal_handler_unblock(deezer_source->artist_tree_selection, deezer_source->signal_handler_artist_selection);
}

static void rb_deezer_source_search_albums_cb(JsonNode* node, void* user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);

    JsonObject* obj = json_node_get_object(node);
    JsonArray* albums = json_object_get_array_member(obj, "data");
    GtkTreeIter iter;
    g_signal_handler_block(deezer_source->album_tree_selection, deezer_source->signal_handler_album_selection);
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
    g_signal_handler_unblock(deezer_source->album_tree_selection, deezer_source->signal_handler_album_selection);
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

static void rb_deezer_source_artist_selected_cb(GtkTreeSelection* artist_tree_view_selection, gpointer user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
    
    GtkTreeIter iter;
    GtkTreeModel* model;
    if (!gtk_tree_selection_get_selected(artist_tree_view_selection, &model, &iter)) {
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

    // Populate the tracks view with top tracks for the artist
    rb_deezer_plugin_api_call(
        plugin, 
        rb_deezer_source_search_tracks_cb, 
        deezer_source, 
        g_strdup_printf("artist/%ld/top", id),
        NULL
    );

    g_object_unref(plugin);
}

static void rb_deezer_source_album_selected_cb(GtkTreeSelection* album_tree_view_selection, gpointer user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
    
    GtkTreeIter iter;
    GtkTreeModel* model;
    if (!gtk_tree_selection_get_selected(album_tree_view_selection, &model, &iter)) {
        return;
    }

    gint64 id;
    gchar* title;
    gtk_tree_model_get(model, &iter, 
        RB_DEEZER_SOURCE_ALBUM_ID_COL, &id, 
        RB_DEEZER_SOURCE_ALBUM_TITLE_COL, &title,
        -1);

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
 * Start playing from the entry view when a row is activated.
 * NB should really wait for search to come back first 
 */ 
static void rb_deezer_source_artist_album_activated_cb(GtkTreeView* artist_tree_view, 
                                                 GtkTreePath* path,
                                                 GtkTreeViewColumn* column, 
                                                 gpointer user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
    RBShell* shell;
    RBShellPlayer* shell_player;
    RhythmDBQueryModel* model;
    
    g_object_get(deezer_source, "shell", &shell, NULL);
    g_object_get(shell, "shell_player", &shell_player,NULL);
    g_object_get(deezer_source->entry_view, "model", &model, NULL);

    GtkTreePath* path2 = gtk_tree_path_new_first();
    RhythmDBEntry* entry = rhythmdb_query_model_tree_path_to_entry(model, path2);
	rb_shell_player_play_entry(shell_player, entry, RB_SOURCE(deezer_source));

    g_object_unref(shell);
    g_object_unref(shell_player);
    g_object_unref(model);
}

/**
 * Show either the search or login notebook tabs depending if we have an access token
 */ 
static void rb_deezer_source_update_page(RBDeezerSource* deezer_source) {
    gchar* access_token = g_settings_get_string(deezer_source->settings, "access-token");
    if (strlen(access_token) > 0) {
        gtk_notebook_set_current_page(deezer_source->notebook, deezer_source->notebook_tab_search);
    } else {
        gtk_notebook_set_current_page(deezer_source->notebook, deezer_source->notebook_tab_login);
        webkit_web_view_load_uri(deezer_source->login_web_view, OAUTH_URL);
    }
}

static void rb_deezer_source_access_token_change_cb(GSettings* settings, 
                                                    gchar* key, 
                                                    gpointer user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
    rb_deezer_source_update_page(deezer_source);
}

static void rb_deezer_source_offline_mode_switch(GObject    *gobject,
                                                 GParamSpec *pspec,
                                                 gpointer    user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
    RBDeezerPlugin* plugin;
    g_object_get(deezer_source, "plugin", &plugin, NULL);
    gboolean offline_mode = gtk_switch_get_active(deezer_source->offline_mode_switch);

	dz_error_t err = dz_connect_offline_mode(plugin->handle, NULL, NULL, offline_mode);
	if (err != DZ_ERROR_NO_ERROR) {
		rb_debug("Error forcing setting offline mode to %d: %d", offline_mode, err);
	}

    // Switch the search to only look for online stuff eh

    g_object_unref(plugin);
}

static void rb_deezer_source_show_popup(RBEntryView* entry_view,
                                        gboolean over_entry,
                                        RBDeezerSource* deezer_source)
{
	if (!over_entry) {
        return;
    }

    if (deezer_source->popup_menu_model == NULL) {
        RBDeezerPlugin* plugin;
        g_object_get(deezer_source, "plugin", &plugin, NULL);

        GtkBuilder* builder = rb_builder_load_plugin_file(G_OBJECT(plugin), "deezer-popup.ui", NULL);
        deezer_source->popup_menu_model = G_MENU_MODEL(gtk_builder_get_object(builder, "deezer-popup"));
        g_object_ref(deezer_source->popup_menu_model);
        g_object_unref(plugin);
        g_object_unref(builder);
    }

    GtkMenu* menu = GTK_MENU(gtk_menu_new_from_model(deezer_source->popup_menu_model));
    // This line is important for some shitty reason
    gtk_menu_attach_to_widget (GTK_MENU (menu), GTK_WIDGET (deezer_source), NULL);
	gtk_menu_popup(menu,
			NULL,
			NULL,
			NULL,
			NULL,
			3,
			gtk_get_current_event_time());
}

static void rb_deezer_source_save_offline_entry(RhythmDBEntry* entry,
                                                RBDeezerSource* deezer_source) {
    const char* track_id = rhythmdb_entry_get_string(entry, RHYTHMDB_PROP_MUSICBRAINZ_TRACKID);
    char path[100];
    snprintf(path, 100, "/dzlocal/tracklist/track/%s", track_id);

    char tracklist[100];
    snprintf(tracklist, 100, "{ \"tracks\" : [ %s ] }", track_id);

    rb_debug("Requesting offline sync path [%s] content [%s]", path, tracklist);
    dz_error_t err = dz_offline_synchronize(deezer_source->plugin->handle, 
        NULL, 
        NULL, 
        path, 
        "0",
        tracklist
    );

    if (err != DZ_ERROR_NO_ERROR) {
        rb_debug("Error requesting offline sync %s", dz_error_string(err));
    }
}

static void rb_deezer_source_save_offline_cb(GSimpleAction *action, 
                                             GVariant *parameter, 
                                             gpointer data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(data);
    GList* selected = rb_entry_view_get_selected_entries(deezer_source->entry_view);
    g_list_foreach(selected, (GFunc)rb_deezer_source_save_offline_entry, deezer_source);
}

/**
 * Connect up the UI elements to things they need to do
 * Keep references to the signal handlers in case we need to stop them for some reason
 */ 
static void rb_deezer_source_link_signals(RBDeezerSource* deezer_source) {
    deezer_source->signal_handler_search = g_signal_connect(deezer_source->search_entry, 
        "search", 
        G_CALLBACK(rb_deezer_source_search_entry_cb),
        deezer_source
    );
   
    deezer_source->signal_handler_artist_selection = g_signal_connect(deezer_source->artist_tree_selection, 
        "changed", 
        G_CALLBACK(rb_deezer_source_artist_selected_cb), 
        deezer_source
    ); 
    
    deezer_source->signal_handler_album_selection = g_signal_connect(deezer_source->album_tree_selection, 
        "changed", 
        G_CALLBACK(rb_deezer_source_album_selected_cb), 
        deezer_source
    ); 

    deezer_source->signal_handler_artist_activated = g_signal_connect(deezer_source->artist_tree_view,
        "row-activated",
        G_CALLBACK(rb_deezer_source_artist_album_activated_cb),
        deezer_source
    );

    deezer_source->signal_handler_album_activated = g_signal_connect(deezer_source->album_tree_view,
        "row-activated",
        G_CALLBACK(rb_deezer_source_artist_album_activated_cb),
        deezer_source
    );

    deezer_source->signal_handler_access_token_change = g_signal_connect(
		deezer_source->settings, 
		"changed::access-token", G_CALLBACK(rb_deezer_source_access_token_change_cb), 
		deezer_source
	);

    g_signal_connect(deezer_source->login_web_view, 
        "load-changed", 
        G_CALLBACK(rb_deezer_source_web_view_load_changed), 
        deezer_source
    );

    deezer_source->signal_handler_offline_mode_switch = g_signal_connect(deezer_source->offline_mode_switch,
        "notify::active", 
        G_CALLBACK(rb_deezer_source_offline_mode_switch),
        deezer_source
    );

    g_signal_connect(deezer_source->entry_view,
        "show_popup",
        G_CALLBACK(rb_deezer_source_show_popup), 
        deezer_source
    );

    // Popup menu actions
    // This is fucking infuriating
    GActionEntry actions[] = {
		{ "save-deezer-offline", rb_deezer_source_save_offline_cb, NULL, NULL, NULL }
	};

    RBShell* shell;
    g_object_get(deezer_source, "shell", &shell, NULL);
    // It's important to use this method, fuck knows why
    _rb_add_display_page_actions (G_ACTION_MAP (g_application_get_default ()),
				      G_OBJECT (shell),
				      actions,
				      G_N_ELEMENTS (actions));
    g_object_unref(shell);
}

static void rb_deezer_source_logout_pressed(GtkButton* button, gpointer user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
    rb_debug("Clearing Deezer access token");
    g_settings_set_string(deezer_source->settings, "access-token", "");
}


static void rb_deezer_source_update_logout_sensitive(GSettings* settings, gchar* key, gpointer user_data) {
    gchar* access_token = g_settings_get_string(settings, "access-token");
    gboolean is_logged_in = strlen(access_token) > 0;
    gtk_widget_set_sensitive(GTK_WIDGET(user_data), is_logged_in);
}

/**
 * Preferences page just lets you log out in case you want to switch accounts
 */ 
static GtkWidget* rb_deezer_source_get_config_widget(RBDisplayPage *page, RBShellPreferences *prefs) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(page);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* logout_button = gtk_button_new_with_label(_("Log out of Deezer"));

    g_signal_connect(
        logout_button, 
        "clicked", 
        G_CALLBACK(rb_deezer_source_logout_pressed), 
        page
    );

    rb_deezer_source_update_logout_sensitive(deezer_source->settings, NULL, logout_button);
    g_signal_connect(
        deezer_source->settings, 
        "changed::access-token", 
        G_CALLBACK(rb_deezer_source_update_logout_sensitive), 
        logout_button
    );
    gtk_box_pack_start(GTK_BOX(box), logout_button, false, false, 4);
    return box;
}

/*************** Login stuff *********************************/
static char* query_val(const char* key, char* querystr) {
    if (querystr == NULL) {
        return NULL;
    }
    struct yuarel_param p[5] = {0};
    int n = yuarel_parse_query(querystr, '&', p, 5);
    for (int i=0; i<n; i++) {
        if (strcmp(key, p[i].key) == 0) {
            return p[i].val;
        }
    }
    return NULL;
}

static void rb_deezer_source_show_login_error(RBDeezerSource* deezer_source) {
    RBShell* shell;
    GtkWindow* window;
    g_object_get(deezer_source, 
        "shell", &shell,
        NULL);
    g_object_get(shell, 
        "window", &window, 
        NULL);

    GtkWidget* dlg = gtk_message_dialog_new(
        window, 
        0, 
        GTK_MESSAGE_ERROR,
        GTK_BUTTONS_CLOSE,
        _("You must log into Deezer for Rhythmbox to access tracks")
    );
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_object_unref(shell);
    g_object_unref(window);
}

static void rb_deezer_source_web_view_load_changed(WebKitWebView* wv, 
                                                   WebKitLoadEvent evt, 
                                                   gpointer user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
    
    if (evt != WEBKIT_LOAD_REDIRECTED) {
        return;
    }

    struct yuarel u;
    const gchar* uri = webkit_web_view_get_uri(wv);
    if (yuarel_parse(&u, (char*)uri)) {
        rb_debug("Error parsing redirected uri %s", uri);
        return;
    }

    char* access_token = query_val("access_token", u.fragment);
    if (access_token != NULL) {
        rb_debug("Got access token");
        g_settings_set_string(deezer_source->settings, "access-token", access_token);

        // Get away from the redirect page now we've retrieved the token
        webkit_web_view_load_uri(wv, "google.com"); 
        // Clear all the cookies so if the user presses log out they have to log in again.
        WebKitWebsiteDataManager* man = webkit_web_view_get_website_data_manager(wv);
        webkit_website_data_manager_clear(man, WEBKIT_WEBSITE_DATA_ALL, 0, NULL, NULL, NULL);
    }

    char* error_reason = query_val("error_reason", u.query);
    if (error_reason != NULL) {
        rb_debug("Error logging into Deezer");
        rb_deezer_source_show_login_error(deezer_source);
        webkit_web_view_load_uri(wv, OAUTH_URL);
    }
}

/*************** Boilerplatey class stuff ********************/

static void rb_deezer_source_init(RBDeezerSource* src) {
    src->settings = g_settings_new(G_SETTINGS_SCHEMA);
}

static void rb_deezer_source_constructed(GObject* src) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(src);
    rb_deezer_source_create_ui(deezer_source);
    rb_deezer_source_link_signals(deezer_source);
    rb_deezer_source_update_page(deezer_source);

    deezer_source->settings = g_settings_new("org.gnome.rhythmbox.plugins.deezer");
    gchar* access_token = g_settings_get_string(deezer_source->settings, "access-token");
    rb_debug("Got access token from settings %s", access_token);
}

static void rb_deezer_source_class_init(RBDeezerSourceClass* cls) {
    GObjectClass* obj_class = G_OBJECT_CLASS(cls);
    obj_class->constructed = rb_deezer_source_constructed;

    // Override methods
    RBSourceClass* src_class = RB_SOURCE_CLASS(cls);
    src_class->get_entry_view = rb_deezer_source_get_entry_view;    

    RBDisplayPageClass* display_page_class = RB_DISPLAY_PAGE_CLASS(cls);
    display_page_class->get_config_widget = rb_deezer_source_get_config_widget;
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
