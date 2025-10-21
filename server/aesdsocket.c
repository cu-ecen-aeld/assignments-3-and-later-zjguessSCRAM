/**
 * aesdsocket.c - Multi-threaded, thread-list-managed version
 *
 * - Multiple simultaneous client connections (thread per client)
 * - Synchronized writes to /var/tmp/aesdsocketdata using mutex
 * - Timestamp appended every 10 seconds via a timer thread
 * - Graceful shutdown on SIGINT/SIGTERM (requests threads exit and joins them)
 * - Truncates /var/tmp/aesdsocketdata at startup (required by tests)
 */

#define _POSIX_C_SOURCE 200809L

#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <syslog.h>
#include <sys/queue.h>   /* SLIST_* macros */
#include <fcntl.h>

#define PORT_NUM 9000
#define DATAFILE "/var/tmp/aesdsocketdata"
#define LISTEN_BACKLOG 10

/* Global control flags / handles */
static volatile sig_atomic_t exit_requested = 0;
static int global_listen_fd = -1;

/* Mutex used to synchronize all file writes/reads */
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Thread node structure for singly-linked list */
struct thread_data {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    bool thread_complete;
    SLIST_ENTRY(thread_data) entries;
};

/* List head */
SLIST_HEAD(thread_list, thread_data);
static struct thread_list thread_head = SLIST_HEAD_INITIALIZER(thread_head);

/* ---------- Forward declarations ---------- */
void signal_handler(int signo);
void *timestamp_thread_func(void *arg);
void *client_thread_func(void *arg);
int open_listen_socket(void);
void cleanup_completed_threads(void);
void cleanup_all_threads_and_free(void);

/* ---------- Signal handler ---------- */
void signal_handler(int signo)
{
    /* Set flag and close listening socket to unblock accept() */
    exit_requested = 1;
    if (global_listen_fd != -1) {
        close(global_listen_fd);
        global_listen_fd = -1;
    }
    syslog(LOG_INFO, "Caught signal %d, exit requested", signo);
}

/* ---------- Timestamp thread ---------- */
/* Appends "timestamp:..." every 10 seconds. Protected by file_mutex. */
void *timestamp_thread_func(void *arg)
{
    (void)arg;
    while (!exit_requested) {
        for (int i = 0; i < 10 && !exit_requested; ++i) {
            sleep(1);
        }
        if (exit_requested) break;

        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        char timestr[128];
        /* RFC-2822-like strftime format used in tests */
        strftime(timestr, sizeof(timestr), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", &tm_now);

        pthread_mutex_lock(&file_mutex);
        FILE *fp = fopen(DATAFILE, "a");
        if (fp) {
            fwrite(timestr, 1, strlen(timestr), fp);
            fflush(fp);
            fclose(fp);
        } else {
            syslog(LOG_ERR, "Timestamp: failed to open %s: %s", DATAFILE, strerror(errno));
        }
        pthread_mutex_unlock(&file_mutex);
    }
    return NULL;
}

/* ---------- Client thread ---------- */
/* Receives data until newline, appends packet(s) atomically, and sends full file back */
void *client_thread_func(void *arg)
{
    struct thread_data *tdata = (struct thread_data *)arg;
    int client_fd = tdata->client_fd;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &tdata->client_addr.sin_addr, client_ip, sizeof(client_ip));
    syslog(LOG_INFO, "Accepted connection from %s", client_ip);

    size_t buf_cap = 1024;
    char *acc = malloc(buf_cap);
    if (!acc) {
        syslog(LOG_ERR, "malloc failed");
        close(client_fd);
        tdata->thread_complete = true;
        return NULL;
    }
    size_t acc_len = 0;

    char recvbuf[512];
    ssize_t rlen;

    while (!exit_requested && (rlen = recv(client_fd, recvbuf, sizeof(recvbuf), 0)) > 0) {
        /* Accumulate */
        if (acc_len + (size_t)rlen > buf_cap) {
            size_t newcap = buf_cap * 2;
            while (acc_len + (size_t)rlen > newcap) newcap *= 2;
            char *tmp = realloc(acc, newcap);
            if (!tmp) {
                syslog(LOG_ERR, "realloc failed");
                break;
            }
            acc = tmp;
            buf_cap = newcap;
        }
        memcpy(acc + acc_len, recvbuf, rlen);
        acc_len += (size_t)rlen;

        /* Process any complete newline-terminated packets in acc */
        while (true) {
            void *nl = memchr(acc, '\n', acc_len);
            if (!nl) break;

            size_t pktlen = (char *)nl - acc + 1;

            /* Write packet and send full file back atomically with file_mutex */
            pthread_mutex_lock(&file_mutex);

            /* append packet */
            FILE *fw = fopen(DATAFILE, "a");
            if (fw) {
                size_t written = fwrite(acc, 1, pktlen, fw);
                (void)written;
                fflush(fw);
                fclose(fw);
            } else {
                syslog(LOG_ERR, "failed to open %s for append: %s", DATAFILE, strerror(errno));
            }

            /* send entire file back to client */
            FILE *fr = fopen(DATAFILE, "r");
            if (fr) {
                char sendbuf[1024];
                size_t nread;
                while ((nread = fread(sendbuf, 1, sizeof(sendbuf), fr)) > 0) {
                    ssize_t s = send(client_fd, sendbuf, (ssize_t)nread, 0);
                    if (s < 0) {
                        /* send error; we'll break out */
                        break;
                    }
                }
                fclose(fr);
            } else {
                syslog(LOG_ERR, "failed to open %s for read: %s", DATAFILE, strerror(errno));
            }

            pthread_mutex_unlock(&file_mutex);

            /* remove the packet from acc (shift remaining bytes down) */
            size_t remain = acc_len - pktlen;
            if (remain > 0) memmove(acc, acc + pktlen, remain);
            acc_len = remain;
        }
    }

    if (rlen == -1) {
        syslog(LOG_ERR, "recv error from %s: %s", client_ip, strerror(errno));
    }

    free(acc);
    close(client_fd);
    syslog(LOG_INFO, "Closed connection from %s", client_ip);

    tdata->thread_complete = true;
    return NULL;
}

/* ---------- Open listening socket ---------- */
int open_listen_socket(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv = {0};
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;
    serv.sin_port = htons(PORT_NUM);

    if (bind(sock, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        close(sock);
        return -1;
    }
    if (listen(sock, LISTEN_BACKLOG) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

/* ---------- Cleanup helper: join + remove completed threads ---------- */
void cleanup_completed_threads(void)
{
    struct thread_data *iter = SLIST_FIRST(&thread_head);
    struct thread_data *next;

    while (iter != NULL) {
        next = SLIST_NEXT(iter, entries);
        if (iter->thread_complete) {
            /* join and remove */
            pthread_join(iter->thread_id, NULL);
            SLIST_REMOVE(&thread_head, iter, thread_data, entries);
            free(iter);
            /* after removal, continue with next (already stored) */
        }
        iter = next;
    }
}

/* ---------- Cleanup all threads (used on shutdown) ---------- */
void cleanup_all_threads_and_free(void)
{
    struct thread_data *iter = SLIST_FIRST(&thread_head);
    struct thread_data *next;

    while (iter != NULL) {
        next = SLIST_NEXT(iter, entries);
        pthread_join(iter->thread_id, NULL);
        free(iter);
        iter = next;
    }
    SLIST_INIT(&thread_head);
}

/* ---------- main ---------- */
int main(int argc, char *argv[])
{
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    bool daemon_mode = (argc == 2 && strcmp(argv[1], "-d") == 0);

    /* Setup signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Truncate the data file at startup so tests start with an empty file */
    FILE *tf = fopen(DATAFILE, "w");
    if (tf) fclose(tf);

    /* Create listening socket */
    int listen_fd = open_listen_socket();
    if (listen_fd < 0) {
        syslog(LOG_ERR, "Failed to open listen socket: %s", strerror(errno));
        closelog();
        return 1;
    }
    global_listen_fd = listen_fd;

    /* Optionally daemonize */
    if (daemon_mode) {
        if (daemon(0, 0) < 0) {
            syslog(LOG_ERR, "daemon() failed: %s", strerror(errno));
            close(listen_fd);
            closelog();
            return 1;
        }
    }

    /* Start timestamp thread */
    pthread_t timestamp_tid;
    if (pthread_create(&timestamp_tid, NULL, timestamp_thread_func, NULL) != 0) {
        syslog(LOG_ERR, "Failed to create timestamp thread");
        close(listen_fd);
        closelog();
        return 1;
    }

    /* Accept loop */
    while (!exit_requested) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (client_fd < 0) {
            if (exit_requested) break;
            if (errno == EINTR) continue;
            /* On some errors, just continue accepting */
            continue;
        }

        /* allocate thread node */
        struct thread_data *node = malloc(sizeof(*node));
        if (!node) {
            syslog(LOG_ERR, "malloc failed for thread node");
            close(client_fd);
            continue;
        }
        node->client_fd = client_fd;
        node->client_addr = cli_addr;
        node->thread_complete = false;
        SLIST_INSERT_HEAD(&thread_head, node, entries);

        /* create thread */
        if (pthread_create(&node->thread_id, NULL, client_thread_func, node) != 0) {
            syslog(LOG_ERR, "pthread_create failed");
            SLIST_REMOVE(&thread_head, node, thread_data, entries);
            close(client_fd);
            free(node);
            continue;
        }

        /* Reclaim any finished threads */
        cleanup_completed_threads();
    }

    /* Shutdown: join remaining client threads and timestamp thread */
    cleanup_all_threads_and_free();
    pthread_join(timestamp_tid, NULL);

    /* Cleanup resources */
    close(listen_fd);
    remove(DATAFILE);
    closelog();
    pthread_mutex_destroy(&file_mutex);

    return 0;
}

