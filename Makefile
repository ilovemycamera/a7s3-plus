# Cross-build Makefile for a7s3-plus ARM64 camera modules (Sony A7S III FW 5.01)

CLANG   ?= $(shell command -v /opt/homebrew/opt/llvm/bin/clang || command -v clang)
CLANGXX ?= $(shell command -v /opt/homebrew/opt/llvm/bin/clang++ || command -v clang++)
LLD     ?= $(shell command -v /opt/homebrew/opt/lld/bin/ld.lld || command -v /opt/homebrew/bin/ld.lld || command -v ld.lld)

TARGET_FLAGS = --target=aarch64-linux-gnu -fuse-ld=$(LLD) -shared -fPIC -O2 -nostdlib -mno-outline-atomics
CXXFLAGS     = $(TARGET_FLAGS) -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables

CORE_TARGETS = \
	runtime/cp_opengate_patch.so \
	runtime/menu_repair.so \
	runtime/high_res_display_v10.so \
	runtime/big6_key_hook.so \
	runtime/og_menu_loader.so \
	runtime/og_menu_daemon_v15.so \
	runtime/og_3840x2560_patch.so \
	runtime/og_3840x2560_v2.so \
	runtime/ulog_spy \
	runtime/persistent_preload.so \
	runtime/persistent_preload_v2.so \
	runtime/libcinema_worker.so \
	runtime/libjit_warm.so

.PHONY: all core doom clean

all: core doom

core: $(CORE_TARGETS)

runtime/cp_opengate_patch.so: runtime/cp_opengate_patch.cpp runtime/cp_og_patches.h
	$(CLANGXX) $(CXXFLAGS) -Wl,-init,_init $< -o $@

runtime/menu_repair.so: runtime/menu_repair.cpp
	$(CLANGXX) $(CXXFLAGS) -Wl,-init,_init $< -o $@

runtime/high_res_display_v10.so: runtime/high_res_display_v10.cpp
	$(CLANGXX) $(CXXFLAGS) -Wl,-init,_init $< -o $@

runtime/big6_key_hook.so: runtime/big6_key_hook.cpp runtime/big6_key_hook.s
	$(CLANGXX) $(CXXFLAGS) -Wl,-soname,big6_key_hook.so -Wl,-init,_init runtime/big6_key_hook.cpp runtime/big6_key_hook.s -o $@

runtime/og_menu_loader.so: runtime/og_menu_loader.cpp
	$(CLANGXX) $(CXXFLAGS) -Wl,-init,_init $< -o $@

runtime/og_menu_daemon_v15.so: runtime/og_menu_daemon.cpp runtime/cp_og_patches.h runtime/og_2560_patches.h
	$(CLANGXX) $(CXXFLAGS) -fno-builtin -Wl,-soname,libOgMenuDaemon.so -Wl,-init,_init -Wl,--unresolved-symbols=ignore-all $< -o $@

runtime/og_3840x2560_patch.so: runtime/og_3840x2560_patch.cpp runtime/og_2560_patches.h
	$(CLANGXX) $(CXXFLAGS) -Wl,-init,_init $< -o $@

runtime/og_3840x2560_v2.so: runtime/og_3840x2560_v2.cpp runtime/og_2560_patches.h
	$(CLANGXX) $(CXXFLAGS) -fno-builtin -Wl,-init,_init $< -o $@

runtime/ulog_spy: runtime/ulog_spy.cpp
	$(CLANGXX) --target=aarch64-linux-gnu -fuse-ld=$(LLD) -static -O2 -nostdlib -mno-outline-atomics -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables -fno-builtin -Wl,-e,_start $< -o $@

runtime/persistent_preload.so: runtime/persistent_preload.c
	$(CLANG) $(TARGET_FLAGS) -ffreestanding -fno-builtin -Wl,-soname,libcinema.so -Wl,-init,_init -Wl,-z,nodelete -Wl,--allow-shlib-undefined $< -o $@

runtime/persistent_preload_v2.so: runtime/persistent_preload_v2.c
	$(CLANG) $(TARGET_FLAGS) -ffreestanding -fno-builtin -Wl,--hash-style=both -Wl,-soname,libcinema_preload.so -Wl,-init,_init -Wl,-z,nodelete $< -o $@

runtime/libcinema_worker.so: runtime/cinema_worker.c
	$(CLANG) $(TARGET_FLAGS) -ffreestanding -fno-builtin -Wl,--hash-style=both -Wl,--allow-shlib-undefined -Wl,-soname,libcinema_worker.so -Wl,-init,_init -Wl,-z,nodelete $< -o $@

runtime/libjit_warm.so: runtime/libjit_warm.c
	$(CLANG) $(TARGET_FLAGS) -ffreestanding -fno-builtin -Wl,--hash-style=both -Wl,--allow-shlib-undefined -Wl,-init,_init $< -o $@

doom:
	CLANG="$(CLANG)" LLD="$(LLD)" bash runtime/build_doom.sh

clean:
	rm -rf runtime/doom_build runtime/*.o runtime/doom_a7s3.so
