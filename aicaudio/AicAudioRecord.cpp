#include "AicAudioRecord.h"

AicAudioRecord*  AicAudioRecord::instance = NULL;

static void AudioRecordCallback(int event, void* user, void *info) {
    if (event == android::AudioRecord::EVENT_NEW_POS) {
        //ALOGI(" EVENT_NEW_POS \n");
    }
    else if (event == android::AudioRecord::EVENT_MORE_DATA) {		
        android::AudioRecord::Buffer* pBuff = (android::AudioRecord::Buffer*)info;
        //ALOGI("pBuff->size;%d\n",pBuff->size);
    }
    else if (event == android::AudioRecord::EVENT_OVERRUN) {
        ALOGW(" EVENT_OVERRUN \n");
    }
}

static void* AudioRecordThread( void *inArg ) {
    while (true) {
        AicAudioRecord::getInstance()->readAudio();       
    }
    return NULL;
}

AicAudioRecord* AicAudioRecord::getInstance() {
    if (instance == nullptr)
        instance = new AicAudioRecord();
    return instance;
}

void AicAudioRecord::init() {
    audio_source_t inputSource = AUDIO_SOURCE_REMOTE_SUBMIX;
    audio_format_t audioFormat = AUDIO_FORMAT_PCM_16_BIT;
    audio_channel_mask_t channelConfig = AUDIO_CHANNEL_IN_STEREO;
    bufferSizeInBytes = 1600;
    int sampleRateInHz = 48000;
    int iNbChannels = 2;
    int iBytesPerSample = 2; //16bits pcm,2 Bytes
    int frameSize = 0; //frameSize = iNbChannels* iBytesPerSample
    size_t minFrameCount = 0; //get from AudioRecord Object
    int iWriteDataCount = 0;

    mSocketServer = std::unique_ptr<SocketServer>(new SocketServer());
    if (mSocketServer) {
        mSocketServer->init();
    }

    iNbChannels = (channelConfig == AUDIO_CHANNEL_IN_STEREO) ? 2 : 1;
    frameSize = iNbChannels * iBytesPerSample;
    android::status_t status = android::AudioRecord::getMinFrameCount(&minFrameCount, sampleRateInHz, audioFormat, channelConfig);
    if (status != android::NO_ERROR) {
	ALOGE("%s  AudioRecord.getMinFrameCount fail \n", __FUNCTION__);
        return;
    }

    bufferSizeInBytes = minFrameCount * frameSize;
    ALOGE("liukai: %s, %s, minFrameCount %d, frameSize %d", __FILE__, __FUNCTION__, (int)minFrameCount, frameSize);
    inBuffer = malloc(bufferSizeInBytes);
    if (inBuffer == NULL) {
        ALOGE("%s alloc mem failed \n", __FUNCTION__);
        return;
    } 
    int g_iNotificationPeriodInFrames = sampleRateInHz / 10;

    android::String16 mPackageOp("AicAudioRecord");
    mAudioRecord = new android::AudioRecord(mPackageOp);
    mAudioRecord->set(inputSource, sampleRateInHz, audioFormat, channelConfig, 0, AudioRecordCallback, NULL, 0, true, AUDIO_SESSION_ALLOCATE);
    if (mAudioRecord->initCheck() != android::NO_ERROR) {
        ALOGE("AudioRecord initCheck error!\n");
        return;         
    }

    if (mAudioRecord->setPositionUpdatePeriod(g_iNotificationPeriodInFrames) != android::NO_ERROR) {
        ALOGE("AudioRecord setPositionUpdatePeriod error!\n");
        return;
    }
   
    if (mAudioRecord->start() != android::NO_ERROR) {
        ALOGE("AudioRecord start error!\n");
	return;
    }

    size_t stack_size = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    stack_size = 1024 * 1024 * 100;
    pthread_attr_setstacksize(&attr, stack_size);

    pthread_t audioreadthread_tid = 0;
    if (pthread_create(&audioreadthread_tid, &attr, AudioRecordThread, this) != 0) {
        ALOGE("Create AudioRecordThread  Fail \n ");
        return;
    }
    ALOGI("AicAudioRecord init success\n");
}

int AicAudioRecord::readAudio() {
    int readLen = mAudioRecord->read(inBuffer, bufferSizeInBytes / 4);
    if (readLen > 0) {
        //ALOGI("readAudio::%d, buffersize:%d\n", readLen, bufferSizeInBytes);
        if (mSocketServer) {
            mSocketServer->sendAudioData(inBuffer, readLen);
        } 
    } else {
        ALOGE("AicAudioRecord::readAudio(), readLen %d, buffersize:%d\n", readLen, bufferSizeInBytes);
    }
    return readLen;
}

AicAudioRecord::~AicAudioRecord() {
   mAudioRecord->stop();
          
   if (inBuffer) {
      free(inBuffer);
      inBuffer = NULL;
   }
}
