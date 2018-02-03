#include "rb-deezer-plugin.h"
#include "rb-deezer-source.h"
#include "rb-deezer-entry-type.h"
#include "rb-deezer-player.h"
#include "rb-deezer-offline-source.h"
#include "rb-deezer-offline-entry-type.h"
#include "rb-debug.h"
#include "rb-shell.h"
#include "rb-shell-player.h"
#include "rb-display-page-group.h"
#include "rhythmdb.h"
#include "rb-file-helpers.h"

#include <glib/gi18n-lib.h>
#include <deezer-offline.h>

enum {
	PROP_OFFLINE_AVAILABLE = 2,
};

// I hate that C makes you do this
static void rb_deezer_plugin_init (RBDeezerPlugin *);
static void impl_activate (PeasActivatable* );
static void impl_deactivate	(PeasActivatable *plugin);
static void set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void rb_deezer_plugin_class_init (RBDeezerPluginClass *klass);
static void rb_deezer_plugin_class_finalize (RBDeezerPluginClass *klass);
static void peas_activatable_iface_init (PeasActivatableInterface *iface);


static void rb_deezer_plugin_access_token_changed(GSettings* settings, 
												  gchar* key,
												  gpointer user_data) {
	RBDeezerPlugin* pl = RB_DEEZER_PLUGIN(user_data);
	gchar* access_token = g_settings_get_string(pl->settings, "access-token");
	if (strlen(access_token) == 0) {
		rb_debug("Access token is empty");
		return;
	}

	rb_debug("Access token received, passing to Deezer connect handle %p", pl->handle);
	dz_error_t err = dz_connect_set_access_token(pl->handle, NULL, NULL, access_token);
	if (err != DZ_ERROR_NO_ERROR) {
		rb_debug("Error setting access token in Deezer lib %d", err);
	}

	// Force online operation to ensure we're logged in (if we're actually online)
	err = dz_connect_offline_mode(pl->handle, NULL, NULL, false);
	if (err != DZ_ERROR_NO_ERROR) {
		rb_debug("Error forcing online mode after access token receipt %d", err);
	}
}

static void rb_deezer_plugin_init (RBDeezerPlugin *plugin) {
	rb_debug ("Deezer plugin init");

	plugin->settings = g_settings_new(G_SETTINGS_SCHEMA);

	g_signal_connect(
		plugin->settings, 
		"changed::access-token", G_CALLBACK(rb_deezer_plugin_access_token_changed), 
		plugin
	);

	plugin->soup_session = soup_session_new();
}

static void set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {			
	RBDeezerPlugin* deezer_plugin = RB_DEEZER_PLUGIN(object);
	switch (prop_id) {						
	case PROP_OBJECT:						
		g_object_set_data_full (object,
					"rb-shell",
					g_value_dup_object (value),
					g_object_unref);
		break;
	case PROP_OFFLINE_AVAILABLE:
		deezer_plugin->offline_available = g_value_get_boolean(value);
		break;
	default:							
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;						
	}
}

static void get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {									
	RBDeezerPlugin* deezer_plugin = RB_DEEZER_PLUGIN(object);
	switch (prop_id) {				
	case PROP_OBJECT:
		g_value_set_object(value, g_object_get_data (object, "rb-shell")); 
		break;	
	case PROP_OFFLINE_AVAILABLE:
		g_value_set_boolean(value, deezer_plugin->offline_available);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;	
	}
}

static void rb_deezer_plugin_class_init (RBDeezerPluginClass *klass) {
	GObjectClass *object_class = G_OBJECT_CLASS (klass);	
	object_class->set_property = set_property;		
	object_class->get_property = get_property;	
	
	g_object_class_override_property (
		object_class, 
		PROP_OBJECT, "object"
	);

	g_object_class_install_property(object_class, 
		PROP_OFFLINE_AVAILABLE, 
		g_param_spec_boolean("offline-available", 
			"offline available", 
			"Whether offline mode is available from Deezer SDK", 
			false, 
			G_PARAM_READWRITE)
	);
}

static void rb_deezer_plugin_class_finalize (RBDeezerPluginClass *klass) {}

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
	RBDeezerPlugin,
	rb_deezer_plugin,
	PEAS_TYPE_EXTENSION_BASE,
	0,
	G_IMPLEMENT_INTERFACE_DYNAMIC (PEAS_TYPE_ACTIVATABLE,
					 			peas_activatable_iface_init)
);

struct _rb_deezer_plugin_connect_event_cb_args {
	RBDeezerPlugin* plugin;
	dz_connect_event_handle event;
};

static gboolean rb_deezer_plugin_connect_event_cb_main(gpointer data) {
	struct _rb_deezer_plugin_connect_event_cb_args* args =
		(struct _rb_deezer_plugin_connect_event_cb_args*)data;
	RBDeezerPlugin* plugin = args->plugin;
	dz_connect_event_handle event = args->event;
	dz_connect_event_t event_type = dz_connect_event_get_type(event);
	switch (event_type) {
		case DZ_CONNECT_EVENT_UNKNOWN:
			rb_debug("DZ_CONNECT_EVENT_UNKNOWN");                           /**< Connect event has not been set yet, not a valid value. */
			break;
		case DZ_CONNECT_EVENT_USER_OFFLINE_AVAILABLE:
			rb_debug("DZ_CONNECT_EVENT_USER_OFFLINE_AVAILABLE");            /**< User logged in, and credentials from offline store are loaded. */
			g_object_set(plugin, "offline-available", true, NULL);
			break;
		case DZ_CONNECT_EVENT_USER_ACCESS_TOKEN_OK:
			rb_debug("DZ_CONNECT_EVENT_USER_ACCESS_TOKEN_OK");              /**< (Not available) dz_connect_login_with_email() ok, and access_token is available */
			break;
		case DZ_CONNECT_EVENT_USER_ACCESS_TOKEN_FAILED:
			rb_debug("DZ_CONNECT_EVENT_USER_ACCESS_TOKEN_FAILED");          /**< (Not available) dz_connect_login_with_email() failed */
			break;
		case DZ_CONNECT_EVENT_USER_LOGIN_OK:
			rb_debug("DZ_CONNECT_EVENT_USER_LOGIN_OK");                     /**< Login with access_token ok, infos from user available. */
			break;
		case DZ_CONNECT_EVENT_USER_LOGIN_FAIL_NETWORK_ERROR:
			rb_debug("DZ_CONNECT_EVENT_USER_LOGIN_FAIL_NETWORK_ERROR");     /**< Login with access_token failed because of network condition. */
			break;
		case DZ_CONNECT_EVENT_USER_LOGIN_FAIL_BAD_CREDENTIALS:
			rb_debug("DZ_CONNECT_EVENT_USER_LOGIN_FAIL_BAD_CREDENTIALS");   /**< Login with access_token failed because of bad credentials. */
			break;
		case DZ_CONNECT_EVENT_USER_LOGIN_FAIL_USER_INFO:
			rb_debug("DZ_CONNECT_EVENT_USER_LOGIN_FAIL_USER_INFO");         /**< Login with access_token failed because of other problem. */
			break;
		case DZ_CONNECT_EVENT_USER_LOGIN_FAIL_OFFLINE_MODE:
			rb_debug("DZ_CONNECT_EVENT_USER_LOGIN_FAIL_OFFLINE_MODE");      /**< Login with access_token failed because we are in forced offline mode. */
			break;
		case DZ_CONNECT_EVENT_USER_NEW_OPTIONS:
			rb_debug("DZ_CONNECT_EVENT_USER_NEW_OPTIONS");                  /**< User options have just changed. */
			break;
		case DZ_CONNECT_EVENT_ADVERTISEMENT_START:
			rb_debug("DZ_CONNECT_EVENT_ADVERTISEMENT_START");               /**< A new advertisement needs to be displayed. */
			break;
		case DZ_CONNECT_EVENT_ADVERTISEMENT_STOP:
			rb_debug("DZ_CONNECT_EVENT_ADVERTISEMENT_STOP");                /**< An advertisement needs to be stopped. */
			break;
	}
	DZ_OBJECT_RELEASE(event);
	free(args);
	return G_SOURCE_REMOVE;
}

static void rb_deezer_plugin_connect_event_cb(dz_connect_handle handle,
								dz_connect_event_handle event,
								void* user_data) {
	// Deezer callbacks not are not executed by the main thread
	DZ_OBJECT_RETAIN(event);
	struct _rb_deezer_plugin_connect_event_cb_args* args = 
		malloc(sizeof(struct _rb_deezer_plugin_connect_event_cb_args));
	args->event = event;
	args->plugin = RB_DEEZER_PLUGIN(user_data);
	gdk_threads_add_idle(rb_deezer_plugin_connect_event_cb_main, args);
}

static bool rb_deezer_plugin_crash_reporting_cb() {
	rb_debug("DEEZER HAS CRASHED O NOES");
	return false;
}

static void rb_deezer_plugin_init_api(RBDeezerPlugin* plugin) {
	char* cache_file = rb_find_user_cache_file("dz_cache");
	struct dz_connect_configuration config; 
	memset(&config, 0, sizeof(struct dz_connect_configuration));
	config.app_id = APP_ID;
	config.product_id = "Javaplayer";
	config.product_build_id = "00001";
	// config.user_profile_path = cache_file;
	config.app_has_crashed_delegate = rb_deezer_plugin_crash_reporting_cb;
	config.connect_event_cb = rb_deezer_plugin_connect_event_cb;

	plugin->handle = dz_connect_new(&config);
	if (plugin->handle == NULL) {
		rb_debug("Failed to create connect handle");
	}

	rb_debug("Connected to deezer, device_id %s", dz_connect_get_device_id(plugin->handle));

	dz_error_t err = dz_connect_activate(plugin->handle, plugin);
	if (err != DZ_ERROR_NO_ERROR) {
		rb_debug("Error connecting to deezer %d", err);
	}

	// Try to apply access token now if we have one already
	rb_deezer_plugin_access_token_changed(NULL, NULL, plugin);

	// Configure cache
	err = dz_connect_cache_path_set(plugin->handle, NULL, NULL, cache_file);
	if (err != DZ_ERROR_NO_ERROR) {
		rb_debug("Error setting cache path %d", err);
	}

	guint cache_size = g_settings_get_uint(plugin->settings, "cache-size-kb");
	err = dz_connect_smartcache_quota_set(plugin->handle, NULL, NULL, cache_size);
	if (err != DZ_ERROR_NO_ERROR) {
		rb_debug("Error setting smartcache_quota %d", err);
	}
}

static void impl_activate (PeasActivatable* pl) {
	rb_debug ("Deezer plugin activated");
	RBDeezerPlugin* plugin = RB_DEEZER_PLUGIN(pl);
	RBShell* shell;
    RhythmDB* db;
	RBShellPlayer* shell_player;
	g_object_get (plugin, "object", &shell, NULL);
    g_object_get (shell, 
		"db", &db, 
		"shell_player", &shell_player,
		NULL);	

	// Do everything we need to with the Deezer API library first
	rb_deezer_plugin_init_api(plugin);

    // Install DeezerEntryType
	RBDeezerEntryType* entry_type = g_object_new(
		rb_deezer_entry_type_get_type(), 
		"name", "deezer-entry-type", 
		NULL
	);
	rhythmdb_register_entry_type(db, RHYTHMDB_ENTRY_TYPE(entry_type));

	// Install DeezerSource
	RBDeezerSource* source = g_object_new(
		rb_deezer_source_get_type(), 
		"shell", shell,
		"name", _("Deezer"),
		"entry-type", entry_type,
		"plugin", plugin,
		NULL
	);
	
	RBDisplayPageGroup* group = rb_display_page_group_get_by_id("shared");
	rb_shell_append_display_page(
		shell, 
		RB_DISPLAY_PAGE(source), 
		RB_DISPLAY_PAGE(group)
	);

	// Connect the two
	rb_shell_register_entry_type_for_source(
		shell, 
		RB_SOURCE(source),
		RHYTHMDB_ENTRY_TYPE(entry_type) 
	);

	// Add the custom player for this entry type
	RBDeezerPlayer* custom_player = g_object_new(rb_deezer_player_get_type(), 
		"plugin", plugin,
		NULL);
	rb_shell_player_add_custom_player(
		shell_player, 
		RHYTHMDB_ENTRY_TYPE(entry_type), 
		RB_PLAYER(custom_player)
	);

	// Offline functions
	RBDeezerOfflineEntryType* offline_entry_type = g_object_new(
		rb_deezer_offline_entry_type_get_type(), 
		"name", "deezer-offline-entry-type",
		NULL
	);
	rhythmdb_register_entry_type(db, RHYTHMDB_ENTRY_TYPE(offline_entry_type));
	RBDeezerOfflineSource* offline_source = g_object_new(
		rb_deezer_offline_source_get_type(), 
		"shell", shell,
		"name", _("Deezer Offline"),
		"entry-type", offline_entry_type,
		"plugin", plugin,
		NULL
	);
	rb_shell_append_display_page(
		shell, 
		RB_DISPLAY_PAGE(offline_source), 
		RB_DISPLAY_PAGE(group)
	);
	rb_shell_register_entry_type_for_source(
		shell, 
		RB_SOURCE(offline_source),
		RHYTHMDB_ENTRY_TYPE(offline_entry_type) 
	);
	rb_shell_player_add_custom_player(
		shell_player, 
		RHYTHMDB_ENTRY_TYPE(offline_entry_type), 
		RB_PLAYER(custom_player)
	);

	g_object_unref (db);
	g_object_unref (shell);
	g_object_unref (shell_player);
}

static void impl_deactivate	(PeasActivatable *plugin) {
	rb_debug ("Deezer plugin deactivated");

	//TODO Should I be removing source, entry type etc here?
}

static void peas_activatable_iface_init(PeasActivatableInterface *iface) {	
	iface->activate = impl_activate;
	iface->deactivate = impl_deactivate;	
}					

G_MODULE_EXPORT void peas_register_types (PeasObjectModule *module)
{
    // Register the plugin class and friends with the GObject system
	rb_deezer_plugin_register_type(G_TYPE_MODULE(module));
	_rb_deezer_source_register_type(G_TYPE_MODULE(module));
	_rb_deezer_entry_type_register_type(G_TYPE_MODULE(module));
	_rb_deezer_player_register_type(G_TYPE_MODULE(module));
	_rb_deezer_offline_source_register_type(G_TYPE_MODULE(module));
	_rb_deezer_offline_entry_type_register_type(G_TYPE_MODULE(module));

    // Register the plugin as an activatable thing
	peas_object_module_register_extension_type (module,
						    PEAS_TYPE_ACTIVATABLE,
						    rb_deezer_plugin_get_type());
}

typedef struct {
	rb_deezer_plugin_api_cb_func callback;
	void* user_data;
} api_callback_wrapper;

static void rb_deezer_plugin_api_callback(SoupSession* session, SoupMessage* msg, void* data) {
	api_callback_wrapper* wrapper = (api_callback_wrapper*)data;
	GError* err;
	JsonNode* node = json_from_string(msg->response_body->data, &err);
	if (node != NULL) {
		wrapper->callback(node, wrapper->user_data);
	} else {
		rb_debug("Error parsing JSON response");
	}
	free(wrapper);
}

void rb_deezer_plugin_api_call(RBDeezerPlugin* pl, 
							  rb_deezer_plugin_api_cb_func callback, 
							  void* user_data, 
							  char* endpoint, 
							  ... /* Parameters: key, value, key2, value2 */) {
	char* uri_base = g_strdup_printf("https://api.deezer.com/%s", endpoint);
	SoupURI* uri = soup_uri_new(uri_base);
	GHashTable* query_params = g_hash_table_new(g_str_hash, g_str_equal);

	va_list list;
	va_start(list, endpoint);
	for (char* key = va_arg(list, char*); key != NULL; key = va_arg(list, char*)) {
		char* value = va_arg(list, char*);
		g_hash_table_insert(query_params, key, value);
	}
	va_end(list);

	gchar* access_token = g_settings_get_string(pl->settings, "access-token");
	if (strlen(access_token) > 0) {
		g_hash_table_insert(query_params, "access_token", access_token);
	}

	soup_uri_set_query_from_form(uri, query_params);
	SoupMessage* msg = soup_message_new_from_uri("GET", uri);

	api_callback_wrapper* wrap = malloc(sizeof(api_callback_wrapper));
	wrap->callback = callback;
	wrap->user_data = user_data;
	soup_session_queue_message(pl->soup_session, msg, rb_deezer_plugin_api_callback, wrap);

	g_hash_table_destroy(query_params);
	g_free(uri_base);
	soup_uri_free(uri);
}

const char* dz_error_string(dz_error_t err) {
	switch (err) {
	case DZ_ERROR_NO_ERROR:
		return "DZ_ERROR_NO_ERROR";
    case DZ_ERROR_ERROR_ARG:
		return "DZ_ERROR_ERROR_ARG";
    case DZ_ERROR_ERROR_STATE:
		return "DZ_ERROR_ERROR_STATE";
    case DZ_ERROR_NOT_IMPLEMENTED:
		return "DZ_ERROR_NOT_IMPLEMENTED";
    case DZ_ERROR_ASYNC_CANCELED:
		return "DZ_ERROR_ASYNC_CANCELED";
    case DZ_ERROR_NOT_ENOUGH_MEMORY:
		return "DZ_ERROR_NOT_ENOUGH_MEMORY";
    case DZ_ERROR_OS_ERROR:
		return "DZ_ERROR_OS_ERROR";
    case DZ_ERROR_UNSUPPORTED:
		return "DZ_ERROR_UNSUPPORTED";
    case DZ_ERROR_CLASS_NOT_FOUND:
		return "DZ_ERROR_CLASS_NOT_FOUND";
    case DZ_ERROR_JSON_PARSING:
		return "DZ_ERROR_JSON_PARSING";
    case DZ_ERROR_XML_PARSING:
		return "DZ_ERROR_XML_PARSING";
    case DZ_ERROR_PARSING:
		return "DZ_ERROR_PARSING";
    case DZ_ERROR_CLASS_INSTANTIATION:
		return "DZ_ERROR_CLASS_INSTANTIATION";
    case DZ_ERROR_RUNNABLE_ALREADY_STARTED:
		return "DZ_ERROR_RUNNABLE_ALREADY_STARTED";
    case DZ_ERROR_RUNNABLE_NOT_STARTED:
		return "DZ_ERROR_RUNNABLE_NOT_STARTED";
    case DZ_ERROR_CACHE_RESOURCE_OPEN_FAILED:
		return "DZ_ERROR_CACHE_RESOURCE_OPEN_FAILED";
    case DZ_ERROR_FS_FULL:
		return "DZ_ERROR_FS_FULL";
    case DZ_ERROR_FILE_EXISTS:
		return "DZ_ERROR_FILE_EXISTS";
    case DZ_ERROR_IO_ERROR:
		return "DZ_ERROR_IO_ERROR";
    case DZ_ERROR_CATEGORY_CONNECT:
		return "DZ_ERROR_CATEGORY_CONNECT";
    case DZ_ERROR_CONNECT_SESSION_LOGIN_FAILED:
		return "DZ_ERROR_CONNECT_SESSION_LOGIN_FAILED";
    case DZ_ERROR_USER_PROFILE_PERM_DENIED:
		return "DZ_ERROR_USER_PROFILE_PERM_DENIED";
    case DZ_ERROR_CACHE_DIRECTORY_PERM_DENIED:
		return "DZ_ERROR_CACHE_DIRECTORY_PERM_DENIED";
    case DZ_ERROR_CONNECT_SESSION_NOT_ONLINE:
		return "DZ_ERROR_CONNECT_SESSION_NOT_ONLINE";
    case DZ_ERROR_CONNECT_SESSION_OFFLINE_MODE:
		return "DZ_ERROR_CONNECT_SESSION_OFFLINE_MODE";
    case DZ_ERROR_CONNECT_NO_OFFLINE_CACHE:
		return "DZ_ERROR_CONNECT_NO_OFFLINE_CACHE";
    case DZ_ERROR_CATEGORY_PLAYER:
		return "DZ_ERROR_CATEGORY_PLAYER";
    case DZ_ERROR_PLAYER_QUEUELIST_NONE_SET:
		return "DZ_ERROR_PLAYER_QUEUELIST_NONE_SET";
    case DZ_ERROR_PLAYER_QUEUELIST_BAD_INDEX:
		return "DZ_ERROR_PLAYER_QUEUELIST_BAD_INDEX";
    case DZ_ERROR_PLAYER_QUEUELIST_NO_MEDIA:
		return "DZ_ERROR_PLAYER_QUEUELIST_NO_MEDIA";
    case DZ_ERROR_PLAYER_QUEUELIST_NO_RIGHTS:
		return "DZ_ERROR_PLAYER_QUEUELIST_NO_RIGHTS";
    case DZ_ERROR_PLAYER_QUEUELIST_RIGHT_TIMEOUT:
		return "DZ_ERROR_PLAYER_QUEUELIST_RIGHT_TIMEOUT";
    case DZ_ERROR_PLAYER_QUEUELIST_RADIO_TOO_MANY_SKIP:
		return "DZ_ERROR_PLAYER_QUEUELIST_RADIO_TOO_MANY_SKIP";
    case DZ_ERROR_PLAYER_QUEUELIST_NO_MORE_TRACK:
		return "DZ_ERROR_PLAYER_QUEUELIST_NO_MORE_TRACK";
    case DZ_ERROR_PLAYER_PAUSE_NOT_STARTED:
		return "DZ_ERROR_PLAYER_PAUSE_NOT_STARTED";
    case DZ_ERROR_PLAYER_PAUSE_ALREADY_PAUSED:
		return "DZ_ERROR_PLAYER_PAUSE_ALREADY_PAUSED";
    case DZ_ERROR_PLAYER_UNPAUSE_NOT_STARTED:
		return "DZ_ERROR_PLAYER_UNPAUSE_NOT_STARTED";
    case DZ_ERROR_PLAYER_UNPAUSE_NOT_PAUSED:
		return "DZ_ERROR_PLAYER_UNPAUSE_NOT_PAUSED";
    case DZ_ERROR_PLAYER_SEEK_NOT_SEEKABLE_NOT_STARTED:
		return "DZ_ERROR_PLAYER_SEEK_NOT_SEEKABLE_NOT_STARTED";
    case DZ_ERROR_PLAYER_SEEK_NOT_SEEKABLE_NO_DURATION:
		return "DZ_ERROR_PLAYER_SEEK_NOT_SEEKABLE_NO_DURATION";
    case DZ_ERROR_PLAYER_SEEK_NOT_SEEKABLE_NOT_INDEXED:
		return "DZ_ERROR_PLAYER_SEEK_NOT_SEEKABLE_NOT_INDEXED";
    case DZ_ERROR_PLAYER_SEEK_NOT_SEEKABLE:
		return "DZ_ERROR_PLAYER_SEEK_NOT_SEEKABLE";
    case DZ_ERROR_CATEGORY_MEDIASTREAMER:
		return "DZ_ERROR_CATEGORY_MEDIASTREAMER";
    case DZ_ERROR_MEDIASTREAMER_BAD_URL_SCHEME:
		return "DZ_ERROR_MEDIASTREAMER_BAD_URL_SCHEME";
    case DZ_ERROR_MEDIASTREAMER_BAD_URL_HOST:
		return "DZ_ERROR_MEDIASTREAMER_BAD_URL_HOST";
    case DZ_ERROR_MEDIASTREAMER_BAD_URL_TRACK:
		return "DZ_ERROR_MEDIASTREAMER_BAD_URL_TRACK";
    case DZ_ERROR_MEDIASTREAMER_NOT_AVAILABLE_OFFLINE:
		return "DZ_ERROR_MEDIASTREAMER_NOT_AVAILABLE_OFFLINE";
    case DZ_ERROR_MEDIASTREAMER_NOT_READABLE:
		return "DZ_ERROR_MEDIASTREAMER_NOT_READABLE";
    case DZ_ERROR_MEDIASTREAMER_NO_DURATION:
		return "DZ_ERROR_MEDIASTREAMER_NO_DURATION";
    case DZ_ERROR_MEDIASTREAMER_NOT_INDEXED:
		return "DZ_ERROR_MEDIASTREAMER_NOT_INDEXED";
    case DZ_ERROR_MEDIASTREAMER_SEEK_NOT_SEEKABLE:
		return "DZ_ERROR_MEDIASTREAMER_SEEK_NOT_SEEKABLE";
    case DZ_ERROR_MEDIASTREAMER_NO_DATA:
		return "DZ_ERROR_MEDIASTREAMER_NO_DATA";
    case DZ_ERROR_MEDIASTREAMER_END_OF_STREAM:
		return "DZ_ERROR_MEDIASTREAMER_END_OF_STREAM";
    case DZ_ERROR_MEDIASTREAMER_ALREADY_MAPPED:
		return "DZ_ERROR_MEDIASTREAMER_ALREADY_MAPPED";
    case DZ_ERROR_MEDIASTREAMER_NOT_MAPPED:
		return "DZ_ERROR_MEDIASTREAMER_NOT_MAPPED";
    case DZ_ERROR_CATEGORY_OFFLINE:
		return "DZ_ERROR_CATEGORY_OFFLINE";
    case DZ_ERROR_OFFLINE_FS_FULL:
		return "DZ_ERROR_OFFLINE_FS_FULL";
    case DZ_ERROR_PLAYER_BAD_URL:
		return "DZ_ERROR_PLAYER_BAD_URL";
	default: 
		return "UNKNOWN";
	}
}