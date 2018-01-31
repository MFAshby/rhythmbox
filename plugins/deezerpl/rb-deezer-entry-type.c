#include "rb-deezer-entry-type.h"

enum {
    PROP_DZ_ENTRY_NAME = 1,
    N_PROPERTIES
};

struct _RBDeezerEntryType {
    RhythmDBEntryType parent;
    char* name;
};

static void rb_deezer_entry_type_init(RBDeezerEntryType* entry_type) {}

static void rb_deezer_entry_get_prop(GObject* object, 
                            guint property_id, 
                            GValue* value, 
                            GParamSpec* pspec) {

    RBDeezerEntryType* entry = RB_DEEZER_ENTRY_TYPE(object);
    switch (property_id) {
        case PROP_DZ_ENTRY_NAME:
            g_value_set_string(value, entry->name);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            break;
    }
}

static void rb_deezer_entry_set_prop(GObject* object,
                            guint property_id,
                            const GValue* value,
                            GParamSpec* pspec) {
    RBDeezerEntryType* entry = RB_DEEZER_ENTRY_TYPE(object);
    switch (property_id) {
        case PROP_DZ_ENTRY_NAME:
            g_free(entry->name);
            entry->name = g_value_dup_string(value);
            break;
        default: 
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            break;
    }
}

static void rb_deezer_entry_type_class_init(RBDeezerEntryTypeClass* entry_type_cls) {
    GObjectClass* cls = G_OBJECT_CLASS(entry_type_cls);
    cls->get_property = rb_deezer_entry_get_prop;
    cls->set_property = rb_deezer_entry_set_prop;
    
    g_object_class_install_property(cls, PROP_DZ_ENTRY_NAME, 
        g_param_spec_string(
            "name", 
            "Name", 
            "Name of the entry type", 
            "", 
            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
        )
    );
}

static void rb_deezer_entry_type_class_finalize(RBDeezerEntryTypeClass* entry_type_cls) {
    
}

G_DEFINE_DYNAMIC_TYPE(
    RBDeezerEntryType, 
    rb_deezer_entry_type,
    rhythmdb_entry_type_get_type()
)

void _rb_deezer_entry_type_register_type(GTypeModule* type_module) {
    rb_deezer_entry_type_register_type(type_module);
}