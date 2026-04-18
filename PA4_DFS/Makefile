CC     = gcc
CFLAGS = -Wall -g
LIBS   = -lssl -lcrypto

all: dfc dfs

dfc: dfc.c
	$(CC) $(CFLAGS) -o dfc dfc.c $(LIBS)

dfs: dfs.c
	$(CC) $(CFLAGS) -o dfs dfs.c

clean:
	rm -f dfc dfs
