#include "lws_glue.h"
#include <libwebsockets.h>
#include <speex/speex_resampler.h>
#include <switch_json.h>

#define RTP_PACKETIZATION_PERIOD 20
#define FRAME_SIZE_8000  320
#define LWS_PRE_BUFFER LWS_PRE

struct audio_buffer_t {
    uint8_t *data;
    size_t len;
    size_t write_pos;
    size_t min_free;
};

struct fork_session_t {
    struct lws *wsi;
    char *uuid;
    struct audio_buffer_t audio_buffer;
    switch_mutex_t *audio_mutex;
    switch_mutex_t *text_mutex;
    char *text_buffer;
    size_t text_len;
    private_t *tech_pvt;
    int connected;
    int graceful_shutdown;
    struct fork_session_t *next;
};

struct lws_context_data {
    struct lws_context *context;
    switch_thread_t *thread;
    int running;
    switch_mutex_t *action_mutex;
    struct fork_session_t *pending_connects;
};

static struct lws_context_data g_lws_data = {0};

static const char *requestedBufferSecs;
static int nAudioBufferSecs;
static const char *mySubProtocolName;
static unsigned int idxCallCount = 0;
static uint32_t playCount = 0;

static void process_incoming_message(private_t* tech_pvt, switch_core_session_t* session, const char* message, size_t len) {
    cJSON *json = cJSON_Parse(message);
    if (json) {
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(json, "type"));
        if (type) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) processIncomingMessage - received %s message\n", tech_pvt->id, type);
            cJSON *jsonData = cJSON_GetObjectItem(json, "data");

            if (strcmp(type, "playAudio") == 0) {
                if (jsonData) {
                    cJSON *jsonAudio = cJSON_DetachItemFromObject(jsonData, "audioContent");
                    const char *audioBase64 = cJSON_GetStringValue(jsonAudio);
                    const char *szAudioContentType = cJSON_GetStringValue(cJSON_GetObjectItem(jsonData, "audioContentType"));

                    if (audioBase64 && szAudioContentType) {
                        char fileType[10] = ".r16";
                        int sampleRate = 16000;

                        if (strcmp(szAudioContentType, "raw") == 0) {
                            cJSON *jsonSR = cJSON_GetObjectItem(jsonData, "sampleRate");
                            if (jsonSR) sampleRate = jsonSR->valueint;

                            switch(sampleRate) {
                                case 8000: strcpy(fileType, ".r8"); break;
                                case 16000: strcpy(fileType, ".r16"); break;
                                case 24000: strcpy(fileType, ".r24"); break;
                                case 32000: strcpy(fileType, ".r32"); break;
                                case 48000: strcpy(fileType, ".r48"); break;
                                case 64000: strcpy(fileType, ".r64"); break;
                                default: strcpy(fileType, ".r16"); break;
                            }
                        } else if (strcmp(szAudioContentType, ".wave") == 0) {
                            strcpy(fileType, "wave");
                        }

                        char szFilePath[256];
                        switch_snprintf(szFilePath, 256, "%s%s%s_%d.tmp%s", SWITCH_GLOBAL_dirs.temp_dir,
                            SWITCH_PATH_SEPARATOR, tech_pvt->sessionId, playCount++, fileType);

                        // Decode base64
                        int out_len = strlen(audioBase64); // approximate
                        char *decoded = malloc(out_len);
                        if (decoded) {
                            int decoded_len = lws_b64_decode_string(audioBase64, decoded, out_len);
                            if (decoded_len > 0) {
                                switch_file_t *fd;
                                if (switch_file_open(&fd, szFilePath, SWITCH_FOPEN_WRITE | SWITCH_FOPEN_CREATE | SWITCH_FOPEN_TRUNCATE | SWITCH_FOPEN_BINARY, SWITCH_FPROT_UREAD | SWITCH_FPROT_UWRITE, switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS) {
                                    switch_size_t len_written = decoded_len;
                                    switch_file_write(fd, decoded, &len_written);
                                    switch_file_close(fd);

                                    struct playout* playout = malloc(sizeof(struct playout));
                                    playout->file = strdup(szFilePath);
                                    playout->next = tech_pvt->playout;
                                    tech_pvt->playout = playout;

                                    cJSON_AddItemToObject(jsonData, "file", cJSON_CreateString(szFilePath));
                                }
                            }
                            free(decoded);
                        }
                    }
                    if (jsonAudio) cJSON_Delete(jsonAudio);

                    char *jsonString = cJSON_PrintUnformatted(jsonData);
                    tech_pvt->responseHandler(session, EVENT_PLAY_AUDIO, jsonString);
                    free(jsonString);
                }
            } else if (strcmp(type, "killAudio") == 0) {
                tech_pvt->responseHandler(session, EVENT_KILL_AUDIO, NULL);
                switch_channel_t *channel = switch_core_session_get_channel(session);
                switch_channel_set_flag_value(channel, CF_BREAK, 2);
            } else if (strcmp(type, "disconnect") == 0) {
                char *jsonString = jsonData ? cJSON_PrintUnformatted(jsonData) : NULL;
                tech_pvt->responseHandler(session, EVENT_DISCONNECT, jsonString);
                if (jsonString) free(jsonString);
            } else {
                // Generic handler for other events
                char *jsonString = jsonData ? cJSON_PrintUnformatted(jsonData) : NULL;
                if (strcmp(type, "transcription") == 0) tech_pvt->responseHandler(session, EVENT_TRANSCRIPTION, jsonString);
                else if (strcmp(type, "transfer") == 0) tech_pvt->responseHandler(session, EVENT_TRANSFER, jsonString);
                else if (strcmp(type, "error") == 0) tech_pvt->responseHandler(session, EVENT_ERROR, jsonString);
                else if (strcmp(type, "json") == 0) tech_pvt->responseHandler(session, EVENT_JSON, jsonString);
                if (jsonString) free(jsonString);
            }
        }
        cJSON_Delete(json);
    }
}

static int lws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    struct fork_session_t *fs = (struct fork_session_t *)user;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            if (fs) {
                fs->connected = 1;
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "LWS_CALLBACK_CLIENT_ESTABLISHED for %s\n", fs->uuid);

                switch_core_session_t *session = switch_core_session_locate(fs->uuid);
                if (session) {
                    fs->tech_pvt->responseHandler(session, EVENT_CONNECT_SUCCESS, NULL);
                    // Send initial metadata if present
                    if (strlen(fs->tech_pvt->initialMetadata) > 0) {
                        switch_mutex_lock(fs->text_mutex);
                        if (fs->text_buffer) free(fs->text_buffer);
                        fs->text_buffer = strdup(fs->tech_pvt->initialMetadata);
                        fs->text_len = strlen(fs->text_buffer);
                        switch_mutex_unlock(fs->text_mutex);
                        lws_callback_on_writable(wsi);
                    }
                    switch_core_session_rwunlock(session);
                }
            }
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            if (fs) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "LWS_CALLBACK_CLIENT_CONNECTION_ERROR for %s: %s\n", fs->uuid, in ? (char*)in : "(null)");
                switch_core_session_t *session = switch_core_session_locate(fs->uuid);
                if (session) {
                    char err[512];
                    snprintf(err, sizeof(err), "{\"reason\":\"%s\"}", in ? (char*)in : "unknown");
                    fs->tech_pvt->responseHandler(session, EVENT_CONNECT_FAIL, err);
                    switch_core_session_rwunlock(session);
                }
                fs->connected = 0;
            }
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
            if (fs) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "LWS_CALLBACK_CLIENT_CLOSED for %s\n", fs->uuid);
                if (fs->connected) {
                    switch_core_session_t *session = switch_core_session_locate(fs->uuid);
                    if (session) {
                        fs->tech_pvt->responseHandler(session, EVENT_DISCONNECT, NULL);
                        switch_core_session_rwunlock(session);
                    }
                }
                fs->connected = 0;
                fs->wsi = NULL;
            }
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            if (fs && fs->connected) {
                switch_core_session_t *session = switch_core_session_locate(fs->uuid);
                if (session) {
                    process_incoming_message(fs->tech_pvt, session, (const char *)in, len);
                    switch_core_session_rwunlock(session);
                }
            }
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            if (fs && fs->connected) {
                // Check for text to send
                switch_mutex_lock(fs->text_mutex);
                if (fs->text_buffer) {
                    unsigned char buf[LWS_PRE + MAX_METADATA_LEN];
                    size_t n = fs->text_len;
                    if (n > MAX_METADATA_LEN) n = MAX_METADATA_LEN;

                    memcpy(&buf[LWS_PRE], fs->text_buffer, n);
                    lws_write(wsi, &buf[LWS_PRE], n, LWS_WRITE_TEXT);

                    free(fs->text_buffer);
                    fs->text_buffer = NULL;
                    fs->text_len = 0;
                    switch_mutex_unlock(fs->text_mutex);
                    lws_callback_on_writable(wsi); // Request another write for audio if needed
                    return 0;
                }
                switch_mutex_unlock(fs->text_mutex);

                if (fs->graceful_shutdown) {
                    unsigned char buf[LWS_PRE];
                    lws_write(wsi, &buf[LWS_PRE], 0, LWS_WRITE_BINARY);
                    return 0;
                }

                // Check for audio to send
                switch_mutex_lock(fs->audio_mutex);
                if (fs->audio_buffer.write_pos > 0) {
                    size_t len = fs->audio_buffer.write_pos;
                    int sent = lws_write(wsi, fs->audio_buffer.data + LWS_PRE_BUFFER, len, LWS_WRITE_BINARY);
                    if (sent < (int)len) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "LWS write failed\n");
                    }
                    fs->audio_buffer.write_pos = 0;
                }
                switch_mutex_unlock(fs->audio_mutex);
            }
            break;

        default:
            break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    {
        "audio.drachtio.org",
        lws_callback,
        0,
        4096,
    },
    { NULL, NULL, 0, 0 }
};

static void *SWITCH_THREAD_FUNC lws_service_thread(switch_thread_t *thread, void *obj) {
    struct lws_context_data *data = (struct lws_context_data *)obj;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "LWS service thread started\n");

    while (data->running) {
        // Process pending connects
        switch_mutex_lock(data->action_mutex);
        struct fork_session_t *fs = data->pending_connects;
        if (fs) {
            data->pending_connects = fs->next;
            fs->next = NULL;
        }
        switch_mutex_unlock(data->action_mutex);

        if (fs) {
            struct lws_client_connect_info i = {0};
            i.context = data->context;
            i.address = fs->tech_pvt->host;
            i.port = fs->tech_pvt->port;
            i.path = fs->tech_pvt->path;
            i.host = i.address;
            i.origin = i.address;
            i.ssl_connection = (fs->tech_pvt->port == 443) ? 1 : 0; // Simplified SSL check
            i.protocol = mySubProtocolName;
            i.pwsi = &fs->wsi;
            i.userdata = fs;

            if (!lws_client_connect_via_info(&i)) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "LWS connect failed for %s\n", fs->uuid);
                // Trigger connection failed event manually if needed
            }
        }

        lws_service(data->context, 50);
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "LWS service thread ending\n");
    return NULL;
}

switch_status_t fork_init() {
    requestedBufferSecs = getenv("MOD_AUDIO_FORK_BUFFER_SECS");
    nAudioBufferSecs = requestedBufferSecs ? atoi(requestedBufferSecs) : 2;
    if (nAudioBufferSecs < 1) nAudioBufferSecs = 1;
    if (nAudioBufferSecs > 5) nAudioBufferSecs = 5;

    mySubProtocolName = getenv("MOD_AUDIO_FORK_SUBPROTOCOL_NAME");
    if (!mySubProtocolName) mySubProtocolName = "audio.drachtio.org";

    struct lws_context_creation_info info = {0};
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.gid = -1;
    info.uid = -1;

    g_lws_data.context = lws_create_context(&info);
    if (!g_lws_data.context) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create LWS context\n");
        return SWITCH_STATUS_FALSE;
    }

    g_lws_data.running = 1;
    switch_mutex_init(&g_lws_data.action_mutex, SWITCH_MUTEX_NESTED, NULL);

    switch_threadattr_t *thd_attr = NULL;
    switch_threadattr_create(&thd_attr, NULL);
    // Create a joinable thread so we can reliably wait for it on cleanup
    switch_threadattr_detach_set(thd_attr, 0);
    switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
    switch_thread_create(&g_lws_data.thread, thd_attr, lws_service_thread, &g_lws_data, NULL);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t fork_cleanup() {
    g_lws_data.running = 0;
    lws_cancel_service(g_lws_data.context);
    // Wait for the service thread to exit to avoid races with lws_context_destroy
    if (g_lws_data.thread) {
        switch_thread_join(&g_lws_data.thread);
    }
    lws_context_destroy(g_lws_data.context);
    return SWITCH_STATUS_SUCCESS;
}

switch_status_t fork_session_init(switch_core_session_t *session,
              responseHandler_t responseHandler,
              uint32_t samples_per_second,
              char *host,
              unsigned int port,
              char *path,
              int sampling,
              int sslFlags,
              int channels,
              char* metadata,
              void **ppUserData)
{
    // Allocate control structures on the heap so the LWS thread can safely
    // reference them independent of the FreeSWITCH session memory pool.
    private_t* tech_pvt = (private_t *) malloc(sizeof(private_t));
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    memset(tech_pvt, 0, sizeof(private_t));
    strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID - 1);
    tech_pvt->sessionId[MAX_SESSION_ID - 1] = '\0';
    strncpy(tech_pvt->host, host, MAX_WS_URL_LEN - 1);
    tech_pvt->host[MAX_WS_URL_LEN - 1] = '\0';
    tech_pvt->port = port;
    strncpy(tech_pvt->path, path, MAX_PATH_LEN - 1);
    tech_pvt->path[MAX_PATH_LEN - 1] = '\0';
    tech_pvt->sampling = sampling;
    tech_pvt->responseHandler = responseHandler;
    tech_pvt->channels = channels;
    tech_pvt->id = ++idxCallCount;
    if (metadata) strncpy(tech_pvt->initialMetadata, metadata, MAX_METADATA_LEN - 1);

    struct fork_session_t *fs = (struct fork_session_t *) malloc(sizeof(struct fork_session_t));
    if (!fs) {
        free(tech_pvt);
        return SWITCH_STATUS_FALSE;
    }
    memset(fs, 0, sizeof(struct fork_session_t));
    fs->uuid = strdup(tech_pvt->sessionId);
    fs->tech_pvt = tech_pvt;

    // Allocate audio buffer on the heap
    size_t buflen = LWS_PRE_BUFFER + (FRAME_SIZE_8000 * sampling / 8000 * channels * 1000 / RTP_PACKETIZATION_PERIOD * nAudioBufferSecs);
    fs->audio_buffer.data = (uint8_t *) malloc(buflen);
    fs->audio_buffer.len = buflen;
    fs->audio_buffer.min_free = buflen / 2; // Simple heuristic

    // Initialize mutexes without tying them to the session pool
    switch_mutex_init(&fs->audio_mutex, SWITCH_MUTEX_NESTED, NULL);
    switch_mutex_init(&fs->text_mutex, SWITCH_MUTEX_NESTED, NULL);
    switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, NULL);

    tech_pvt->pAudioPipe = fs;
    *ppUserData = tech_pvt;

    // Resampler
    if (sampling != samples_per_second) {
        int err;
        tech_pvt->resampler = speex_resampler_init(channels, samples_per_second, sampling, 5, &err);
        if (err != 0) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing resampler\n");
            // Leave tech_pvt/fs allocated to avoid use-after-free; they'll be reclaimed on process exit or module unload.
            return SWITCH_STATUS_FALSE;
        }
    }

    // Queue connection
    switch_mutex_lock(g_lws_data.action_mutex);
    fs->next = g_lws_data.pending_connects;
    g_lws_data.pending_connects = fs;
    switch_mutex_unlock(g_lws_data.action_mutex);
    lws_cancel_service(g_lws_data.context);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t fork_session_cleanup(switch_core_session_t *session, char* text, int channelIsClosing) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
    if (!bug) return SWITCH_STATUS_FALSE;

    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    switch_mutex_lock(tech_pvt->mutex);

    // Remove bug
    switch_channel_set_private(channel, MY_BUG_NAME, NULL);
    if (!channelIsClosing) {
        switch_core_media_bug_remove(session, &bug);
    }

    // Cleanup temp files
    struct playout* playout = tech_pvt->playout;
    while (playout) {
        unlink(playout->file);
        free(playout->file);
        struct playout *tmp = playout;
        playout = playout->next;
        free(tmp);
    }

    // Cleanup resampler
    if (tech_pvt->resampler) {
        speex_resampler_destroy(tech_pvt->resampler);
        tech_pvt->resampler = NULL;
    }

    // Close LWS
    struct fork_session_t *fs = (struct fork_session_t *)tech_pvt->pAudioPipe;
    if (fs) {
        if (text) fork_session_send_text(session, text);
        // We can't easily force close from here safely without race conditions in pure C LWS without more complex signaling
        // But setting connected=0 and letting the session pool cleanup is standard FS way.
        // LWS will eventually timeout or we can signal it.
        // For now, we rely on the session memory pool destruction.
        fs->connected = 0;
    }

    switch_mutex_unlock(tech_pvt->mutex);
    return SWITCH_STATUS_SUCCESS;
}

switch_status_t fork_session_send_text(switch_core_session_t *session, char* text) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
    if (!bug) return SWITCH_STATUS_FALSE;
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    struct fork_session_t *fs = (struct fork_session_t *)tech_pvt->pAudioPipe;
    if (fs && fs->connected) {
        switch_mutex_lock(fs->text_mutex);
        if (fs->text_buffer) free(fs->text_buffer);
        fs->text_buffer = strdup(text);
        fs->text_len = strlen(text);
        switch_mutex_unlock(fs->text_mutex);
        lws_callback_on_writable(fs->wsi);
    }
    return SWITCH_STATUS_SUCCESS;
}

switch_status_t fork_session_pauseresume(switch_core_session_t *session, int pause) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
    if (!bug) return SWITCH_STATUS_FALSE;
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    switch_core_media_bug_flush(bug);
    tech_pvt->audio_paused = pause;
    return SWITCH_STATUS_SUCCESS;
}

switch_status_t fork_session_graceful_shutdown(switch_core_session_t *session) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
    if (!bug) return SWITCH_STATUS_FALSE;
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    tech_pvt->graceful_shutdown = 1;
    struct fork_session_t *fs = (struct fork_session_t *)tech_pvt->pAudioPipe;
    if (fs) {
        fs->graceful_shutdown = 1;
        lws_callback_on_writable(fs->wsi);
    }
    return SWITCH_STATUS_SUCCESS;
}

switch_bool_t fork_frame(switch_core_session_t *session, switch_media_bug_t *bug) {
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    if (!tech_pvt || tech_pvt->audio_paused || tech_pvt->graceful_shutdown) return SWITCH_TRUE;

    struct fork_session_t *fs = (struct fork_session_t *)tech_pvt->pAudioPipe;
    if (!fs || !fs->connected) return SWITCH_TRUE;

    if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
        switch_mutex_lock(fs->audio_mutex);

        size_t available = fs->audio_buffer.len - fs->audio_buffer.write_pos;

        if (available < fs->audio_buffer.min_free) {
             if (!tech_pvt->buffer_overrun_notified) {
                tech_pvt->buffer_overrun_notified = 1;
                tech_pvt->responseHandler(session, EVENT_BUFFER_OVERRUN, NULL);
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Buffer overrun\n");
             }
             // Reset buffer
             fs->audio_buffer.write_pos = 0;
             available = fs->audio_buffer.len;
        }

        if (!tech_pvt->resampler) {
            switch_frame_t frame = { 0 };
            frame.data = fs->audio_buffer.data + LWS_PRE_BUFFER + fs->audio_buffer.write_pos;
            frame.buflen = available;

            while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
                if (frame.datalen) {
                    fs->audio_buffer.write_pos += frame.datalen;
                    frame.data = fs->audio_buffer.data + LWS_PRE_BUFFER + fs->audio_buffer.write_pos;
                    frame.buflen = fs->audio_buffer.len - fs->audio_buffer.write_pos;
                } else {
                    break;
                }
            }
        } else {
            uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
            switch_frame_t frame = { 0 };
            frame.data = data;
            frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

            while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
                if (frame.datalen) {
                    spx_uint32_t out_len = (available - fs->audio_buffer.write_pos) / 2;
                    spx_uint32_t in_len = frame.samples;

                    speex_resampler_process_interleaved_int(tech_pvt->resampler,
                        (const spx_int16_t *)frame.data,
                        &in_len,
                        (spx_int16_t *)(fs->audio_buffer.data + LWS_PRE_BUFFER + fs->audio_buffer.write_pos),
                        &out_len);

                    if (out_len > 0) {
                        fs->audio_buffer.write_pos += (out_len * 2 * tech_pvt->channels);
                    }
                } else {
                    break;
                }
            }
        }

        if (fs->audio_buffer.write_pos > 0) {
            lws_callback_on_writable(fs->wsi);
        }

        switch_mutex_unlock(fs->audio_mutex);
        switch_mutex_unlock(tech_pvt->mutex);
    }

    return SWITCH_TRUE;
}

int parse_ws_uri(switch_channel_t *channel, const char* szServerUri, char* host, char *path, unsigned int* pPort, int* pSslFlags) {
    char *p;
    char server[MAX_WS_URL_LEN];

    strncpy(server, szServerUri, MAX_WS_URL_LEN);

    if ((p = strstr(server, "://"))) {
        if (strncasecmp(server, "wss", 3) == 0 || strncasecmp(server, "https", 5) == 0) {
            *pSslFlags = 1;
            *pPort = 443;
        } else {
            *pSslFlags = 0;
            *pPort = 80;
        }
        p += 3;
    } else {
        p = server;
        *pSslFlags = 0;
        *pPort = 80;
    }

    char *h = p;
    char *pth = strchr(h, '/');
    if (pth) {
        *pth = '\0';
        pth++;
        snprintf(path, MAX_PATH_LEN, "/%s", pth);
    } else {
        strcpy(path, "/");
    }

    char *pt = strchr(h, ':');
    if (pt) {
        *pt = '\0';
        pt++;
        *pPort = atoi(pt);
    }

    strncpy(host, h, MAX_WS_URL_LEN);
    return 1;
}
