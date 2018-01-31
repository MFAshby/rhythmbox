# plugin library file called libdeezerpl in order not to conflict with libdeezer.so
# as provided by Deezer SDK. Otherwise build will not work.

plugindir = $(PLUGINDIR)/deezer
plugindatadir = $(PLUGINDATADIR)/deezer
plugin_LTLIBRARIES = libdeezerpl.la

libdeezerpl_la_SOURCES = \
  rb-deezer-source.h \
  rb-deezer-plugin.h \
  rb-deezer-source.c \
  rb-deezer-plugin.c \
  rb-deezer-player.c \
  rb-deezer-player.h \
  rb-deezer-entry-type.h \
  rb-deezer-entry-type.c \
  yuarel.c \
  yuarel.h
  
libdeezerpl_la_LDFLAGS = $(PLUGIN_LIBTOOL_FLAGS) 
libdeezerpl_la_LIBTOOLFLAGS = --tag=disable-static
libdeezerpl_la_LIBADD = \
  $(top_builddir)/shell/librhythmbox-core.la \
  $(DEEZER_LIBS) 

AM_CPPFLAGS = \
  -DGNOMELOCALEDIR=\""$(datadir)/locale"\" \
  -DG_LOG_DOMAIN=\"Rhythmbox\" \
  -I$(top_srcdir) \
  -I$(top_srcdir)/lib \
  -I$(top_srcdir)/metadata \
  -I$(top_srcdir)/rhythmdb \
  -I$(top_srcdir)/widgets \
  -I$(top_srcdir)/sources \
  -I$(top_srcdir)/plugins \
  -I$(top_srcdir)/shell \
  -I$(top_srcdir)/backends \
  -DPIXMAP_DIR=\""$(datadir)/pixmaps"\" \
  -DSHARE_DIR=\"$(pkgdatadir)\" \
  -DDATADIR=\""$(datadir)"\" \
  $(RHYTHMBOX_CFLAGS) \
  $(DEEZER_CFLAGS) \
  -D_DEFAULT_SOURCE \
  -std=c11

plugin_in_files = deezer.plugin.in
%.plugin: %.plugin.in $(INTLTOOL_MERGE) $(wildcard $(top_srcdir)/po/*po) ; $(INTLTOOL_MERGE) $(top_srcdir)/po $< $@ -d -u -c $(top_builddir)/po/.intltool-merge-cache

plugin_DATA = $(plugin_in_files:.plugin.in=.plugin)
EXTRA_DIST = $(plugin_in_files)
CLEANFILES = $(plugin_DATA)
DISTCLEANFILES = $(plugin_DATA)
