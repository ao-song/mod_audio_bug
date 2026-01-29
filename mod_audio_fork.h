#ifndef __MOD_FORK_H__
#define __MOD_FORK_H__

#include <switch.h>
#include <unistd.h>

#define MY_BUG_NAME "audio_fork"
#define MAX_SESSION_ID (256)
#define MAX_WS_URL_LEN (512)
#define MAX_PATH_LEN (128)

#define EVENT_TRANSCRIPTION   "mod_audio_bug::transcription"
#define EVENT_TRANSFER        "mod_audio_bug::transfer"
#define EVENT_PLAY_AUDIO      "mod_audio_bug::play_audio"
#define EVENT_KILL_AUDIO      "mod_audio_bug::kill_audio"
#define EVENT_DISCONNECT      "mod_audio_bug::disconnect"
#define EVENT_ERROR           "mod_audio_bug::error"
#define EVENT_CONNECT_SUCCESS "mod_audio_bug::connect"
#define EVENT_CONNECT_FAIL    "mod_audio_bug::connect_failed"
#define EVENT_BUFFER_OVERRUN  "mod_audio_bug::buffer_overrun"
#define EVENT_JSON            "mod_audio_bug::json"

#define MAX_METADATA_LEN (8192)

struct playout {
  char *file;
  struct playout* next;
};

typedef void (*responseHandler_t)(switch_core_session_t* session, const char* eventName, char* json);

struct private_data {
	switch_mutex_t *mutex;
	char sessionId[MAX_SESSION_ID];
  void *resampler;
  responseHandler_t responseHandler;
  void *pAudioPipe;
  int ws_state;
  char host[MAX_WS_URL_LEN];
  unsigned int port;
  char path[MAX_PATH_LEN];
  int sampling;
  struct playout* playout;
  int  channels;
  unsigned int id;
  int buffer_overrun_notified;
  int audio_paused;
  int graceful_shutdown;
  char initialMetadata[MAX_METADATA_LEN];
};

typedef struct private_data private_t;

#endif
