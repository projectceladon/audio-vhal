/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hw_virtual"
#define ATRACE_TAG ATRACE_TAG_AUDIO
// #define LOG_NDEBUG 0
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>

#include <log/log.h>

#include <hardware/audio.h>
#include <hardware/hardware.h>
#include <system/audio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <cutils/properties.h>
#include <cutils/sockets.h>
#include <cutils/trace.h>
#include <sys/system_properties.h>
#include <pthread.h>

#define STUB_DEFAULT_SAMPLE_RATE 48000
#define STUB_DEFAULT_AUDIO_FORMAT AUDIO_FORMAT_PCM_16_BIT

#define STUB_INPUT_BUFFER_MILLISECONDS 10
#define STUB_INPUT_DEFAULT_CHANNEL_MASK AUDIO_CHANNEL_IN_STEREO

#define STUB_OUTPUT_BUFFER_MILLISECONDS 10
#define STUB_OUTPUT_DEFAULT_CHANNEL_MASK AUDIO_CHANNEL_OUT_STEREO

enum
{
    CMD_OPEN = 0,
    CMD_CLOSE = 1,
    CMD_DATA = 2,
    CMD_STREAM_START = 3,
    CMD_STREAM_STOP = 4
};

enum
{
    AUDIO_IN = 0,
    AUDIO_OUT = 1
};

struct audio_socket_configuration_info
{
    uint32_t sample_rate;
    uint32_t channel; // 0; The number of channel 1: The mask of channel
    uint32_t format;
    uint32_t frame_count;
};

struct audio_socket_info
{
    uint32_t cmd;
    union
    {
        struct audio_socket_configuration_info asci;
        uint32_t data_size;
        uint32_t offset;
    };
};

struct stub_audio_device
{
    struct audio_hw_device device;
    bool mic_mute;
};

struct stub_stream_out
{
    struct audio_stream_out stream;
    int64_t last_write_time_us;
    uint32_t sample_rate;
    audio_channel_mask_t channel_mask;
    audio_format_t format;
    size_t frame_count;
};

struct stub_stream_in
{
    struct audio_stream_in stream;
    int64_t last_read_time_us;
    uint32_t sample_rate;
    audio_channel_mask_t channel_mask;
    audio_format_t format;
    size_t frame_count;
    struct stub_audio_device *dev;
};

struct audio_server_socket
{
    int container_id;
    int audio_mask; // 0; The number of channel 1: The mask of channel
    //Audio out socket
    struct stub_stream_out *sso;
    int out_fd;
    bool out_stream_standby;
    pthread_t oss_thread; // out socket server thread
    int oss_exit;         // out socket server thread exit
    int oss_fd;           // out socket server fd
    //INET socket
    int out_tcp_port;
    int oss_epoll_fd;
    struct epoll_event oss_epoll_event[1];
    bool oss_is_sent_open_cmd;
    pthread_mutex_t mutexlock_out;
    int64_t oss_write_count;

    //Audio in socket
    struct stub_stream_in *ssi;
    int in_fd;
    pthread_t iss_thread; // in socket server thread
    int iss_exit;         // in socket server thread exit
    int iss_fd;           // iut socket server fd
    //INET socket
    int in_tcp_port;
    int iss_epoll_fd;
    struct epoll_event iss_epoll_event[1];
    bool iss_read_flag;
    int input_buffer_milliseconds; // INPUT_BUFFER_MILLISECONDS
    pthread_mutex_t mutexlock_in;
};

static struct audio_server_socket ass;

static void sighandler(int signum)
{
    if (signum == SIGPIPE)
    {
        ALOGW("SIGPIPE will issue when writing data to the closing client socket. "
              "Virtual audio will crash without stack and cannot recovery. "
              "Catch SIGPIPE");
    }
    else
    {
        ALOGW("sig %d is caught.", signum);
    }
}

static int close_socket_fd(int *psd)
{
    if (*psd > 0)
    {
        ALOGV("Close %d", *psd);
        shutdown(*psd, SHUT_RDWR);
        close(*psd);
        *psd = -1;
        return 0;
    }
    else
    {
        ALOGV("sd is %d. Do not need close anymore.", *psd);
        return 0;
    }
    return -1;
}

static int send_open_cmd(struct audio_server_socket *pass, int audio_type)
{
    if (!pass)
    {
        ALOGE("pass pointer is NULL.");
        return -1;
    }

    int ret;
    int client_fd = -1;
    struct audio_socket_info asi;
    memset(&asi, 0, sizeof(struct audio_socket_info));
    asi.cmd = CMD_OPEN;

    switch (audio_type)
    {
    case AUDIO_IN:
        ALOGV("%s pass->in_fd = %d ", __func__, pass->in_fd);
        client_fd = pass->in_fd;
        if (pass->ssi)
        {
            asi.asci.sample_rate = pass->ssi->sample_rate;
            if (ass.audio_mask == 1)
            {
                asi.asci.channel = pass->ssi->channel_mask;
            }
            else
            {
                asi.asci.channel = audio_channel_count_from_in_mask(pass->ssi->channel_mask);
            }
            asi.asci.format = pass->ssi->format;
            asi.asci.frame_count = pass->ssi->frame_count;
            ALOGV("%s AUDIO_IN asi.asci.sample_rate: %d asi.asci.channel: %d "
                  "asi.asci.format: %d asi.asci.frame_count: %d\n",
                  __func__, asi.asci.sample_rate, asi.asci.channel,
                  asi.asci.format, asi.asci.frame_count);
        }
        break;
    case AUDIO_OUT:
        client_fd = pass->out_fd;
        if (pass->sso)
        {
            asi.asci.sample_rate = pass->sso->sample_rate;
            if (ass.audio_mask == 1)
            {
                asi.asci.channel = pass->sso->channel_mask;
            }
            else
            {
                asi.asci.channel = audio_channel_count_from_out_mask(pass->sso->channel_mask);
            }
            asi.asci.format = pass->sso->format;
            asi.asci.frame_count = pass->sso->frame_count;
            ALOGV("%s AUDIO_OUT asi.asci.sample_rate: %d asi.asci.channel: %d "
                  "asi.asci.format: %d asi.asci.frame_count: %d\n",
                  __func__, asi.asci.sample_rate, asi.asci.channel,
                  asi.asci.format, asi.asci.frame_count);
        }
        break;

    default:
        ALOGW("Unkown type: %d", audio_type);
        return -1;
        break;
    }
    if (client_fd < 0)
    {
        ALOGW("client_fd is %d. Do not send open command to client.", client_fd);
        return -1;
    }
    do
    {
        ret = write(client_fd, &asi, sizeof(struct audio_socket_info));
    } while (ret < 0 && errno == EINTR);
    if (ret != sizeof(struct audio_socket_info))
    {
        ALOGE("%s: could not notify the client(%d) to open: ret=%d: %s.",
              __FUNCTION__, client_fd, ret, strerror(errno));
        return -1;
    }

    ALOGV("%s Notify the audio client(%d) to open.", __func__, client_fd);
    return 0;
}

static int send_close_cmd(int client_fd)
{
    ALOGV("%s client_fd = %d", __func__, client_fd);
    int ret;
    struct audio_socket_info asi;
    asi.cmd = CMD_CLOSE;
    asi.data_size = 0;
    if (client_fd > 0)
    {
        do
        {
            ret = write(client_fd, &asi, sizeof(struct audio_socket_info));
        } while (ret < 0 && errno == EINTR);
        if (ret != sizeof(struct audio_socket_info))
        {
            ALOGE("%s: could not notify the client(%d) to "
                  "close: ret=%d: %s.",
                  __FUNCTION__, client_fd, ret, strerror(errno));
            return -1;
        }
        else
        {
            ALOGW("%s Notify the client(%d) to close.", __func__, client_fd);
            return 0;
        }
    }
    else
    {
        ALOGW("Client is %d. Do not set close command to client.", client_fd);
        return 0;
    }
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    const struct stub_stream_out *out = (const struct stub_stream_out *)stream;

    ALOGV("out_get_sample_rate: %u", out->sample_rate);
    return out->sample_rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    struct stub_stream_out *out = (struct stub_stream_out *)stream;

    ALOGV("out_set_sample_rate: %d", rate);
    out->sample_rate = rate;
    return 0;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    const struct stub_stream_out *out = (const struct stub_stream_out *)stream;
    size_t buffer_size = out->frame_count *
                         audio_stream_out_frame_size(&out->stream);

    ALOGV("out_get_buffer_size: %zu", buffer_size);
    return buffer_size;
}

static audio_channel_mask_t out_get_channels(const struct audio_stream *stream)
{
    const struct stub_stream_out *out = (const struct stub_stream_out *)stream;

    ALOGV("out_get_channels: %x", out->channel_mask);
    return out->channel_mask;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    const struct stub_stream_out *out = (const struct stub_stream_out *)stream;

    ALOGV("out_get_format: %d", out->format);
    return out->format;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    struct stub_stream_out *out = (struct stub_stream_out *)stream;

    ALOGV("out_set_format: %d", format);
    out->format = format;
    return 0;
}

static int out_standby(struct audio_stream *stream)
{
    ALOGV("out_standby");
    int ret;
    if (ass.out_fd > 0)
    {
        struct audio_socket_info asi;
        memset(&asi, 0, sizeof(struct audio_socket_info));
        asi.cmd = CMD_STREAM_STOP;
        do
        {
            ret = write(ass.out_fd, &asi, sizeof(struct audio_socket_info));
        } while (ret < 0 && errno == EINTR);
        if (ret != sizeof(struct audio_socket_info))
        {
            ALOGE("%s: could not notify the client(%d) to stop streaming: ret=%d: %s.",
                  __FUNCTION__, ass.out_fd, ret, strerror(errno));
            return -1;
        }
        ass.out_stream_standby = true;
    }
    else
    {

        ALOGE("%s: Audio out client is not connected. "
              "port(%d) ass.out_fd(%d).",
              __FUNCTION__, ass.out_tcp_port, ass.out_fd);
        return -1;
    }
    return ret;
    // out->last_write_time_us = 0; unnecessary as a stale write time has same effect
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    ALOGV("out_dump");
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    ALOGV("out_set_parameters");
    return 0;
}

static char *out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    ALOGV("out_get_parameters");
    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    ALOGV("out_get_latency");
    return STUB_OUTPUT_BUFFER_MILLISECONDS;
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    ALOGV("out_set_volume: Left:%f Right:%f", left, right);
    return 0;
}

static void out_update_source_metadata(struct audio_stream_out *stream,
                                       const struct source_metadata *source_metadata)
{
    ALOGV("update_source_metadata called. Do nothing as of now.");
}

static ssize_t out_write_to_client(struct audio_stream_out *stream, const void *buffer,
                                   size_t bytes, int timeout)
{
    ssize_t ret = -1;
    ssize_t result = 0;
    int nevents = 0;
    int ne;
    if (ass.out_fd > 0)
    {
        if (ass.out_stream_standby == true)
        {
            struct audio_socket_info asi;
            int ret;
            asi.cmd = CMD_STREAM_START;
            do
            {
                ret = write(ass.out_fd, &asi, sizeof(struct audio_socket_info));
            } while (ret < 0 && errno == EINTR);
            if (ret != sizeof(struct audio_socket_info))
            {
                ALOGE("%s: could not notify the client(%d) to start streaming: ret=%d: %s.",
                      __FUNCTION__, ass.out_fd, ret, strerror(errno));
            }
            ass.out_stream_standby = false;
        }
        nevents = epoll_wait(ass.oss_epoll_fd, ass.oss_epoll_event, 1, timeout);
        if (nevents < 0)
        {
            if (errno != EINTR)
                ALOGE("epoll_wait() unexpected error: %s", strerror(errno));
            return errno;
        }
        else if (nevents == 0)
        {
            ALOGW("out_write_to_client: Client cannot be written in given time.");
            return -1;
        }
        else if (nevents > 0)
        {
            for (ne = 0; ne < nevents; ne++) //In fact, only one event.
            {
                if (ass.oss_epoll_event[ne].data.fd == ass.out_fd)
                {
                    if ((ass.oss_epoll_event[ne].events & (EPOLLERR | EPOLLHUP)) != 0)
                    {
                        ALOGE("EPOLLERR or EPOLLHUP after epoll_wait() !?");
                        if (epoll_ctl(ass.oss_epoll_fd, EPOLL_CTL_DEL, ass.out_fd, NULL))
                        {
                            ALOGE("Failed to delete audio in file descriptor to epoll");
                        }
                        pthread_mutex_lock(&ass.mutexlock_out);
                        close_socket_fd(&(ass.out_fd)); // Try to clear the cache data in socket.
                        ass.oss_is_sent_open_cmd = 0;
                        if (ATRACE_ENABLED())
                        {
                            ATRACE_INT("avh_out_client_EPOLLERR_or_EPOLLHUP", ass.oss_is_sent_open_cmd);
                        }
                        pthread_mutex_unlock(&ass.mutexlock_out);
                    }
                    else if ((ass.oss_epoll_event[ne].events & EPOLLOUT) != 0)
                    {
                        struct audio_socket_info asi;
                        asi.cmd = CMD_DATA;
                        asi.data_size = bytes;
                        ALOGV("%s asi.data_size: %d\n", __func__, asi.data_size);
                        do
                        {
                            if (ATRACE_ENABLED())
                            {
                                ATRACE_INT("avh_CMD_DATA_count_before_write", ass.oss_write_count);
                            }
                            result = write(ass.out_fd, &asi, sizeof(struct audio_socket_info));
                            if (ATRACE_ENABLED())
                            {
                                ATRACE_INT("avh_CMD_DATA_count_after_write", ass.oss_write_count);
                            }
                        } while (result < 0 && errno == EINTR);
                        if (result != sizeof(struct audio_socket_info))
                        {
                            ALOGE("%s: could not notify the audio out client(%d) "
                                  "to receive: result=%zd: %s.",
                                  __FUNCTION__, ass.out_fd, result, strerror(errno));
                            continue;
                        }
                        ALOGV("%s Notify the audio out client(%d) to receive.", __func__, ass.out_fd);

                        ALOGV("out_write_to_client: write buffer to socket.");
                        if (ATRACE_ENABLED())
                        {
                            ATRACE_INT("avh_data_count_before_write", ass.oss_write_count);
                        }
                        result = write(ass.out_fd, buffer, bytes);
                        if (ATRACE_ENABLED())
                        {
                            ATRACE_INT("avh_data_count_after_write", ass.oss_write_count);
                        }
                        ass.oss_write_count++;
                        if (result < 0)
                        {
                            ALOGE("out_write_to_client: Fail to write to audio out client(%d)"
                                  " with error(%s)",
                                  ass.out_fd, strerror(errno));
                        }
                        else if (result == 0)
                        {
                            ALOGW("out_write_to_client: audio out client(%d) is closed.", ass.out_fd);
                        }
                        else
                        {
                            ALOGV("out_write_to_client: Write to audio out client. "
                                  "ass.out_fd: %d bytes: %zu",
                                  ass.out_fd, bytes);
                            if (bytes != (size_t)result)
                            {
                                ALOGW("out_write_to_client: (!^!) result(%zd) data is written. "
                                      "But bytes(%zu) is expected.",
                                      result, bytes);
                            }
                        }
                        ret = result;
                    }
                    else
                    {
                        ALOGW("out_write_to_client: epoll unknown event. port(%d) "
                              "ass.out_fd(%d). Return bytes(%zu) directly.",
                              ass.out_tcp_port, ass.out_fd, bytes);
                    }
                }
                else
                {
                    ALOGV("out_write_to_client: epoll_wait unknown source fd.");
                }
            }
            return ret;
        }
    }
    else
    {

        ALOGV("out_write_to_client: (->v->) Audio out client is not connected. "
              "port(%d) ass.out_fd(%d). Return bytes(%zu) directly.",
              ass.out_tcp_port, ass.out_fd, bytes);
        return -1;
    }
    return ret;
}

static ssize_t out_write(struct audio_stream_out *stream, const void *buffer,
                         size_t bytes)
{
    if (ATRACE_ENABLED())
    {
        ATRACE_BEGIN("avh_out_write");
    }
    ALOGV("out_write: %p, bytes: %zu", buffer, bytes);

    struct stub_stream_out *out = (struct stub_stream_out *)stream;
    ssize_t ret = bytes;
    ssize_t result = -1;

    /* XXX: fake timing for audio output */
    struct timespec t = {.tv_sec = 0, .tv_nsec = 0};
    clock_gettime(CLOCK_MONOTONIC, &t);
    const int64_t now = (t.tv_sec * 1000000000LL + t.tv_nsec) / 1000;
    const int64_t elapsed_time_since_last_write = now - out->last_write_time_us;
    int64_t sleep_time = bytes * 1000000LL / audio_stream_out_frame_size(stream) /
                             out_get_sample_rate(&stream->common) -
                         elapsed_time_since_last_write;
    int64_t frame_time_ms = bytes * 1000LL / audio_stream_out_frame_size(stream) /
                            out_get_sample_rate(&stream->common); // ms
    int64_t timeout = sleep_time / 1000LL;                        // ms
    if (timeout < 1)
    {
        timeout = 1;
    }
    if (timeout > frame_time_ms)
    {
        timeout = frame_time_ms;
    }
    if (bytes > 0)
    {
        result = out_write_to_client(stream, buffer, bytes, timeout);
        if (result < 0)
        {
            ALOGV("The result of out_write_to_client is %zd", result);
        }
        else if (result > 0)
        {
            ret = result;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t);
    const int64_t new_now = (t.tv_sec * 1000000000LL + t.tv_nsec) / 1000;
    sleep_time = sleep_time - (new_now - now);
    int64_t frame_time_us = bytes * 1000000LL / audio_stream_out_frame_size(stream) /
                            out_get_sample_rate(&stream->common); // us
    if (sleep_time > 0 && sleep_time <= frame_time_us)
    {
        usleep(sleep_time);
    }
    else
    {
        // we don't sleep when we exit standby (this is typical for a real alsa buffer).
        sleep_time = 0;
    }
    out->last_write_time_us = new_now + sleep_time;
    // last_write_time_us is an approximation of when the (simulated) alsa
    // buffer is believed completely full. The usleep above waits for more space
    // in the buffer, but by the end of the sleep the buffer is considered
    // topped-off.
    //
    // On the subsequent out_write(), we measure the elapsed time spent in
    // the mixer. This is subtracted from the sleep estimate based on frames,
    // thereby accounting for drain in the alsa buffer during mixing.
    // This is a crude approximation; we don't handle underruns precisely.
    if (ATRACE_ENABLED())
    {
        ATRACE_END();
    }
    return ret;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    *dsp_frames = 0;
    ALOGV("out_get_render_position: dsp_frames: %p", dsp_frames);
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    ALOGV("out_add_audio_effect: %p", effect);
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    ALOGV("out_remove_audio_effect: %p", effect);
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    *timestamp = 0;
    ALOGV("out_get_next_write_timestamp: %ld", (long int)(*timestamp));
    return -EINVAL;
}

static void *out_socket_sever_thread(void *args)
{
    struct audio_server_socket *pass = (struct audio_server_socket *)args;
    int ret = 0;
    int so_reuseaddr = 1;
    int new_client_fd = -1;
    struct sockaddr_in addr_in;

    ALOGV("%s Constructing audio out socket server...", __func__);

    pass->oss_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (pass->oss_fd < 0)
    {
        ALOGE("%s:%d Fail to construct audio out socket with error: %s",
              __func__, __LINE__, strerror(errno));
        return NULL;
    }
    if (setsockopt(pass->oss_fd, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(int)) < 0)
    {
        ALOGE("%s setsockopt(SO_REUSEADDR) failed. pass->oss_fd: %d\n", __func__, pass->oss_fd);
        return NULL;
    }

    memset(&addr_in, 0, sizeof(addr_in));
    addr_in.sin_family = AF_INET;
    addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
    addr_in.sin_port = htons(pass->out_tcp_port);
    ret = bind(pass->oss_fd, (struct sockaddr *)&addr_in, sizeof(struct sockaddr_in));
    if (ret < 0)
    {
        ALOGE("%s Failed to bind port(%d). ret: %d %s", __func__, pass->out_tcp_port, ret, strerror(errno));
        return NULL;
    }

    ret = listen(pass->oss_fd, 5);
    if (ret < 0)
    {
        ALOGE("%s Failed to listen on port %d", __func__, pass->out_tcp_port);
        return NULL;
    }

    while (!pass->oss_exit)
    {
        ALOGW("%s Wait a audio out client to connect...", __func__);
        socklen_t alen;
        alen = sizeof(struct sockaddr_in);
        new_client_fd = accept(pass->oss_fd, (struct sockaddr *)&addr_in, &alen);
        if (new_client_fd < 0)
        {
            ALOGE("%s The audio in socket server maybe shutdown as quit command is got. "
                  "Or else, error happen. port: %d %s",
                  __func__, pass->out_tcp_port, strerror(errno));
        }
        else
        {
            ALOGW("%s Currently only receive one out client. Close previous "
                  "client(%d)",
                  __func__, pass->out_fd);

            if (pass->out_fd > 0)
            {
                pthread_mutex_lock(&ass.mutexlock_out);
                if (ATRACE_ENABLED())
                {
                    ATRACE_INT("avh_osst_before_send_close_cmd", pass->oss_is_sent_open_cmd);
                }
                pass->oss_is_sent_open_cmd = 0;
                if (send_close_cmd(pass->out_fd) < 0)
                {
                    ALOGE("Fail to notify audio out client(%d) to close.", pass->out_fd);
                }
                if (ATRACE_ENABLED())
                {
                    ATRACE_INT("avh_osst_after_send_close_cmd", pass->oss_is_sent_open_cmd);
                }
                pthread_mutex_unlock(&ass.mutexlock_out);

                if (epoll_ctl(pass->oss_epoll_fd, EPOLL_CTL_DEL, pass->out_fd, NULL))
                {
                    ALOGE("Failed to delete audio out file descriptor to epoll");
                }
                close_socket_fd(&(pass->out_fd));
            }

            ALOGW("%s A new audio out client connected to server. "
                  "new_client_fd = %d",
                  __func__, new_client_fd);
            pass->out_fd = new_client_fd;
            pass->out_stream_standby = true; 
            if (pass->out_fd > 0)
            {
                pthread_mutex_lock(&ass.mutexlock_out);
                pass->oss_write_count = 0;
                if (ATRACE_ENABLED())
                {
                    ATRACE_INT("avh_osst_before_send_open_cmd", pass->oss_is_sent_open_cmd);
                }
                if (pass->sso && pass->oss_is_sent_open_cmd == 0) // Make sure parameters are ready.
                {
                    if (send_open_cmd(pass, AUDIO_OUT) < 0)
                    {
                        ALOGE("Fail to send OPEN command to audio out client(%d)", pass->out_fd);
                    }
                    else
                    {
                        pass->oss_is_sent_open_cmd = 1;
                        ALOGE("pass->oss_is_sent_open_cmd is set to %d", pass->oss_is_sent_open_cmd);
                    }
                }
                if (ATRACE_ENABLED())
                {
                    ATRACE_INT("avh_osst_after_send_open_cmd", pass->oss_is_sent_open_cmd);
                }
                pthread_mutex_unlock(&ass.mutexlock_out);

                struct epoll_event event;
                event.events = EPOLLOUT;
                event.data.fd = pass->out_fd;
                if (epoll_ctl(pass->oss_epoll_fd, EPOLL_CTL_ADD, pass->out_fd, &event))
                {
                    ALOGE("Failed to add audio out file descriptor to epoll");
                }
            }
        }
    }
    ALOGW("%s Quit. port %d(%d)", __func__, pass->out_tcp_port, pass->out_fd);
    pthread_mutex_lock(&ass.mutexlock_out);
    pass->oss_is_sent_open_cmd = 0;
    close_socket_fd(&(pass->out_fd));
    close_socket_fd(&(pass->oss_fd));
    pthread_mutex_unlock(&ass.mutexlock_out);
    return NULL;
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    const struct stub_stream_in *in = (const struct stub_stream_in *)stream;

    ALOGV("in_get_sample_rate: %u", in->sample_rate);
    return in->sample_rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    struct stub_stream_in *in = (struct stub_stream_in *)stream;

    ALOGV("in_set_sample_rate: %u", rate);
    in->sample_rate = rate;
    return 0;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    const struct stub_stream_in *in = (const struct stub_stream_in *)stream;
    size_t buffer_size = in->frame_count *
                         audio_stream_in_frame_size(&in->stream);

    ALOGV("in_get_buffer_size: %zu", buffer_size);
    return buffer_size;
}

static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
    const struct stub_stream_in *in = (const struct stub_stream_in *)stream;

    ALOGV("in_get_channels: %x", in->channel_mask);
    return in->channel_mask;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    const struct stub_stream_in *in = (const struct stub_stream_in *)stream;

    ALOGV("in_get_format: %d", in->format);
    return in->format;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    struct stub_stream_in *in = (struct stub_stream_in *)stream;

    ALOGV("in_set_format: %d", format);
    in->format = format;
    return 0;
}

static int in_standby(struct audio_stream *stream)
{
    struct stub_stream_in *in = (struct stub_stream_in *)stream;
    in->last_read_time_us = 0;
    return 0;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    return 0;
}

static char *in_get_parameters(const struct audio_stream *stream,
                               const char *keys)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

static ssize_t in_read_from_client(struct audio_stream_in *stream, void *buffer,
                                   size_t bytes, int timeout, uint32_t offset)
{
    ssize_t ret = -1;
    ssize_t result = 0;
    int nevents = 0;
    int ne;
    if (ass.in_fd > 0)
    {
        ALOGV("%s epoll_wait %d.", __func__, ass.iss_epoll_fd);
        nevents = epoll_wait(ass.iss_epoll_fd, ass.iss_epoll_event, 1, timeout);
        if (nevents < 0)
        {
            if (errno != EINTR)
                ALOGE("epoll_wait() unexpected error: %s", strerror(errno));
            return errno;
        }
        else if (nevents == 0)
        {
            ALOGW("in_read_from_client: Client cannot be read in given time.");
            memset(buffer, 0, bytes);
            return bytes;
        }
        else if (nevents > 0)
        {
            for (ne = 0; ne < nevents; ne++) // In fact, only one event.
            {
                if (ass.iss_epoll_event[ne].data.fd == ass.in_fd)
                {
                    if ((ass.iss_epoll_event[ne].events & (EPOLLERR | EPOLLHUP)) != 0)
                    {
                        ALOGE("EPOLLERR or EPOLLHUP after epoll_wait() !?");
                        if (epoll_ctl(ass.iss_epoll_fd, EPOLL_CTL_DEL, ass.in_fd, NULL))
                        {
                            ALOGE("Failed to delete audio in file descriptor to epoll");
                        }
                        close(ass.in_fd);
                        ass.in_fd = -1;
                    }
                    else if ((ass.iss_epoll_event[ne].events & EPOLLIN) != 0)
                    {
                        result = read(ass.in_fd, buffer, bytes);
                        if (result < 0)
                        {
                            ALOGE("in_read_from_client: Fail to read from audio in client(%d) "
                                  "with error (%s)",
                                  ass.in_fd, strerror(errno));
                        }
                        else if (result == 0)
                        {
                            ALOGE("in_read_from_client: Audio in client(%d) is closed.", ass.in_fd);
                        }
                        else
                        {
                            ALOGV("in_read_from_client: Read from port %d ass.in_fd %d bytes "
                                  "%zu, result: %zd",
                                  ass.in_tcp_port, ass.in_fd, bytes, result);
                            if (bytes != (size_t)result)
                            {
                                ALOGW("in_read_from_client: (!^!) result(%zd) data is read. But "
                                      "bytes(%zu) is expected.",
                                      result, bytes);
                            }
                            ret = result;
                        }
                    }
                    else
                    {
                        ALOGW("in_read_from_client: epoll unknown event. port(%d) ass.in_fd(%d)"
                              ". Memset data to 0. Return bytes(%zu) directly.",
                              ass.in_tcp_port, ass.in_fd, bytes);
                    }
                }
                else
                {
                    ALOGV("in_read_from_client: epoll_wait unknown");
                }
            }
            return ret;
        }
    }
    else
    {
        ALOGV("in_read_from_client: (->v->) Audio in client is not connected. port(%d)"
              " ass.in_fd(%d). Memset data to 0. Return bytes(%zu) directly.",
              ass.in_tcp_port, ass.in_fd, bytes);
        memset(buffer, 0, bytes);
        return bytes;
    }
    return ret;
}

static ssize_t in_read(struct audio_stream_in *stream, void *buffer,
                       size_t bytes)
{
    ALOGV("in_read: %p, bytes %zu", buffer, bytes);
    if (!ass.iss_read_flag)
    {
        if (ass.in_fd > 0)
        {
            pthread_mutex_lock(&ass.mutexlock_in);
            ALOGV("in_read: send_open_cmd pthread_mutex_lock");
            if (send_open_cmd(&ass, AUDIO_IN) < 0)
            {
                ALOGE("%s: Fail to send OPEN command to audio in client(%d)", __func__, ass.in_fd);
            }
            pthread_mutex_unlock(&ass.mutexlock_in);
        }
        ass.iss_read_flag = true;
    }

    struct stub_stream_in *in = (struct stub_stream_in *)stream;
    struct stub_audio_device *adev = in->dev;
    ssize_t ret = bytes;
    ssize_t result = -1;

    /* XXX: fake timing for audio input */
    struct timespec t = {.tv_sec = 0, .tv_nsec = 0};
    clock_gettime(CLOCK_MONOTONIC, &t);
    const int64_t now = (t.tv_sec * 1000000000LL + t.tv_nsec) / 1000;

    // we do a full sleep when exiting standby.
    const bool standby = in->last_read_time_us == 0;
    int64_t elapsed_time_since_last_read = 0;
    if (ass.in_fd > 0)
    {
        elapsed_time_since_last_read = now - in->last_read_time_us;
    }
    else
    {
        elapsed_time_since_last_read = standby ? 0 : now - in->last_read_time_us;
    }
    int64_t sleep_time = bytes * 1000000LL / audio_stream_in_frame_size(stream) /
                             in_get_sample_rate(&stream->common) -
                         elapsed_time_since_last_read; // us
    int64_t frame_time_ms = bytes * 1000LL / audio_stream_in_frame_size(stream) /
                            in_get_sample_rate(&stream->common); // ms
    int64_t timeout = sleep_time / 1000LL;                       // ms
    if (timeout < 1)
    {
        timeout = 1;
    }
    if (timeout > frame_time_ms)
    {
        timeout = frame_time_ms;
    }
    if (bytes > 0)
    {
        result = in_read_from_client(stream, buffer, bytes, timeout, -1);
        if (result < 0)
        {
            ALOGV("The result of in_read_from_client is %zd", result);
        }
        else if (result > 0)
        {
            ret = result;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t);
    const int64_t new_now = (t.tv_sec * 1000000000LL + t.tv_nsec) / 1000;
    sleep_time = sleep_time - (new_now - now);
    int64_t frame_time_us = bytes * 1000000LL / audio_stream_in_frame_size(stream) /
                            in_get_sample_rate(&stream->common); // us
    if (sleep_time > 0 && sleep_time <= frame_time_us)
    {
        usleep(sleep_time);
    }
    else
    {
        sleep_time = 0;
    }
    in->last_read_time_us = new_now + sleep_time;
    // last_read_time_us is an approximation of when the (simulated) alsa
    // buffer is drained by the read, and is empty.
    //
    // On the subsequent in_read(), we measure the elapsed time spent in
    // the recording thread. This is subtracted from the sleep estimate based on frames,
    // thereby accounting for fill in the alsa buffer during the interim.
    if (adev->mic_mute)
        memset(buffer, 0, bytes);
    return ret;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static void *in_socket_sever_thread(void *args)
{
    struct audio_server_socket *pass = (struct audio_server_socket *)args;
    int ret = 0;
    int so_reuseaddr = 1;
    int new_client_fd = -1;
    struct sockaddr_in addr_in;

    ALOGV("%s Constructing audio in socket server...", __func__);
    pass->iss_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (pass->iss_fd < 0)
    {
        ALOGE("%s:%d Fail to construct audio in socket with error: %s",
              __func__, __LINE__, strerror(errno));
        return NULL;
    }
    if (setsockopt(pass->iss_fd, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(int)) < 0)
    {
        ALOGE("%s setsockopt(SO_REUSEADDR) failed. pass->iss_fd: %d\n", __func__, pass->iss_fd);
        return NULL;
    }

    memset(&addr_in, 0, sizeof(addr_in));
    addr_in.sin_family = AF_INET;
    addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
    addr_in.sin_port = htons(pass->in_tcp_port);

    ret = bind(pass->iss_fd, (struct sockaddr *)&addr_in, sizeof(struct sockaddr_in));
    if (ret < 0)
    {
        ALOGE("%s Failed to bind port(%d). ret: %d, %s",
              __func__, pass->in_tcp_port, ret, strerror(errno));
        return NULL;
    }

    ret = listen(pass->iss_fd, 5);
    if (ret < 0)
    {
        ALOGE("%s Failed to listen on port %d", __func__, pass->in_tcp_port);
        return NULL;
    }

    while (!pass->iss_exit)
    {
        ALOGW("%s Wait a audio in client to connect...", __func__);
        socklen_t alen = 0;
        alen = sizeof(struct sockaddr_in);
        new_client_fd = accept(pass->iss_fd, (struct sockaddr *)&addr_in, &alen);
        ALOGW("%s Inet new_client_fd %d",  __func__, new_client_fd);

        if (new_client_fd < 0)
        {
            ALOGE("%s The audio in socket server maybe shutdown as quit command is got. "
                  "Or else, error happen. port: %d %s",
                  __func__, pass->in_tcp_port, strerror(errno));
        }
        else
        {
            ALOGW("%s Currently only receive one input client. Close previous client(%d)",
                  __func__, pass->in_fd);
            pthread_mutex_lock(&pass->mutexlock_in);
            if (ass.iss_read_flag && pass->in_fd > 0 && pass->in_fd != new_client_fd)
            {
                ALOGV("%s:%d send_close_cmd pthread_mutex_lock pass->in_fd %d", __func__, __LINE__, pass->in_fd);
                if (send_close_cmd(pass->in_fd) < 0)
                {
                    ALOGE("Fail to notify audio in client(%d) to close.", pass->in_fd);
                }
            }
            pthread_mutex_unlock(&pass->mutexlock_in);

            if (pass->in_fd > 0)
            {
                if (epoll_ctl(pass->iss_epoll_fd, EPOLL_CTL_DEL, pass->in_fd, NULL))
                {
                    ALOGE("Failed to delete audio in file descriptor to epoll");
                }
                close_socket_fd(&(pass->in_fd));
            }

            ALOGW("%s A new audio in client connected to server. "
                  "new_client_fd = %d. Set it to pass->in_fd",
                  __func__, new_client_fd);
            pass->in_fd = new_client_fd;
            if (pass->in_fd > 0)
            {
                pthread_mutex_lock(&pass->mutexlock_in);
                if (pass->ssi && ass.iss_read_flag) // Make sure parameters are ready.
                {
                    ALOGV("%s:%d send_close_cmd", __func__, __LINE__);
                    if (send_open_cmd(pass, AUDIO_IN) < 0)
                    {
                        ALOGE("Fail to send OPEN command to audio in client(%d)", pass->in_fd);
                    }
                }
                pthread_mutex_unlock(&pass->mutexlock_in);

                struct epoll_event event;
                event.events = EPOLLIN;
                event.data.fd = pass->in_fd;
                if (epoll_ctl(pass->iss_epoll_fd, EPOLL_CTL_ADD, pass->in_fd, &event))
                {
                    ALOGE("Failed to add audio in file descriptor to epoll");
                }
                ALOGI("Success to add audio in file descriptor %d to epoll, iss_epoll_fd %d", pass->in_fd, pass->iss_epoll_fd);
            }
        }
    }

    ALOGW("%s Quit. (%d)", __func__,pass->in_fd);
    pthread_mutex_lock(&pass->mutexlock_in);
    close_socket_fd(&(pass->in_fd));
    close_socket_fd(&(pass->iss_fd));
    pthread_mutex_unlock(&pass->mutexlock_in);
    return NULL;
}

static size_t samples_per_milliseconds(size_t milliseconds,
                                       uint32_t sample_rate,
                                       size_t channel_count)
{
    return milliseconds * sample_rate * channel_count / 1000;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out,
                                   const char *address __unused)
{
    ALOGV("adev_open_output_stream...");

    *stream_out = NULL;
    struct stub_stream_out *out =
        (struct stub_stream_out *)calloc(1, sizeof(struct stub_stream_out));
    if (!out)
        return -ENOMEM;

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
    out->sample_rate = config->sample_rate;
    if (out->sample_rate == 0)
        out->sample_rate = STUB_DEFAULT_SAMPLE_RATE;
    out->channel_mask = config->channel_mask;
    if (out->channel_mask == AUDIO_CHANNEL_NONE)
        out->channel_mask = STUB_OUTPUT_DEFAULT_CHANNEL_MASK;
    out->format = config->format;
    if (out->format == AUDIO_FORMAT_DEFAULT)
        out->format = STUB_DEFAULT_AUDIO_FORMAT;
    out->frame_count = samples_per_milliseconds(
        STUB_OUTPUT_BUFFER_MILLISECONDS,
        out->sample_rate, 1);
    out->stream.update_source_metadata = out_update_source_metadata;

    ALOGV("adev_open_output_stream: sample_rate: %u, channels: %x, format: %d,"
          " frames: %zu",
          out->sample_rate, out->channel_mask, out->format,
          out->frame_count);
    *stream_out = &out->stream;
    ass.sso = out;
    pthread_mutex_lock(&ass.mutexlock_out);

    if (ATRACE_ENABLED())
    {
        ATRACE_INT("avh_adv_open_output_stream_before_send_open_cmd", ass.oss_is_sent_open_cmd);
    }
    if (ass.oss_is_sent_open_cmd == 0)
    {
        if (send_open_cmd(&ass, AUDIO_OUT) < 0)
        {
            ALOGE("Fail to send OPEN command to audio out client(%d)", ass.out_fd);
        }
        else
        {
            ass.oss_is_sent_open_cmd = 1;
            ALOGE("ass.oss_is_sent_open_cmd is set to %d", ass.oss_is_sent_open_cmd);
        }
    }
    if (ATRACE_ENABLED())
    {
        ATRACE_INT("avh_adv_open_output_stream_after_send_open_cmd", ass.oss_is_sent_open_cmd);
    }
    pthread_mutex_unlock(&ass.mutexlock_out);
    return 0;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    pthread_mutex_lock(&ass.mutexlock_out);
    if (send_close_cmd(ass.out_fd) < 0)
    {
        ALOGE("Fail to notify audio out client(%d) to close.", ass.out_fd);
    }
    ass.sso = NULL;
    ass.oss_is_sent_open_cmd = 0;
    pthread_mutex_unlock(&ass.mutexlock_out);
    ALOGV("adev_close_output_stream...");
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    ALOGV("adev_set_parameters");
    return -ENOSYS;
}

static char *adev_get_parameters(const struct audio_hw_device *dev,
                                 const char *keys)
{
    ALOGV("adev_get_parameters");
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    ALOGV("adev_init_check");
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    ALOGV("adev_set_voice_volume: %f", volume);
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    ALOGV("adev_set_master_volume: %f", volume);
    return -ENOSYS;
}

static int adev_get_master_volume(struct audio_hw_device *dev, float *volume)
{
    ALOGV("adev_get_master_volume: %f", *volume);
    return -ENOSYS;
}

static int adev_set_master_mute(struct audio_hw_device *dev, bool muted)
{
    ALOGV("adev_set_master_mute: %d", muted);
    return -ENOSYS;
}

static int adev_get_master_mute(struct audio_hw_device *dev, bool *muted)
{
    ALOGV("adev_get_master_mute: %d", *muted);
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    ALOGV("adev_set_mode: %d", mode);
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    ALOGD("adev_set_mic_mute: %d", state);
    struct stub_audio_device *adev = (struct stub_audio_device *)dev;
    adev->mic_mute = state;
    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    struct stub_audio_device *adev = (struct stub_audio_device *)dev;
    ALOGD("adev_get_mic_mute: %d", adev->mic_mute);
    *state = adev->mic_mute;
    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         const struct audio_config *config)
{
    size_t buffer_size = samples_per_milliseconds(
        ass.input_buffer_milliseconds,
        config->sample_rate,
        audio_channel_count_from_in_mask(
            config->channel_mask));

    if (!audio_has_proportional_frames(config->format))
    {
        // Since the audio data is not proportional choose an arbitrary size for
        // the buffer.
        buffer_size *= 4;
    }
    else
    {
        buffer_size *= audio_bytes_per_sample(config->format);
    }
    ALOGV("adev_get_input_buffer_size: %zu", buffer_size);
    return buffer_size;
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in,
                                  audio_input_flags_t flags __unused,
                                  const char *address __unused,
                                  audio_source_t source __unused)
{
    ALOGV("adev_open_input_stream...");
    struct stub_audio_device *adev = (struct stub_audio_device *)dev;
    *stream_in = NULL;
    struct stub_stream_in *in = (struct stub_stream_in *)calloc(1, sizeof(struct stub_stream_in));
    if (!in)
        return -ENOMEM;
    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;
    in->sample_rate = config->sample_rate;
    if (in->sample_rate == 0)
        in->sample_rate = STUB_DEFAULT_SAMPLE_RATE;
    in->channel_mask = config->channel_mask;
    if (in->channel_mask == AUDIO_CHANNEL_NONE)
        in->channel_mask = STUB_INPUT_DEFAULT_CHANNEL_MASK;
    in->format = config->format;
    if (in->format == AUDIO_FORMAT_DEFAULT)
        in->format = STUB_DEFAULT_AUDIO_FORMAT;
    in->frame_count = samples_per_milliseconds(
        ass.input_buffer_milliseconds, in->sample_rate, 1);
    in->dev = adev;

    ALOGV("adev_open_input_stream: sample_rate: %u, channels: %x, format: %d,"
          "frames: %zu",
          in->sample_rate, in->channel_mask, in->format,
          in->frame_count);
    *stream_in = &in->stream;
    ass.ssi = in;

    return 0;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                    struct audio_stream_in *stream)
{
    ALOGV("adev_close_input_stream...");
    pthread_mutex_lock(&ass.mutexlock_in);
    if (ass.iss_read_flag && ass.in_fd > 0)
    {
        ALOGV("%s:%d send_close_cmd pthread_mutex_lock ass.in_fd %d", __func__, __LINE__, ass.in_fd);
        if (send_close_cmd(ass.in_fd) < 0)
        {
            ALOGE("%s Fail to notify audio out client(%d) to close.", __func__, ass.in_fd);
        }
    }
    ass.iss_read_flag = false;
    pthread_mutex_unlock(&ass.mutexlock_in);

    free(stream);
    return;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    ALOGV("adev_dump");
    return 0;
}

static int adev_close(hw_device_t *device)
{
    ALOGV("adev_close");
    ass.oss_exit = 1;
    if (epoll_ctl(ass.oss_epoll_fd, EPOLL_CTL_DEL, ass.out_fd, NULL))
    {
        ALOGE("Failed to delete audio in file descriptor to epoll");
    }
    if (close(ass.oss_epoll_fd))
    {
        ALOGE("Failed to close output epoll file descriptor");
    }
    pthread_mutex_lock(&ass.mutexlock_out);
    close_socket_fd(&(ass.out_fd));
    close_socket_fd(&(ass.oss_fd));
    ass.oss_is_sent_open_cmd = 0;
    pthread_mutex_unlock(&ass.mutexlock_out);
    pthread_mutex_destroy(&ass.mutexlock_out);
    ass.oss_write_count = 0;

    ass.iss_exit = 1;
    ass.iss_read_flag = false;
    if (epoll_ctl(ass.oss_epoll_fd, EPOLL_CTL_DEL, ass.out_fd, NULL))
    {
        ALOGE("Failed to delete audio in file descriptor to epoll");
    }
    if (close(ass.iss_epoll_fd))
    {
        ALOGE("Failed to close output epoll file descriptor");
    }
    pthread_mutex_lock(&ass.mutexlock_in);
    close_socket_fd(&(ass.in_fd));
    close_socket_fd(&(ass.iss_fd));
    pthread_mutex_unlock(&ass.mutexlock_in);
    pthread_mutex_destroy(&ass.mutexlock_in);

    free(device);
    ass.ssi = NULL;
    return 0;
}

static int adev_open(const hw_module_t *module, const char *name,
                     hw_device_t **device)
{
    ALOGV("adev_open: %s", name);

    //It will generate SIGPIPE when write to a closed socket, which will kill
    //the process. Ignore the signal SIGPIPE to avoid the process crash.
    signal(SIGPIPE, sighandler);

    struct stub_audio_device *adev;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct stub_audio_device));
    if (!adev)
        return -ENOMEM;

    adev->device.common.tag = HARDWARE_DEVICE_TAG;
    adev->device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->device.common.module = (struct hw_module_t *)module;
    adev->device.common.close = adev_close;

    adev->device.init_check = adev_init_check;
    adev->device.set_voice_volume = adev_set_voice_volume;
    adev->device.set_master_volume = adev_set_master_volume;
    adev->device.get_master_volume = adev_get_master_volume;
    adev->device.set_master_mute = adev_set_master_mute;
    adev->device.get_master_mute = adev_get_master_mute;
    adev->device.set_mode = adev_set_mode;
    adev->device.set_mic_mute = adev_set_mic_mute;
    adev->device.get_mic_mute = adev_get_mic_mute;
    adev->device.set_parameters = adev_set_parameters;
    adev->device.get_parameters = adev_get_parameters;
    adev->device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->device.open_output_stream = adev_open_output_stream;
    adev->device.close_output_stream = adev_close_output_stream;
    adev->device.open_input_stream = adev_open_input_stream;
    adev->device.close_input_stream = adev_close_input_stream;
    adev->device.dump = adev_dump;

    *device = &adev->device.common;

    char buf[PROPERTY_VALUE_MAX] = {
        '\0',
    };

    ALOGI("Using inet socket to process audio.");

    ass.sso = NULL;
    ass.out_fd = -1;
    ass.oss_fd = -1;
    ass.oss_exit = 0;

    ass.out_tcp_port = 8768;
    if (property_get("virtual.audio.out.tcp.port", buf, "") > 0)
    {
        ass.out_tcp_port = atoi(buf);
    }
    ALOGI("Out tcp port of INET socket %d", ass.out_tcp_port);

    pthread_create(&ass.oss_thread, NULL, out_socket_sever_thread, &ass);
    ass.oss_epoll_fd = epoll_create1(0);
    if (ass.oss_epoll_fd == -1)
    {
        ALOGE("Failed to create output epoll file descriptor");
    }
    ass.oss_is_sent_open_cmd = 0;
    pthread_mutex_init(&ass.mutexlock_out, 0);
    ass.oss_write_count = 0;

    ass.ssi = NULL;
    ass.in_fd = -1;
    ass.iss_fd = -1;
    ass.iss_exit = 0;
    ass.in_tcp_port = 8767;
    if (property_get("virtual.audio.in.tcp.port", buf, "") > 0)
    {
        ass.in_tcp_port = atoi(buf);
    }
    ALOGI("In tcp port of INET socket %d", ass.in_tcp_port);

    pthread_create(&ass.iss_thread, NULL, in_socket_sever_thread, &ass);
    ass.iss_epoll_fd = epoll_create1(0);
    if (ass.iss_epoll_fd == -1)
    {
        ALOGE("Failed to create output epoll file descriptor");
    }
    ass.iss_read_flag = false;
    if (property_get("virtual.audio.in.buffer_milliseconds", buf, "10") > 0)
    {
        ass.input_buffer_milliseconds = atoi(buf);
        if (ass.input_buffer_milliseconds < STUB_INPUT_BUFFER_MILLISECONDS)
        {
            ass.input_buffer_milliseconds = STUB_INPUT_BUFFER_MILLISECONDS;
        }
        else if (ass.input_buffer_milliseconds > 1000)
        {
            ALOGW("Input buffer milliseconds is greater than 1000ms. Set it to 1000ms.");
            ass.input_buffer_milliseconds = 1000;
        }
    }
    else
    {
        ass.input_buffer_milliseconds = STUB_INPUT_BUFFER_MILLISECONDS;
    }
    ALOGV("Input buffer milliseconds is %dms.", ass.input_buffer_milliseconds);

    if (property_get("acg.audio.channel.mask.enable", buf, "0") > 0)
    {
        ass.audio_mask = atoi(buf);
        if (ass.audio_mask > 0)
        {
            ass.audio_mask = 1;
        }
        else
        {
            ass.audio_mask = 0;
        }
    }
    else
    {
        ass.audio_mask = 0;
    }
    ALOGV("Audio mask is %s.", ass.audio_mask ? "the mask of channel" : "the number of channel");
    pthread_mutex_init(&ass.mutexlock_in, 0);

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Virtal audio HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};
