#include "rb-deezer-source.h"

#include <string.h>
#include <webkit2/webkit2.h>
#include <gtk/gtk.h>
#include <glib/gi18n-lib.h>
#include <deezer-track.h>
#include <json-glib/json-glib.h>

#include "rb-deezer-plugin.h"
#include "rb-debug.h"
#include "rb-shell.h"
#include "rb-shell-player.h"
#include "rb-util.h"
#include "rb-source.h"
#include "yuarel.h"

enum {
    RB_DEEZER_SOURCE_PROP_PLUGIN = 1,
};

struct _RBDeezerSource {
    RBSource parent;
    RBDeezerPlugin* plugin;
    RBEntryView* entry_view;
};

static void rb_deezer_source_show_login(RBDeezerSource*);
static void rb_deezer_source_show_search(RBDeezerSource*);
RBEntryView* get_entry_view (RBSource *source);

const char* OAUTH_URL = "https://connect.deezer.com/oauth/auth.php?"
                "app_id=" APP_ID
                "&redirect_uri=http://127.0.0.1:8080"
                "&perms=basic_access,offline_access"
                "&response_type=token";

static void rb_deezer_source_init(RBDeezerSource* src) {}

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

RBEntryView* get_entry_view (RBSource *source) {
    return RB_DEEZER_SOURCE(source)->entry_view;
}

static void rb_deezer_source_class_init(RBDeezerSourceClass* cls) {
    GObjectClass* obj_class = G_OBJECT_CLASS(cls);
    obj_class->set_property = rb_deezer_source_set_property;
    obj_class->get_property = rb_deezer_source_get_property;

    // Override methods
    RBSourceClass* src_class = RB_SOURCE_CLASS(cls);
    src_class->get_entry_view = get_entry_view;    

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

void _rb_deezer_source_register_type(GTypeModule* type_module) {
    rb_deezer_source_register_type(type_module);
}

static void rb_deezer_source_clear(RBDeezerSource* deezer_source) {
    gtk_container_foreach(
        GTK_CONTAINER(deezer_source), 
        (GtkCallback)gtk_widget_destroy, 
        NULL
    );
}

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

static void web_view_load_changed(WebKitWebView* wv, WebKitLoadEvent evt, gpointer user_data) {
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
        RBDeezerPlugin* plugin;
        g_object_get(deezer_source, "plugin", &plugin, NULL);
        g_object_set(plugin, "access-token", access_token, NULL);
        g_object_unref(plugin);
        rb_deezer_source_show_search(deezer_source);
    }

    char* error_reason = query_val("error_reason", u.query);
    if (error_reason != NULL) {
        rb_debug("Error logging into Deezer");
        rb_deezer_source_show_login_error(deezer_source);
        rb_deezer_source_show_login(deezer_source);
    }
}

static void rb_deezer_source_show_login(RBDeezerSource* deezer_source) {
    rb_deezer_source_clear(deezer_source);

    WebKitWebView* wv = g_object_new(webkit_web_view_get_type(), NULL);
    webkit_web_view_load_uri(wv, OAUTH_URL);
    g_signal_connect(
        wv, 
        "load-changed", (GCallback)web_view_load_changed, deezer_source
    );

    gtk_box_pack_start(
        GTK_BOX(deezer_source), 
        GTK_WIDGET(wv), 
        true, 
        true, 
        0
    );
    gtk_widget_show_all(GTK_WIDGET(deezer_source));
}

static void rb_deezer_source_set_track_component(JsonNode* node,
                                                guint rhythmdb_prop,
                                                RhythmDB* db,
                                                RhythmDBEntry* entry) {
    // Query always returns an array
    GValue val = G_VALUE_INIT;
    json_node_get_value(node, &val);
    rhythmdb_entry_set(db, entry, rhythmdb_prop, &val);
    g_value_unset(&val);
}

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
    char stream_url[255];
    snprintf(stream_url, 255, "dzmedia:///track/%d", id);

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
        
        rhythmdb_commit(db);
    }
    
    rhythmdb_query_model_add_entry(model, entry, -1);
}

struct add_results_data {
    RBDeezerSource* deezer_source;
    char* api_results;
};

static void rb_deezer_source_search_api_callback (JsonNode* result, void* user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
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

    // Traverse the data and create entries
    JsonObject* root = json_node_get_object(result);
    RhythmDBQueryModel* model = rhythmdb_query_model_new_empty(db);

    JsonArray* data = json_object_get_array_member(root, "data");
    for (uint i = 0; i<json_array_get_length(data); i++) {
        JsonNode* data_element = json_array_get_element(data, i);
        rb_deezer_source_add_track(deezer_source, 
            db, 
            entry_type, 
            model, 
            data_element
        );
    }

    // Set the entries
    RBEntryView* ev = rb_source_get_entry_view(RB_SOURCE(deezer_source));
    rb_entry_view_set_model(ev, model);

    g_object_unref(shell);
    g_object_unref(db);
}

static void rb_deezer_source_search_entry_cb(RBSearchEntry* se, const gchar* text, gpointer user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
    RBDeezerPlugin* pl;
    g_object_get(deezer_source, "plugin", &pl, NULL);

    rb_deezer_plugin_api_call(
        pl, 
        rb_deezer_source_search_api_callback, 
        deezer_source, 
        "search", 
        "q", text, 
        NULL
    );

    g_object_unref(pl);
}

static void rb_deezer_source_show_search(RBDeezerSource* deezer_source) {
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

    rb_deezer_source_clear(deezer_source);

    RBSearchEntry* se = rb_search_entry_new(false);
    g_signal_connect(
        se, 
        "search", 
        (GCallback)rb_deezer_source_search_entry_cb,
        deezer_source
    );

    RBEntryView* ev = rb_entry_view_new(
        db,
        G_OBJECT(shell_player),
        false, 
        false 
    );
    rb_entry_view_append_column(ev, RB_ENTRY_VIEW_COL_TITLE, true);
    rb_entry_view_append_column(ev, RB_ENTRY_VIEW_COL_ARTIST, true);
    rb_entry_view_append_column(ev, RB_ENTRY_VIEW_COL_DURATION, true);
    deezer_source->entry_view = ev;

    g_object_set(
        deezer_source, 
        "orientation", 
        GTK_ORIENTATION_VERTICAL, 
        NULL
    );
    gtk_box_pack_start(GTK_BOX(deezer_source), GTK_WIDGET(se), false, true, 4);
    gtk_box_pack_start(GTK_BOX(deezer_source), GTK_WIDGET(ev), true, true, 4);

    gtk_widget_show_all(GTK_WIDGET(deezer_source));

    g_object_unref(shell);
    g_object_unref(db);
}

void rb_deezer_source_setup(RBDeezerSource* deezer_source) {
    rb_debug("source_setup");
    RBDeezerPlugin* plugin;
    const gchar* access_token;
    g_object_get(deezer_source, "plugin", &plugin, NULL);
    g_object_get(plugin, "access_token", &access_token, NULL);
    rb_debug("Checking access token %s", access_token);
    if (strlen(access_token) > 0) {
        rb_deezer_source_show_search(deezer_source);
    } else {
        rb_deezer_source_show_login(deezer_source);
    }
    g_object_unref(plugin);
}