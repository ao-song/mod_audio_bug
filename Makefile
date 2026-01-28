MODNAME=mod_audio_fork
MODOBJ=mod_audio_fork.o lws_glue.o

# Try to find FreeSwitch make include
FS_MAKE=$(shell which freeswitch-config >/dev/null 2>&1 && freeswitch-config --prefix)/include/freeswitch/module_make.inc

# Fallback if not found (adjust path as needed)
ifeq ($(wildcard $(FS_MAKE)),)
FS_MAKE=/usr/local/freeswitch/include/freeswitch/module_make.inc
endif

PKG_CFLAGS=$(shell pkg-config --cflags libwebsockets speexdsp)
PKG_LDFLAGS=$(shell pkg-config --libs libwebsockets speexdsp)

include $(FS_MAKE)
