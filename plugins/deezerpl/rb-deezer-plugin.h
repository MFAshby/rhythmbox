#ifndef DEEZER_PLUGIN_H
#define DEEZER_PLUGIN_H

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib-object.h>
#include <deezer-connect.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "rhythmdb-entry-type.h"
#include "rb-plugin-macros.h"
#include "rb-deezer-source.h"

#define G_SETTINGS_SCHEMA "org.gnome.rhythmbox.plugins.deezer"
#define APP_ID "264662"

G_BEGIN_DECLS

struct _RBDeezerPlugin {
	PeasExtensionBase parent;
	dz_connect_handle handle;
	SoupSession* soup_session;
	GSettings* settings;
};

G_DECLARE_FINAL_TYPE(
	RBDeezerPlugin,
	rb_deezer_plugin,
	RB, DEEZER_PLUGIN,
	PeasExtensionBase
);

// This function needs to be exported for plugin to be loaded
G_MODULE_EXPORT void peas_register_types (PeasObjectModule *module);

// API calls are routed through here.
typedef void (*rb_deezer_plugin_api_cb_func) (JsonNode* node, void* user_data);

void rb_deezer_plugin_api_call(RBDeezerPlugin* pl, 
							  rb_deezer_plugin_api_cb_func callback, 
							  void* user_data, 
							  char* endpoint, 
							  ... /* Parameters: key, value, key2, value2 */);


G_END_DECLS

#endif