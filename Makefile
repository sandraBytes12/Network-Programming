CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2

all: main_server main_client

main_server: main_server.c
	$(CC) $(CFLAGS) -o main_server main_server.c

main_client: main_client.c
	$(CC) $(CFLAGS) -o main_client main_client.c

clean:
	rm -f main_server main_client

.PHONY: all clean

