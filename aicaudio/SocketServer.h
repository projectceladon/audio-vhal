#ifndef _SOCKET_SERVER_H_
#define _SOCKET_SERVER_H_

#include <mutex>
#include <thread>

class SocketServer {
 public:
  SocketServer();
  ~SocketServer();

  int init();
  int sendAudioData(void* data, size_t len);

 private:
  void threadFunc();

 private:
  const char* kAudioSock = "/ipc/audio-sock";
  char kAudioSockId[50];
  static const int kMaxConnections = 2;
  int mClientFd = -1;
  std::unique_ptr<std::thread> mThread;
  // std::mutex mMutex;
  int mServerFd = -1;
  int mConnectionCount = 0;
};
#endif  //_SOCKET_SERVER_H_
