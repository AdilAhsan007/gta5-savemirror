ifdef PS4_PAYLOAD_SDK
    include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk
else
    $(error PS4_PAYLOAD_SDK is undefined - see BUILD.md)
endif

PAYLOAD := gta5_savemirror.elf
SRCS    := main.c
CFLAGS  := -Wall -Werror -O2

all: $(PAYLOAD)

$(PAYLOAD): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@

clean:
	-rm -f *.o *.elf
