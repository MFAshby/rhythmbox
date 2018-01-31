#include "rb-deezer-login.h"
#include "rb-deezer-plugin.h"
#include "yuarel.h"
#include "rb-shell.h"
#include "rb-debug.h"
#include <webkit2/webkit2.h>
#include <string.h>
#include <glib/gi18n-lib.h>

const char* OAUTH_URL = "https://connect.deezer.com/oauth/auth.php?"
                "app_id=" APP_ID
                "&redirect_uri=http://127.0.0.1:8080"
                "&perms=basic_access,offline_access"
                "&response_type=token";

static char* query_val(const char* key, char* querystr) {
    if (querystr == NULL) {
        return NULL;
    }
    struct yuarel_param p[5] = {0};
    int n = yuarel_parse_query(querystr, '&', p, 5);
    for (int i=0; i<n; i++) {
        if (strcmp(key, p[i].key) == 0) {
            return p[i].val;
        }
    }
    return NULL;
}


static void rb_deezer_source_show_login_error(RBDeezerSource* deezer_source) {
    RBShell* shell;
    GtkWindow* window;
    g_object_get(deezer_source, 
        "shell", &shell,
        NULL);
    g_object_get(shell, 
        "window", &window, 
        NULL);

    GtkWidget* dlg = gtk_message_dialog_new(
        window, 
        0, 
        GTK_MESSAGE_ERROR,
        GTK_BUTTONS_CLOSE,
        _("You must log into Deezer for Rhythmbox to access tracks")
    );
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_object_unref(shell);
    g_object_unref(window);
}

static void web_view_load_changed(WebKitWebView* wv, WebKitLoadEvent evt, gpointer user_data) {
    RBDeezerSource* deezer_source = RB_DEEZER_SOURCE(user_data);
    
    if (evt != WEBKIT_LOAD_REDIRECTED) {
        return;
    }

    struct yuarel u;
    const gchar* uri = webkit_web_view_get_uri(wv);
    if (yuarel_parse(&u, (char*)uri)) {
        rb_debug("Error parsing redirected uri %s", uri);
        return;
    }

    char* access_token = query_val("access_token", u.fragment);
    if (access_token != NULL) {
        rb_debug("Got access token");
        RBDeezerPlugin* plugin;
        g_object_get(deezer_source, "plugin", &plugin, NULL);
        g_object_set(plugin, "access-token", access_token, NULL);
        g_object_unref(plugin);
        rb_deezer_source_show_search(deezer_source);
    }

    char* error_reason = query_val("error_reason", u.query);
    if (error_reason != NULL) {
        rb_debug("Error logging into Deezer");
        rb_deezer_source_show_login_error(deezer_source);
        rb_deezer_source_show_login(deezer_source);
    }
}
