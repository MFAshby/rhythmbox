#ifndef DEEZER_OFFLINE_SOURCE_H
#define DEEZER_OFFLINE_SOURCE_H

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib-object.h>
#include "rb-browser-source.h"

G_BEGIN_DECLS

typedef struct _RBDeezerPlugin RBDeezerPlugin;

G_DECLARE_FINAL_TYPE(
    RBDeezerOfflineSource, 
    rb_deezer_offline_source, 
    RB, DEEZER_OFFLINE_SOURCE, 
    RBBrowserSource
);

void _rb_deezer_offline_source_register_type (GTypeModule *type_module);

G_END_DECLS

#endif // DEEZER_OFFLINE_SOURCE_H