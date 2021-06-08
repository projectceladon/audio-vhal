#include <media/AudioRecord.h>
#include "SocketServer.h"

#if (LOG_TAG==NULL)
#undef LOG_TAG
#define LOG_TAG "r_submix_AicAudioRecord"
#endif

#include <utils/Log.h>

using namespace android;

class AicAudioRecord {
public:
    void init();
    static AicAudioRecord* getInstance();
    int readAudio();
    ~AicAudioRecord();
private:
    std::unique_ptr<SocketServer> mSocketServer;
    FILE *g_pAudioRecordFile;
    void *inBuffer;
    int bufferSizeInBytes;
    sp<AudioRecord> mAudioRecord;
    static AicAudioRecord* instance;
    AicAudioRecord() {}
};
