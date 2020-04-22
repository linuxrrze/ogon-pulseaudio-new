/***
  This file is part of PulseAudio.

  Copyright (c) 2015-2018 Thincast Technologies GmbH

  Authors:
  David FORT "Hardening" <contact@hardening-consulting.com>
  Bernhard Miklautz <bernhard.miklautz@thincast.com>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>
#include <pulse/mainloop.h>

#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/macro.h>
#include <pulsecore/sink.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/poll.h>

#include <pulsecore/dbus-shared.h>
#include <pulsecore/iochannel.h>

#include <winpr/wtsapi.h>
#include <freerdp/server/rdpsnd.h>
#include <freerdp/server/server-common.h>


PA_MODULE_AUTHOR("David FORT");
PA_MODULE_DESCRIPTION("Output sound through your ogon server");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE(
    "sink_name=<name of sink> "
    "sink_properties=<properties for the sink> "
    "format=<sample format> "
    "rate=<sample rate> "
    "channels=<number of channels> "
    "channel_map=<channel map>");

static const char* const valid_modargs[] = {
    "sink_name",
    "sink_properties",
    "format",
    "rate",
    "channels",
    "channel_map",
    NULL
};

enum {
    SINK_MESSAGE_PASS_CHANNEL = PA_SINK_MESSAGE_MAX,
    SINK_MESSAGE_STOP_SINK
};

#define BLOCK_USEC (20 * 1000)

/** @brief state of our sink */
enum ogon_sink_state {
    OGON_SINK_STATE_INIT,
    OGON_SINK_STATE_NEGOCIATING,
    OGON_SINK_STATE_RUNNING
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_dbus_connection *dbus_conn;

    pa_sample_spec ss;
    pa_modargs *module_args;
    pa_channel_map map;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_usec_t block_usec;
    pa_usec_t timestamp;
    BOOL got_max_latency;
    int skipped_bytes;

    enum ogon_sink_state state;
    BOOL run_io_thread;
    uint32_t ogon_sid;
    pa_iochannel *io;
    int channel_event_fd;
    pa_rtpoll_item *rtpoll_item;
    RdpsndServerContext *rdpSoundServer;
};

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;
    long lat;

    /* pa_log_debug("sink_process_msg: code %d", code); */
    switch (code) {
        case PA_SINK_MESSAGE_SET_VOLUME:
            break;

        case PA_SINK_MESSAGE_SET_MUTE:
            break;

        case PA_SINK_MESSAGE_SET_STATE:
            if (PA_PTR_TO_UINT(data) == PA_SINK_RUNNING) {
                pa_log_debug("sink_process_msg: running");

                u->timestamp = pa_rtclock_now();
            } else {
                pa_log_debug("sink_process_msg: not running");
            }
            break;

        case PA_SINK_MESSAGE_GET_REQUESTED_LATENCY:
            break;

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t now;
            now = pa_rtclock_now();
            lat = (u->timestamp > now) ? (u->timestamp - now) : 0ULL;
            pa_log_debug("sink_process_msg: lat %ld", lat);
            *((pa_usec_t*) data) = lat;
            return 0;
        }

        case SINK_MESSAGE_PASS_CHANNEL: {
            /* we are now responsible of the channel socket */
            struct pollfd *pollfd;

            u->rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, 1);
            pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
            pollfd->fd = u->channel_event_fd;
            pollfd->events = POLLIN;
            pollfd->revents = 0;
            return 0;
        }

        case SINK_MESSAGE_STOP_SINK: {
            u->run_io_thread = FALSE;
            return 0;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static void sink_update_requested_latency_cb(pa_sink *s) {
    struct userdata *u;
    size_t nbytes;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    u->block_usec = BLOCK_USEC;
    pa_log_debug("1 block_usec %ld", u->block_usec);

    u->got_max_latency = FALSE;
    if (u->block_usec == (pa_usec_t) -1) {
        u->block_usec = s->thread_info.max_latency;
        pa_log_debug("2 block_usec %ld", u->block_usec);
        u->got_max_latency = TRUE;
    }

    //u->block_usec = pa_sink_get_requested_latency_within_thread(s);

    nbytes = pa_usec_to_bytes(u->block_usec, &s->sample_spec);
    pa_sink_set_max_rewind_within_thread(s, nbytes);
    pa_sink_set_max_request_within_thread(s, nbytes);
    pa_log_debug("updating latency to %lu", nbytes);
}

static void process_rewind(struct userdata *u, pa_usec_t now) {
    size_t rewind_nbytes, in_buffer;
    pa_usec_t delay;

    pa_assert(u);

    rewind_nbytes = u->sink->thread_info.rewind_nbytes;

    if (!PA_SINK_IS_OPENED(u->sink->thread_info.state) || rewind_nbytes <= 0)
        goto do_nothing;

    pa_log_debug("Requested to rewind %lu bytes.", (unsigned long) rewind_nbytes);

    if (u->timestamp <= now)
        goto do_nothing;

    delay = u->timestamp - now;
    in_buffer = pa_usec_to_bytes(delay, &u->sink->sample_spec);

    if (in_buffer <= 0)
        goto do_nothing;

    if (rewind_nbytes > in_buffer)
        rewind_nbytes = in_buffer;

    pa_sink_process_rewind(u->sink, rewind_nbytes);
    u->timestamp -= pa_bytes_to_usec(rewind_nbytes, &u->sink->sample_spec);
    u->skipped_bytes += rewind_nbytes;

    pa_log_debug("Rewound %lu bytes.", (unsigned long) rewind_nbytes);
    return;

do_nothing:

    pa_sink_process_rewind(u->sink, 0);
}

static int rdpSampleDivider(const pa_sample_spec *spec) {
    int ret;
    switch (spec->format) {
        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_S16LE:
            ret = 2;
            break;
        case PA_SAMPLE_U8:
        default:
            ret = 1;
            break;
    }

    return ret * spec->channels;
}

static int sendSamples(struct userdata *u, pa_memchunk *memchunk) {
    int toSend, ret;
    void *p;
    int sampleDivider = rdpSampleDivider(&u->sink->sample_spec);
    RdpsndServerContext *sndCtx = u->rdpSoundServer;

    toSend = memchunk->length;
    if (u->skipped_bytes > 0) {
        if (toSend > u->skipped_bytes) {
            toSend -= u->skipped_bytes;
            u->skipped_bytes = 0;
        } else {
            u->skipped_bytes -= toSend;
            return toSend;
        }
    }

    p = pa_memblock_acquire(memchunk->memblock);
    if ((memchunk->memblock != u->sink->silence.memblock) &&
            u->rtpoll_item &&
            sndCtx->SendSamples(u->rdpSoundServer,
                (uint8_t *) p + memchunk->index, toSend / sampleDivider,
                (UINT16) ((u->timestamp / 1000) & 0xffff)) != CHANNEL_RC_OK)
    {
        ret = -1;
    } else {
        ret = toSend;
    }
    pa_memblock_release(memchunk->memblock);
    return ret;
}


static void process_render(struct userdata *u, pa_usec_t now) {
    pa_memchunk memchunk;

    pa_assert(u);

    if (u->got_max_latency)
        return;

    /* This is the configured latency. Sink inputs connected to us
       might not have a single frame more than the maxrequest value
       queued. Hence: at maximum read this many bytes from the sink
       inputs. */

    /* Fill the buffer up the latency size */
    while (u->timestamp < now + u->block_usec) {
        int ret;
        size_t renderSize = u->sink->thread_info.max_request;

        pa_sink_render(u->sink, renderSize, &memchunk);
        pa_assert(memchunk.length > 0);

        ret = sendSamples(u, &memchunk);

        /* pa_log_debug("Ate %lu bytes.", (unsigned long) u->memchunk.length); */
        if (ret < 0) {
            pa_log_error("lost channel connection");
            break;
        }

        pa_memblock_unref(memchunk.memblock);

        u->timestamp += pa_bytes_to_usec(memchunk.length, &u->sink->sample_spec);
    }

    /* pa_log_debug("Ate in sum %lu bytes (of %lu)", (unsigned long) ate, (unsigned long) nbytes); */
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    pa_thread_mq_install(&u->thread_mq);

    u->timestamp = pa_rtclock_now();

    for (;;) {
        struct pollfd *pollfd;
        pa_usec_t now = 0;
        int ret;

        if (!u->run_io_thread) {
            pa_log_debug("should stop running");
            goto finish;
        }

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state))
            now = pa_rtclock_now();

        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
            process_rewind(u, now);

        if (u->rtpoll_item) {
            pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
            if (pollfd->revents) {
                ret = rdpsnd_server_handle_messages(u->rdpSoundServer);
                if (ret != CHANNEL_RC_OK && ret != ERROR_NO_DATA) {
                    pa_log_error("error handling incoming message, shutting down");
                    pa_rtpoll_item_free(u->rtpoll_item);
                    u->rtpoll_item = 0;
                }
            }
        }

        /* Render some data and drop it immediately */
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            if (u->timestamp <= now)
                process_render(u, now);

            pa_rtpoll_set_timer_absolute(u->rtpoll, u->timestamp);
        } else {
            pa_rtpoll_set_timer_disabled(u->rtpoll);
        }

        /* Hmm, nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    u->state = OGON_SINK_STATE_INIT;
    u->rdpSoundServer->Stop(u->rdpSoundServer);
    rdpsnd_server_context_free(u->rdpSoundServer);
    u->rdpSoundServer = NULL;

    pa_log_debug("Thread shutting down");
}

static BOOL select_compatible_audio_format(RdpsndServerContext* context) {
    UINT16 i = 0, j = 0;
    UINT16 serverFormat, clientFormat;

    for (i = 0; i < context->num_client_formats; i++) {
        pa_log_debug("client format #%u: %s (%u channels, %u samples/sec, %u bits/sample)",
                     i,
                     audio_format_get_tag_string(context->client_formats[i].wFormatTag),
                     context->client_formats[i].nChannels,
                     context->client_formats[i].nSamplesPerSec,
                     context->client_formats[i].wBitsPerSample);
    }

    for (i = 0; i < context->num_server_formats; i++) {
        pa_log_debug("server format #%u: %s (%u channels, %u samples/sec, %u bits/sample)",
                     i,
                     audio_format_get_tag_string(context->server_formats[i].wFormatTag),
                     context->server_formats[i].nChannels,
                     context->server_formats[i].nSamplesPerSec,
                     context->server_formats[i].wBitsPerSample);
    }

    /* TODO: since we do not yet use the freerd dsp encoder, we currently only
             accept PCM formats and simply select the first compatible match */

    for (i = 0; i < context->num_client_formats; i++) {
        if (context->client_formats[i].wFormatTag != WAVE_FORMAT_PCM)
            continue;

        for (j = 0; j < context->num_server_formats; j++) {
            if (audio_format_compatible(&context->server_formats[j], &context->client_formats[i])) {
                clientFormat = i;
                serverFormat = j;
                goto setFormats;
            }
        }
    }

     pa_log_error("unable to find a matching format");
     return FALSE;


setFormats:
    pa_log_debug("selected server format: #%u", serverFormat);
    pa_log_debug("selected client format: #%u", clientFormat);

    context->src_format = &context->server_formats[serverFormat];

    if (context->SelectFormat(context, clientFormat) != CHANNEL_RC_OK) {
        pa_log_error("error setting client audio format");
        return FALSE;
    }

    return TRUE;
}

static void pa_sample_spec_from_rdp(pa_sample_spec *spec, AUDIO_FORMAT *format) {
    spec->rate = format->nSamplesPerSec;
    spec->channels = format->nChannels;

    switch (format->wFormatTag) {
        case WAVE_FORMAT_PCM:
            switch (format->wBitsPerSample) {
                case 8:
                    spec->format = PA_SAMPLE_U8;
                    break;
                case 16:
                    spec->format = PA_SAMPLE_S16LE;
                    break;
            }
            break;
        case WAVE_FORMAT_ALAW:
            spec->format = PA_SAMPLE_ALAW;
            break;
        case WAVE_FORMAT_MULAW:
            spec->format = PA_SAMPLE_ULAW;
            break;
        default:
            pa_log_error("%s: format %d not handled\n", __FUNCTION__, format->wFormatTag);
            break;
    }
}

static void rdpSoundServerActivated(RdpsndServerContext* context) {
    char defaultSinkName[256];
    pa_sink_new_data data;
    pa_sink *sink;
    struct userdata *u = (struct userdata *)context->data;
    RdpsndServerContext *sndCtxt = u->rdpSoundServer;
    size_t nbytes;

    pa_log_debug("sound channel activated");

    /* unwire from the main thread */
    pa_iochannel_set_noclose(u->io, true);
    pa_iochannel_free(u->io);
    u->io = NULL;

    if (!select_compatible_audio_format(context)) {
        pa_log_error("could not select audio formats");
        return;
    }

    if (!u->sink) {
        /* creates the sink */
        pa_sample_spec_from_rdp(&u->ss, &sndCtxt->client_formats[sndCtxt->selected_client_format]);

        pa_sink_new_data_init(&data);
        data.driver = __FILE__;
        data.module = u->module;
        snprintf(defaultSinkName, sizeof(defaultSinkName), "ogon-%d", u->ogon_sid);
        pa_sink_new_data_set_name(&data, pa_modargs_get_value(u->module_args, "sink_name", defaultSinkName));
        pa_sink_new_data_set_sample_spec(&data, &u->ss);
        pa_sink_new_data_set_channel_map(&data, &u->map);
        pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, _("ogon Output"));
        pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "abstract");

        if (pa_modargs_get_proplist(u->module_args, "sink_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
            pa_log("Invalid properties");
            pa_sink_new_data_done(&data);
            goto fail;
        }

        u->sink = sink = pa_sink_new(u->module->core, &data, PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY|PA_SINK_NETWORK);
        pa_sink_new_data_done(&data);

        if (!sink) {
            pa_log("Failed to create sink object.");
            goto fail;
        }

        sink->parent.process_msg = sink_process_msg;
        sink->update_requested_latency = sink_update_requested_latency_cb;
        sink->userdata = u;
        pa_sink_set_asyncmsgq(sink, u->thread_mq.inq);
        pa_sink_set_rtpoll(sink, u->rtpoll);

        u->block_usec = BLOCK_USEC;
        nbytes = pa_usec_to_bytes(u->block_usec, &u->sink->sample_spec);
        pa_sink_set_max_rewind(u->sink, nbytes);
        pa_sink_set_max_request(u->sink, nbytes);

        if (!(u->thread = pa_thread_new("ogon-sink", thread_func, u))) {
            pa_log("Failed to create thread.");
            goto fail;
        }

        pa_sink_set_latency_range(sink, 0, BLOCK_USEC);

        pa_sink_put(sink);
    }

    u->state = OGON_SINK_STATE_RUNNING;
    pa_asyncmsgq_post(u->thread_mq.inq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_PASS_CHANNEL, NULL, 0, NULL, NULL);

    pa_log_info("ogon module loaded");

fail:
    return;
}

static int run_sound_channel(struct userdata *u);

static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *message, void *userdata) {
    DBusError error;
    uint32_t reason, session_id;
    struct userdata *u = (struct userdata *)userdata;

    dbus_error_init(&error);

    if (dbus_message_is_signal(message, "ogon.SessionManager.session.notification", "SessionNotification")) {
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_UINT32, &reason,
                                   DBUS_TYPE_UINT32, &session_id,
                                   DBUS_TYPE_INVALID)) {
            pa_log_error("unable to parse session manager message: %s: %s", error.name, error.message);
            goto finish;
        }

        pa_log_debug("got sessionId=%d and reason=%d (WTS_REMOTE_CONNECT=%d)", session_id, reason, (int)WTS_REMOTE_CONNECT);
        if (session_id != u->ogon_sid)
            goto finish;

        switch (reason) {
            case WTS_REMOTE_CONNECT:
                run_sound_channel(u);
                break;

            case WTS_REMOTE_DISCONNECT:
                pa_log_debug("session has disconnected");
                break;

            case WTS_SESSION_LOGOFF:
                pa_log_info("session %d has logged off, terminating", session_id);
                u->core->mainloop->quit(u->core->mainloop, 0);
                break;
        }
    }

finish:
    dbus_error_free(&error);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int initialize_dbus(struct userdata *u) {
    DBusError error;

    dbus_error_init(&error);

    if (!(u->dbus_conn = pa_dbus_bus_get(u->module->core, DBUS_BUS_SYSTEM, &error)) || dbus_error_is_set(&error)) {
        pa_log_error("Unable to contact D-Bus system bus: %s: %s", error.name, error.message);
        goto fail;
    }

    if (!dbus_connection_add_filter(pa_dbus_connection_get(u->dbus_conn), filter_cb, u, NULL)) {
        pa_log_error("Failed to add filter function");
        goto fail;
    }

    if (pa_dbus_add_matches(pa_dbus_connection_get(u->dbus_conn), &error,
                "type='signal',interface='ogon.SessionManager.session.notification',member='SessionNotification'", NULL) < 0)
    {
        pa_log_error("Unable to subscribe to SessionManager signals: %s: %s", error.name, error.message);
        goto fail;
    }

    return 0;

fail:
    if (u->dbus_conn)
        pa_dbus_connection_unref(u->dbus_conn);

    return -1;
}

static void sound_channel_io_callback(pa_iochannel *io, void *userdata) {
    struct userdata *u = (struct userdata *)userdata;

    while (u->io && pa_iochannel_is_readable(u->io)) {
        int ret;
        ret = rdpsnd_server_handle_messages(u->rdpSoundServer);
        switch (ret) {
            case CHANNEL_RC_OK:
                break;
            case ERROR_NO_DATA:
                if (u->io)
                    pa_iochannel_enable_events(u->io);
                break;
            default:
                pa_log_debug("connection closed");
                pa_iochannel_free(u->io);
                u->io = NULL;
                u->rdpSoundServer->Stop(u->rdpSoundServer);
                rdpsnd_server_context_free(u->rdpSoundServer);
                u->rdpSoundServer = NULL;
                u->state = OGON_SINK_STATE_INIT;
                break;
        }
    }
}

static int run_sound_channel(struct userdata *u) {
    HANDLE channelEvent;
    int ret = -1;
    RdpsndServerContext *sndCtxt;

    switch (u->state) {
        case OGON_SINK_STATE_INIT:
            pa_log_debug("initializing sink");
            break;
        case OGON_SINK_STATE_NEGOCIATING:
            pa_log_info("already negotiating, continuing...");
            return 0;
        case OGON_SINK_STATE_RUNNING:
            pa_log_info("stopping the sink thread, before starting the negotiation");

            if (u->sink) {
                pa_sink_unlink(u->sink);
                pa_asyncmsgq_post(u->thread_mq.inq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_STOP_SINK, NULL, 0, NULL, NULL);
            }

            pa_thread_free(u->thread);
            u->thread = NULL;

            pa_assert(u->state == OGON_SINK_STATE_INIT);

            if (u->sink) {
                pa_sink_unref(u->sink);
            }
            u->sink = NULL;
            break;
        default:
            pa_log_error("unknown state %d", u->state);
            return -1;
    }

    u->run_io_thread = TRUE;

    u->rdpSoundServer = sndCtxt = rdpsnd_server_context_new(WTS_CURRENT_SERVER_HANDLE);
    if (!sndCtxt) {
        pa_log("unable to create a RDP server sound context.");
        return -1;
    }

    sndCtxt->data = u;
    sndCtxt->num_server_formats = server_rdpsnd_get_formats(&sndCtxt->server_formats);

    if (sndCtxt->num_server_formats > 0)
        sndCtxt->src_format = &sndCtxt->server_formats[0];
    else {
        pa_log_error("no server audio formats available");
        return -1;
    }
    sndCtxt->Activated = rdpSoundServerActivated;

    if (sndCtxt->Initialize(sndCtxt, FALSE) != CHANNEL_RC_OK) {
        pa_log_info("looks like sound channel is not ready yet");
        return -1;
    }

    u->state = OGON_SINK_STATE_NEGOCIATING;
    channelEvent = rdpsnd_server_get_event_handle(u->rdpSoundServer);
    if (!channelEvent || channelEvent == INVALID_HANDLE_VALUE) {
        pa_log_error("invalid channel event %p", channelEvent);
        u->state = OGON_SINK_STATE_INIT;
        goto out;
    }

    u->channel_event_fd = GetEventFileDescriptor(channelEvent);
    if (u->channel_event_fd < 0) {
        pa_log_error("invalid file descriptor in the channel event");
        u->state = OGON_SINK_STATE_INIT;
        goto out;
    }

    u->io = pa_iochannel_new(u->core->mainloop, u->channel_event_fd, -1);
    pa_iochannel_set_callback(u->io, sound_channel_io_callback, u);
    ret = 0;

out:
    return ret;
}

int pa__init(pa_module *m) {
    struct userdata *u = NULL;
    pa_modargs *ma = NULL;
    char *sid_str, *endp;
    uint32_t sid;

    pa_assert(m);

    pa_log_info("ogon module starting");
    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    sid_str = getenv("OGON_SID");
    if (!sid_str) {
        pa_log("missing OGON_SID environment variable");
        goto fail;
    }

    sid = strtol(sid_str, &endp, 10);
    if (*endp) {
        pa_log("invalid ogon sessionId");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->module_args = ma;
    u->ogon_sid = sid;
    u->state = OGON_SINK_STATE_INIT;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);

    u->ss = m->core->default_sample_spec;
    u->map = m->core->default_channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &u->ss, &u->map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    if (initialize_dbus(u) < 0)
        goto fail;

    run_sound_channel(u);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);
    return -1;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return pa_sink_linked_by(u->sink);
}


void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata)) {
        return;
    }

    if (u->dbus_conn) {
        dbus_connection_remove_filter(pa_dbus_connection_get(u->dbus_conn), filter_cb, u);

        pa_dbus_remove_matches(pa_dbus_connection_get(u->dbus_conn),
                               "type='signal',interface='ogon.SessionManager.session.notification',member='SessionNotification'",
                               NULL);

        pa_dbus_connection_unref(u->dbus_conn);
    }

    if (u->sink) {
        pa_sink_unlink(u->sink);
    }

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink) {
        pa_sink_unref(u->sink);
    }

    if (u->rtpoll) {
        pa_rtpoll_free(u->rtpoll);
    }

    if (u->module_args) {
        pa_modargs_free(u->module_args);
    }

    pa_xfree(u);
}
