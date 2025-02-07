
APP = hello_thread

SRCS-y := main.c

DPDK_VERSION = $(shell pkg-config --modversion libdpdk |  grep -o "^\w*\b")
$(info  DPDK_VERSION is $(DPDK_VERSION))

ifeq ($(DPDK_VERSION), none)
DPDK_VERSION=17
RTE_TARGET ?= x86_64-native-linuxapp-gcc
else
RTE_SDK=/usr/share/dpdk
RTE_TARGET=x86_64-default-linuxapp-gcc
endif

# Build using pkg-config variables when DPDK_VERSION more than 17
ifeq ($(shell pkg-config --exists libdpdk && expr $(DPDK_VERSION) \>= 17),1)

all: shared
.PHONY: shared static
shared: build/$(APP)-shared
	ln -sf $(APP)-shared build/$(APP)
static: build/$(APP)-static
	ln -sf $(APP)-static build/$(APP)

PKGCONF ?= pkg-config

PC_FILE := $(shell $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += -O3 $(shell $(PKGCONF) --cflags libdpdk)
CFLAGS += "-DDPDK_VERSION=$(DPDK_VERSION)"
LDFLAGS_SHARED =$(shell $(PKGCONF) --libs libdpdk)
LDFLAGS_STATIC = -Wl,-Bstatic $(shell $(PKGCONF) --static --libs libdpdk)

build/$(APP)-shared: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_SHARED) -lm -g

build/$(APP)-static: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_STATIC) -lm -g

build:
	@mkdir -p $@

.PHONY: clean
clean:
	rm -f build/$(APP) build/$(APP)-static build/$(APP)-shared
	test -d build && rmdir -p build || true

else

ifeq ($(RTE_SDK),)
$(error "Please define RTE_SDK environment variable")
endif

include $(RTE_SDK)/mk/rte.vars.mk

CFLAGS += -O3
CFLAGS += $(WERROR_FLAGS)
CFLAGS += "-DDPDK_VERSION=$(DPDK_VERSION)"

include $(RTE_SDK)/mk/rte.extapp.mk

endif

