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
    char save_pcm_file[64];
    struct audio_socket_configuration_info asci;
    int enable_socket_connect_log;
    int data_block;

} ClientAudioOutSocketInfo;

static ClientAudioOutSocketInfo _caosi[1];

char *const short_options = "hs:p:a:d";
struct option long_options[] = {
    {"help", 1, NULL, 'h'},
    {"server-ip", 1, NULL, 's'},
    {"port", 1, NULL, 'p'},
    {"save-pcm-file", 1, NULL, 'a'},
    {"data-block", 1, NULL, 'd'},
    {0, 0, 0, 0},
};

void *receive_server_command_thread(void *args);
static int connect_audio_out_server(ClientAudioOutSocketInfo *caosi);

int main(int argc, char *argv[])
{
    int c;
    int index = 0;
    char *p_opt_arg = NULL;
    char str[1024];
    pthread_t accept_thread_id; //accept server command thread id
    int accept_thread_flag = 0;

    printf("Set default value:\n");
    strncpy(_caosi->server_ip, "172.100.0.2", sizeof(_caosi->server_ip));
    _caosi->port = 8768;
    strncpy(_caosi->save_pcm_file, "audio_out.pcm", sizeof(_caosi->save_pcm_file));
    _caosi->data_block = 0;

    while ((c = getopt_long(argc, argv, short_options, long_options, &index)) != -1)
    {
        switch (c)
        {
        case 'h':
            printf("%s\t-h, --help help\n"
                   "\t-s, --server-ip server-ip\n"
                   "\t-p, --port\n"
                   "\t-a, --save-pcm-file\n",
                   argv[0]);
            break;
        case 's':
            p_opt_arg = optarg;
            strncpy(_caosi->server_ip, p_opt_arg, sizeof(_caosi->server_ip));
            printf("Set _caosi->server_ip to %s\n", _caosi->server_ip);
            break;
        case 'p':
            p_opt_arg = optarg;
            _caosi->port = atoi(p_opt_arg);
            printf("Set _caosi->port to %d\n", _caosi->port);
            break;
        case 'a':
            p_opt_arg = optarg;
            strncpy(_caosi->save_pcm_file, p_opt_arg, sizeof(_caosi->save_pcm_file));
            printf("Set _caosi->save_pcm_file to %s\n", _caosi->save_pcm_file);
            break;
        case 'd':
            _caosi->data_block = 1;
            printf("Enable _caosi->data_block\n");
            break;
        default:
            printf("Enock: c = %c, index =%d \n", c, index);
        }
    }

    printf("%s _caosi->server_ip = %s _caosi->port = %d _caosi->save_pcm_file: %s"
           "_caosi->data_block = %d\n",
           __func__, _caosi->server_ip, _caosi->port, _caosi->save_pcm_file, _caosi->data_block);

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
                _caosi->loop_exit = 0;
                pthread_create(&accept_thread_id, NULL, receive_server_command_thread, _caosi);
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
                _caosi->loop_exit = 1;
                shutdown(_caosi->sock_client_fd, SHUT_RDWR);
                close(_caosi->sock_client_fd);
                _caosi->sock_client_fd = -1;
                pthread_join(accept_thread_id, NULL);
                accept_thread_flag = 0;
            };
        }
        if (strcmp(str, "dbe") == 0)
        {
            printf("%s dbe(data_block enable)\n", __func__);
            _caosi->data_block = 1;
        }
        if (strcmp(str, "dbd") == 0)
        {
            printf("%s dbd(data_block disenable)\n", __func__);
            _caosi->data_block = 0;
        }
    }

    printf("%s Quit\n", __func__);
    return 0;
}

void *receive_server_command_thread(void *args)
{
    ClientAudioOutSocketInfo *caosi = (ClientAudioOutSocketInfo *)args;
    caosi->sock_client_fd = -1;
    size_t buffer_size = 0;
    void *buffer = NULL;
    void *pointer = NULL;
    FILE *pcm_audio_record_file = NULL;
    struct audio_socket_info asi;
    ssize_t ret;
    ssize_t len;

    pcm_audio_record_file = fopen(caosi->save_pcm_file, "w");
    if (!pcm_audio_record_file)
    {
        printf("%s:%d Fail to open %s. %s\n", __func__, __LINE__, caosi->save_pcm_file, strerror(errno));
        goto FAIL;
    }

    while (caosi->loop_exit == 0) // if loop_exit is 1, quit.
    {
        caosi->enable_socket_connect_log = 0;
        while (caosi->sock_client_fd < 0 && caosi->loop_exit == 0)
        {
            printf("\n%s Start to connect the audio out socket server... caosi->enable_socket_connect_log = %d\n", __func__, caosi->enable_socket_connect_log);
            if (caosi->enable_socket_connect_log == 1)
                printf("%s Try to connect the audio out socket server...\n", __func__);
            caosi->sock_client_fd = connect_audio_out_server(caosi);
            if (caosi->sock_client_fd < 0 && caosi->loop_exit == 0)
            {
                if (caosi->enable_socket_connect_log == 1)
                    printf("Fail to connect the audio out server(%s:%d). Sleep 100 ms and try again.\n", caosi->server_ip, caosi->port);
                usleep(100 * 1000); // sleep 100ms
                continue;
            }
            printf("Connected to the audio out socket server(%d)\n", caosi->sock_client_fd);
        }

        memset(&asi, 0, sizeof(struct audio_socket_info));
        len = sizeof(struct audio_socket_info);
        pointer = &asi;
        if (caosi->data_block)
        {
            printf("%s emulate data block. Donnot receive data. Sleep 10 ms.\n", __func__);
            usleep(10 * 1000);
        }
        else
        {
            while (len > 0 && caosi->loop_exit == 0)
            {
                do
                {
                    ret = read(caosi->sock_client_fd, pointer, len);
                } while (ret < 0 && errno == EINTR && caosi->loop_exit == 0);
                if (ret == 0)
                {
                    printf("%s:%d the audio out socket server may close.\n", __func__, __LINE__);
                    shutdown(caosi->sock_client_fd, SHUT_RDWR);
                    close(caosi->sock_client_fd);
                    caosi->sock_client_fd = -1;
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
                    printf("%s the audio out thread starting on demand\n", __func__);
                    caosi->asci.channel_mask = asi.asci.channel_mask;
                    caosi->asci.format = asi.asci.format;
                    caosi->asci.frame_count = asi.asci.frame_count;
                    caosi->asci.sample_rate = asi.asci.sample_rate;
                    printf("%s caosi->asci.channel_mask: %d caosi->asci.format: %d caosi->asci.frame_count: %d caosi->asci.sample_rate: %d\n",
                           __func__, caosi->asci.channel_mask, caosi->asci.format, caosi->asci.frame_count, caosi->asci.sample_rate);
                    buffer_size = caosi->asci.frame_count * audio_channel_count_from_out_mask(caosi->asci.channel_mask) * audio_bytes_per_sample(caosi->asci.format);
                    printf("%s buffer_size: %zd\n", __func__, buffer_size);
                    do
                    {
                        buffer = malloc(buffer_size);
                    } while (!buffer && caosi->loop_exit == 0);
                    if (!buffer)
                    {
                        printf("%s buffer is NULL. Quit\n", __func__);
                        break;
                    }
                }
                else if (asi.cmd == CMD_CLOSE)
                {
                    printf("%s the audio out thread stopping on demand\n", __func__);
                    shutdown(caosi->sock_client_fd, SHUT_RDWR);
                    close(caosi->sock_client_fd);
                    caosi->sock_client_fd = -1;
                    continue;
                }
                else if (asi.cmd == CMD_DATA)
                {
                    printf("%s asi.data_size: %d. Note that buffersize is %zd\n", __func__, asi.data_size, buffer_size);
                    if (asi.data_size > 0)
                    {

                        len = asi.data_size;
                        pointer = buffer;
                        while (len > 0 && caosi->loop_exit == 0)
                        {
                            do
                            {
                                ret = read(caosi->sock_client_fd, pointer, len);
                            } while (ret < 0 && errno == EINTR && caosi->loop_exit == 0);
                            if (ret == 0)
                            {
                                printf("%s:%d the audio out socket server may close(CMD_DATA).\n", __func__, __LINE__);
                                shutdown(caosi->sock_client_fd, SHUT_RDWR);
                                close(caosi->sock_client_fd);
                                caosi->sock_client_fd = -1;
                                break;
                            }
                            if (ret > 0)
                            {
                                printf("%s Read %zd. FIXME write the data to client device.\n", __func__, ret);
                                fwrite(pointer, 1, ret, pcm_audio_record_file);
                                fflush(pcm_audio_record_file);
                                pointer += ret;
                                len -= ret;
                            }
                        }
                        usleep(10 * 1000);
                    }
                }
                else
                {
                    printf("%s Unknown command.\n", __func__);
                }
            }
        }
    }

FAIL:
    fclose(pcm_audio_record_file);
    pcm_audio_record_file = NULL;
    free(buffer);
    buffer = NULL;
    shutdown(caosi->sock_client_fd, SHUT_RDWR);
    close(caosi->sock_client_fd);
    caosi->sock_client_fd = -1;
    printf("%s Quit\n", __func__);
    return NULL;
}

static int connect_audio_out_server(ClientAudioOutSocketInfo *caosi)
{
    struct sockaddr_in addr;
    caosi->sock_client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (caosi->sock_client_fd < 0)
    {
        if (caosi->enable_socket_connect_log == 1)
            printf("Can't create socket\n");
        return -1;
    }

    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(caosi->port);
    inet_pton(AF_INET, caosi->server_ip, &addr.sin_addr);

    if (connect(caosi->sock_client_fd, (struct sockaddr *)&addr,
                sizeof(struct sockaddr_in)) < 0)
    {
        if (caosi->enable_socket_connect_log == 1)
            printf("Failed to connect to server socket %s:%d error: %s\n", caosi->server_ip, caosi->port, strerror(errno));
        close(caosi->sock_client_fd);
        caosi->sock_client_fd = -1;
        return -1;
    }
    printf("Connect the audio out server successfully(%s:%d).\n", caosi->server_ip, caosi->port);
    return caosi->sock_client_fd;
}