##############################################################
# AESDSOCKET SERVER MAKEFILE
##############################################################

# Use the CC passed from Buildroot or fallback to default gcc
CC ?= gcc
CFLAGS ?= -Wall -Werror -g
TARGET = aesdsocket

all: $(TARGET)

$(TARGET): aesdsocket.c
	$(CC) $(CFLAGS) aesdsocket.c -o $(TARGET)

clean:
	rm -f $(TARGET)

