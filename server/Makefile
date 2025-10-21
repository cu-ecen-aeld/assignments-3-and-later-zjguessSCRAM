CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Werror -g
TARGET ?= aesdsocket
LDFLAGS ?= -lpthread -lrt 
SRC = aesdsocket.c
OBJ = $(SRC:.c=.o)


all: $(TARGET)


$(TARGET): $(OBJ)
		$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

%.o: %.c 
		$(CC) $(CFLAGS) -c $< -o $@

clean:
		rm -f $(TARGET) $(OBJ)

install:
		@echo "No install target defined"


.PHONY: all clean install
