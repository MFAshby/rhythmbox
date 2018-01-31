#ifndef RB_DEEZER_ENTRY_TYPE_H
#define RB_DEEZER_ENTRY_TYPE_H

#include <glib-object.h>
#include "rhythmdb-entry-type.h"

G_DECLARE_FINAL_TYPE (
    RBDeezerEntryType,
    rb_deezer_entry_type,
    RB, DEEZER_ENTRY_TYPE,
    RhythmDBEntryType
);

void _rb_deezer_entry_type_register_type(GTypeModule*);

#endif