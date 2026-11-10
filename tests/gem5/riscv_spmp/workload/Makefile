# Authored for later execution. This Makefile has not been run in this task.
CROSS ?= riscv64-unknown-elf-
OUT ?= build
CFLAGS = -O1 -g -ffreestanding -fno-builtin -fno-common -fno-pic -fno-pie \
         -msmall-data-limit=0 -mcmodel=medany -mno-relax -nostdlib -nostartfiles -static -Wall -Wextra
LDFLAGS = -Wl,-T,link.ld -Wl,--build-id=none -Wl,-Map,$@.map

.PHONY: all clean
all: $(foreach xlen,32 64,$(foreach feature,off on,$(foreach count,0 1 16 64,$(OUT)/spmp-rv$(xlen)-$(feature)-n$(count).elf)))

$(OUT):
	mkdir -p $@

$(OUT)/spmp-rv32-%.elf: start.S spmp_test.c link.ld | $(OUT)
	@feature=$$(echo $* | cut -d- -f1); count=$$(echo $* | sed 's/.*-n//'); \
	$(CROSS)gcc $(CFLAGS) -march=rv32imac_zicsr -mabi=ilp32 \
	  -DSPMP_FEATURE=$$(test "$$feature" = on && echo 1 || echo 0) \
	  -DSPMP_COUNT=$$count -DTEST_XLEN=32 start.S spmp_test.c $(LDFLAGS) -o $@

$(OUT)/spmp-rv64-%.elf: start.S spmp_test.c link.ld | $(OUT)
	@feature=$$(echo $* | cut -d- -f1); count=$$(echo $* | sed 's/.*-n//'); \
	$(CROSS)gcc $(CFLAGS) -march=rv64imac_zicsr -mabi=lp64 \
	  -DSPMP_FEATURE=$$(test "$$feature" = on && echo 1 || echo 0) \
	  -DSPMP_COUNT=$$count -DTEST_XLEN=64 start.S spmp_test.c $(LDFLAGS) -o $@

clean:
	rm -rf $(OUT)
