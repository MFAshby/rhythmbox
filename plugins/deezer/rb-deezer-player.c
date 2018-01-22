#include "rb-deezer-player.h"

#include <deezer-player.h>

#include "rb-deezer-plugin.h"
#include "rb-player.h"

enum {
    RB_DEEZER_PLAYER_PROP_PLUGIN = 1
};

struct _RBDeezerPlayer {
    GObject parent;
    RBDeezerPlugin* plugin;
};

static void rb_deezer_player_player_init(RBPlayer* player) {

}

static void rb_deezer_player_init(RBDeezerPlayer* player) {
    
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


static void rb_deezer_player_class_init(RBDeezerPlayerClass* class) {
    GObjectClass* object_class =  G_OBJECT_CLASS(class);
    object_class->get_property = get_property;
    object_class->set_property = set_property;
    
    g_object_class_install_property(object_class, 
        RB_DEEZER_PLAYER_PROP_PLUGIN,
        g_param_spec_object(
            "plugin", "plugin", "The RBDeezerPlugin", 
            rb_deezer_plugin_get_type(), 
            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY)
    );
}

static void rb_deezer_player_class_finalize(RBDeezerPlayerClass* class) {
    
}

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    RBDeezerPlayer,
    rb_deezer_player,
    G_TYPE_OBJECT,
    0,
    G_IMPLEMENT_INTERFACE_DYNAMIC(
        RB_TYPE_PLAYER,
        rb_deezer_player_player_init
    ) 
);

void _rb_deezer_player_register_type(GTypeModule* m) {
    rb_deezer_player_register_type(m);
}