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

#include "tcp_stream_socket_client.h"
#include "audio_sink.h"
#include "audio_source.h"
#include "android_audio_core.h"
#include <atomic>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace vhal::client;
using namespace std;

class AudioClient {
private:
    bool is_running_sink;
    bool is_running_source;

    bool is_open_for_source;
    bool is_close_for_source;
    bool is_stream_started;
    bool is_stream_stopped;
    bool is_data_received;
    bool is_open_for_sink;
    bool is_close_for_sink;

    void resetCmdFlagsForAudioSink();
    void resetCmdFlagsForAudioSource();

public :

    bool getOpenOutStreamStatus();
    bool getCloseOutStreamStatus();
    bool getStartOutStreamStatus();
    bool getStopOutStreamStatus();
    bool getDataOutStreamStatus();
    bool getOpenInStreamStatus();
    bool getCloseInStreamStatus();

    int startDummyStreamerForInput();
    int startDummyStreamerForOutput();
    void stopDummyStreamerForInput();
    void stopDummyStreamerForOutput();
};
