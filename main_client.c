#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PORT 5050
#define LINE_SIZE 4096
#define MAX_USERS 128

typedef struct {
    char name[64];
    int color;
} UserColor;

static UserColor users[MAX_USERS];
static int user_count = 0;

/* Send all bytes, handling partial TCP sends. */
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

/* Read one newline-terminated server response. */
static int read_line(int fd, char *buffer, size_t capacity) {
    size_t used = 0;

    while (used + 1 < capacity) {
        char ch;

        ssize_t n = recv(fd, &ch, 1, 0);

        if (n == 0)
            return 0;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (ch == '\n')
            break;

        if (ch != '\r')
            buffer[used++] = ch;
    }

    buffer[used] = '\0';
    return 1;
}

/* Clean fields used by the communication protocol. */
static void clean_field(char *text) {
    for (; *text; text++) {
        if (*text == '|' || *text == '\n' || *text == '\r')
            *text = ' ';
    }
}

/*
 * Give each username a random terminal color.
 * Colors are unique among currently known users in this client.
 */
static int color_for(const char *name) {
    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].name, name) == 0)
            return users[i].color;
    }

    if (user_count >= MAX_USERS)
        return 37;

    int color;
    int unique;

    do {
        color = 16 + rand() % 216;
        unique = 1;

        for (int i = 0; i < user_count; i++) {
            if (users[i].color == color)
                unique = 0;
        }

    } while (!unique);

    snprintf(users[user_count].name,
             sizeof(users[user_count].name), "%s", name);

    users[user_count].color = color;
    user_count++;

    return color;
}

/* Remove a departed user from the local color table. */
static void remove_color(const char *name) {
    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].name, name) == 0) {
            for (int j = i; j + 1 < user_count; j++)
                users[j] = users[j + 1];

            user_count--;
            return;
        }
    }
}

/* Print a username using its assigned color. */
static void print_colored_name(const char *name) {
    int color = color_for(name);

    printf("\033[38;5;%dm%s\033[0m", color, name);
}

/* Process and display a message received from the server. */
static void print_server_message(const char *line) {
    char copy[LINE_SIZE];

    snprintf(copy, sizeof(copy), "%s", line);

    char *kind = strtok(copy, "|");

    if (kind == NULL)
        return;

    if (strcmp(kind, "CHAT") == 0) {
        char *name = strtok(NULL, "|");
        char *ip = strtok(NULL, "|");
        char *message = strtok(NULL, "");

        if (name && ip && message) {
            printf("[");

            print_colored_name(name);

            printf(" (%s)] %s\n", ip, message);
        }
    }

    else if (strcmp(kind, "JOINED") == 0) {
        char *name = strtok(NULL, "|");
        char *ip = strtok(NULL, "|");

        if (name && ip) {
            printf("\n");

            print_colored_name(name);

            printf(" (%s) joined the chat room!\n", ip);
        }
    }

    else if (strcmp(kind, "LEFT") == 0) {
        char *name = strtok(NULL, "|");
        char *ip = strtok(NULL, "|");

        if (name && ip) {
            printf("\n");

            print_colored_name(name);

            printf(" (%s) left the room!\n", ip);

            remove_color(name);
        }
    }

    else if (strcmp(kind, "MEMBER") == 0) {
        char *name = strtok(NULL, "|");
        char *ip = strtok(NULL, "|");

        if (name && ip) {
            color_for(name);

            printf("  ");

            print_colored_name(name);

            printf(" (%s) is already in this room.\n", ip);
        }
    }

    fflush(stdout);
}

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr,
                "Usage: %s SERVER_IP [new|ROOM_NUMBER]\n",
                argv[0]);
        return 1;
    }

    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    /* Create the TCP socket. */
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in server;

    memset(&server, 0, sizeof(server));

    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);

    if (inet_pton(AF_INET, argv[1],
                  &server.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", argv[1]);
        close(sock);
        return 1;
    }

    /* Connect to the chat server. */
    if (connect(sock, (struct sockaddr *)&server,
                sizeof(server)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    char line[LINE_SIZE];

    /* Receive the current room list. */
    int result = read_line(sock, line, sizeof(line));

    if (result <= 0 || strcmp(line, "ROOMS") != 0) {
        fprintf(stderr, "Server did not provide a room list.\n");
        close(sock);
        return 1;
    }

    printf("Server says following options are available:\n\n");

    int available_rooms = 0;

    while ((result = read_line(sock, line, sizeof(line))) > 0) {
        if (strcmp(line, "ENDROOMS") == 0)
            break;

        if (strncmp(line, "ROOM|", 5) == 0) {
            int room_id = 0;
            int people = 0;

            if (sscanf(line + 5, "%d|%d",
                       &room_id, &people) == 2) {
                printf("Room %d: %d %s\n\n",
                       room_id,
                       people,
                       people == 1 ? "person" : "people");

                available_rooms++;
            }
        }
    }

    if (result <= 0) {
        fprintf(stderr, "Server disconnected.\n");
        close(sock);
        return 1;
    }

    /* Determine which room the user wants to join. */
    char choice[64];

    if (argc == 3) {
        snprintf(choice, sizeof(choice), "%s", argv[2]);
    } else {
        if (available_rooms == 0) {
            printf("No rooms available. Creating a new room.\n");
            snprintf(choice, sizeof(choice), "new");
        } else {
            printf("Choose the room number or type [new] to create a new room: ");

            if (fgets(choice, sizeof(choice), stdin) == NULL) {
                close(sock);
                return 1;
            }

            choice[strcspn(choice, "\r\n")] = '\0';
        }
    }

    if (choice[0] == '\0') {
        fprintf(stderr, "No room selected.\n");
        close(sock);
        return 1;
    }

    if (strcmp(choice, "new") != 0) {
        char *end = NULL;
        long id = strtol(choice, &end, 10);

        if (!*choice || !end || *end || id <= 0) {
            fprintf(stderr,
                    "Room must be 'new' or a positive room number.\n");
            close(sock);
            return 1;
        }
    }

    char request[128];

    snprintf(request, sizeof(request), "JOIN|%s\n", choice);

    if (send_text(sock, request) < 0) {
        perror("send");
        close(sock);
        return 1;
    }

    /* Wait for the server to accept the room selection. */
    result = read_line(sock, line, sizeof(line));

    if (result <= 0) {
        fprintf(stderr, "Server disconnected during room selection.\n");
        close(sock);
        return 1;
    }

    if (strncmp(line, "ERROR|", 6) == 0) {
        fprintf(stderr, "%s\n", line + 6);
        close(sock);
        return 1;
    }

    if (strcmp(line, "NAME") != 0) {
        fprintf(stderr, "Unexpected server response: %s\n", line);
        close(sock);
        return 1;
    }

    /* Ask for the username. */
    char username[64];

    printf("Type your user name: ");

    if (fgets(username, sizeof(username), stdin) == NULL) {
        close(sock);
        return 1;
    }

    username[strcspn(username, "\r\n")] = '\0';
    clean_field(username);

    if (!username[0]) {
        fprintf(stderr, "Username cannot be empty.\n");
        close(sock);
        return 1;
    }

    snprintf(request, sizeof(request), "USER|%s\n", username);

    if (send_text(sock, request) < 0) {
        perror("send");
        close(sock);
        return 1;
    }

    /* Receive welcome information and existing room members. */
    int joined = 0;
    int room_id = 0;

    while ((result = read_line(sock, line, sizeof(line))) > 0) {
        if (strncmp(line, "ERROR|", 6) == 0) {
            fprintf(stderr, "%s\n", line + 6);
            close(sock);
            return 1;
        }

        if (strncmp(line, "WELCOME|", 8) == 0) {
            char ip[64] = "";

            if (sscanf(line + 8, "%d|%63s",
                       &room_id, ip) == 2) {
                printf("Connected to %s with room number %d\n",
                       argv[1], room_id);
                printf("Your address: %s\n", ip);
            }
        }

        else if (strcmp(line, "ENDMEMBERS") == 0) {
            joined = 1;
            break;
        }

        else {
            print_server_message(line);
        }
    }

    if (!joined) {
        fprintf(stderr, "Could not join the room.\n");
        close(sock);
        return 1;
    }

    color_for(username);

    printf("\nYou are in room %d.\n", room_id);
    printf("Type a message and press Enter.\n");
    printf("Enter an empty message to exit.\n\n");
    printf("> ");
    fflush(stdout);

    /*
     * Main chat loop.
     * select() monitors both keyboard input and server messages.
     */
    char receive_buffer[LINE_SIZE * 2];
    size_t used = 0;

    int running = 1;

    while (running) {
        fd_set readfds;

        FD_ZERO(&readfds);

        FD_SET(sock, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int maxfd = sock > STDIN_FILENO ? sock : STDIN_FILENO;

        int ready = select(maxfd + 1, &readfds,
                           NULL, NULL, NULL);

        if (ready < 0) {
            if (errno == EINTR)
                continue;

            perror("select");
            break;
        }

        /* Check for incoming server messages. */
        if (FD_ISSET(sock, &readfds)) {
            char data[1024];

            ssize_t n = recv(sock, data, sizeof(data), 0);

            if (n <= 0) {
                printf("\nServer connection closed.\n");
                break;
            }

            for (ssize_t i = 0; i < n; i++) {
                if (data[i] == '\n') {
                    receive_buffer[used] = '\0';

                    print_server_message(receive_buffer);

                    used = 0;

                    printf("> ");
                    fflush(stdout);
                }

                else if (data[i] != '\r' &&
                         used < sizeof(receive_buffer) - 1) {
                    receive_buffer[used++] = data[i];
                }
            }
        }

        /* Check for keyboard input. */
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            char message[LINE_SIZE];

            if (fgets(message, sizeof(message), stdin) == NULL) {
                send_text(sock, "QUIT\n");
                running = 0;
                break;
            }

            message[strcspn(message, "\r\n")] = '\0';

            /* Empty message means disconnect. */
            if (!message[0]) {
                send_text(sock, "QUIT\n");
                running = 0;
                break;
            }

            clean_field(message);

            char outgoing[LINE_SIZE + 8];

            snprintf(outgoing, sizeof(outgoing),
                     "MSG|%s\n", message);

            if (send_text(sock, outgoing) < 0) {
                perror("send");
                running = 0;
            }

            printf("> ");
            fflush(stdout);
        }
    }

    close(sock);

    printf("Disconnected.\n");

    return 0;
}

