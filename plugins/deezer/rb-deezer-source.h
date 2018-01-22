#ifndef DEEZER_SOURCE_H
#define DEEZER_SOURCE_H

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib-object.h>
#include "rb-source.h"

G_BEGIN_DECLS

typedef struct _RBDeezerPlugin RBDeezerPlugin;

G_DECLARE_FINAL_TYPE(
    RBDeezerSource, 
    rb_deezer_source, 
    RB, DEEZER_SOURCE, 
    RBSource
);

void _rb_deezer_source_register_type (GTypeModule *type_module);

void rb_deezer_source_setup(RBDeezerSource*);

G_END_DECLS

#endif