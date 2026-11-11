CROSS ?= riscv64-unknown-elf-
OUT ?= build
CFLAGS = -O1 -g -ffreestanding -fno-builtin -fno-common -fno-pic -fno-pie \
         -msmall-data-limit=0 -mcmodel=medany -mno-relax -nostdlib -nostartfiles -static -Wall -Wextra
LDFLAGS = -Wl,-T,link.ld -Wl,--build-id=none -Wl,-Map,$@.map

MODULES = test_csr test_matching test_permissions test_paging_fences test_extensions
ELFS = $(foreach mod,$(MODULES),$(OUT)/$(mod)-rv32.elf $(OUT)/$(mod)-rv64.elf)

.PHONY: all clean
all: $(ELFS)

$(OUT):
	mkdir -p $@

$(OUT)/%-rv32.elf: start.S trap.c %.c spmp_harness.h link.ld | $(OUT)
	$(CROSS)gcc $(CFLAGS) -march=rv32imac_zicsr -mabi=ilp32 \
	  start.S trap.c $*.c $(LDFLAGS) -o $@

$(OUT)/%-rv64.elf: start.S trap.c %.c spmp_harness.h link.ld | $(OUT)
	$(CROSS)gcc $(CFLAGS) -march=rv64imac_zicsr -mabi=lp64 \
	  start.S trap.c $*.c $(LDFLAGS) -o $@

clean:
	rm -rf $(OUT)
