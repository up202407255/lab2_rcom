CC = gcc
CFLAGS = -Wall -Wextra -O2

all: download

download: src/ftp_download.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f download