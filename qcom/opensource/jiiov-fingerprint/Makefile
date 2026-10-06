# SPDX-License-Identifier: GPL-2.0-only
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
M ?= $(CURDIR)

.PHONY: all modules modules_install clean
all: modules

modules clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) INSTALL_MOD_STRIP=1 $@
