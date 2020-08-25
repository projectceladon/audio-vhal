/*
 * Copyright (C) 2012 The Android Open Source Project
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

//#define LOG_NDEBUG 0
//#define LOG_NNDEBUG 0
#define LOG_TAG "Sensor:"

#ifdef LOG_NNDEBUG
#define ALOGVV(...) ALOGV(__VA_ARGS__)
#else
#define ALOGVV(...) ((void)0)
#endif

#include "Sensor.h"
#include "../VirtualBuffer.h"
#include "../VirtualFakeCamera2.h"
#include "system/camera_metadata.h"
#include <cmath>
#include <cstdlib>
#include <log/log.h>

#define MAX(a, b) ((a > b) ? a : b)
#define MIN(a, b) ((a < b) ? a : b)
#define CLAP(a) (MAX((MIN(a, 0xff)), 0x00))

namespace android
{

// const nsecs_t Sensor::kExposureTimeRange[2] =
//    {1000L, 30000000000L} ; // 1 us - 30 sec
// const nsecs_t Sensor::kFrameDurationRange[2] =
//    {33331760L, 30000000000L}; // ~1/30 s - 30 sec
const nsecs_t Sensor::kExposureTimeRange[2] = {1000L,
					       300000000L}; // 1 us - 0.3 sec
const nsecs_t Sensor::kFrameDurationRange[2] = {
    33331760L, 300000000L}; // ~1/30 s - 0.3 sec

const nsecs_t Sensor::kMinVerticalBlank = 10000L;

const uint8_t Sensor::kColorFilterArrangement =
    ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB;

// Output image data characteristics
const uint32_t Sensor::kMaxRawValue = 4000;
const uint32_t Sensor::kBlackLevel = 1000;

// Sensor sensitivity
const float Sensor::kSaturationVoltage = 0.520f;
const uint32_t Sensor::kSaturationElectrons = 2000;
const float Sensor::kVoltsPerLuxSecond = 0.100f;

const float Sensor::kElectronsPerLuxSecond = Sensor::kSaturationElectrons /
					     Sensor::kSaturationVoltage *
					     Sensor::kVoltsPerLuxSecond;

const float Sensor::kBaseGainFactor =
    (float)Sensor::kMaxRawValue / Sensor::kSaturationElectrons;

const float Sensor::kReadNoiseStddevBeforeGain = 1.177; // in electrons
const float Sensor::kReadNoiseStddevAfterGain = 2.100;  // in digital counts
const float Sensor::kReadNoiseVarBeforeGain =
    Sensor::kReadNoiseStddevBeforeGain * Sensor::kReadNoiseStddevBeforeGain;
const float Sensor::kReadNoiseVarAfterGain =
    Sensor::kReadNoiseStddevAfterGain * Sensor::kReadNoiseStddevAfterGain;

const int32_t Sensor::kSensitivityRange[2] = {100, 1600};
const uint32_t Sensor::kDefaultSensitivity = 100;

/** A few utility functions for math, normal distributions */

// Take advantage of IEEE floating-point format to calculate an approximate
// square root. Accurate to within +-3.6%
float sqrtf_approx(float r)
{
	// Modifier is based on IEEE floating-point representation; the
	// manipulations boil down to finding approximate log2, dividing by two,
	// and then inverting the log2. A bias is added to make the relative
	// error symmetric about the real answer.
	const int32_t modifier = 0x1FBB4000;

	int32_t r_i = *(int32_t *)(&r);
	r_i = (r_i >> 1) + modifier;

	return *(float *)(&r_i);
}

Sensor::Sensor(uint32_t width, uint32_t height)
    : Thread(false), mResolution{width, height}, mActiveArray{0, 0, width,
							      height},
      mRowReadoutTime(kFrameDurationRange[0] / height), mGotVSync(false),
      mExposureTime(kFrameDurationRange[0] - kMinVerticalBlank),
      mFrameDuration(kFrameDurationRange[0]), mGainFactor(kDefaultSensitivity),
      mNextBuffers(NULL), mFrameNumber(0), mCapturedBuffers(NULL),
      mListener(NULL), mScene(width, height, kElectronsPerLuxSecond)
{
	// ALOGE("Sensor created with pixel array %d x %d", width,
	// height);
}

Sensor::~Sensor() { shutDown(); }

status_t Sensor::startUp()
{
	ALOGV("%s: E", __FUNCTION__);

	int res;
	mCapturedBuffers = NULL;
	res =
	    run("VirtualFakeCamera2::Sensor", ANDROID_PRIORITY_URGENT_DISPLAY);

	if (res != OK) {
		ALOGE("Unable to start up sensor capture thread: %d", res);
	}
	return res;
}

status_t Sensor::shutDown()
{
	ALOGV("%s: E", __FUNCTION__);

	int res;
	res = requestExitAndWait();
	if (res != OK) {
		ALOGE("Unable to shut down sensor capture thread: %d", res);
	}
	return res;
}

Scene &Sensor::getScene() { return mScene; }

void Sensor::setExposureTime(uint64_t ns)
{
	Mutex::Autolock lock(mControlMutex);
	ALOGVV("Exposure set to %f", ns / 1000000.f);
	mExposureTime = ns;
}

void Sensor::setFrameDuration(uint64_t ns)
{
	Mutex::Autolock lock(mControlMutex);
	ALOGVV("Frame duration set to %f", ns / 1000000.f);
	mFrameDuration = ns;
}

void Sensor::setSensitivity(uint32_t gain)
{
	Mutex::Autolock lock(mControlMutex);
	ALOGVV("Gain set to %d", gain);
	mGainFactor = gain;
}

void Sensor::setDestinationBuffers(Buffers *buffers)
{
	Mutex::Autolock lock(mControlMutex);
	mNextBuffers = buffers;
}

void Sensor::setFrameNumber(uint32_t frameNumber)
{
	Mutex::Autolock lock(mControlMutex);
	mFrameNumber = frameNumber;
}

bool Sensor::waitForVSync(nsecs_t reltime)
{
	int res;
	Mutex::Autolock lock(mControlMutex);

	mGotVSync = false;
	res = mVSync.waitRelative(mControlMutex, reltime);
	if (res != OK && res != TIMED_OUT) {
		ALOGE("%s: Error waiting for VSync signal: %d", __FUNCTION__,
		      res);
		return false;
	}
	return mGotVSync;
}

bool Sensor::waitForNewFrame(nsecs_t reltime, nsecs_t *captureTime)
{
	Mutex::Autolock lock(mReadoutMutex);
	if (mCapturedBuffers == NULL) {
		int res;
		res = mReadoutAvailable.waitRelative(mReadoutMutex, reltime);
		if (res == TIMED_OUT) {
			return false;
		} else if (res != OK || mCapturedBuffers == NULL) {
			ALOGE("Error waiting for sensor readout signal: %d",
			      res);
			return false;
		}
	}
	mReadoutComplete.signal();

	*captureTime = mCaptureTime;
	mCapturedBuffers = NULL;
	return true;
}

Sensor::SensorListener::~SensorListener() {}

void Sensor::setSensorListener(SensorListener *listener)
{
	Mutex::Autolock lock(mControlMutex);
	mListener = listener;
}

status_t Sensor::readyToRun()
{
	ALOGV("Starting up sensor thread");
	mStartupTime = systemTime();
	mNextCaptureTime = 0;
	mNextCapturedBuffers = NULL;
	return OK;
}

bool Sensor::threadLoop()
{
	/**
	 * Sensor capture operation main loop.
	 *
	 * Stages are out-of-order relative to a single frame's processing, but
	 * in-order in time.
	 */

	/**
	 * Stage 1: Read in latest control parameters
	 */
	uint64_t exposureDuration;
	uint64_t frameDuration;
	uint32_t gain;
	Buffers *nextBuffers;
	uint32_t frameNumber;
	SensorListener *listener = NULL;
	{
		Mutex::Autolock lock(mControlMutex);
		exposureDuration = mExposureTime;
		frameDuration = mFrameDuration;
		gain = mGainFactor;
		nextBuffers = mNextBuffers;
		frameNumber = mFrameNumber;
		listener = mListener;
		// Don't reuse a buffer set
		mNextBuffers = NULL;

		// Signal VSync for start of readout
		ALOGVV("Sensor VSync");
		mGotVSync = true;
		mVSync.signal();
	}

	/**
	 * Stage 3: Read out latest captured image
	 */

	Buffers *capturedBuffers = NULL;
	nsecs_t captureTime = 0;

	nsecs_t startRealTime = systemTime();
	// Stagefright cares about system time for timestamps, so base simulated
	// time on that.
	nsecs_t simulatedTime = startRealTime;
	nsecs_t frameEndRealTime = startRealTime + frameDuration;

	if (mNextCapturedBuffers != NULL) {
		ALOGVV("Sensor starting readout");
		// Pretend we're doing readout now; will signal once enough time
		// has elapsed
		capturedBuffers = mNextCapturedBuffers;
		captureTime = mNextCaptureTime;
	}
	simulatedTime += mRowReadoutTime + kMinVerticalBlank;

	// TODO: Move this signal to another thread to simulate readout
	// time properly
	if (capturedBuffers != NULL) {
		ALOGVV("Sensor readout complete");
		Mutex::Autolock lock(mReadoutMutex);
		if (mCapturedBuffers != NULL) {
			ALOGV("Waiting for readout thread to catch up!");
			mReadoutComplete.wait(mReadoutMutex);
		}

		mCapturedBuffers = capturedBuffers;
		mCaptureTime = captureTime;
		mReadoutAvailable.signal();
		capturedBuffers = NULL;
	}

	/**
	 * Stage 2: Capture new image
	 */
	mNextCaptureTime = simulatedTime;
	mNextCapturedBuffers = nextBuffers;

	if (mNextCapturedBuffers != NULL) {
		if (listener != NULL) {
			listener->onSensorEvent(frameNumber,
						SensorListener::EXPOSURE_START,
						mNextCaptureTime);
		}
		ALOGVV("Starting next capture: Exposure: %f ms, gain: %d",
		       (float)exposureDuration / 1e6, gain);
		mScene.setExposureDuration((float)exposureDuration / 1e9);
		mScene.calculateScene(mNextCaptureTime);

		// Might be adding more buffers, so size isn't constant
		for (size_t i = 0; i < mNextCapturedBuffers->size(); i++) {
			const StreamBuffer &b = (*mNextCapturedBuffers)[i];
			ALOGVV("Sensor capturing buffer %zu: stream %d,"
			       " %d x %d, format %x, stride %d, buf %p, img %p",
			       i, b.streamId, b.width, b.height, b.format,
			       b.stride, b.buffer, b.img);
			switch (b.format) {
			case HAL_PIXEL_FORMAT_RAW16:
				captureRaw(b.img, gain, b.stride);
				break;
			case HAL_PIXEL_FORMAT_RGB_888:
				captureRGB(b.img, gain, b.width, b.height);
				break;
			case HAL_PIXEL_FORMAT_RGBA_8888:
				captureRGBA(b.img, gain, b.width, b.height);
				break;
			case HAL_PIXEL_FORMAT_BLOB:
				if (b.dataSpace != HAL_DATASPACE_DEPTH) {
					// Add auxillary buffer of the right
					// size Assumes only one BLOB (JPEG)
					// buffer in mNextCapturedBuffers
					StreamBuffer bAux;
					bAux.streamId = 0;
					bAux.width = b.width;
					bAux.height = b.height;
					bAux.format =
					    HAL_PIXEL_FORMAT_RGBA_8888; // HAL_PIXEL_FORMAT_YCbCr_420_888;
					bAux.stride = b.width;
					bAux.buffer = NULL;
					// TODO: Reuse these
					bAux.img =
					    new uint8_t[b.width * b.height * 3];
					mNextCapturedBuffers->push_back(bAux);
				} else {
					captureDepthCloud(b.img);
				}
				break;
			case HAL_PIXEL_FORMAT_YCbCr_420_888:
				captureNV21(b.img, gain, b.width, b.height);
				break;
			case HAL_PIXEL_FORMAT_YV12:
				// TODO:
				ALOGE("%s: Format %x is TODO", __FUNCTION__,
				      b.format);
				break;
			case HAL_PIXEL_FORMAT_Y16:
				captureDepth(b.img, gain, b.width, b.height);
				break;
			default:
				ALOGE("%s: Unknown format %x, no output",
				      __FUNCTION__, b.format);
				break;
			}
		}
	}

	ALOGVV("Sensor vertical blanking interval");
	nsecs_t workDoneRealTime = systemTime();
	const nsecs_t timeAccuracy = 2e6; // 2 ms of imprecision is ok
	if (workDoneRealTime < frameEndRealTime - timeAccuracy) {
		timespec t;
		t.tv_sec = (frameEndRealTime - workDoneRealTime) / 1000000000L;
		t.tv_nsec = (frameEndRealTime - workDoneRealTime) % 1000000000L;

		int ret;
		do {
			ret = nanosleep(&t, &t);
		} while (ret != 0);
	}
	ALOGVV("Frame cycle took %d ms, target %d ms",
	       (int)((systemTime() - startRealTime) / 1000000),
	       (int)(frameDuration / 1000000));
	return true;
};

void Sensor::captureRaw(uint8_t *img, uint32_t gain, uint32_t stride)
{
	ALOGVV("%s", __FUNCTION__);
	float totalGain = gain / 100.0 * kBaseGainFactor;
	float noiseVarGain = totalGain * totalGain;
	float readNoiseVar =
	    kReadNoiseVarBeforeGain * noiseVarGain + kReadNoiseVarAfterGain;

	int bayerSelect[4] = {Scene::R, Scene::Gr, Scene::Gb, Scene::B}; // RGGB
	mScene.setReadoutPixel(0, 0);
	for (unsigned int y = 0; y < mResolution[1]; y++) {
		int *bayerRow = bayerSelect + (y & 0x1) * 2;
		uint16_t *px = (uint16_t *)img + y * stride;
		for (unsigned int x = 0; x < mResolution[0]; x++) {
			uint32_t electronCount;
			electronCount =
			    mScene.getPixelElectrons()[bayerRow[x & 0x1]];

			// TODO: Better pixel saturation curve?
			electronCount = (electronCount < kSaturationElectrons)
					    ? electronCount
					    : kSaturationElectrons;

			// TODO: Better A/D saturation curve?
			uint16_t rawCount = electronCount * totalGain;
			rawCount =
			    (rawCount < kMaxRawValue) ? rawCount : kMaxRawValue;

			// Calculate noise value
			// TODO: Use more-correct Gaussian instead of uniform
			// noise
			float photonNoiseVar = electronCount * noiseVarGain;
			float noiseStddev =
			    sqrtf_approx(readNoiseVar + photonNoiseVar);
			// Scaled to roughly match gaussian/uniform noise stddev
			float noiseSample =
			    std::rand() * (2.5 / (1.0 + RAND_MAX)) - 1.25;

			rawCount += kBlackLevel;
			rawCount += noiseStddev * noiseSample;

			*px++ = rawCount;
		}
	}
	ALOGVV("Raw sensor image captured");
}

void Sensor::captureRGBA(uint8_t *img, uint32_t gain, uint32_t width,
			 uint32_t height)
{
	ALOGVV("%s", __FUNCTION__);
	width = 640;
	height = 480;

	ClientVideoBuffer *handle = ClientVideoBuffer::getClientInstance();

	uint8_t *bufData =
	    handle->clientBuf[handle->clientUsedCount % 8].buffer;

	if (handle->clientRevCount < handle->clientUsedCount) {
		ALOGV("%s: Total Frame recv vs Total Renderred [%d:%d]",
		      __func__, handle->clientRevCount,
		      handle->clientUsedCount);
	}
	handle->clientUsedCount++;

	uint32_t w, h;
	int shift = 14, offset = 8192;
	int C0 = 22987, C1 = -11698, C2 = -5636, C3 = 29049;

	int y1, y2, u1, v1;
	const unsigned char *pY1 = &bufData[0];
	const unsigned char *pY2 = &bufData[width];
	const unsigned char *pU = &bufData[width * height];
	const unsigned char *pV = &bufData[(int)(width * height * 1.25)];

	unsigned char *pD1 = img;
	unsigned char *pD2 = img + width * 4;

	for (h = 0; h < height; h += 2) {
		for (w = 0; w < width; w += 2) {
			v1 = *pV - 128;
			pV++;
			u1 = *pU - 128;
			pU++;

			y1 = *pY1;
			y2 = *pY2;

			*pD1++ = CLAP(y1 + ((v1 * C0 + offset) >> shift)); // r
			*pD1++ = CLAP(
			    y1 + ((u1 * C2 + v1 * C1 + offset) >> shift)); // g
			*pD1++ = CLAP(y1 + ((u1 * C3 + offset) >> shift)); // b
			*pD1++ = 0xff;					   // a
			*pD2++ = CLAP(y2 + ((v1 * C0 + offset) >> shift)); // r
			*pD2++ = CLAP(
			    y2 + ((u1 * C2 + v1 * C1 + offset) >> shift)); // g
			*pD2++ = CLAP(y2 + ((u1 * C3 + offset) >> shift)); // b
			*pD2++ = 0xff;					   // a

			pY1++;
			pY2++;
			y1 = *pY1;
			y2 = *pY2;

			*pD1++ = CLAP(y1 + ((v1 * C0 + offset) >> shift)); // r
			*pD1++ = CLAP(
			    y1 + ((u1 * C2 + v1 * C1 + offset) >> shift)); // g
			*pD1++ = CLAP(y1 + ((u1 * C3 + offset) >> shift)); // b
			*pD1++ = 0xff;					   // a
			*pD2++ = CLAP(y2 + ((v1 * C0 + offset) >> shift)); // r
			*pD2++ = CLAP(
			    y2 + ((u1 * C2 + v1 * C1 + offset) >> shift)); // g
			*pD2++ = CLAP(y2 + ((u1 * C3 + offset) >> shift)); // b
			*pD2++ = 0xff;					   // a
			pY1++;
			pY2++;
		}
		pY1 += width;
		pY2 += width;
		pD1 += 4 * width;
		pD2 += 4 * width;
	}
	ALOGVV("RGBA sensor image captured");
}

void Sensor::captureRGB(uint8_t *img, uint32_t gain, uint32_t width,
			uint32_t height)
{

	float totalGain = gain / 100.0 * kBaseGainFactor;
	// In fixed-point math, calculate total scaling from electrons to 8bpp
	int scale64x = 64 * totalGain * 255 / kMaxRawValue;
	unsigned int DivH = (float)mResolution[1] / height * (0x1 << 10);
	unsigned int DivW = (float)mResolution[0] / width * (0x1 << 10);

	for (unsigned int outY = 0; outY < height; outY++) {
		unsigned int y = outY * DivH >> 10;
		uint8_t *px = img + outY * width * 3;
		mScene.setReadoutPixel(0, y);
		unsigned int lastX = 0;
		const uint32_t *pixel = mScene.getPixelElectrons();
		for (unsigned int outX = 0; outX < width; outX++) {
			uint32_t rCount, gCount, bCount;
			unsigned int x = outX * DivW >> 10;
			if (x - lastX > 0) {
				for (unsigned int k = 0; k < (x - lastX); k++) {
					pixel = mScene.getPixelElectrons();
				}
			}
			lastX = x;
			// TODO: Perfect demosaicing is a cheat
			rCount =
			    (pixel[Scene::R] + (outX + outY) % 64) * scale64x;
			gCount =
			    (pixel[Scene::Gr] + (outX + outY) % 64) * scale64x;
			bCount =
			    (pixel[Scene::B] + (outX + outY) % 64) * scale64x;

			*px++ = rCount < 255 * 64 ? rCount / 64 : 255;
			*px++ = gCount < 255 * 64 ? gCount / 64 : 255;
			*px++ = bCount < 255 * 64 ? bCount / 64 : 255;
		}
	}
	ALOGVV("RGB sensor image captured");
}

void Sensor::saveNV21(uint8_t *img, uint32_t size)
{
	ALOGVV("%s", __FUNCTION__);

	FILE *f;
	f = fopen("/data/local/tmp/savenv21.nv21", "wb");
	if (NULL == f) {
		ALOGVV("%s:%d Fail to open /data/local/tmp/savenv21.nv21.",
		       __func__, __LINE__);
		return;
	}
	ALOGVV("%s:%d Start to save /data/local/tmp/savenv21.nv21.", __func__,
	       __LINE__);
	for (uint32_t i = 0; i < size; i++) {
		fwrite(img + i, 1, 1, f);
	}
	ALOGVV("%s:%d Finish to save /data/local/tmp/savenv21.nv21.", __func__,
	       __LINE__);
	fclose(f);
}

void Sensor::captureNV21(uint8_t *img, uint32_t gain, uint32_t width,
			 uint32_t height)
{
	ALOGVV("%s", __FUNCTION__);

	ClientVideoBuffer *handle = ClientVideoBuffer::getClientInstance();

	uint8_t *bufData =
	    handle->clientBuf[handle->clientUsedCount % 8].buffer;
	if (handle->clientRevCount < handle->clientUsedCount) {
		ALOGV("%s: Total Frame recv vs Total Renderred [%d:%d]",
		      __func__, handle->clientRevCount,
		      handle->clientUsedCount);
	}

	width = 640;
	height = 480;
	// Code to convert i420 to NV21
	uint32_t ySize = width * height;
	uint32_t totalSize = width * height * 3 / 2;
	uint32_t i420UIndex = ySize;
	uint32_t i420VIndex = ySize * 5 / 4;
	// Copy y
	memcpy(img, bufData, ySize);
	// Duplicate uv
	for (uint32_t uvIndex = ySize; uvIndex < totalSize; uvIndex += 2) {
		*(img + uvIndex + 1) = *(bufData + i420UIndex++);
		*(img + uvIndex) = *(bufData + i420VIndex++);
	}

	// capture try end
	ALOGV("NV21 sensor image captured");
	if (debug_picture_take) {
		saveNV21(img, width * height * 3);
	}
}

void Sensor::captureDepth(uint8_t *img, uint32_t gain, uint32_t width,
			  uint32_t height)
{
	ALOGVV("%s", __FUNCTION__);

	float totalGain = gain / 100.0 * kBaseGainFactor;
	// In fixed-point math, calculate scaling factor to 13bpp millimeters
	int scale64x = 64 * totalGain * 8191 / kMaxRawValue;
	unsigned int DivH = (float)mResolution[1] / height * (0x1 << 10);
	unsigned int DivW = (float)mResolution[0] / width * (0x1 << 10);

	for (unsigned int outY = 0; outY < height; outY++) {
		unsigned int y = outY * DivH >> 10;
		uint16_t *px = ((uint16_t *)img) + outY * width;
		mScene.setReadoutPixel(0, y);
		unsigned int lastX = 0;
		const uint32_t *pixel = mScene.getPixelElectrons();
		for (unsigned int outX = 0; outX < width; outX++) {
			uint32_t depthCount;
			unsigned int x = outX * DivW >> 10;
			if (x - lastX > 0) {
				for (unsigned int k = 0; k < (x - lastX); k++) {
					pixel = mScene.getPixelElectrons();
				}
			}
			lastX = x;
			depthCount = pixel[Scene::Gr] * scale64x;
			*px++ = depthCount < 8191 * 64 ? depthCount / 64 : 0;
		}
		// TODO: Handle this better
		// simulatedTime += mRowReadoutTime;
	}
	ALOGVV("Depth sensor image captured");
}

void Sensor::captureDepthCloud(uint8_t *img)
{
	ALOGVV("%s", __FUNCTION__);

	android_depth_points *cloud =
	    reinterpret_cast<android_depth_points *>(img);

	cloud->num_points = 16;

	// TODO: Create point cloud values that match RGB scene
	const int FLOATS_PER_POINT = 4;
	const float JITTER_STDDEV = 0.1f;
	for (size_t y = 0, i = 0; y < 4; y++) {
		for (size_t x = 0; x < 4; x++, i++) {
			float randSampleX =
			    std::rand() * (2.5f / (1.0f + RAND_MAX)) - 1.25f;
			randSampleX *= JITTER_STDDEV;

			float randSampleY =
			    std::rand() * (2.5f / (1.0f + RAND_MAX)) - 1.25f;
			randSampleY *= JITTER_STDDEV;

			float randSampleZ =
			    std::rand() * (2.5f / (1.0f + RAND_MAX)) - 1.25f;
			randSampleZ *= JITTER_STDDEV;

			cloud->xyzc_points[i * FLOATS_PER_POINT + 0] =
			    x - 1.5f + randSampleX;
			cloud->xyzc_points[i * FLOATS_PER_POINT + 1] =
			    y - 1.5f + randSampleY;
			cloud->xyzc_points[i * FLOATS_PER_POINT + 2] =
			    3.f + randSampleZ;
			cloud->xyzc_points[i * FLOATS_PER_POINT + 3] = 0.8f;
		}
	}

	ALOGVV("Depth point cloud captured");
}

} // namespace android
