CC      = gcc
RM      = rm -f
LOG_FILE = /var/tmp/aesdsocketdata

default: all

all: aesdsocket

aesdsocket: aesdsocket.c
	$(CROSS_COMPILE)$(CC) $(CFLAGS) -o aesdsocket aesdsocket.c

clean veryclean:
	$(RM) aesdsocket
	$(RM) $(LOG_FILE)
