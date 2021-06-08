#include <arpa/inet.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include "audio.h"
#include <sys/mman.h>

enum
{
    CMD_OPEN = 0,
    CMD_CLOSE = 1,
    CMD_DATA = 2
};

struct audio_socket_configuration_info
{
    uint32_t sample_rate;
    uint32_t channel_mask;
    uint32_t format;
    uint32_t frame_count;
};

struct audio_socket_info
{
    uint32_t cmd;
    union {
        struct audio_socket_configuration_info asci;
        uint32_t data_size;
        uint32_t offset;
    };
};

typedef struct
{
    int sock_client_fd;
    int loop_exit; // Control receive_server_command_thread
    char pcm_file[64];
    int container_id;
    struct audio_socket_configuration_info asci;
    pthread_t audio_thread_id; //Audio in data thread
    int loop_quit;             // Control audio_in_thread
    int enable_socket_connect_log;
    int data_block;
} ClientAudioInSocketInfo;

static ClientAudioInSocketInfo _caisi[1];
void *receive_server_command_thread(void *args);
static int connect_audio_out_server(ClientAudioInSocketInfo *caisi);
void *audio_in_thread(void *args);
static const char *k_sock_init = "./workdir/ipc/audio-in-sock";
static char *k_sock = "";
int share_fd = -1;
void* share_buffer = NULL;
int share_buffer_size = 0;
ssize_t buffer_size = 0;
void *buffer = NULL;
FILE *file_pcm = NULL;

char *const short_options = "hs:i:c:d";
struct option long_options[] = {
    {"help", 1, NULL, 'h'},
    {"instance-id", 1, NULL, 'i'},
    {"pcm-file", 1, NULL, 'c'},
    {"data-block", 1, NULL, 'd'},
    {0, 0, 0, 0},
};
ssize_t receive_msg(int fd, void *ptr, size_t nbytes, int *recvfd) {
    struct msghdr   msg;
    struct iovec    iov[1];
    ssize_t  res;
    union {
      struct cmsghdr    cm;
      char       control[CMSG_SPACE(sizeof(int))];
    } control_un;
    struct cmsghdr  *cmptr;
    msg.msg_control = control_un.control;
    msg.msg_controllen = sizeof(control_un.control);
    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    iov[0].iov_base = ptr;
    iov[0].iov_len = nbytes;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    if ( (res = recvmsg(fd, &msg, 0)) <= 0) {
        if (res < 0) {
          printf("recvmsg: error: %s\n", strerror(errno));
        }
        return res;
    }
    if ( (cmptr = CMSG_FIRSTHDR(&msg)) != NULL &&
        cmptr->cmsg_len == CMSG_LEN(sizeof(int))) {
        if (cmptr->cmsg_level != SOL_SOCKET)
             printf("control level != SOL_SOCKET\n");
        if (cmptr->cmsg_type != SCM_RIGHTS)
             printf("control type != SCM_RIGHTS\n");
        *recvfd = *((int *) CMSG_DATA(cmptr));
        share_buffer_size = *(int *)ptr;
        printf("recvmsg: share buffer size: %zu, share_fd = %d, nbytes = %zu, ptr = %d\n", res, share_fd, nbytes, share_buffer_size);
    }
    return res;
}

int Read(int mSockAudio, uint8_t* buf, int len) {
    int left = len;
    int share_fd_prev = share_fd;
    while (left > 0) {
        int size = receive_msg(mSockAudio, buf, left, &share_fd);
        if (share_fd_prev != share_fd) {
            return size;
        }
        if (size <= 0) {
            printf("audio socket server is closed or has some error\n");
            return -1;
        }

        if (size > 0) {
            buf += size;
            left -= size;
        }
    }
    return (len - left);
}

int Parse(struct audio_socket_info asi) {
    ssize_t ret = 0;
    if (asi.cmd == CMD_OPEN) {
        printf("%s: receive CMD_OPEN\n", __func__);
        _caisi->asci.channel_mask = asi.asci.channel_mask;
        _caisi->asci.format = asi.asci.format;
        _caisi->asci.frame_count = asi.asci.frame_count;
        _caisi->asci.sample_rate = asi.asci.sample_rate;
        printf("%s: _caisi->asci.channel_mask: %d _caisi->asci.format: %d _caisi->asci.frame_count: %d _caisi->asci.sample_rate: %d\n",
                __func__, _caisi->asci.channel_mask, _caisi->asci.format, _caisi->asci.frame_count, _caisi->asci.sample_rate);
        buffer_size = _caisi->asci.frame_count * audio_channel_count_from_out_mask(_caisi->asci.channel_mask) * audio_bytes_per_sample(_caisi->asci.format);
        do
        {
            buffer = malloc(buffer_size);
        } while (!buffer && _caisi->loop_exit == 0);
        if (!buffer)
        {
            printf("%s: buffer is NULL. Quit\n", __func__);
        }
        else
        {
            memset(buffer, 0, buffer_size);
        }

        file_pcm = fopen(_caisi->pcm_file, "rb");
        if (!file_pcm)
        {
            printf("%s:%d Fail to open %s. %s\n", __func__, __LINE__, _caisi->pcm_file, strerror(errno));
        }

    }
    else if (asi.cmd == CMD_CLOSE)
    {
        printf("%s: receive CMD_CLOSE\n", __func__);
        _caisi->loop_quit = 1;
        shutdown(_caisi->sock_client_fd, SHUT_RDWR);
        close(_caisi->sock_client_fd);
        _caisi->sock_client_fd = -1;
        if (share_buffer != NULL && share_buffer_size != 0 && munmap(share_buffer, share_buffer_size) < 0)
        {
            printf("Could not unmap %s", strerror(errno));
        }
        printf("%s: Success to munmap share_buffer\n", __func__);
        share_buffer = NULL;
        share_buffer_size = 0;
        share_fd = -1;
    }
    else if (asi.cmd == CMD_DATA)
    {
        if (asi.offset >= 0)
        {
            printf("%s get CMD_DATA offset %d \n", __func__, asi.offset);
            if (share_buffer != NULL)
            {
                if (feof(file_pcm))
                {
                    fclose(file_pcm);
                    file_pcm = fopen(_caisi->pcm_file, "rb");
                    if (!file_pcm)
                    {
                        printf("%s:%d Fail to open %s. %s\n", __func__, __LINE__, _caisi->pcm_file, strerror(errno));
                    }
                }
                if (_caisi->data_block)
                {
                    printf("%s emulate data block. Donnot send data. Sleep 10 ms.\n", __func__);
                    usleep(10 * 1000);
                }
                else
                {
                    printf("read audio in data from the client device. buffer_size: %zd\n", buffer_size);
                    ret = fread(buffer, buffer_size, 1, file_pcm);
                    if (ret < 0)
                    {
                        printf("%s: %d Fail to read %s. %s\n", __func__, __LINE__, _caisi->pcm_file, strerror(errno));
                    }
                    if (ret > 0)
                    {
                        memcpy((uint8_t*)share_buffer + asi.offset, buffer, buffer_size);
                    }
                }
            }
        }
    } else {
        printf("Unknown audio command %d\n", asi.cmd);
        return -1;
    }
    return 1;
}

int main(int argc, char *argv[])
{
    int c;
    int index = 0;
    char *p_opt_arg = NULL;
    char str[1024];
    pthread_t accept_thread_id; //accept server command thread id
    int accept_thread_flag = 0;

    printf("Set default value:\n");
    strncpy(_caisi->pcm_file, "./yrzr_8000_mono.pcm", sizeof(_caisi->pcm_file));
    _caisi->data_block = 0;
    _caisi->container_id = 0;
    while ((c = getopt_long(argc, argv, short_options, long_options, &index)) != -1)
    {
        switch (c)
        {
        case 'h':
            printf("%s\t-h, --help help\n"
                   "\t-c, --pcm-file\n"
                   "\t-d, --data-block\n"
                   "\t-i, --instance-id\n",
                   argv[0]);
            break;
        case 'i':
            p_opt_arg = optarg;
            _caisi->container_id = atoi(p_opt_arg);
            printf("Set _caisi->container_id to %d\n", _caisi->container_id);
            break;
        case 'c':
            p_opt_arg = optarg;
            strncpy(_caisi->pcm_file, p_opt_arg, sizeof(_caisi->pcm_file));
            printf("Set _caisi->pcm_file to %s\n", _caisi->pcm_file);
            break;
        case 'd':
            _caisi->data_block = 1;
            printf("Enable _caisi->data_block\n");
            break;
        default:
            printf("Enock: c = %c, index =%d \n", c, index);
        }
    }
    char k_sock_id[30];
    sprintf(k_sock_id, "%s%d", k_sock_init, _caisi->container_id );
    k_sock = (char*)malloc(sizeof(char)*(sizeof(k_sock_id)+1));
    strncpy(k_sock, k_sock_id,sizeof(k_sock_id));

    printf("%s k_sock = %s _caisi->pcm_file: %s "
           "_caisi->data_block = %d\n",
           __func__, k_sock, _caisi->pcm_file, _caisi->data_block);

    while (1)
    {
        memset(str, '\0', sizeof(str));
        printf("%s Please input comand('quit' for quit):", __func__);
        scanf("%s", str);
        if (strcmp(str, "quit") == 0)
        {
            printf("%s quit\n", __func__);
            break;
        }
        printf("%s The command is : %s\n\n", __func__, str);
        if (strcmp(str, "start") == 0)
        {
            printf("%s start\n", __func__);
            if (!accept_thread_flag)
            {
                _caisi->loop_exit = 0;
                pthread_create(&accept_thread_id, NULL, receive_server_command_thread, _caisi);
                accept_thread_flag = 1;
            }
            else
            {
                printf("%s Accept server command thread is already running\n", __func__);
            }
        }
        if (strcmp(str, "stop") == 0)
        {
            printf("%s stop\n", __func__);
            if (accept_thread_flag)
            {
                _caisi->loop_exit = 1;
                _caisi->loop_quit = 1;
                shutdown(_caisi->sock_client_fd, SHUT_RDWR);
                close(_caisi->sock_client_fd);
                _caisi->sock_client_fd = -1;
                pthread_join(accept_thread_id, NULL);
                accept_thread_flag = 0;
            };
        }
        if (strcmp(str, "dbe") == 0)
        {
            printf("%s dbe(data_block enable)\n", __func__);
            _caisi->data_block = 1;
        }
        if (strcmp(str, "dbd") == 0)
        {
            printf("%s dbd(data_block disenable)\n", __func__);
            _caisi->data_block = 0;
        }
    }
    return 0;
}

void *receive_server_command_thread(void *args)
{
    ClientAudioInSocketInfo *caisi = (ClientAudioInSocketInfo *)args;
    caisi->sock_client_fd = -1;
    struct audio_socket_info asi;
    ssize_t len;
    while (caisi->loop_exit == 0) // if loop_exit is 1, quit.
    {
        caisi->enable_socket_connect_log = 0;

        printf("\n%s Start to connect the audio in socket server... caisi->enable_socket_connect_log = %d\n", __func__, caisi->enable_socket_connect_log);
        while (caisi->sock_client_fd < 0 && caisi->loop_exit == 0)
        {
            if (caisi->enable_socket_connect_log == 1)
                printf("%s Try to connect the audio in socket server... Error log is close.\n", __func__);
            caisi->sock_client_fd = connect_audio_out_server(caisi);
            if (caisi->sock_client_fd < 0 && caisi->loop_exit == 0)
            {
                if (caisi->enable_socket_connect_log == 1)
                    printf("Fail to connect the audio in server(%s). Sleep 100 ms and try again. \n", k_sock);
                usleep(100 * 1000); // sleep 100ms
                continue;
            }
            else
            {
                printf("Connected to the audio in socket server(%d)\n", caisi->sock_client_fd);
            }
        }
        memset(&asi, 0, sizeof(struct audio_socket_info));
        len = sizeof(struct audio_socket_info);
        while (len > 0 && caisi->loop_exit == 0 && caisi->sock_client_fd > 0)
        {
            void* ptr = &asi;
            int res = Read(caisi->sock_client_fd, ptr, sizeof(struct audio_socket_info));
            if (res < 0)
                break;
            else if (res == sizeof(struct audio_socket_info))
            {
                res = Parse(asi);
                if (res < 0) {
                    break;
                }
            }
            else if (share_fd == -1)
            {
                printf("audio work thread, should not be here, res %d\n", res);
                break;
            }
            else
            {
                if ((share_buffer = mmap(NULL, share_buffer_size, PROT_READ |
                        PROT_WRITE, MAP_SHARED, share_fd, 0)) == (void *)-1)
                {
                     printf("Could not map %s", strerror(errno));
                     break;
                }
                printf("%s: Success to mmap share_buffer %p\n", share_buffer, __func__);
            }
        }
    }
    share_buffer = NULL;
    shutdown(caisi->sock_client_fd, SHUT_RDWR);
    close(caisi->sock_client_fd);
    caisi->sock_client_fd = -1;
    pthread_join(caisi->audio_thread_id, NULL);
    printf("%s Quit\n", __func__);
    return NULL;
}

static int connect_audio_out_server(ClientAudioInSocketInfo *caisi)
{
    struct sockaddr_un addr;
    caisi->sock_client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (caisi->sock_client_fd < 0)
    {
        if (caisi->enable_socket_connect_log == 1)
            printf("Can't create socket\n");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(&addr.sun_path[0], k_sock, strlen(k_sock));

    if (connect(caisi->sock_client_fd, (struct sockaddr *)&addr,
                sizeof(sa_family_t) + strlen(k_sock) + 1) < 0)
    {
        if (caisi->enable_socket_connect_log == 1)
            printf("Failed to connect to server socket %s error: %s\n", k_sock, strerror(errno));
        close(caisi->sock_client_fd);
        caisi->sock_client_fd = -1;
        return -1;
    }
    printf("Connect the audio in server successfully(%s).\n", k_sock);
    return caisi->sock_client_fd;
}
