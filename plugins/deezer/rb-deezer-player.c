#include "rb-deezer-player.h"

#include <deezer-player.h>

#include "rb-deezer-plugin.h"
#include "rb-player.h"
#include "rb-debug.h"
#include "rb-util.h"

enum {
    RB_DEEZER_PLAYER_PROP_PLUGIN = 1
};

typedef enum {
    RB_DZ_PLAYER_STOPPED = 1,
    RB_DZ_PLAYER_LOADING,
    RB_DZ_PLAYER_PLAYING,
    RB_DZ_PLAYER_PAUSED,
} RBDeezerPlayerState;

struct _RBDeezerPlayer {
    GObject parent;

    // Reference back to the plugin required for player init
    RBDeezerPlugin* plugin;

    // Reference to deezer player object
    dz_player_handle player_handle;
    
    // Things we need to hold onto for the currently opened track
    gpointer stream_data;
    GDestroyNotify stream_data_destroy;

    // Current state
    RBDeezerPlayerState state;
    int64_t progress_micros;
    float volume;

    // Things that can happen in the future
    gboolean play_pending;
    gboolean stream_ready;
};

static void rb_deezer_player_destroy_stream_data(RBDeezerPlayer* deezer_player) {
    if (deezer_player->stream_data && deezer_player->stream_data_destroy) {
        deezer_player->stream_data_destroy(deezer_player->stream_data);
    }
    deezer_player->stream_data = NULL;
    deezer_player->stream_data_destroy = NULL;
}

static gboolean rb_deezer_player_check_error(const char* desc, dz_error_t err, GError** gerror) {
    if (err != DZ_ERROR_NO_ERROR) {
        g_set_error(gerror, 
            RB_PLAYER_ERROR, 
            RB_PLAYER_ERROR_GENERAL,
            "%s: %d", desc, err
        );
    }
    return err == DZ_ERROR_NO_ERROR;
}

static gboolean rb_deezer_player_check_error_emit(RBDeezerPlayer* deezer_player, const char* desc, dz_error_t err) {
    GError* gerror;
    if (rb_deezer_player_check_error(desc, err, &gerror)) {
        return true;
    } else {
        _rb_player_emit_error(RB_PLAYER(deezer_player), deezer_player->stream_data, gerror);
        g_error_free(gerror);
        return false;
    }
}

static gboolean rb_deezer_player_open(RBPlayer *player,
						                const char *uri,
						                gpointer stream_data,
						                GDestroyNotify stream_data_destroy,
						                GError **error) {
    RBDeezerPlayer* deezer_player = RB_DEEZER_PLAYER(player);

    // Hold onto some data that is required
    rb_deezer_player_destroy_stream_data(deezer_player);
    deezer_player->stream_data = stream_data;
    deezer_player->stream_data_destroy = stream_data_destroy;

    dz_error_t err = dz_player_load(deezer_player->player_handle, NULL, NULL, uri);
    if (rb_deezer_player_check_error("Error opening track", err, error)) {
        deezer_player->state = RB_DZ_PLAYER_LOADING;
        return true;
    } else {
        return false;
    }
}

static gboolean rb_deezer_player_opened(RBPlayer *player) {
    RBDeezerPlayer* deezer_player = RB_DEEZER_PLAYER(player);
    return deezer_player->state == RB_DZ_PLAYER_PLAYING 
        || deezer_player->state == RB_DZ_PLAYER_PAUSED;
}

static gboolean rb_deezer_player_close(RBPlayer *player,
                                        const char *uri,
                                        GError **error) {
    RBDeezerPlayer* deezer_player = RB_DEEZER_PLAYER(player);
    dz_error_t err = dz_player_stop(deezer_player->player_handle, NULL, NULL);
    if (rb_deezer_player_check_error("Error stopping", err, error)) {
        deezer_player->state = RB_DZ_PLAYER_STOPPED;
        rb_deezer_player_destroy_stream_data(deezer_player);
        return true;
    } else {
        return false;
    }
}

static gboolean rb_deezer_player_actually_play(RBDeezerPlayer* deezer_player, GError** error) {
    dz_error_t err = dz_player_play(deezer_player->player_handle, 
        NULL, 
        NULL, 
        DZ_PLAYER_PLAY_CMD_START_TRACKLIST, 
        DZ_INDEX_IN_QUEUELIST_CURRENT
    );

    if (rb_deezer_player_check_error("Error playing track", err, error)) {
        deezer_player->state = RB_DZ_PLAYER_PLAYING;
        _rb_player_emit_playing_stream(RB_PLAYER(deezer_player), deezer_player->stream_data);
        return true;
    } else {
        return false;
    }
}

static gboolean	rb_deezer_player_play(RBPlayer *player,
                                        RBPlayerPlayType play_type,
                                        gint64 crossfade,
                                        GError **error) {
    RBDeezerPlayer* deezer_player = RB_DEEZER_PLAYER(player);
    if (deezer_player->state == RB_DZ_PLAYER_PAUSED) {
        // No need to load just call resume
        dz_error_t err = dz_player_resume(deezer_player->player_handle, NULL, NULL);
        if (rb_deezer_player_check_error("Error resuming", err, error)) {
            deezer_player->state = RB_DZ_PLAYER_PLAYING;
            _rb_player_emit_playing_stream(RB_PLAYER(deezer_player), deezer_player->stream_data);
            return true;
        } else {
            return false;
        }
    } else if (deezer_player->stream_ready) {
        // We've already loaded the tracks, go go go 
        deezer_player->stream_ready = false;
        return rb_deezer_player_actually_play(deezer_player, error);
    } else {
        // We'll start playing once the stream is loaded
        deezer_player->play_pending = true;
        return true;
    }
}

static void rb_deezer_player_pause(RBPlayer *player) {
    RBDeezerPlayer* deezer_player = RB_DEEZER_PLAYER(player);
    dz_error_t err = dz_player_pause(deezer_player->player_handle, NULL, NULL);
    if (rb_deezer_player_check_error_emit(deezer_player, "Error pausing", err)) {
        deezer_player->state = RB_DZ_PLAYER_PAUSED;
    }
}

static gboolean rb_deezer_player_playing(RBPlayer *player) {
    RBDeezerPlayer* deezer_player = RB_DEEZER_PLAYER(player);
    return deezer_player->state == RB_DZ_PLAYER_PLAYING;
}

static void rb_deezer_player_set_volume(RBPlayer *player, float volume) {
    RBDeezerPlayer* deezer_player = RB_DEEZER_PLAYER(player);
    deezer_player->volume = volume;

    dz_error_t err = dz_player_set_output_volume(deezer_player->player_handle, 
        NULL, 
        NULL, 
        100 * volume // dz_player wants percentage, rb gives 0-1
    );
    if (err != DZ_ERROR_NO_ERROR) {
        rb_debug("Failed to set volume %d", err);
    }
}

static float rb_deezer_player_get_volume(RBPlayer *player) {
    RBDeezerPlayer* deezer_player = RB_DEEZER_PLAYER(player);
    return deezer_player->volume;
}

static gboolean rb_deezer_player_seekable(RBPlayer *player) {
    return true;
}

static void rb_deezer_player_set_time(RBPlayer *player, gint64 newtime_nanos) {
    rb_debug("Seek called with new time %ld", newtime_nanos);
    RBDeezerPlayer* deezer_player = RB_DEEZER_PLAYER(player);
    dz_error_t err = dz_player_seek(deezer_player->player_handle, NULL, NULL, newtime_nanos / 1000); // ns -> us
    if (rb_deezer_player_check_error_emit(deezer_player, "Error seeking", err)) {
        // Documentation says we go into pause mode if we were originally not already playing or paused
        if (deezer_player->state == RB_DZ_PLAYER_STOPPED) {
            deezer_player->state = RB_DZ_PLAYER_PAUSED;
        }
    }
}

static gint64 rb_deezer_player_get_time(RBPlayer *player) {
    RBDeezerPlayer* deezer_player = RB_DEEZER_PLAYER(player);
    return deezer_player->progress_micros;
}

static const char* rb_deezer_player_event_type_desc(dz_player_event_t event_type) {
    switch (event_type) {
        case DZ_PLAYER_EVENT_QUEUELIST_LOADED:
            return "DZ_PLAYER_EVENT_QUEUELIST_LOADED";
        case DZ_PLAYER_EVENT_LIMITATION_FORCED_PAUSE:
            return "DZ_PLAYER_EVENT_LIMITATION_FORCED_PAUSE";
        case DZ_PLAYER_EVENT_QUEUELIST_NO_RIGHT:
            return "DZ_PLAYER_EVENT_QUEUELIST_NO_RIGHT";
        case DZ_PLAYER_EVENT_QUEUELIST_TRACK_NOT_AVAILABLE_OFFLINE:
            return "DZ_PLAYER_EVENT_QUEUELIST_TRACK_NOT_AVAILABLE_OFFLINE";
        case DZ_PLAYER_EVENT_QUEUELIST_TRACK_RIGHTS_AFTER_AUDIOADS:
            return "DZ_PLAYER_EVENT_QUEUELIST_TRACK_RIGHTS_AFTER_AUDIOADS";
        case DZ_PLAYER_EVENT_QUEUELIST_SKIP_NO_RIGHT:
            return "DZ_PLAYER_EVENT_QUEUELIST_SKIP_NO_RIGHT";
        case DZ_PLAYER_EVENT_QUEUELIST_TRACK_SELECTED:
            return "DZ_PLAYER_EVENT_QUEUELIST_TRACK_SELECTED";
        case DZ_PLAYER_EVENT_QUEUELIST_NEED_NATURAL_NEXT:
            return "DZ_PLAYER_EVENT_QUEUELIST_NEED_NATURAL_NEXT";
        case DZ_PLAYER_EVENT_MEDIASTREAM_DATA_READY:
            return "DZ_PLAYER_EVENT_MEDIASTREAM_DATA_READY";
        case DZ_PLAYER_EVENT_MEDIASTREAM_DATA_READY_AFTER_SEEK:
            return "DZ_PLAYER_EVENT_MEDIASTREAM_DATA_READY_AFTER_SEEK";
        case DZ_PLAYER_EVENT_RENDER_TRACK_START_FAILURE:
            return "DZ_PLAYER_EVENT_RENDER_TRACK_START_FAILURE";
        case DZ_PLAYER_EVENT_RENDER_TRACK_START:
            return "DZ_PLAYER_EVENT_RENDER_TRACK_START";
        case DZ_PLAYER_EVENT_RENDER_TRACK_END:
            return "DZ_PLAYER_EVENT_RENDER_TRACK_END";
        case DZ_PLAYER_EVENT_RENDER_TRACK_PAUSED:
            return "DZ_PLAYER_EVENT_RENDER_TRACK_PAUSED";
        case DZ_PLAYER_EVENT_RENDER_TRACK_SEEKING:
            return "DZ_PLAYER_EVENT_RENDER_TRACK_SEEKING";
        case DZ_PLAYER_EVENT_RENDER_TRACK_UNDERFLOW:
            return "DZ_PLAYER_EVENT_RENDER_TRACK_UNDERFLOW";
        case DZ_PLAYER_EVENT_RENDER_TRACK_RESUMED:
            return "DZ_PLAYER_EVENT_RENDER_TRACK_RESUMED";
        case DZ_PLAYER_EVENT_RENDER_TRACK_REMOVED:
            return "DZ_PLAYER_EVENT_RENDER_TRACK_REMOVED";
        default: 
            return "Unhandled event type";
    }
}

static void rb_deezer_player_queuelist_loaded_cb(RBDeezerPlayer* deezer_player) {
    rb_debug("Queuelist loaded");
    if (deezer_player->play_pending) {
        deezer_player->play_pending = false;
        GError* err;
        if (!rb_deezer_player_actually_play(deezer_player, &err)) {
            _rb_player_emit_error(RB_PLAYER(deezer_player), deezer_player->stream_data, err);
            g_error_free(err);
        }
    } else {
        deezer_player->stream_ready = true;
    }
}

static void rb_deezer_player_track_end_cb(RBDeezerPlayer* deezer_player) {
    rb_debug("Track end");
    deezer_player->state = RB_DZ_PLAYER_STOPPED;
    _rb_player_emit_eos(RB_PLAYER(deezer_player), deezer_player->stream_data, false);
}

struct _player_event_args {
    RBDeezerPlayer* deezer_player;
    dz_player_event_handle event_handle;
};

static gboolean rb_deezer_player_event_cb_main(gpointer data) {
    struct _player_event_args* args = (struct _player_event_args*)data;
    RBDeezerPlayer* deezer_player = args->deezer_player;
    dz_player_event_handle event = args->event_handle;
    free(data);

    dz_player_event_t event_type = dz_player_event_get_type(event);
    switch (event_type) {
        case DZ_PLAYER_EVENT_RENDER_TRACK_END:
            rb_deezer_player_track_end_cb(deezer_player);
            break;
        case DZ_PLAYER_EVENT_QUEUELIST_LOADED:
            rb_deezer_player_queuelist_loaded_cb(deezer_player);
            break;
        default:
            rb_debug("Unhandled event %s", rb_deezer_player_event_type_desc(event_type));
            break;
    }

    return G_SOURCE_REMOVE;
}

static void rb_deezer_player_event_cb(dz_player_handle handle,
                                      dz_player_event_handle event,
                                      void* user_data) {
    struct _player_event_args* wrap = malloc(sizeof(struct _player_event_args));
    wrap->deezer_player = RB_DEEZER_PLAYER(user_data);
    wrap->event_handle = event;
    gdk_threads_add_idle(rb_deezer_player_event_cb_main, wrap);
}

struct _progress_update_args {
    RBDeezerPlayer* deezer_player;
    dz_useconds_t progress;
};

static gboolean rb_deezer_player_onrenderprogress_cb_main_thread(gpointer user_data) {
    // Unpack and do the thing
    struct _progress_update_args* args = user_data;
    RBDeezerPlayer* deezer_player = args->deezer_player;
    dz_useconds_t progress = args->progress;
    free(args);

    deezer_player->progress_micros = progress;
    _rb_player_emit_tick(RB_PLAYER(deezer_player), 
        deezer_player->stream_data, 
        deezer_player->progress_micros * 1000, // us -> ns 
        -1);
    // Just do it the once    
    return G_SOURCE_REMOVE;
}

static void rb_deezer_player_onrenderprogress_cb(dz_player_handle handle,
                                                dz_useconds_t progress,
                                                void* user_data) {
    // Pack it up and dispatch to the main thread                                                
    struct _progress_update_args* args = malloc(sizeof(struct _progress_update_args));
    args->deezer_player = RB_DEEZER_PLAYER(user_data);
    args->progress = progress;
    gdk_threads_add_idle(rb_deezer_player_onrenderprogress_cb_main_thread, args);
}

// Need to init the deezer play AFTER constructino properties are set
static void rb_deezer_player_constructed(GObject* object) {
    RBDeezerPlayer* player = RB_DEEZER_PLAYER(object);
    RBDeezerPlugin* plugin;
    g_object_get(player, "plugin", &plugin, NULL);
    rb_debug("Initializing player with DZ connect handle %p", plugin->handle);
    player->player_handle = dz_player_new(plugin->handle);
    
    dz_player_set_event_cb(player->player_handle, rb_deezer_player_event_cb);
    
    dz_player_set_render_progress_cb(player->player_handle, rb_deezer_player_onrenderprogress_cb, 100000); // 100 ms

    dz_error_t err = dz_player_activate(player->player_handle, player);
    if (err != DZ_ERROR_NO_ERROR) {
        rb_debug("Error activating Deezer player %d", err);
    }
    g_object_unref(plugin);
}

static void set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
    RBDeezerPlayer* player = RB_DEEZER_PLAYER(object);
    switch (prop_id) {
        case RB_DEEZER_PLAYER_PROP_PLUGIN:
            player->plugin = g_value_get_object(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    RBDeezerPlayer* player = RB_DEEZER_PLAYER(object);
    switch (prop_id) {
        case RB_DEEZER_PLAYER_PROP_PLUGIN:
            g_value_set_object(value, player->plugin);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void rb_player_init(RBPlayerIface *iface) {
    iface->open = rb_deezer_player_open;
	iface->opened = rb_deezer_player_opened;
	iface->close = rb_deezer_player_close;
	iface->play = rb_deezer_player_play;
	iface->pause = rb_deezer_player_pause;
	iface->playing = rb_deezer_player_playing;
	iface->set_volume = rb_deezer_player_set_volume;
	iface->get_volume = rb_deezer_player_get_volume;
	iface->seekable = rb_deezer_player_seekable;
	iface->set_time = rb_deezer_player_set_time;
	iface->get_time = rb_deezer_player_get_time;
	iface->multiple_open = (RBPlayerFeatureFunc) rb_false_function;
}

static void rb_deezer_player_init(RBDeezerPlayer* player) {}

static void rb_deezer_player_class_init(RBDeezerPlayerClass* class) {
    GObjectClass* object_class =  G_OBJECT_CLASS(class);
    object_class->get_property = get_property;
    object_class->set_property = set_property;
    object_class->constructed = rb_deezer_player_constructed;
    
    g_object_class_install_property(object_class, 
        RB_DEEZER_PLAYER_PROP_PLUGIN,
        g_param_spec_object(
            "plugin", "plugin", "The RBDeezerPlugin", 
            rb_deezer_plugin_get_type(), 
            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY)
    );
}

static void rb_deezer_player_class_finalize(RBDeezerPlayerClass* class) {}

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    RBDeezerPlayer,
    rb_deezer_player,
    G_TYPE_OBJECT,
    0,
    G_IMPLEMENT_INTERFACE_DYNAMIC(
        RB_TYPE_PLAYER,
        rb_player_init
    ) 
);

void _rb_deezer_player_register_type(GTypeModule* m) {
    rb_deezer_player_register_type(m);
}