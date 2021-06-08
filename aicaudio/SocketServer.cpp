#define LOG_NDEBUG 0
#include <utils/Log.h>
#include <cutils/properties.h> // for property_get

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include "SocketServer.h"

SocketServer::SocketServer() {}

SocketServer::~SocketServer() {
    if (mServerFd >= 0) {
        close(mServerFd);
    }
}

int SocketServer::init() {
    mThread = std::unique_ptr<std::thread>(
        new std::thread(&SocketServer::threadFunc, this));
    return 0;
}

void SocketServer::threadFunc() {
    mServerFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mServerFd < 0) {
        ALOGE("Failed to create server socket");
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    char prop_id[PROPERTY_VALUE_MAX];
    memset(prop_id, 0, sizeof(prop_id));
    property_get("ro.container.id", prop_id, "0");
    sprintf(kAudioSockId, "%s%s", kAudioSock, prop_id);
    strncpy(&addr.sun_path[0], kAudioSockId, strlen(kAudioSockId));

    unlink(kAudioSockId);
    if (bind(mServerFd, (struct sockaddr*)&addr,
           sizeof(sa_family_t) + strlen(kAudioSockId) + 1) < 0) {
        ALOGE("Failed to bind server socket address");
        return;
    }
    else 
        ALOGE("Succeed to bind server socket address");

    // TODO: use group access only for security
    struct stat st;
    __mode_t mod = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
    if (fstat(mServerFd, &st) == 0) {
        mod |= st.st_mode;
    }
    chmod(kAudioSockId, mod);

    if (listen(mServerFd, 1) < 0) {
        ALOGE("Failed to listen on server socket");
        return;
    }
    else
        ALOGE("Succeed to listen on server socket");

    while (true) {
        struct sockaddr_un addr;
        socklen_t sockLen;

        mClientFd = accept(mServerFd, (struct sockaddr*)&addr, &sockLen);
        if (mClientFd < 0) {
            ALOGE("liukai: Failed to accept client connection, errno is %d", errno);
            perror("Failed to accept client connection");
            continue;
        }
        else {
            ALOGE("liukai: Succeed to accept client connection");
        }
    }
}

int SocketServer::sendAudioData(void* data, size_t len) {
    if (mClientFd < 0) {
        //ALOGE("Failed to accept client connection when sendAudioData");
        return -1;
    } else {
        //ALOGE("SocketServer::sendAudioData(), sending audio data");
        ssize_t length = 0;
        length = send(mClientFd, data, len, 0);
        if (length <= 0) {
            close(mClientFd);
            mClientFd = -1;
        }
        return 0;
    }
}
