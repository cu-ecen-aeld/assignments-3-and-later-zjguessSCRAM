#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <signal.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>

#define PORT 9000
#define DATAFILE "/var/tmp/aesdsocketdata"

volatile sig_atomic_t exit_requested = 0;
int global_sockfd = -1;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct thread_node {
    pthread_t tid;
    struct thread_node *next;
} thread_node_t;

thread_node_t *thread_list_head = NULL;

void signal_handler(int signo) {
    exit_requested = 1;
    if (global_sockfd != -1) close(global_sockfd);
    syslog(LOG_INFO, "Caught signal %d, exiting", signo);
}

// Add thread to linked list
void add_thread(pthread_t tid) {
    thread_node_t *node = malloc(sizeof(thread_node_t));
    node->tid = tid;
    node->next = thread_list_head;
    thread_list_head = node;
}

// Join all threads in linked list
void join_all_threads(void) {
    thread_node_t *curr = thread_list_head;
    while (curr) {
        pthread_join(curr->tid, NULL);
        thread_node_t *tmp = curr;
        curr = curr->next;
        free(tmp);
    }
}

// Timestamp thread
void *timestamp_thread(void *arg) {
    while (!exit_requested) {
        sleep(10); // 10 seconds

        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        char timestamp[128];
        strftime(timestamp, sizeof(timestamp),
                 "timestamp:%a, %d %b %Y %H:%M:%S %z\n",
                 &tm_now);

        pthread_mutex_lock(&file_mutex);
        FILE *fp = fopen(DATAFILE, "a");
        if (fp) {
            fwrite(timestamp, 1, strlen(timestamp), fp);
            fflush(fp);
            fclose(fp);
        }
        pthread_mutex_unlock(&file_mutex);
    }
    return NULL;
}

int open_socket(void) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;

    int optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, 10) < 0) {
        close(sockfd);
        return -1;
    }
    return sockfd;
}

// Client thread function
void *client_thread(void *arg) {
    int clientfd = *((int *)arg);
    free(arg);

    char temp[512];
    size_t bufsize = 1024;
    char *recvbuf = malloc(bufsize);
    size_t datalen = 0;

    while (!exit_requested) {
        ssize_t n = recv(clientfd, temp, sizeof(temp), 0);
        if (n <= 0) break;

        size_t temp_offset = 0;
        while (temp_offset < n) {
            if (datalen + (n - temp_offset) >= bufsize) {
                bufsize *= 2;
                char *newbuf = realloc(recvbuf, bufsize);
                if (!newbuf) goto cleanup;
                recvbuf = newbuf;
            }

            size_t copylen = n - temp_offset;
            memcpy(recvbuf + datalen, temp + temp_offset, copylen);
            datalen += copylen;
            temp_offset += copylen;

            size_t start = 0;
            for (size_t i = 0; i < datalen; i++) {
                if (recvbuf[i] == '\n') {
                    size_t pktlen = i - start + 1;

                    pthread_mutex_lock(&file_mutex);
                    FILE *fp = fopen(DATAFILE, "a");
                    if (fp) {
                        fwrite(recvbuf + start, 1, pktlen, fp);
                        fflush(fp);
                        fclose(fp);
                    }
                    pthread_mutex_unlock(&file_mutex);

                    start = i + 1;
                }
            }

            if (start > 0 && start < datalen) {
                memmove(recvbuf, recvbuf + start, datalen - start);
                datalen -= start;
            } else if (start == datalen) {
                datalen = 0;
            }
        }
    }

cleanup:
    free(recvbuf);
    close(clientfd);
    return NULL;
}

void listen_socket(int sockfd) {
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    pthread_t ts_tid;
    pthread_create(&ts_tid, NULL, timestamp_thread, NULL);
    add_thread(ts_tid);

    while (!exit_requested) {
        struct sockaddr_in cli_addr;
        socklen_t clilen = sizeof(cli_addr);

        int *clientfd = malloc(sizeof(int));
        *clientfd = accept(sockfd, (struct sockaddr *)&cli_addr, &clilen);
        if (*clientfd < 0) {
            free(clientfd);
            if (exit_requested) break;
            continue;
        }

        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, clientfd);
        add_thread(tid);
    }

    join_all_threads();
    closelog();
}

int main(int argc, char *argv[]) {
    int daemon_mode = (argc == 2 && strcmp(argv[1], "-d") == 0);

    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int sockfd = open_socket();
    if (sockfd < 0) return 1;
    global_sockfd = sockfd;

    if (daemon_mode) {
        if (fork() > 0) exit(EXIT_SUCCESS);
        setsid();
        if (fork() > 0) exit(EXIT_SUCCESS);
        chdir("/");
        umask(0);
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
    }

    listen_socket(sockfd);

    if (global_sockfd != -1) close(global_sockfd);
    remove(DATAFILE);
    return 0;
}

