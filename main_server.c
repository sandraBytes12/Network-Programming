
/*
 * main_server.c
 * Network Programming Project - Check Point 3
 * Multi-user, multi-room TCP chat server.
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 5050
#define MAX_CLIENTS 128
#define MAX_ROOMS 128
#define LINE_SIZE 4096
#define BACKLOG 32

typedef struct {
    int fd;
    int active;
    int phase;  /* 0 = room selection, 1 = username, 2 = chatting */
    int room;
    char ip[INET_ADDRSTRLEN];
    char name[64];
    char input[LINE_SIZE];
    size_t used;
} Client;

static Client clients[MAX_CLIENTS];
static int next_room_id = 1;

/* Send an entire message, handling partial TCP sends. */
static int send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0)
            return -1;

        sent += (size_t)n;
    }

    return 0;
}

static int send_text(int fd, const char *message) {
    return send_all(fd, message, strlen(message));
}

/* Prevent protocol separators from appearing inside fields. */
static void clean_field(char *text) {
    for (; *text; text++) {
        if (*text == '|' || *text == '\n' || *text == '\r')
            *text = ' ';
    }
}

/* Count users currently chatting in a room. */
static int room_count(int room) {
    int count = 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active &&
            clients[i].phase == 2 &&
            clients[i].room == room) {
            count++;
        }
    }

    return count;
}

/* Check whether a room still exists. */
static int room_exists(int room) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].room == room)
            return 1;
    }

    return 0;
}

/* Send the current room list to a newly connected client. */
static void send_room_list(int fd) {
    char line[128];
    int rooms[MAX_ROOMS];
    int count = 0;

    send_text(fd, "ROOMS\n");

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active || clients[i].room <= 0)
            continue;

        int room = clients[i].room;
        int found = 0;

        for (int j = 0; j < count; j++) {
            if (rooms[j] == room)
                found = 1;
        }

        if (!found && count < MAX_ROOMS)
            rooms[count++] = room;
    }

    /* Sort room numbers for predictable display. */
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (rooms[j] < rooms[i]) {
                int temp = rooms[i];
                rooms[i] = rooms[j];
                rooms[j] = temp;
            }
        }
    }

    for (int i = 0; i < count; i++) {
        snprintf(line, sizeof(line), "ROOM|%d|%d\n",
                 rooms[i], room_count(rooms[i]));
        send_text(fd, line);
    }

    send_text(fd, "ENDROOMS\n");
}

/* Display the up-to-date list of connected clients on the server. */
static void print_client_list(void) {
    printf("\n========== Connected clients ==========\n");

    int count = 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            if (clients[i].phase == 2) {
                printf("Room %d | %-20s | %s\n",
                       clients[i].room,
                       clients[i].name,
                       clients[i].ip);
            }
            else if (clients[i].phase == 1) {
                printf("Room %d | %-20s | %s (choosing username)\n",
                       clients[i].room,
                       "Pending client",
                       clients[i].ip);
            }
            else {
                printf("%-20s | %s (selecting room)\n",
                       "Pending client",
                       clients[i].ip);
            }

            count++;
        }
    }

    if (count == 0)
        printf("(no connected clients)\n");

    printf("=======================================\n");
    fflush(stdout);
}

/* Broadcast a message only to users in the specified room. */
static void broadcast_room(int room, const char *message, int except_fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active &&
            clients[i].phase == 2 &&
            clients[i].room == room &&
            clients[i].fd != except_fd) {
            send_text(clients[i].fd, message);
        }
    }
}

/* Usernames must be unique within a room. */
static int username_in_room(int room, const char *name, int except_fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active &&
            clients[i].phase == 2 &&
            clients[i].room == room &&
            clients[i].fd != except_fd &&
            strcmp(clients[i].name, name) == 0) {
            return 1;
        }
    }

    return 0;
}

/* Remove a disconnected client and notify remaining room members. */
static void remove_client(int index, const char *reason) {
    if (!clients[index].active)
        return;

    int fd = clients[index].fd;
    int room = clients[index].room;

    if (clients[index].phase == 2) {
        char notice[256];

        snprintf(notice, sizeof(notice),
                 "LEFT|%s|%s\n",
                 clients[index].name,
                 clients[index].ip);

        broadcast_room(room, notice, fd);
    }

    close(fd);

    memset(&clients[index], 0, sizeof(clients[index]));
    clients[index].fd = -1;

    if (reason != NULL)
        printf("Client disconnected: %s\n", reason);

    print_client_list();
}

/* Process one complete line received from a client. */
static void handle_line(int index, char *line) {
    Client *c = &clients[index];

    line[strcspn(line, "\r\n")] = '\0';

    /* Phase 0: room selection. */
    if (c->phase == 0) {
        if (strncmp(line, "JOIN|", 5) != 0) {
            send_text(c->fd, "ERROR|Expected a room selection.\n");
            remove_client(index, "invalid room selection");
            return;
        }

        const char *choice = line + 5;

        if (strcmp(choice, "new") == 0) {
            c->room = next_room_id++;
            c->phase = 1;
            send_text(c->fd, "NAME\n");
        } else {
            char *end = NULL;
            long value = strtol(choice, &end, 10);

            if (!*choice || !end || *end ||
                value <= 0 || value > 1000000 ||
                !room_exists((int)value)) {

                send_text(c->fd,
                    "ERROR|That room does not exist. Reconnect and choose an available room.\n");

                remove_client(index, "requested room does not exist");
                return;
            }

            c->room = (int)value;
            c->phase = 1;
            send_text(c->fd, "NAME\n");
        }

        return;
    }

    /* Phase 1: receive and validate username. */
    if (c->phase == 1) {
        if (strncmp(line, "USER|", 5) != 0) {
            send_text(c->fd, "ERROR|Expected a username.\n");
            remove_client(index, "invalid username request");
            return;
        }

        char name[64];

        snprintf(name, sizeof(name), "%s", line + 5);
        clean_field(name);

        char *start = name;

        while (*start == ' ')
            start++;

        memmove(name, start, strlen(start) + 1);

        size_t length = strlen(name);

        while (length > 0 && name[length - 1] == ' ')
            name[--length] = '\0';

        if (length == 0 ||
            username_in_room(c->room, name, c->fd)) {

            send_text(c->fd,
                "ERROR|Username is empty or already in use in this room.\n");

            remove_client(index, "invalid or duplicate username");
            return;
        }

        snprintf(c->name, sizeof(c->name), "%s", name);

        char message[256];

        /* Send the new user their room number and IP address. */
        snprintf(message, sizeof(message),
                 "WELCOME|%d|%s\n", c->room, c->ip);

        send_text(c->fd, message);

        /* Tell the new user who is already in the room. */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (i != index &&
                clients[i].active &&
                clients[i].phase == 2 &&
                clients[i].room == c->room) {

                snprintf(message, sizeof(message),
                         "MEMBER|%s|%s\n",
                         clients[i].name,
                         clients[i].ip);

                send_text(c->fd, message);
            }
        }

        send_text(c->fd, "ENDMEMBERS\n");

        c->phase = 2;

        /* Notify everyone else that this user has joined. */
        snprintf(message, sizeof(message),
                 "JOINED|%s|%s\n", c->name, c->ip);

        broadcast_room(c->room, message, c->fd);

        printf("%s (%s) joined room %d.\n",
               c->name, c->ip, c->room);

        print_client_list();
        return;
    }

    /* Phase 2: normal chat messages. */
    if (c->phase == 2) {
        if (strcmp(line, "QUIT") == 0 || line[0] == '\0') {
            remove_client(index, "user requested disconnect");
            return;
        }

        if (strncmp(line, "MSG|", 4) == 0) {
            char message[LINE_SIZE];

            snprintf(message, sizeof(message), "%s", line + 4);
            clean_field(message);

            char outgoing[LINE_SIZE + 160];

            snprintf(outgoing, sizeof(outgoing),
                     "CHAT|%s|%s|%s\n",
                     c->name, c->ip, message);

            /* Include the sender so their own message is displayed. */
            broadcast_room(c->room, outgoing, -1);
        }
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;

    int listener = socket(AF_INET, SOCK_STREAM, 0);

    if (listener < 0) {
        perror("socket");
        return 1;
    }

    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PORT);

    if (bind(listener, (struct sockaddr *)&address,
             sizeof(address)) < 0) {
        perror("bind");
        close(listener);
        return 1;
    }

    if (listen(listener, BACKLOG) < 0) {
        perror("listen");
        close(listener);
        return 1;
    }

    printf("Chat server listening on port %d...\n", PORT);
    fflush(stdout);

    while (1) {
        fd_set readfds;

        FD_ZERO(&readfds);
        FD_SET(listener, &readfds);

        int maxfd = listener;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active) {
                FD_SET(clients[i].fd, &readfds);

                if (clients[i].fd > maxfd)
                    maxfd = clients[i].fd;
            }
        }

        int ready = select(maxfd + 1, &readfds, NULL, NULL, NULL);

        if (ready < 0) {
            if (errno == EINTR)
                continue;

            perror("select");
            break;
        }

        /* Accept new connections. */
        if (FD_ISSET(listener, &readfds)) {
            struct sockaddr_in peer;
            socklen_t peer_length = sizeof(peer);

            int fd = accept(listener,
                            (struct sockaddr *)&peer,
                            &peer_length);

            if (fd >= 0) {
                int slot = -1;

                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (!clients[i].active) {
                        slot = i;
                        break;
                    }
                }

                if (slot < 0) {
                    send_text(fd, "ERROR|Server is full.\n");
                    close(fd);
                } else {
                    memset(&clients[slot], 0, sizeof(clients[slot]));

                    clients[slot].fd = fd;
                    clients[slot].active = 1;
                    clients[slot].phase = 0;
                    clients[slot].room = 0;

                    inet_ntop(AF_INET, &peer.sin_addr,
                              clients[slot].ip,
                              sizeof(clients[slot].ip));

                    printf("Connection from %s\n", clients[slot].ip);

                    send_room_list(fd);

                    /* Update the server-side client list immediately. */
                    print_client_list();
                }
            }
        }

        /* Read data from connected clients. */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active ||
                !FD_ISSET(clients[i].fd, &readfds))
                continue;

            char data[1024];

            ssize_t n = recv(clients[i].fd,
                             data, sizeof(data), 0);

            if (n <= 0) {
                remove_client(i, "connection closed");
                continue;
            }

            for (ssize_t k = 0; k < n; k++) {
                if (!clients[i].active)
                    break;

                char ch = data[k];

                if (ch == '\n') {
                    clients[i].input[clients[i].used] = '\0';

                    handle_line(i, clients[i].input);

                    clients[i].used = 0;
                } else if (ch != '\r') {
                    if (clients[i].used < LINE_SIZE - 1) {
                        clients[i].input[clients[i].used++] = ch;
                    } else {
                        remove_client(i, "input line too long");
                        break;
                    }
                }
            }
        }
    }

    close(listener);
    return 0;
}


