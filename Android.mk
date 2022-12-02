#
# Copyright (C) 2011 The Android Open Source Project
# Copyright (C) 2019-2022 Intel Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

ifeq ($(TARGET_USE_AUDIO_VHAL), true)

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_VENDOR_MODULE := true
LOCAL_MODULE := audio.primary.$(TARGET_PRODUCT)
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_MODULE_TAGS := optional

LOCAL_SHARED_LIBRARIES := libcutils liblog

LOCAL_SRC_FILES := audio_hw.c

LOCAL_CFLAGS := -Wno-unused-parameter
LOCAL_HEADER_LIBRARIES := libhardware_headers

include $(BUILD_SHARED_LIBRARY)

endif # TARGET_USE_AUDIO_VHAL
