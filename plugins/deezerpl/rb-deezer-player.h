#ifndef RB_DEEZER_PLAYER_H
#define RB_DEEZER_PLAYER_H

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib-object.h>

G_DECLARE_FINAL_TYPE(
    RBDeezerPlayer, 
    rb_deezer_player, 
    RB, DEEZER_PLAYER, 
    GObject
);

void _rb_deezer_player_register_type(GTypeModule*);

#endif