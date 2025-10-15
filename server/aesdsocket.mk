CC ?= $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Werror -g
TARGET = aesdsocket

all: $(TARGET)

$(TARGET): aesdsocket.c
	$(CC) $(CFLAGS) -o $(TARGET) aesdsocket.c 

clean:
	rm -f $(TARGET)

