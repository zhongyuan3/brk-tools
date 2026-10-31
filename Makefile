CFLAGS := -Wall -Wextra -Werror -Wno-unused-parameter
CFLAGS += -O2
CFLAGS += -std=gnu11
CFLAGS += -MMD -MP

BRKFS := brkfs

BRKFS_OBJS := main.o mkfs.o info.o cp.o ls.o cat.o check.o common.o

all: $(BRKFS)

$(BRKFS): $(BRKFS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(BRKFS) mkfs.brkfs cp.brkfs ls.brkfs cat.brkfs *.o *.d

-include $(wildcard *.d)

.PHONY: all clean
