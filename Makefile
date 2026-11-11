HOSTCC ?= cc

HOST_CFLAGS += -Wall -Wextra -Werror -O2 -std=gnu11 -MMD -MP -DNDEBUG

BRKFS := brkfs

BRKFS_OBJS := main.o mkfs.o info.o cp.o ls.o cat.o check.o common.o

all: $(BRKFS)

$(BRKFS): $(BRKFS_OBJS)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $^

%.o: %.c
	$(HOSTCC) $(HOST_CFLAGS) -c -o $@ $<

hooks:
	@git config core.hooksPath .githooks
	@echo "  HOOK    core.hooksPath=.githooks"

clean:
	rm -f $(BRKFS) *.o *.d

-include $(wildcard *.d)

.PHONY: all clean hooks
