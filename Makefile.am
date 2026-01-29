include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_audio_bug

mod_LTLIBRARIES = mod_audio_bug.la
mod_audio_bug_la_SOURCES  = mod_audio_bug.c lws_glue.c
mod_audio_bug_la_CFLAGS   = $(AM_CFLAGS) `pkg-config --cflags libwebsockets speexdsp`

mod_audio_bug_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_audio_bug_la_LDFLAGS  = -avoid-version -module -no-undefined -shared `pkg-config --libs libwebsockets speexdsp`
