CFLAGS += -O2 -Wshadow -Wpointer-arith -fPIC
CFLAGS += -Wstrict-prototypes -Wmissing-prototypes
CFLAGS += -Wnonnull -Wunused -Wuninitialized -fvisibility=hidden
CFLAGS += -g -Wall -Wextra -std=gnu99 -pipe -ggdb3 -I. -I..

LIBS += -lm
CC = gcc

EXTOBJS = ../common/*.o
APPS = v4l2cat yuyv-util

GCNO = $(APPS:=.gcno)
GCDA = $(APPS:=.gcda)

all: $(APPS)

v4l2cat: v4l2cat.c
	$(CC) $(CFLAGS) -o v4l2cat v4l2cat.c $(EXTOBJS) $(LIBS)

yuyv-util: yuyv-util.c
	$(CC) $(CFLAGS) -o yuyv-util yuyv-util.c $(EXTOBJS) $(LIBS)

clean:
	$(RM) $(APPS) *.o

