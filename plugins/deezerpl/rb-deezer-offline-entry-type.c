#include "rb-deezer-offline-entry-type.h"

struct _RBDeezerOfflineEntryType {
    RhythmDBEntryType parent;
};

static void rb_deezer_offline_entry_type_init(RBDeezerOfflineEntryType* entry_type) {}

static void rb_deezer_offline_entry_type_class_init(RBDeezerOfflineEntryTypeClass* entry_type_cls) {}

static void rb_deezer_offline_entry_type_class_finalize(RBDeezerOfflineEntryTypeClass* entry_type_cls) {}

G_DEFINE_DYNAMIC_TYPE(
    RBDeezerOfflineEntryType, 
    rb_deezer_offline_entry_type,
    rhythmdb_entry_type_get_type()
)

void _rb_deezer_offline_entry_type_register_type(GTypeModule* type_module) {
    rb_deezer_offline_entry_type_register_type(type_module);
}