/*
 * Copyright (C) 2023 Intel Corporation
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
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <log/log.h>
#include "AudioFixture.h"

#define MODULE_NAME "/system/vendor/lib64/audio.primary.cic_cloud.so"
#define STUB_OUTPUT_BUFFER_MILLISECONDS 10

using namespace std;
using namespace std::chrono_literals;

const hw_module_t *mod;
audio_hw_device_t* halDevice = nullptr;

void AudioFixture::runAudioInputThread() {
    mAudioClient.startDummyStreamerForInput();
}


void AudioFixture::runAudioOutputThread() {
    mAudioClient.startDummyStreamerForOutput();
}

int load(const char *id,
        const struct hw_module_t **pHmi)
{
    int status = -EINVAL;
    void *handle = NULL;
    struct hw_module_t *hmi = NULL;
    const char *sym = HAL_MODULE_INFO_SYM_AS_STR;

    handle = dlopen(MODULE_NAME, RTLD_NOW);
    if (handle == NULL) {
        char const *err_str = dlerror();
        ALOGE("load: module=%s, %s", MODULE_NAME, err_str ? err_str : "unknown");
        status = -EINVAL;
        goto done;
    }
    ALOGI("load: module=%s success", MODULE_NAME);

    /* Get the address of the struct hal_module_info. */
    hmi = (struct hw_module_t *)dlsym(handle, sym);
    if (hmi == NULL) {
        ALOGE("load: couldn't find symbol %s", sym);
        status = -EINVAL;
        goto done;
    }
    ALOGD("load: Found symbol %s", sym);

    /* Check that the id matches */
    if (strcmp(id, hmi->id) != 0) {
        ALOGE("load: id=%s != hmi->id=%s", id, hmi->id);
        status = -EINVAL;
        goto done;
    }

    hmi->dso = handle;
    ALOGI("load: Got Handle %s", sym);

    /* success */
    status = 0;

done:
    if (status != 0) {
        hmi = NULL;
        if (handle != NULL) {
            dlclose(handle);
            handle = NULL;
        }
    } else {
        ALOGI("loaded HAL id=%s path=%s hmi=%p handle=%p",
                id, MODULE_NAME, hmi, handle);
    }

    *pHmi = hmi;

    return status;
}

int load_audio_module() {
    int ret;
    ret = load(AUDIO_HARDWARE_MODULE_ID, &mod);
    if (ret) 
        ALOGE("%s:Failed to load Audio Hardware Module",__FUNCTION__);
    else
        return mod->methods->open(mod, AUDIO_HARDWARE_INTERFACE,
                                 TO_HW_DEVICE_T_OPEN(&halDevice));
    return -1;
}

int audio_hw_device_close() {
    return halDevice->common.close(&halDevice->common);
}


AudioFixture::AudioFixture() {
}

void AudioFixture::SetUp() {
    input_thread = new thread(&AudioFixture::runAudioInputThread, this);
    output_thread = new thread(&AudioFixture::runAudioOutputThread, this);
    this_thread::sleep_for(500ms);
}

void AudioFixture::TearDown() {
    mAudioClient.stopDummyStreamerForInput();
    mAudioClient.stopDummyStreamerForOutput();
    input_thread->join();
    output_thread->join();
    delete input_thread;
    delete output_thread;
    input_thread = nullptr;
    output_thread = nullptr;
}

AudioFixture::~AudioFixture() {
}

TEST_F(AudioFixture, ModuleLoadTest)
{
    // Verify the return value of load_audio_module API
    EXPECT_EQ(load_audio_module(), 0);
    this_thread::sleep_for(1000ms);
}

TEST_F(AudioFixture, GetOutStreamCmdStatus)
{
    // Verify that None of the CMDs received for audio source by streamer
    ASSERT_FALSE(mAudioClient.getOpenOutStreamStatus());
    ASSERT_FALSE(mAudioClient.getCloseOutStreamStatus());
    ASSERT_FALSE(mAudioClient.getStartOutStreamStatus());
    ASSERT_FALSE(mAudioClient.getDataOutStreamStatus());
    ASSERT_FALSE(mAudioClient.getStopOutStreamStatus());
}

TEST_F(AudioFixture, GetInputStreamCmdStatus)
{
    // Verify that None of the CMDs received for audio sink by streamer
    ASSERT_FALSE(mAudioClient.getOpenInStreamStatus());
    ASSERT_FALSE(mAudioClient.getCloseInStreamStatus());
}

TEST_F(AudioFixture, InitApiTest)
{
    // Verify the return value of init_check API
    EXPECT_EQ(halDevice->init_check((audio_hw_device*)halDevice), 0);
}

TEST_F(AudioFixture, GetMicStatusTest)
{
    // Declare a boolean satuts to get the mic status
    bool status;

    // Call get_mic_mute API and get the status
    EXPECT_EQ(halDevice->get_mic_mute((audio_hw_device*)halDevice, &status), 0);

    // Verify that by default mic status is set to false
    ASSERT_FALSE(status);

    // Call set_mic_mute API and set the mic mute status to true
    EXPECT_EQ(halDevice->set_mic_mute((audio_hw_device*)halDevice, true), 0);

    // Call get_mic_mute API and get the status
    EXPECT_EQ(halDevice->get_mic_mute((audio_hw_device*)halDevice, &status), 0);

    // Verify that mic mute status is set to true
    ASSERT_TRUE(status);
}

TEST_F(AudioFixture, SetMicMuteTest)
{
    // Declare a boolean satuts to get the mic status
    bool status;

    // Call set_mic_mute API and send status as false
    EXPECT_EQ(halDevice->set_mic_mute((audio_hw_device*)halDevice, false), 0);

    // Call get_mic_mute API and get the status
    EXPECT_EQ(halDevice->get_mic_mute((audio_hw_device*)halDevice, &status), 0);

    // Verify the mic status is set to false
    ASSERT_FALSE(status);
}

TEST_F(AudioFixture, OutGetSampleRateTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = 2;
    config.format = AUDIO_FORMAT_DEFAULT;
    config.frame_count = 480;

    // Declare an audio_stream_out struct pointer
    struct audio_stream_out *stream_out;

    // Verify that Open CMD yet not received
    ASSERT_FALSE(mAudioClient.getOpenOutStreamStatus());

    // Call to open_output_stream API
    EXPECT_EQ(halDevice->open_output_stream((audio_hw_device*)halDevice, 0, 0, 
          AUDIO_OUTPUT_FLAG_NONE, &config, &stream_out, NULL), 0);

    // Verify the return value of get_sample_rate API
    EXPECT_EQ(stream_out->common.get_sample_rate((audio_stream *)stream_out), config.sample_rate);
}

TEST_F(AudioFixture, OutSetSampleRateTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = 2;
    config.format = AUDIO_FORMAT_DEFAULT;
    config.frame_count = 480;

    // Declare an audio_stream_out struct pointer
    struct audio_stream_out *stream_out;

    // Set sample rate to 44k
    uint32_t rate = 44000;

    // Call to open_output_stream API
    EXPECT_EQ(halDevice->open_output_stream((audio_hw_device*)halDevice, 0, 0, 
          AUDIO_OUTPUT_FLAG_NONE, &config, &stream_out, NULL), 0);

    // Verify the return value of get_sample_rate API
    EXPECT_EQ(stream_out->common.get_sample_rate((audio_stream *)stream_out), config.sample_rate);
    // Verify the return value of set_sample_rate API
    EXPECT_EQ(stream_out->common.set_sample_rate((audio_stream *)stream_out, rate), 0);
    // Verify the return value of get_sample_rate API
    EXPECT_EQ(stream_out->common.get_sample_rate((audio_stream *)stream_out), rate);
}


TEST_F(AudioFixture, OutGetChannelMaskTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    config.format = AUDIO_FORMAT_DEFAULT;
    config.frame_count = 480;

    // Declare an audio_stream_out struct pointer
    struct audio_stream_out *stream_out;

    // Call to open_output_stream API
    EXPECT_EQ(halDevice->open_output_stream((audio_hw_device*)halDevice, 0, 0, 
          AUDIO_OUTPUT_FLAG_NONE, &config, &stream_out, NULL), 0);

    // Verify the return value of get_channels API
    EXPECT_EQ(stream_out->common.get_channels((audio_stream *)stream_out), config.channel_mask);
}

TEST_F(AudioFixture, OutGetLatencyTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    config.format = AUDIO_FORMAT_DEFAULT;
    config.frame_count = 480;

    // Declare an audio_stream_out struct pointer
    struct audio_stream_out *stream_out;

    // Call to open_output_stream API
    EXPECT_EQ(halDevice->open_output_stream((audio_hw_device*)halDevice, 0, 0, 
          AUDIO_OUTPUT_FLAG_NONE, &config, &stream_out, NULL), 0);

    // Verify the return value of out_get_latency API
    EXPECT_EQ(stream_out->get_latency(stream_out), STUB_OUTPUT_BUFFER_MILLISECONDS);
}

TEST_F(AudioFixture, OutGetBufferSizeTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    config.format = AUDIO_FORMAT_DEFAULT;
    config.frame_count = 480;

    // Declare an audio_stream_out struct pointer
    struct audio_stream_out *stream_out;

    // Store the buffer size
    uint32_t buffer_size = 1920;

    // Call to open_output_stream API
    EXPECT_EQ(halDevice->open_output_stream((audio_hw_device*)halDevice, 0, 0, 
          AUDIO_OUTPUT_FLAG_NONE, &config, &stream_out, NULL), 0);

    // Verify the return value of out_get_buffer_size API
    EXPECT_EQ(stream_out->common.get_buffer_size((audio_stream *)stream_out), buffer_size);
}

TEST_F(AudioFixture, OutGetFormatTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = 2;
    config.format = AUDIO_FORMAT_PCM_16_BIT;
    config.frame_count = 480;

    // Declare an audio_stream_out struct pointer
    struct audio_stream_out *stream_out;

    // Call to open_output_stream API
    EXPECT_EQ(halDevice->open_output_stream((audio_hw_device*)halDevice, 0, 0, 
          AUDIO_OUTPUT_FLAG_NONE, &config, &stream_out, NULL), 0);

    // Verify the return value of get_format API
    EXPECT_EQ(stream_out->common.get_format((audio_stream *)stream_out), config.format);
}

TEST_F(AudioFixture, OutSetFormatTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = 2;
    config.format = AUDIO_FORMAT_PCM_16_BIT;
    config.frame_count = 480;

    // Declare an audio_stream_out struct pointer
    struct audio_stream_out *stream_out;

    // Set audio format to 8bit PCM format
    audio_format_t format = AUDIO_FORMAT_PCM_8_BIT;

    // Call to open_output_stream API
    EXPECT_EQ(halDevice->open_output_stream((audio_hw_device*)halDevice, 0, 0, 
          AUDIO_OUTPUT_FLAG_NONE, &config, &stream_out, NULL), 0);

    // Verify the return value of get_sample_rate API
    EXPECT_EQ(stream_out->common.get_format((audio_stream *)stream_out), config.format);
    EXPECT_EQ(stream_out->common.set_format((audio_stream *)stream_out, format), 0);
    EXPECT_EQ(stream_out->common.get_format((audio_stream *)stream_out), format);
}

TEST_F(AudioFixture, OpenOutStreamTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = 2;
    config.format = AUDIO_FORMAT_DEFAULT;
    config.frame_count = 480;

    // Declare an audio_stream_out struct pointer
    struct audio_stream_out *stream_out;

    // Verify that Close CMD yet not received
    ASSERT_FALSE(mAudioClient.getCloseOutStreamStatus());

    // Call to open_output_stream API
    EXPECT_EQ(halDevice->open_output_stream((audio_hw_device*)halDevice, 0, 0, 
          AUDIO_OUTPUT_FLAG_NONE, &config, &stream_out, NULL), 0);

    // Sleep for 1sec so that streamer receives the CMD
    this_thread::sleep_for(1000ms);

    // Verify that Open CMD received
    ASSERT_TRUE(mAudioClient.getOpenOutStreamStatus());

    // Verify that Close CMD yet not received
    ASSERT_FALSE(mAudioClient.getCloseOutStreamStatus());
}

TEST_F(AudioFixture, OpenCloseOutStreamTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = 2;
    config.format = AUDIO_FORMAT_DEFAULT;
    config.frame_count = 480;

    // Declare an audio_stream_out struct pointer
    struct audio_stream_out *stream_out;

    // Call to open_output_stream API
    EXPECT_EQ(halDevice->open_output_stream((audio_hw_device*)halDevice, 0, 0, 
          AUDIO_OUTPUT_FLAG_NONE, &config, &stream_out, NULL), 0);

    // Sleep for 1sec so that streamer receives the CMD
    this_thread::sleep_for(1000ms);

    // Verify that Open CMD received by streamer
    ASSERT_TRUE(mAudioClient.getOpenOutStreamStatus());

    // Verify that Close CMD is yet not received
    ASSERT_FALSE(mAudioClient.getCloseOutStreamStatus());

    // Call to close_output_stream API
    halDevice->close_output_stream((audio_hw_device*)halDevice, stream_out);

    // Sleep for 1sec so that streamer receives the CMD
    this_thread::sleep_for(1000ms);

    // Verify that Open and Close both CMD received
    ASSERT_TRUE(mAudioClient.getCloseOutStreamStatus());
    ASSERT_TRUE(mAudioClient.getOpenOutStreamStatus());
}

TEST_F(AudioFixture, OutWriteTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = 2;
    config.format = AUDIO_FORMAT_DEFAULT;
    config.frame_count = 480;

    // Declare an audio_stream_out struct pointer
    struct audio_stream_out *stream_out;

    // Take a buffer of size 1920 and fill it with 0's
    size_t bytes = 1920;
    char buffer[bytes];
    memset(buffer, 0, sizeof(buffer));

    // Check that Before calling open_output_stream, Startstream and
    // Data CMD not received by streamer
    ASSERT_FALSE(mAudioClient.getStartOutStreamStatus());
    ASSERT_FALSE(mAudioClient.getDataOutStreamStatus());

    // Call to open_output_stream API
    EXPECT_EQ(halDevice->open_output_stream((audio_hw_device*)halDevice, 0, 0, 
          AUDIO_OUTPUT_FLAG_NONE, &config, &stream_out, NULL), 0);

    // Call to out_write API
    stream_out->write(stream_out,(void*)buffer,bytes);

    // Sleep for 1sec so that streamer receives the CMD
    this_thread::sleep_for(1000ms);

    // Verify that Startstream and Data CMD received at streamer
    ASSERT_TRUE(mAudioClient.getStartOutStreamStatus());
    ASSERT_TRUE(mAudioClient.getDataOutStreamStatus());

    // Call to close_output_stream API
    halDevice->close_output_stream((audio_hw_device*)halDevice, stream_out);
}

TEST_F(AudioFixture, StandbyTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = 2;
    config.format = AUDIO_FORMAT_DEFAULT;
    config.frame_count = 480;

    // Declare an audio_stream_out struct pointer
    struct audio_stream_out *stream_out;

    // Take a buffer of size 1920 and fill it with 0's
    size_t bytes = 1920;
    char buffer[bytes];
    memset(buffer, 0, sizeof(buffer));

    // Check that Before calling open_output_stream, Startstream and
    // Data CMD not received by streamer
    ASSERT_FALSE(mAudioClient.getStartOutStreamStatus());
    ASSERT_FALSE(mAudioClient.getDataOutStreamStatus());

    // Call to open_output_stream API
    EXPECT_EQ(halDevice->open_output_stream((audio_hw_device*)halDevice, 0, 0, 
          AUDIO_OUTPUT_FLAG_NONE, &config, &stream_out, NULL), 0);

    // Call to out_write API
    stream_out->write(stream_out,(void*)buffer,bytes);

    // Sleep for 1sec so that streamer receives the CMD
    this_thread::sleep_for(1000ms);

    // Verify that Startstream and Data CMD received at streamer
    ASSERT_TRUE(mAudioClient.getStartOutStreamStatus());
    ASSERT_TRUE(mAudioClient.getDataOutStreamStatus());

    // Verify that StopStream is not called until now
    ASSERT_FALSE(mAudioClient.getStopOutStreamStatus());

    // Call to out_standby API
    stream_out->common.standby((audio_stream*)stream_out);

    // Sleep for 1sec so that streamer receives the CMD
    this_thread::sleep_for(1000ms);

    // Verify that StopStream is received at streamer
    ASSERT_TRUE(mAudioClient.getStopOutStreamStatus());

    // Call to close_output_stream API
    halDevice->close_output_stream((audio_hw_device*)halDevice, stream_out);
}

TEST_F(AudioFixture, OpenCloseInputStreamTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = AUDIO_CHANNEL_NONE;
    config.format = AUDIO_FORMAT_DEFAULT;
    config.frame_count = 480;

    // Declare an audio_stream_in struct pointer
    struct audio_stream_in *stream_in;

    // Take a buffer of size 1920 and fill it with 0's
    size_t bytes = 1920;
    char buffer[bytes];
    memset(buffer, 0, sizeof(buffer));

    // Check that Before calling open_input_stream, Open and 
    // Close are not received by streamer
    ASSERT_FALSE(mAudioClient.getOpenInStreamStatus());
    ASSERT_FALSE(mAudioClient.getCloseInStreamStatus());

    // Call to open_input_stream API
    EXPECT_EQ(halDevice->open_input_stream((audio_hw_device*)halDevice, 0, 0, &config, &stream_in, 
          AUDIO_INPUT_FLAG_NONE, NULL, AUDIO_SOURCE_REMOTE_SUBMIX), 0);

    // Call to in_read API
    stream_in->read(stream_in,(void*)buffer,bytes);

    // Verify that Open CMD received
    ASSERT_TRUE(mAudioClient.getOpenInStreamStatus());

    // Call to close_input_stream API
    halDevice->close_input_stream((audio_hw_device*)halDevice, stream_in);

    // Sleep for 1sec so that streamer receives the CMD
    this_thread::sleep_for(1000ms);

    // Check that After calling close_input_stream, Close CMD is received by streamer
    ASSERT_TRUE(mAudioClient.getCloseInStreamStatus());
}

TEST_F(AudioFixture, GetSampleRateInputStreamTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = AUDIO_CHANNEL_NONE;
    config.format = AUDIO_FORMAT_DEFAULT;
    config.frame_count = 480;

    // Declare an audio_stream_in struct pointer
    struct audio_stream_in *stream_in;

    // Call to open_input_stream API
    EXPECT_EQ(halDevice->open_input_stream((audio_hw_device*)halDevice, 0, 0, &config, &stream_in, 
          AUDIO_INPUT_FLAG_NONE, NULL, AUDIO_SOURCE_REMOTE_SUBMIX), 0);

    // Verify the return value of get_sample_rate API
    EXPECT_EQ(stream_in->common.get_sample_rate((audio_stream *)stream_in), config.sample_rate);

}

TEST_F(AudioFixture, SetSampleRateInputStreamTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = AUDIO_CHANNEL_NONE;
    config.format = AUDIO_FORMAT_DEFAULT;
    config.frame_count = 480;

    // Declare an audio_stream_in struct pointer
    struct audio_stream_in *stream_in;

    // Set sample rate to 44k
    uint32_t rate = 44000;

    // Call to open_input_stream API
    EXPECT_EQ(halDevice->open_input_stream((audio_hw_device*)halDevice, 0, 0, &config, &stream_in, 
          AUDIO_INPUT_FLAG_NONE, NULL, AUDIO_SOURCE_REMOTE_SUBMIX), 0);

    // Verify the return value of set_sample_rate API
    EXPECT_EQ(stream_in->common.set_sample_rate((audio_stream *)stream_in, rate), 0);
    // Verify the return value of get_sample_rate API
    EXPECT_EQ(stream_in->common.get_sample_rate((audio_stream *)stream_in), rate);
}

TEST_F(AudioFixture, GetFormatInputStreamTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = AUDIO_CHANNEL_NONE;
    config.format = AUDIO_FORMAT_PCM_16_BIT;
    config.frame_count = 480;

    // Declare an audio_stream_in struct pointer
    struct audio_stream_in *stream_in;

    // Call to open_input_stream API
    EXPECT_EQ(halDevice->open_input_stream((audio_hw_device*)halDevice, 0, 0, &config, &stream_in, 
          AUDIO_INPUT_FLAG_NONE, NULL, AUDIO_SOURCE_REMOTE_SUBMIX), 0);

    // Verify the return value of get_format API
    EXPECT_EQ(stream_in->common.get_format((audio_stream *)stream_in), config.format);
}

TEST_F(AudioFixture, SetFormatInputStreamTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = AUDIO_CHANNEL_NONE;
    config.format = AUDIO_FORMAT_PCM_16_BIT;
    config.frame_count = 480;

    // Declare an audio_stream_in struct pointer
    struct audio_stream_in *stream_in;

    // Set audio format to 8bit PCM format
    audio_format_t format = AUDIO_FORMAT_PCM_8_BIT;

    // Call to open_input_stream API
    EXPECT_EQ(halDevice->open_input_stream((audio_hw_device*)halDevice, 0, 0, &config, &stream_in, 
          AUDIO_INPUT_FLAG_NONE, NULL, AUDIO_SOURCE_REMOTE_SUBMIX), 0);

    // Verify the return value of get_format API
    EXPECT_EQ(stream_in->common.get_format((audio_stream *)stream_in), config.format);
    // Verify the return value of set_format API
    EXPECT_EQ(stream_in->common.set_format((audio_stream *)stream_in, format), 0);
    EXPECT_EQ(stream_in->common.get_format((audio_stream *)stream_in), format);
}

TEST_F(AudioFixture, GetChannelsInputStreamTest)
{
    // Declare an audio_config var
    struct audio_config config;

    // Fill the audio_config structure
    config.sample_rate = 48000;
    config.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    config.format = AUDIO_FORMAT_PCM_16_BIT;
    config.frame_count = 480;

    // Declare an audio_stream_in struct pointer
    struct audio_stream_in *stream_in;

    // Call to open_input_stream API
    EXPECT_EQ(halDevice->open_input_stream((audio_hw_device*)halDevice, 0, 0, &config, &stream_in, 
          AUDIO_INPUT_FLAG_NONE, NULL, AUDIO_SOURCE_REMOTE_SUBMIX), 0);

    // Verify the return value of get_channels API
    EXPECT_EQ(stream_in->common.get_channels((audio_stream *)stream_in), config.channel_mask);
}

TEST_F(AudioFixture, CloseVhalTest)
{
    // Call to adev_close API
    EXPECT_EQ(audio_hw_device_close(), 0);
}

