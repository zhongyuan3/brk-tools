CFLAGS := -Wall -Wextra -Werror -Wno-unused-parameter
CFLAGS += -O2
CFLAGS += -std=gnu11

MKFS := mkfs.brkfs
CP := cp.brkfs
LS := ls.brkfs
CAT := cat.brkfs

all: $(MKFS) $(CP) $(LS) $(CAT)

$(MKFS): mkfs.o common.o
	$(CC) $(CFLAGS) -o $@ $^

$(CP): cp.o common.o
	$(CC) $(CFLAGS) -o $@ $^

$(LS): ls.o common.o
	$(CC) $(CFLAGS) -o $@ $^

$(CAT): cat.o common.o
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(MKFS) $(CP) $(LS) $(CAT) *.o

.PHONY: all clean
