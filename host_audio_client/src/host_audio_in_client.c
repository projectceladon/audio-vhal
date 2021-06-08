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
    };
};

typedef struct
{
    int sock_client_fd;
    int loop_exit; // Control receive_server_command_thread
    char server_ip[20];
    int port;
    char pcm_file[64];
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

char *const short_options = "hs:p:c:d";
struct option long_options[] = {
    {"help", 1, NULL, 'h'},
    {"server-ip", 1, NULL, 's'},
    {"port", 1, NULL, 'p'},
    {"pcm-file", 1, NULL, 'c'},
    {"data-block", 1, NULL, 'd'},
    {0, 0, 0, 0},
};

int main(int argc, char *argv[])
{
    int c;
    int index = 0;
    char *p_opt_arg = NULL;
    char str[1024];
    pthread_t accept_thread_id; //accept server command thread id
    int accept_thread_flag = 0;

    printf("Set default value:\n");
    strncpy(_caisi->server_ip, "172.100.0.2", sizeof(_caisi->server_ip));
    _caisi->port = 8767;
    strncpy(_caisi->pcm_file, "./yrzr_8000_mono.pcm", sizeof(_caisi->pcm_file));
    _caisi->data_block = 0;

    while ((c = getopt_long(argc, argv, short_options, long_options, &index)) != -1)
    {
        switch (c)
        {
        case 'h':
            printf("%s\t-h, --help help\n"
                   "\t-s, --server-ip server-ip\n"
                   "\t-p, --port\n"
                   "\t-c, --pcm-file\n"
                   "\t-d, --data-block\n",
                   argv[0]);
            break;
        case 's':
            p_opt_arg = optarg;
            strncpy(_caisi->server_ip, p_opt_arg, sizeof(_caisi->server_ip));
            printf("Set _caisi->server_ip to %s\n", _caisi->server_ip);
            break;
        case 'p':
            p_opt_arg = optarg;
            _caisi->port = atoi(p_opt_arg);
            printf("Set _caisi->port to %d\n", _caisi->port);
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
    printf("%s _caisi->server_ip = %s _caisi->port = %d _caisi->pcm_file: %s "
           "_caisi->data_block = %d\n",
           __func__, _caisi->server_ip, _caisi->port, _caisi->pcm_file, _caisi->data_block);

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

    printf("%s Quit\n", __func__);
    return 0;
}

void *receive_server_command_thread(void *args)
{
    ClientAudioInSocketInfo *caisi = (ClientAudioInSocketInfo *)args;
    caisi->sock_client_fd = -1;
    struct audio_socket_info asi;
    ssize_t ret;
    ssize_t len;
    int audio_thread_flag = 0;
    void *pointer = NULL;
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
                    printf("Fail to connect the audio in server(%s:%d). Sleep 100 ms and try again. \n", caisi->server_ip, caisi->port);
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
        pointer = &asi;
        while (len > 0 && caisi->loop_exit == 0)
        {
            do
            {
                ret = read(caisi->sock_client_fd, pointer, len);
            } while (ret < 0 && errno == EINTR && caisi->loop_exit == 0);
            if (ret == 0)
            {
                printf("%s:%d the audio in socket server may close. Close this client.\n", __func__, __LINE__);
                shutdown(caisi->sock_client_fd, SHUT_RDWR);
                close(caisi->sock_client_fd);
                caisi->sock_client_fd = -1;
                break;
            }
            if (ret > 0)
            {
                pointer += ret;
                len -= ret;
            }
        }

        if (len == 0)
        {
            if (asi.cmd == CMD_OPEN)
            {
                printf("%s the audio in thread starting on demand\n", __func__);
                caisi->asci.channel_mask = asi.asci.channel_mask;
                caisi->asci.format = asi.asci.format;
                caisi->asci.frame_count = asi.asci.frame_count;
                caisi->asci.sample_rate = asi.asci.sample_rate;
                printf("%s caisi->asci.channel_mask: %d caisi->asci.format: %d caisi->asci.frame_count: %d caisi->asci.sample_rate: %d\n",
                       __func__, caisi->asci.channel_mask, caisi->asci.format, caisi->asci.frame_count, caisi->asci.sample_rate);
                if (!audio_thread_flag)
                {
                    printf("%s receive_server_command_thread thread starting on demand\n", __func__);
                    audio_thread_flag = 1;
                    caisi->loop_quit = 0;
                    pthread_create(&caisi->audio_thread_id, NULL, audio_in_thread, caisi);
                }
            }
            else if (asi.cmd == CMD_CLOSE)
            {
                if (audio_thread_flag)
                {
                    printf("%s the audio in thread stopping on demand\n", __func__);
                    caisi->loop_quit = 1;
                    audio_thread_flag = 0;
                    shutdown(caisi->sock_client_fd, SHUT_RDWR);
                    close(caisi->sock_client_fd);
                    caisi->sock_client_fd = -1;
                }
            }
            else
            {
                printf("%s Unknown command.\n", __func__);
            }
        }
    }

    shutdown(caisi->sock_client_fd, SHUT_RDWR);
    close(caisi->sock_client_fd);
    caisi->sock_client_fd = -1;
    pthread_join(caisi->audio_thread_id, NULL);
    printf("%s Quit\n", __func__);
    return NULL;
}

static int connect_audio_out_server(ClientAudioInSocketInfo *caisi)
{
    struct sockaddr_in addr;
    caisi->sock_client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (caisi->sock_client_fd < 0)
    {
        if (caisi->enable_socket_connect_log == 1)
            printf("Can't create socket\n");
        return -1;
    }

    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(caisi->port);
    inet_pton(AF_INET, caisi->server_ip, &addr.sin_addr);

    if (connect(caisi->sock_client_fd, (struct sockaddr *)&addr,
                sizeof(struct sockaddr_in)) < 0)
    {
        if (caisi->enable_socket_connect_log == 1)
            printf("Failed to connect to server socket %s:%d error: %s\n", caisi->server_ip, caisi->port, strerror(errno));
        close(caisi->sock_client_fd);
        caisi->sock_client_fd = -1;
        return -1;
    }
    printf("Connect the audio in server successfully(%s:%d).\n", caisi->server_ip, caisi->port);
    return caisi->sock_client_fd;
}

void *audio_in_thread(void *args)
{
    ClientAudioInSocketInfo *caisi = (ClientAudioInSocketInfo *)args;
    ssize_t buffer_size = 0;
    ssize_t ret = 0;
    void *buffer = NULL;
    FILE *file_pcm = NULL;

    buffer_size = caisi->asci.frame_count * audio_channel_count_from_out_mask(caisi->asci.channel_mask) * audio_bytes_per_sample(caisi->asci.format);
    printf("%s buffer_size: %zd\n", __func__, buffer_size);
    do
    {
        buffer = malloc(buffer_size);
    } while (!buffer && caisi->loop_exit == 0);
    if (!buffer)
    {
        printf("%s buffer is NULL. Quit\n", __func__);
        return NULL;
    }
    else
    {
        memset(buffer, 0, buffer_size);
    }

    file_pcm = fopen(caisi->pcm_file, "rb");
    if (!file_pcm)
    {
        printf("%s:%d Fail to open %s. %s\n", __func__, __LINE__, caisi->pcm_file, strerror(errno));
        return NULL;
    }
    while (caisi->loop_quit == 0 && caisi->loop_exit == 0) // if loop_quit is 1, quit.
    {
        if (feof(file_pcm))
        {
            fclose(file_pcm);
            file_pcm = fopen(caisi->pcm_file, "rb");
            if (!file_pcm)
            {
                printf("%s:%d Fail to open %s. %s\n", __func__, __LINE__, caisi->pcm_file, strerror(errno));
                break;
            }
        }
        if (caisi->data_block)
        {
            printf("%s emulate data block. Donnot send data. Sleep 10 ms.\n", __func__);
            usleep(10 * 1000);
        }
        else
        {

            printf("%s FIXME: read audio in data from the client device. buffer_size: %zd\n", __func__, buffer_size);
            ret = fread(buffer, buffer_size, 1, file_pcm);
            if (ret < 0)
            {
                printf("%s: %d Fail to read %s. %s\n", __func__, __LINE__, caisi->pcm_file, strerror(errno));
                break;
            }

            if (ret > 0)
            {
                do
                {
                    ret = write(caisi->sock_client_fd, buffer, buffer_size);
                } while (ret < 0 && errno == EINTR && caisi->loop_quit == 0);
                if (ret == 0)
                {
                    printf("%s:%d the audio out socket server may close.\n", __func__, __LINE__);
                    goto FAIL;
                }
                if (ret > 0)
                {
                    printf("%s Write %zd data to the audio in server.\n", __func__, ret);
                }
            }
            usleep(10 * 1000);
        }
    }

FAIL:
    if (buffer)
    {
        free(buffer);
        buffer = NULL;
    }
    if (file_pcm)
    {
        fclose(file_pcm);
        file_pcm = NULL;
    }
    printf("%s Quit\n", __func__);
    return NULL;
}