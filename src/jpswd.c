#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>


#define SOCKET_PATH "/tmp/my_daemon_socket"
#define PIDFILE "/var/run/my_daemon.pid"
#define MAX_CONNECTIONS 5

static volatile int keep_running = 1;

void log_message(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

void handle_signal(int signum) {
    if (signum == SIGTERM || signum == SIGINT) {
        keep_running = 0;
    }
}

void cleanup(int unix_socket, FILE *pidfile, FILE *lockfile) {
    if (unix_socket >= 0) {
        close(unix_socket);
    }

    if (pidfile) {
        fclose(pidfile);
        unlink(PIDFILE);
    }

    if (lockfile) {
        fclose(lockfile);
        unlink("/var/run/my_daemon.pid");
    }
}

int main() {
    // Signal handling
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    // Daemonize
    pid_t pid = fork();
    if (pid < 0) {
        log_message("Fork failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (pid > 0) {
        // Parent process (original)
        exit(EXIT_SUCCESS);
    }

    // Child process (daemon)
    umask(0);
    setsid();

    // Ensure only one instance
    FILE *lockfile = fopen("/var/run/my_daemon.pid", "w");
    if (!lockfile) {
        log_message("Could not create lock file: %s\n", strerror(errno));
        cleanup(-1, NULL, NULL);
        exit(EXIT_FAILURE);
    }

    if (lockf(fileno(lockfile), F_TLOCK, 0) < 0) {
        log_message("Daemon already running\n");
        fclose(lockfile);
        exit(EXIT_FAILURE);
    }

    // Create and write PID to pidfile
    FILE *pidfile = fopen(PIDFILE, "w");
    if (!pidfile) {
        log_message("Could not create PID file: %s\n", strerror(errno));
        cleanup(-1, NULL, lockfile);
        exit(EXIT_FAILURE);
    }

    fprintf(pidfile, "%d\n", getpid());

    // Create Unix socket
    int unix_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (unix_socket < 0) {
        log_message("Error creating Unix socket: %s\n", strerror(errno));
        cleanup(unix_socket, pidfile, lockfile);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    // Bind Unix socket
    if (bind(unix_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_message("Error binding Unix socket: %s\n", strerror(errno));
        cleanup(unix_socket, pidfile, lockfile);
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(unix_socket, MAX_CONNECTIONS) < 0) {
        log_message("Error listening on Unix socket: %s\n", strerror(errno));
        cleanup(unix_socket, pidfile, lockfile);
        exit(EXIT_FAILURE);
    }

    // Main loop
    while (keep_running) {
        // Accept connection
        int client_socket = accept(unix_socket, NULL, NULL);
        if (client_socket < 0) {
            log_message("Error accepting connection: %s\n", strerror(errno));
            continue;
        }

        // Connect to IPv4 socket
        int ipv4_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (ipv4_socket < 0) {
            log_message("Error creating IPv4 socket: %s\n", strerror(errno));
            close(client_socket);
            continue;
        }

        struct sockaddr_in ipv4_addr;
        ipv4_addr.sin_family = AF_INET;
        ipv4_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        ipv4_addr.sin_port = htons(9999);

        if (connect(ipv4_socket, (struct sockaddr*)&ipv4_addr, sizeof(ipv4_addr)) < 0) {
            log_message("Error connecting to IPv4 socket: %s\n", strerror(errno));
            close(client_socket);
            close(ipv4_socket);
            continue;
        }

        // Forward data from Unix socket to IPv4 socket
        char buffer[1024];
        ssize_t bytes_read;
        while ((bytes_read = read(client_socket, buffer, sizeof(buffer))) > 0) {
            write(ipv4_socket, buffer, bytes_read);
        }

        // Cleanup
        close(client_socket);
        close(ipv4_socket);
    }

    // Cleanup
    cleanup(unix_socket, pidfile, lockfile);

    return EXIT_SUCCESS;
}