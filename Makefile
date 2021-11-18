# SPDX-License-Identifier: GPL-2.0

# Check for linux source path dependency
ifndef LINUX_SRC
all:
        @echo LINUX_SRC env variable is not defined. 
else
all:build tar.zip
endif

LINUX_SRC_PATH := $(LINUX_SRC)/linux
BPF_SAMPLES_PATH := $(LINUX_SRC_PATH)/samples/bpf
TOOLS_PATH := $(BPF_SAMPLES_PATH)/../../tools
L3AF_SRC_PATH := $(BPF_SAMPLES_PATH)/ratelimiting


# List of programs to build
hostprogs-y := ratelimiting

# Libbpf dependencies
LIBBPF = $(TOOLS_PATH)/lib/bpf/libbpf.a

CGROUP_HELPERS := ../../../tools/testing/selftests/bpf/cgroup_helpers.o
TRACE_HELPERS := ../../../tools/testing/selftests/bpf/trace_helpers.o

ratelimiting-objs := ratelimiting_user.o ../bpf_load.o

# Tell kbuild to always build the programs
always := $(hostprogs-y)
always += ratelimiting_kern.o

KBUILD_HOSTCFLAGS += -I$(objtree)/usr/include
KBUILD_HOSTCFLAGS += -I$(srctree)/tools/lib/
KBUILD_HOSTCFLAGS += -I$(srctree)/tools/testing/selftests/bpf/
KBUILD_HOSTCFLAGS += -I$(srctree)/tools/lib/ -I$(srctree)/tools/include -I$(srctree)/tools/include/uapi
KBUILD_HOSTCFLAGS += -I$(srctree)/tools/perf

HOSTCFLAGS_bpf_load.o += -I$(objtree)/usr/include -Wno-unused-variable
HOSTCFLAGS_trace_helpers.o += -I$(srctree)/tools/lib/bpf/

HOSTCFLAGS_ratelimiting_user.o +=  -I. -I$(BPF_SAMPLES_PATH) -I$(srctree)/tools/lib/bpf/ -g -LTEST/libbpf.a

KBUILD_HOSTLDLIBS               += $(LIBBPF) -lelf
HOSTLDLIBS_test_overhead        += -lrt

LLC ?= llc
CLANG ?= clang
LLVM_OBJCOPY ?= llvm-objcopy
BTF_PAHOLE ?= pahole

# Detect that we're cross compiling and use the cross compiler
ifdef CROSS_COMPILE
HOSTCC = $(CROSS_COMPILE)gcc
CLANG_ARCH_ARGS = -target $(ARCH)
endif

tar.zip:
	@rm -rf l3af_ratelimiting
	@rm -f l3af_ratelimiting.tar.gz
	@mkdir l3af_ratelimiting
	@cp $(L3AF_SRC_PATH)/ratelimiting_kern.o l3af_ratelimiting/
	@cp $(L3AF_SRC_PATH)/ratelimiting l3af_ratelimiting/
	@tar -cvf l3af_ratelimiting.tar ./l3af_ratelimiting
	@gzip l3af_ratelimiting.tar

build: $(LIBBPF)
	$(MAKE) -C $(LINUX_SRC_PATH)/ $(L3AF_SRC_PATH)/ BPF_SAMPLES_PATH=$(BPF_SAMPLES_PATH)
clean:
	$(MAKE) -C $(LINUX_SRC_PATH) M=$(L3AF_SRC_PATH)/ clean
	@rm -f ../*.o
	@rm -f *~
	@rm -f l3af_ratelimiting.tar.gz

$(LIBBPF): FORCE

# Fix up variables inherited from Kbuild that tools/ build system won't like
	$(MAKE) -C $(dir $@) RM='rm -rf' EXTRA_CFLAGS="$(TPROGS_CFLAGS)" \
		LDFLAGS=$(TPROGS_LDFLAGS) srctree=$(BPF_SAMPLES_PATH)/../../ O=

FORCE:

# Verify LLVM compiler tools are available and bpf target is supported by llc
.PHONY: verify_cmds verify_target_bpf $(CLANG) $(LLC)

#verify_cmds: $(CLANG) $(LLC)
	@for TOOL in $^ ; do \
                if ! (which -- "$${TOOL}" > /dev/null 2>&1); then \
                        echo "*** ERROR: Cannot find LLVM tool $${TOOL}" ;\
                        exit 1; \
                else true; fi; \
        done

#verify_target_bpf: verify_cmds
	@if ! (${LLC} -march=bpf -mattr=help > /dev/null 2>&1); then \
                echo "*** ERROR: LLVM (${LLC}) does not support 'bpf' target" ;\
                echo "   NOTICE: LLVM version >= 3.7.1 required" ;\
                exit 2; \
        else true; fi

$(BPF_SAMPLES_PATH)/*.c: verify_target_bpf $(LIBBPF)
$(src)/*.c: verify_target_bpf $(LIBBPF)

$(obj)/%.o: $(src)/%.c 
	$(Q)$(CLANG) $(NOSTDINC_FLAGS) $(LINUXINCLUDE) $(EXTRA_CFLAGS) -I$(obj) \
		-I$(srctree)/tools/testing/selftests/bpf/ \
		-D__KERNEL__ -D__BPF_TRACING__ -Wno-unused-value -Wno-pointer-sign \
		-D__TARGET_ARCH_$(ARCH) -Wno-compare-distinct-pointer-types \
		-Wno-gnu-variable-sized-type-not-at-end \
		-Wno-address-of-packed-member -Wno-tautological-compare \
		-Wno-unknown-warning-option $(CLANG_ARCH_ARGS) \
		-I$(srctree)/samples/bpf/ -include asm_goto_workaround.h \
		-O2 -emit-llvm -c $< -o -| $(LLC) -march=bpf $(LLC_FLAGS) -filetype=obj -o $@
ifeq ($(DWARF2BTF),y)
	$(BTF_PAHOLE) -J $@
endif

