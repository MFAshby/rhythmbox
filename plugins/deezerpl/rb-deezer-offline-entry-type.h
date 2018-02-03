#ifndef RB_OFFLINE_DEEZER_ENTRY_TYPE_H
#define RB_OFFLINE_DEEZER_ENTRY_TYPE_H

#include <glib-object.h>
#include "rhythmdb-entry-type.h"

G_DECLARE_FINAL_TYPE (
    RBDeezerOfflineEntryType,
    rb_deezer_offline_entry_type,
    RB, DEEZER_OFFLINE_ENTRY_TYPE,
    RhythmDBEntryType
);

void _rb_deezer_offline_entry_type_register_type(GTypeModule*);

#endif // RB_OFFLINE_DEEZER_ENTRY_TYPE_H