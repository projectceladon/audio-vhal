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

#include <stdio.h>
#include <cstdlib>
#include <thread>
#include <hardware.h>
#include <audio.h>
#include "android_audio_core.h"
#include "gtest/gtest.h"
#include "AudioClient.h"

class AudioFixture : public ::testing::Test
{
public:
    /*! Creates an instance of AudioFixture */
    AudioFixture();

    /*! Destroys an instance of AudioFixture */
    ~AudioFixture();

    virtual void SetUp();

    virtual void TearDown();

    std::thread* input_thread;
    std::thread* output_thread;
    AudioClient mAudioClient;

private:
    /*! Disabled Copy Constructor */
    AudioFixture(const AudioFixture&);

    /*! Disabled Assignment operator */
    AudioFixture& operator=(const AudioFixture&);

    void runAudioInputThread();

    void runAudioOutputThread();

};

