LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_LDLIBS += -landroid -llog
LOCAL_CFLAGS += -fexceptions
LOCAL_MODULE_TAGS:= optional

LOCAL_SRC_FILES := main.cpp AudioFixture.cpp AudioClient.cpp
LOCAL_SHARED_LIBRARIES += liblog libcutils audio.primary.$(TARGET_PRODUCT) libvhal-client
LOCAL_MULTILIB := 64
LOCAL_VENDOR_MODULE := true
LOCAL_STATIC_LIBRARIES += libgtest_main libgtest libgmock

LOCAL_HEADER_LIBRARIES := libhardware_headers

LOCAL_CPPFLAGS := $(LOG_FLAGS) $(WARNING_LEVEL) $(DEBUG_FLAGS) $(VERSION_FLAGS)

LOCAL_C_INCLUDES += \
    $(LOCAL_PATH) \
    vendor/intel/external/project-celadon/libvhal-client \
    hardware/libhardware/include/hardware \
    system/media/audio/include/system \
    vendor/intel/external/project-celadon/libvhal-client/include/libvhal \
    external/gtest/include \
    external/gtest \
    bionic

LOCAL_MODULE:= AudioFixtureTest

include $(BUILD_EXECUTABLE)
