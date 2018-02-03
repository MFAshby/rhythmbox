#include "rb-deezer-offline-source.h"
#include "rb-deezer-plugin.h"
#include "rb-debug.h"
#include <deezer-offline.h>
#include <deezer-api.h>
#include <string.h>

static void rb_deezer_offline_source_state_cb(void* delegate,
											  void* operation_userdata,
											  dz_error_t error,
											  dz_object_handle result);

/**
 * Offline source shows available cached offline tracks
 */ 
struct _RBDeezerOfflineSource {
    RBBrowserSource parent;
};


struct _rb_deezer_offline_source_on_event_cb_args {
	RBDeezerOfflineSource* src;
	dz_offline_event_handle event;
};

static gboolean rb_deezer_offline_source_on_event_cb_main(gpointer data) {
	struct _rb_deezer_offline_source_on_event_cb_args* args = 
		(struct _rb_deezer_offline_source_on_event_cb_args*)data;
	// RBDeezerOfflineSource* src = args->src;
	dz_offline_event_handle event = args->event;
	dz_offline_event_t event_type = dz_offline_event_get_type(event);
	switch (event_type) {
		case DZ_OFFLINE_EVENT_UNKNOWN:
			rb_debug("DZ_OFFLINE_EVENT_UNKNOWN");
			break;           /**< Offline event has not been set yet, not a valid value. */
		case DZ_OFFLINE_EVENT_NEW_SYNC_STATE:
			rb_debug("DZ_OFFLINE_EVENT_NEW_SYNC_STATE");
			dz_offline_sync_state_t state = dz_offline_event_resource_get_sync_state(event);
			const char* resource_id = dz_offline_event_get_resource_id(event);
			const char* resource_type = dz_offline_event_get_resource_type(event);
			const char* resource_state = dz_offline_state_to_cchar(state);
			rb_debug("Resource [%s] Type [%s] State [%s]", resource_id, resource_type, resource_state);		
			break;    /**< Send when the state on a resource has changed. */
		case DZ_OFFLINE_EVENT_DOWNLOAD_PROGRESS:
			rb_debug("DZ_OFFLINE_EVENT_DOWNLOAD_PROGRESS");
			break; /**< Send progress event on a resource operation. */
	}
	DZ_OBJECT_RELEASE(event);
	free(args);
	return G_SOURCE_REMOVE;
}

static void rb_deezer_offline_source_on_event_cb(dz_connect_handle handle,
												  dz_offline_event_handle event,
												  void* delegate) {
	RBDeezerOfflineSource* src = RB_DEEZER_OFFLINE_SOURCE(delegate);
	struct _rb_deezer_offline_source_on_event_cb_args* args 
		= malloc(sizeof(struct _rb_deezer_offline_source_on_event_cb_args));
	DZ_OBJECT_RETAIN(event);
	args->event = event;
	args->src = src;
	gdk_threads_add_idle(rb_deezer_offline_source_on_event_cb_main, args);
}

struct _rb_deezer_offline_source_state_cb_args {
    RBDeezerOfflineSource* src;
    dz_object_handle result;
};

// static void rb_deezer_offline_source_log_data_cb(void* delegate,
//                                                    void* operation_userdata,
//                                                    dz_error_t error,
//                                                    dz_object_handle result) {
//     const char* resource_json = dz_offline_resource_to_json(result);
//     GError* err;
//     JsonNode* node = json_from_string(resource_json, &err);
//     rb_debug("resource:\n%s", json_to_string(node, true) );
// }

static void rb_deezer_offline_api_request_cb(dz_api_request_processing_handle handle,
                                             dz_api_result_t  ret,
                                             dz_stream_object responsedata,
                                             void*            userdata
                                             ) {
    // const char* resource_json = dz_offline_resource_to_json(result);
    // dz_player_event_track_selected_dzapiinfo
    GError* err;
    JsonNode* node = json_from_string((const char*)responsedata, &err);
    rb_debug("API result:\n%s", json_to_string(node, true) );
}

static void rb_deezer_offline_source_track_data_cb(void* delegate,
                                                   void* operation_userdata,
                                                   dz_error_t error,
                                                   dz_object_handle result) {
    const char* resource_json = dz_offline_resource_to_json(result);
    GError* err;
    JsonNode* node = json_from_string(resource_json, &err);
    rb_debug("resource:\n%s", json_to_string(node, true) );

    RBDeezerOfflineSource* src = RB_DEEZER_OFFLINE_SOURCE(operation_userdata);
    RBDeezerPlugin* plugin;
    g_object_get(src, "plugin", &plugin, NULL);

    JsonObject* obj = json_node_get_object(node);
    JsonArray* arr = json_object_get_array_member(obj, "tracks");
    for (guint i=0; i<json_array_get_length(arr); i++) {
        int track_id = json_array_get_int_element(arr, i);
        char buf[100];
        // snprintf(buf, 100, "/dzlib/track_offline/%d", track_id);
        snprintf(buf, 100, "/track/%d", track_id);
        // DZ(dz_offline_get, plugin->handle, rb_deezer_offline_source_log_data_cb, src, buf);
        dz_api_request_handle hnd = dz_api_request_new(DZ_API_CMD_GET, buf);
        // dz_api_request_handle req_hnd = 
        dz_api_request_processing_async(plugin->handle, hnd, NULL, rb_deezer_offline_api_request_cb, src);
    }
    g_object_unref(plugin);
}

static gboolean rb_deezer_offline_source_state_cb_main(gpointer data) {
    struct _rb_deezer_offline_source_state_cb_args* args 
        = (struct _rb_deezer_offline_source_state_cb_args*)data;
    RBDeezerOfflineSource* src = args->src;
    dz_object_handle result = args->result;

    RBDeezerPlugin* plugin;
    g_object_get(src, "plugin", &plugin, NULL);

    const char* offline_state_json = dz_offline_state_to_json(result);
    GError* err; 
    JsonNode* node = json_from_string(offline_state_json, &err);
    rb_debug("Offline state: \n%s", json_to_string(node, true));

    JsonArray* offline_objs = json_node_get_array(node);
    for (guint i=0; i<json_array_get_length(offline_objs); i++) {
        JsonObject* offline_obj = json_array_get_object_element(offline_objs, i);
        const char* state = json_object_get_string_member(offline_obj, "state");
        const char* path = json_object_get_string_member(offline_obj, "resource_id");
        bool is_tracklist = g_str_has_prefix(path, "/dzlocal/tracklist/");
        bool is_synced = strcmp(state, "synced") == 0;
        if (is_tracklist && is_synced) {
            DZ(dz_offline_get, plugin->handle, rb_deezer_offline_source_track_data_cb, src, path);
        }
    }

    g_object_unref(plugin);
    free(args);
    DZ_OBJECT_RELEASE(result);
    return G_SOURCE_REMOVE;
} 

static void rb_deezer_offline_source_state_cb(void* delegate,
											  void* operation_userdata,
											  dz_error_t error,
											  dz_object_handle result) {
	if (error != DZ_ERROR_NO_ERROR) {
        rb_debug("Eeror getting offline state %s", dz_error_string(error));
        return;
    }
    DZ_OBJECT_RETAIN(result);
    struct _rb_deezer_offline_source_state_cb_args* args 
        = malloc(sizeof(struct _rb_deezer_offline_source_state_cb_args));
    args->src = RB_DEEZER_OFFLINE_SOURCE(operation_userdata);
    args->result = result;
    gdk_threads_add_idle(
        rb_deezer_offline_source_state_cb_main,
        args
    );
}

static void rb_deezer_offline_source_init(RBDeezerOfflineSource* src) {}

static void rb_deezer_offline_constructed(GObject* object) {
    RBDeezerPlugin* plugin;
    g_object_get(object, "plugin", &plugin, NULL);

    // Set ourselves up as offline event listener
	dz_error_t err = dz_offline_eventcb_set(plugin->handle, 
        NULL, 
        RB_DEEZER_OFFLINE_SOURCE(object), 
        rb_deezer_offline_source_on_event_cb
    );
    if (err != DZ_ERROR_NO_ERROR) {
        rb_debug("Error registering offline callback %s", dz_error_string(err));
    }

    // Query initial state
    err = dz_offline_get_state(plugin->handle, 
        rb_deezer_offline_source_state_cb, 
        RB_DEEZER_OFFLINE_SOURCE(object), 
        "/dzlib/tracklists", 
        true
    );
    if (err != DZ_ERROR_NO_ERROR) {
        rb_debug("Error requesting offline content %s", dz_error_string(err));
    }
    g_object_unref(plugin);
}

static void rb_deezer_offline_source_class_init(RBDeezerOfflineSourceClass* cls) {
    GObjectClass* object_cls = G_OBJECT_CLASS(cls);
    object_cls->constructed = rb_deezer_offline_constructed;
}

static void rb_deezer_offline_source_class_finalize(RBDeezerOfflineSourceClass* cls) {}

G_DEFINE_DYNAMIC_TYPE(
    RBDeezerOfflineSource, 
    rb_deezer_offline_source, 
    RB_TYPE_BROWSER_SOURCE
);

// Required for PEAS plugin system
void _rb_deezer_offline_source_register_type(GTypeModule* type_module) {
    rb_deezer_offline_source_register_type(type_module);
}
