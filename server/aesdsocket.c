/**
 * aesdsocket.c
 *
 * This program implements a TCP server that:
 * - Listens on port 9000 for incoming client connections.
 * - Accepts one client connection at a time.
 * - Receives newline-terminated data packets from clients and appends them
 *   to the file /var/tmp/aesdsocketdata.
 * - After receiving each complete packet, sends the full file contents
 *   back to the client.
 * - Logs client connections, disconnections, and errors to syslog.
 * - Supports daemon mode using the "-d" argument.
 * - Cleans up sockets and temporary files when SIGINT or SIGTERM is received.
 */

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
#include <sys/stat.h> 

#define PORT 9000
#define DATAFILE "/var/tmp/aesdsocketdata"

// Global variables for signal handling and socket management
volatile sig_atomic_t exit_requested = 0;  // Flag set by signal handler
int global_sockfd = -1;                    // Listening socket descriptor
int global_clientfd = -1;                  // Current connected client descriptor

/**
 * signal_handler
 * ----------------
 * Triggered by SIGINT or SIGTERM.
 * Sets the exit flag and closes any open sockets.
 */
void signal_handler(int signo) {
    exit_requested = 1;

    if (global_clientfd != -1) {
        close(global_clientfd);
        global_clientfd = -1;
    }

    if (global_sockfd != -1) {
        close(global_sockfd);
        global_sockfd = -1;
    }

    syslog(LOG_INFO, "Caught signal %d, exiting", signo);
}

/**
 * open_socket
 * -----------
 * Creates a TCP socket, binds it to PORT, and starts listening.
 *
 * Returns:
 *   Socket descriptor on success, -1 on failure.
 */
int open_socket(void) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0); // TCP IPv4 socket
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    // Allow quick reuse of the port after program restart
    int optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;          // IPv4
    serv_addr.sin_addr.s_addr = INADDR_ANY;  // Bind to all interfaces
    serv_addr.sin_port = htons(PORT);        // Port 9000

    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, 10) < 0) {
        perror("listen");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/**
 * daemonize
 * ---------
 * Converts the current process into a daemon:
 * - Forks twice to detach from the controlling terminal.
 * - Creates a new session.
 * - Redirects stdin, stdout, stderr to /dev/null.
 */
void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS); // Parent exits

    if (setsid() < 0) exit(EXIT_FAILURE);

    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS); // Parent exits

    umask(0);
    chdir("/");

    // Redirect standard file descriptors
    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
}

/**
 * handle_client
 * -------------
 * Receives data from a connected client, processes newline-terminated packets,
 * appends them to the data file, and sends the full file contents back to the client.
 */
void handle_client(int clientfd, struct sockaddr_in *cli_addr) {
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cli_addr->sin_addr, client_ip, sizeof(client_ip));
    syslog(LOG_INFO, "Accepted connection from %s", client_ip);

    // Open the data file in append mode
    FILE *fp = fopen(DATAFILE, "a");
    if (!fp) {
        syslog(LOG_ERR, "Failed to open %s", DATAFILE);
        close(clientfd);
        return;
    }

    size_t bufsize = 1024;
    char *recvbuf = malloc(bufsize); // Buffer to accumulate packet data
    if (!recvbuf) {
        syslog(LOG_ERR, "malloc failed");
        fclose(fp);
        close(clientfd);
        return;
    }

    size_t datalen = 0;   // Number of bytes currently in recvbuf
    char temp[512];       // Temporary buffer for recv()
    ssize_t n;

    while (!exit_requested && (n = recv(clientfd, temp, sizeof(temp), 0)) > 0) {
        size_t temp_offset = 0;

        while (temp_offset < n) {
            // Resize recvbuf if needed
            if (datalen + (n - temp_offset) >= bufsize) {
                size_t newsize = bufsize * 2;
                while (datalen + (n - temp_offset) >= newsize)
                    newsize *= 2;
                char *newbuf = realloc(recvbuf, newsize);
                if (!newbuf) {
                    syslog(LOG_ERR, "realloc failed");
                    free(recvbuf);
                    fclose(fp);
                    close(clientfd);
                    return;
                }
                recvbuf = newbuf;
                bufsize = newsize;
            }

            // Copy data from temp to recvbuf
            size_t copylen = n - temp_offset;
            memcpy(recvbuf + datalen, temp + temp_offset, copylen);
            datalen += copylen;
            temp_offset += copylen;

            // Process complete packets in recvbuf
            size_t start = 0;
            for (size_t i = 0; i < datalen; ++i) {
                if (recvbuf[i] == '\n') {
                    size_t pktlen = i - start + 1;

                    // Append packet to data file
                    fwrite(recvbuf + start, 1, pktlen, fp);
                    fflush(fp);

                    // Send the full file contents back to the client
                    FILE *fp2 = fopen(DATAFILE, "r");
                    if (!fp2) {
                        syslog(LOG_ERR, "Failed to open %s for reading", DATAFILE);
                        free(recvbuf);
                        fclose(fp);
                        close(clientfd);
                        return;
                    }

                    char sendbuf[1024];
                    ssize_t nn;
                    while ((nn = fread(sendbuf, 1, sizeof(sendbuf), fp2)) > 0) {
                        if (send(clientfd, sendbuf, nn, 0) < 0) {
                            syslog(LOG_ERR, "Failed to send data to client");
                            break;
                        }
                    }
                    fclose(fp2);

                    start = i + 1; // Move start past the processed packet
                }
            }

            // Shift leftover data to the start of the buffer for next recv()
            if (start > 0 && start < datalen) {
                memmove(recvbuf, recvbuf + start, datalen - start);
                datalen -= start;
            } else if (start == datalen) {
                datalen = 0;
            }
        }
    }

    free(recvbuf);
    fclose(fp);
    close(clientfd);
    syslog(LOG_INFO, "Closed connection from %s", client_ip);
}

/**
 * listen_socket
 * -------------
 * Main server loop: accepts incoming connections and handles clients.
 */
int listen_socket(int sockfd) {
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    while (!exit_requested) {
        struct sockaddr_in cli_addr;
        socklen_t clilen = sizeof(cli_addr);

        int clientfd = accept(sockfd, (struct sockaddr *)&cli_addr, &clilen);
        if (clientfd < 0) {
            if (exit_requested) break;
            perror("accept");
            continue;
        }

        global_clientfd = clientfd;
        handle_client(clientfd, &cli_addr);
        global_clientfd = -1;
    }

    closelog();
    return 0;
}

/**
 * main
 * ----
 * Entry point: parses arguments, sets up signals, opens the socket,
 * optionally daemonizes, and starts listening for clients.
 */
int main(int argc, char *argv[]) {
    int daemon_mode = 0;

    // Parse -d argument for daemon mode
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }

    // Register signal handlers for SIGINT/SIGTERM
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Open listening socket
    int sockfd = open_socket();
    if (sockfd < 0) {
        fprintf(stderr, "Failed to open socket\n");
        return 1;
    }
    global_sockfd = sockfd;

    // Run as daemon if requested
    if (daemon_mode) {
        daemonize();
    } else {
        printf("Socket opened successfully on port %d\n", PORT);
    }

    // Start accepting and handling clients
    listen_socket(sockfd);

    // Cleanup on exit
    if (global_sockfd != -1) close(global_sockfd);
    remove(DATAFILE);

    return 0;
}

