/**
 * Copyright (C) 2023 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "AudioClient"
#include "AudioClient.h"
#include <log/log.h>

using namespace std::chrono_literals;
using namespace std;
using namespace vhal::client::audio;
using namespace vhal::client;

int AudioClient::startDummyStreamerForInput()
{
    string       ip_addr("127.0.0.1");
    uint16_t     port_num(18767);
    resetCmdFlagsForAudioSink();
    TcpConnectionInfo conn_info = { ip_addr, port_num };
    AudioSink audio_sink(conn_info,[&](const CtrlMessage& ctrl_msg) {
        switch (ctrl_msg.cmd) {
            case Command::kOpen: {
                is_open_for_sink = true;
                ALOGV("Received Open command from Audio VHal");
                break;
            }
            case Command::kClose: {
                is_close_for_sink = true;
                ALOGV("Received Close command from VHal");
                break;
            }
            default:
                ALOGE("Unknown Command received, exiting with failure");
                exit(1);
        }
    });
    ALOGI("Waiting Audio Open callback For Input..");

    // Need to be alive
    while (is_running_sink) {
        this_thread::sleep_for(5ms);
    }
    return 0;
}

int AudioClient::startDummyStreamerForOutput() {
    string       ip_addr("127.0.0.1");
    uint16_t     port_num(18768);
    resetCmdFlagsForAudioSource();
    TcpConnectionInfo conn_info = { ip_addr, port_num };
    AudioSource audio_source(conn_info, [&](const CtrlMessage& ctrl_msg) {
        switch (ctrl_msg.cmd) {
        case Command::kOpen: {
            is_open_for_source = true;
            ALOGV("Received Open command from Audio VHal");
            break;
        }
        case Command::kStartstream: {
            is_stream_started = true;
            ALOGV("Received Startstream command from Audio VHal");
	    break;
	}
        case Command::kData: {
            is_data_received = true;
            ALOGV("Received Data command from Audio VHal");
            break;
        }
        case Command::kStopstream: {
            is_stream_stopped = true;
            ALOGV("Received Stopstream command from Audio VHal");
	    break;
	}
        case Command::kClose: {
            is_close_for_source = true;
            ALOGV("Received Close command from VHal");
	    break;
	}
        default:
            ALOGE("Unknown Command received, exiting with failure");
            exit(1);
        }
    });
    ALOGI("Waiting Audio Open callback For Output...");
    // Need to be alive
    while (is_running_source) {
        this_thread::sleep_for(5ms);
    }

    return 0;    
}

void AudioClient::stopDummyStreamerForInput() {
    is_running_sink = false;
}

void AudioClient::stopDummyStreamerForOutput() {
    is_running_source = false;
}

void AudioClient::resetCmdFlagsForAudioSource() {
    is_open_for_source = false;
    is_close_for_source = false;
    is_stream_started = false;
    is_data_received = false;
    is_stream_stopped = false;
    is_running_source = true;
}

void AudioClient::resetCmdFlagsForAudioSink() {
    is_open_for_sink = false;
    is_close_for_sink = false;
    is_running_sink = true;
}

bool AudioClient::getOpenOutStreamStatus() {
    return is_open_for_source;
}

bool AudioClient::getCloseOutStreamStatus() {
    return is_close_for_source;
}

bool AudioClient::getStartOutStreamStatus() {
    return is_stream_started;
}

bool AudioClient::getStopOutStreamStatus() {
    return is_stream_stopped;
}

bool AudioClient::getDataOutStreamStatus() {
    return is_data_received;
}
    
bool AudioClient::getOpenInStreamStatus() {
    return is_open_for_sink;
}

bool AudioClient::getCloseInStreamStatus() {
    return is_close_for_sink;
}

