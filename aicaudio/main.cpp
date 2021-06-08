#include <unistd.h>
#include <utils/Log.h>
#include "AicAudioRecord.h"

int main()
{
    signal(SIGPIPE, SIG_IGN);
    ALOGI("starting AicAudioRecord");
    AicAudioRecord* pa = AicAudioRecord::getInstance();
    pa->init();
    while (true) {
       sleep(5);
    }
    return 0;
}
