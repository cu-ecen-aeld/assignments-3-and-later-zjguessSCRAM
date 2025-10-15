/**
 * aesdsocket.c
 *
 * This program implements a simple TCP server that:
 *  - Listens on port 9000 for incoming client connections.
 *  - Accepts one client connection at a time.
 *  - Receives newline-terminated data from the client and appends it
 *    to a file located at /var/tmp/aesdsocketdata.
 *  - Once a full data packet (ending with '\n') is received,
 *    sends the full contents of /var/tmp/aesdsocketdata back to the client.
 *  - Logs connection activity to syslog.
 *  - Runs indefinitely, handling one connection at a time.
 *  - Cleans up sockets and temporary files when SIGINT or SIGTERM is received.
 *  - Supports daemon mode via the "-d" command-line argument.
 */

#include <stdio.h>          // For printf, perror, FILE operations (fopen, fwrite, etc.)
#include <stdlib.h>         // For exit, malloc, free
#include <string.h>         // For memset, strlen, strcmp, strchr
#include <unistd.h>         // For close, fork, chdir, write, read
#include <sys/types.h>      // For socket, bind, accept, etc.
#include <sys/socket.h>     // For socket functions
#include <netinet/in.h>     // For sockaddr_in structure (IPv4)
#include <arpa/inet.h>      // For inet_ntop (convert binary IP to string)
#include <syslog.h>         // For syslog logging
#include <signal.h>         // For signal handling (SIGINT, SIGTERM)
#include <errno.h>          // For errno and strerror
#include <fcntl.h>          // For open(), file permissions
#include <sys/stat.h>       // For umask()

// Define constants
#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"

// Global file descriptors (so we can close them during cleanup)
static int server_fd = -1;    // The listening socket
static int client_fd = -1;    // The connected client socket

// Flag set by the signal handler when SIGINT/SIGTERM is received
static volatile sig_atomic_t caught_signal = 0;

/**
 * signal_handler()
 *
 * This handler runs when SIGINT or SIGTERM is received.
 * It simply sets a global flag that tells the main loop to exit.
 */
void signal_handler(int sig)
{
    caught_signal = 1;
}

/**
 * cleanup()
 *
 * Performs cleanup before exiting:
 *  - Closes open sockets.
 *  - Removes the /var/tmp/aesdsocketdata file.
 *  - Logs a message indicating shutdown.
 */
void cleanup(void)
{
    // Close client socket if open
    if (client_fd != -1)
        close(client_fd);

    // Close server socket if open
    if (server_fd != -1)
        close(server_fd);

    // Remove the temporary data file
    remove(DATA_FILE);

    // Log exit message
    syslog(LOG_INFO, "Caught signal, exiting");

    // Close connection to syslog
    closelog();
}

/**
 * daemonize()
 *
 * Converts the process into a daemon that runs in the background:
 *  1. Forks twice to fully detach from the controlling terminal.
 *  2. Creates a new session.
 *  3. Changes working directory to root.
 *  4. Closes standard input/output/error.
 */
void daemonize(void)
{
    pid_t pid;

    // First fork - create a background child
    pid = fork();
    if (pid < 0)
        exit(EXIT_FAILURE);
    if (pid > 0)
        exit(EXIT_SUCCESS);  // Parent exits

    // Create new session, detach from terminal
    if (setsid() < 0)
        exit(EXIT_FAILURE);

    // Second fork - prevent reacquisition of a terminal
    pid = fork();
    if (pid < 0)
        exit(EXIT_FAILURE);
    if (pid > 0)
        exit(EXIT_SUCCESS);

    // Reset file creation mask
    umask(0);

    // Change working directory to root to avoid locking directories
    chdir("/");

    // Close standard file descriptors (no terminal I/O)
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

/**
 * main()
 *
 * Entry point for aesdsocket. Handles argument parsing, socket setup,
 * connection loop, and graceful shutdown.
 */
int main(int argc, char *argv[])
{
    int daemon_mode = 0;  // Whether to run as daemon (set by -d flag)

    // Check for "-d" argument
    if (argc == 2 && strcmp(argv[1], "-d") == 0)
        daemon_mode = 1;

    // Open connection to system logger
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "aesdsocket starting");

    // Register signal handlers for clean shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // (b) Create the TCP socket (IPv4, stream socket)
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        syslog(LOG_ERR, "Socket creation failed: %s", strerror(errno));
        cleanup();
        exit(EXIT_FAILURE);
    }

    // Allow socket address reuse (avoids "Address already in use" on restart)
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Prepare the socket address structure
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;           // IPv4
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // Bind to all interfaces
    server_addr.sin_port = htons(PORT);         // Port 9000 in network byte order

    // Bind the socket to our port
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        syslog(LOG_ERR, "Bind failed: %s", strerror(errno));
        cleanup();
        exit(EXIT_FAILURE);
    }

    // (5) If "-d" was provided, run as a daemon process
    if (daemon_mode)
        daemonize();

    // Start listening for incoming connections
    if (listen(server_fd, 10) < 0) {
        syslog(LOG_ERR, "Listen failed: %s", strerror(errno));
        cleanup();
        exit(EXIT_FAILURE);
    }

    syslog(LOG_INFO, "Server listening on port %d", PORT);

    // Structures for accepting client connections
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);

    // (h) Main loop — runs until SIGINT or SIGTERM sets caught_signal
    while (!caught_signal)
    {
        // Accept a new client connection (blocking call)
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addrlen);

        // If interrupted by a signal, break out cleanly
        if (client_fd < 0) {
            if (errno == EINTR)
                break;
            syslog(LOG_ERR, "Accept failed: %s", strerror(errno));
            continue;
        }

        // Convert client IP to human-readable format
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        // Log connection establishment
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        // Open or create the data file for appending
        FILE *data_file = fopen(DATA_FILE, "a+");
        if (!data_file) {
            syslog(LOG_ERR, "File open failed: %s", strerror(errno));
            close(client_fd);
            continue;
        }

        // Buffer for receiving data from client
        char buffer[1024];
        ssize_t bytes;
        int newline_found = 0;  // Flag to detect end of a message

        // Receive data from the client until newline or disconnection
        while ((bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0)) > 0) {
            buffer[bytes] = '\0';               // Null-terminate for safety
            fwrite(buffer, sizeof(char), bytes, data_file); // Append to file
            fflush(data_file);                  // Ensure immediate write to disk

            // Check if message ended with newline
            if (strchr(buffer, '\n')) {
                newline_found = 1;
                break;
            }
        }

        // (f) If a complete message (newline) was received,
        // send back the entire contents of the data file
        if (newline_found) {
            fseek(data_file, 0, SEEK_SET); // Rewind file to beginning

            // Read line by line and send to client
            while (fgets(buffer, sizeof(buffer), data_file) != NULL) {
                send(client_fd, buffer, strlen(buffer), 0);
            }
        }

        // Close file and client socket after handling this connection
        fclose(data_file);
        close(client_fd);
        client_fd = -1;

        // Log connection closure
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
    }

    // (i) Cleanup and exit gracefully on SIGINT/SIGTERM
    cleanup();
    return 0;
}

