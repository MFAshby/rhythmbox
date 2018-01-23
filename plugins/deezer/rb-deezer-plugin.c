#include "rb-deezer-plugin.h"
#include "rb-deezer-source.h"
#include "rb-deezer-entry-type.h"
#include "rb-deezer-player.h"
#include "rb-debug.h"
#include "rb-shell.h"
#include "rb-shell-player.h"
#include "rb-display-page-group.h"
#include "rhythmdb.h"
#include "rb-file-helpers.h"

#include <glib/gi18n-lib.h>

enum {
	RB_DEEZER_PLUGIN_PROP_ACCESS_TOKEN = 2 // Property 1 is object
};

static void rb_deezer_plugin_init (RBDeezerPlugin *);
static void impl_activate (PeasActivatable* );
static void impl_deactivate	(PeasActivatable *plugin);
static void set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void rb_deezer_plugin_class_init (RBDeezerPluginClass *klass);
static void rb_deezer_plugin_class_finalize (RBDeezerPluginClass *klass);
static void peas_activatable_iface_init (PeasActivatableInterface *iface);
static void load_access_token_file(RBDeezerPlugin* plugin);
static void save_access_token_file(RBDeezerPlugin* plugin);

static void rb_deezer_plugin_access_token_changed(RBDeezerPlugin* pl, 
												GParamSpec* property, 
												gpointer data) {
	char* access_token;
	g_object_get(pl, "access-token", &access_token, NULL);
	if (strlen(access_token) > 0) {
		rb_debug("Access token received, passing to Deezer connect handle %p", pl->handle);
		dz_error_t err = dz_connect_set_access_token(pl->handle, NULL, NULL, access_token);
		if (err != DZ_ERROR_NO_ERROR) {
			rb_debug("Error setting access token in Deezer lib %d", err);
		}

		// Force online operation to ensure we're logged in
		err = dz_connect_offline_mode(pl->handle, NULL, NULL, false);
		if (err != DZ_ERROR_NO_ERROR) {
			rb_debug("Error forcing online mode after access token receipt %d", err);
		}

		save_access_token_file(pl);
	}
}

static void rb_deezer_plugin_init (RBDeezerPlugin *plugin) {
	rb_debug ("Deezer plugin init");
	plugin->access_token = "";
	g_signal_connect(
		plugin, 
		"notify::access-token", G_CALLBACK(rb_deezer_plugin_access_token_changed), 
		NULL
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
	case RB_DEEZER_PLUGIN_PROP_ACCESS_TOKEN:
		deezer_plugin->access_token = g_value_dup_string(value);
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
	case RB_DEEZER_PLUGIN_PROP_ACCESS_TOKEN:
		g_value_set_string(value, deezer_plugin->access_token);
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

	g_object_class_install_property(
		object_class,
		RB_DEEZER_PLUGIN_PROP_ACCESS_TOKEN, 
		g_param_spec_string(
			"access-token", 
			"Access token", 
			"Access token for deezer API", 
			"", 
			G_PARAM_READWRITE
		)
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

static void rb_deezer_plugin_connect_event_cb(dz_connect_handle handle,
								dz_connect_event_handle event,
								void* user_data) {
	RBDeezerPlugin* plugin = RB_DEEZER_PLUGIN(user_data);
	rb_debug("CONNECT EVENT plugin %p", plugin);
}

static bool rb_deezer_plugin_crash_reporting_cb() {
	rb_debug("DEEZER HAS CRASHED O NOES");
	return false;
}

static void save_access_token_file(RBDeezerPlugin* plugin) {
	char* access_tok_file_path = rb_find_user_data_file("dz_access_token");
	const gchar* access_token;
	g_object_get(plugin, "access-token", &access_token, NULL);
	if (strlen(access_token) > 0) {
		GError* err = NULL;
		if (!g_file_set_contents(access_tok_file_path, access_token, -1, &err)) {
			rb_debug("Error saving access token to %s", err->message);
		} else {
			rb_debug("Saved access token to %s", access_tok_file_path);
		}
	}
}

static void load_access_token_file(RBDeezerPlugin* plugin) {
	char* access_tok_file_path = rb_find_user_data_file("dz_access_token");
	gchar* access_token;
	GError* error = NULL;
	if (g_file_get_contents(access_tok_file_path, &access_token, NULL, &error)) {
		rb_debug("Loaded access token from file %s", access_token);
		g_object_set(plugin, "access-token", access_token, NULL);
	} else {
		rb_debug("Error loading access token %s", error->message);
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

	// Open connection to deezer api
	char* cache_file = rb_find_user_cache_file("dz_cache");
	struct dz_connect_configuration config; 
	memset(&config, 0, sizeof(struct dz_connect_configuration));
	config.app_id = APP_ID;
	config.product_id = "Javaplayer";
	config.product_build_id = "00001";
	config.user_profile_path = cache_file;
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

	// Try to load the access token
	load_access_token_file(plugin);

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
	rb_deezer_source_setup(source);

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

    g_object_unref (db);
	g_object_unref (shell);
	g_object_unref (shell_player);
}

static void impl_deactivate	(PeasActivatable *plugin) {
	rb_debug ("Deezer plugin deactivated");
}

static void peas_activatable_iface_init (PeasActivatableInterface *iface) {	
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
		rb_debug("Error parsing JSON response %s", err->message);
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

	if (strlen(pl->access_token) > 0) {
		g_hash_table_insert(query_params, "access_token", (char*)pl->access_token);
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