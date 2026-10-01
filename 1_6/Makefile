CC = gcc
AR = ar
RM = rm -f
CFLAGS = -g -Wall -Wextra -Wpedantic -Werror -I.

LIB_NAME = libmy_pthread.a
LIB_SRC = my_pthread.c
LIB_OBJ = $(LIB_SRC:.c=.o)

MAINS = main.c
TARGETS = $(MAINS:.c=)

all: $(LIB_NAME) $(TARGETS)

$(LIB_NAME): $(LIB_OBJ)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

main%: main%.o $(LIB_NAME)
	$(CC) $(CFLAGS) $< -L. -lmy_pthread -o $@

clean:
	$(RM) *.o $(LIB_NAME) $(TARGETS)

.PHONY: all clean