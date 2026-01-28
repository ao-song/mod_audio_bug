include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_audio_fork

mod_LTLIBRARIES = mod_audio_fork.la
mod_audio_fork_la_SOURCES  = mod_audio_fork.c lws_glue.c
mod_audio_fork_la_CFLAGS   = $(AM_CFLAGS) `pkg-config --cflags libwebsockets speexdsp`

mod_audio_fork_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_audio_fork_la_LDFLAGS  = -avoid-version -module -no-undefined -shared `pkg-config --libs libwebsockets speexdsp`
