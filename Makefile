# oe-client: Linux build of the developer/QA pieces (the Windows build is vs2022/oe.sln).
#
#   make            build/oe-headless, the QVM tools, build/oe/vm/oegame.qvm, build/oe2/vm/oegame.qvm
#                   and the native modules build/oe/oegamex86_64.so, build/oe2/oegamex86_64.so
#   make client     build/oe (SDL2 + OpenGL window; needs sdl2-config and the GL headers)
#
# oe-headless runs the game modules without a window or sound for the QA bots and golden traces of
# oe-re / oe2-re (OE_PIPE, OE_KEYS, OE_SHOT, ...): see README-OE.md.

MAKEFLAGS += -r
.SUFFIXES:

B       ?= build
CC      ?= gcc
CFLAGS  ?= -O2 -g
WARN     = -Wall -Wno-unused -Wno-format-truncation -Wno-stringop-truncation
ARCH    := $(shell uname -m)
ENGFLAGS = $(CFLAGS) $(WARN) -fno-strict-aliasing -DNO_VM_COMPILED -DUSE_LOCAL_HEADERS \
           -DARCH_STRING=\"$(ARCH)\" -DDLL_EXT=\".so\" -Isrc/zlib
GAMEFLAGS = $(CFLAGS) -fPIC -funsigned-char -fwrapv -std=gnu11 -fvisibility=hidden -Isrc/cgame_common

QCOMMON  = cmd common cvar files ioapi md4 md5 puff q_math q_shared unzip vm vm_interpreted
ZLIB     = adler32 crc32 inffast inflate inftrees zutil
OE       = oe_audio oe_bar oe_files oe_host oe_pmd oe_qcelp oe_res
HEADLESS_SRC = $(QCOMMON:%=src/qcommon/%.c) $(ZLIB:%=src/zlib/%.c) $(OE:%=src/oe/%.c) \
           src/sys/con_tty.c src/sys/con_log.c src/sys/sys_main.c src/sys/sys_unix.c src/null/cl_null.c
CLIENT_SRC = $(QCOMMON:%=src/qcommon/%.c) $(ZLIB:%=src/zlib/%.c) $(OE:%=src/oe/%.c) \
           $(wildcard src/renderer/*.c) src/client/cl_console.c src/client/cl_keys.c src/client/cl_main.c \
           src/client/cl_scrn.c src/client/cl_hw.c src/client/cl_dsmap.c src/client/cl_ui.c src/sdl/sdl_gamma.c src/sdl/sdl_glimp.c src/sdl/sdl_input.c \
           src/sys/con_tty.c src/sys/con_log.c src/sys/sys_main.c src/sys/sys_unix.c \
           $(addprefix src/jpeg-6b/,jcapimin.c jchuff.c jcinit.c jccoefct.c jccolor.c jfdctflt.c jcdctmgr.c \
             jcphuff.c jcmainct.c jcmarker.c jcmaster.c jcomapi.c jcparam.c jcprepct.c jcsample.c jdapimin.c \
             jdapistd.c jdatasrc.c jdcoefct.c jdcolor.c jddctmgr.c jdhuff.c jdinput.c jdmainct.c jdmarker.c \
             jdmaster.c jdpostct.c jdsample.c jdtrans.c jerror.c jidctflt.c jmemmgr.c jmemnobs.c jutils.c)

GAME_COMMON = src/cgame_common/oe_game.c src/cgame_common/brew_shim.c src/cgame_common/oe_hw.c src/cgame_common/oe_options.c src/cgame_common/oe_export.c src/cgame_common/moddata.c

all: headless tools qvm so

headless: $(B)/oe-headless
$(B)/oe-headless: $(HEADLESS_SRC)
	@mkdir -p $(B)
	$(CC) $(ENGFLAGS) -DDEDICATED -DOE_NO_AUDIO_DEVICE -o $@ $^ -ldl -lm -rdynamic

client: $(B)/oe
$(B)/oe: $(CLIENT_SRC)
	@mkdir -p $(B)
	$(CC) $(ENGFLAGS) -DUSE_ICON $(shell sdl2-config --cflags) -o $@ $^ $(shell sdl2-config --libs) -lGL -ldl -lm

so: $(B)/oe/oegame$(ARCH).so $(B)/oe2/oegame$(ARCH).so $(B)/base/ui$(ARCH).so
$(B)/base/ui$(ARCH).so: $(wildcard src/ui/*.c src/ui/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(GAMEFLAGS) -shared -o $@ src/ui/ui_main.c src/ui/ui_menu.c src/ui/ui_parse.c src/ui/ui_syscalls.c -lm
	@mkdir -p $(B)/base/ui && cp base/ui/* $(B)/base/ui/
$(B)/oe/oegame$(ARCH).so: $(GAME_COMMON) src/cgame_common/oe_syscalls.c $(wildcard src/cgame_oe/*.c)
	@mkdir -p $(dir $@)
	$(CC) $(GAMEFLAGS) -shared -o $@ $(GAME_COMMON) src/cgame_common/oe_syscalls.c src/cgame_oe/*.c -lm
$(B)/oe2/oegame$(ARCH).so: $(GAME_COMMON) src/cgame_common/oe_syscalls.c $(wildcard src/cgame_oe2/*.c)
	@mkdir -p $(dir $@)
	$(CC) $(GAMEFLAGS) -shared -o $@ $(GAME_COMMON) src/cgame_common/oe_syscalls.c src/cgame_oe2/*.c -lm

# --- QVM tools and QVMs ---------------------------------------------------------
T = $(B)/tools
tools: $(T)/q3lcc $(T)/q3rcc $(T)/q3cpp $(T)/q3asm
$(T)/lburg: src/tools/lcc/lburg/lburg.c src/tools/lcc/lburg/gram.c
	@mkdir -p $(T)
	$(CC) -O2 -w -Isrc/tools/lcc/lburg -o $@ $^
$(T)/dagcheck.c: src/tools/lcc/src/dagcheck.md $(T)/lburg
	$(T)/lburg $< $@
$(T)/q3rcc: $(wildcard src/tools/lcc/src/*.c) $(T)/dagcheck.c
	@mkdir -p $(T)
	$(CC) -O2 -w -Isrc/tools/lcc/src -o $@ $^
$(T)/q3cpp: $(addprefix src/tools/lcc/cpp/,cpp.c lex.c nlist.c tokens.c macro.c eval.c include.c hideset.c getopt.c unix.c)
	@mkdir -p $(T)
	$(CC) -O2 -w -o $@ $^
$(T)/q3lcc: src/tools/lcc/etc/lcc.c src/tools/lcc/etc/bytecode.c
	@mkdir -p $(T)
	$(CC) -O2 -w -o $@ $^
$(T)/q3asm: src/tools/asm/q3asm.c src/tools/asm/cmdlib.c
	@mkdir -p $(T)
	$(CC) -O2 -w -o $@ $^

LCCFLAGS = -Wf-unsigned_char=1 -DQ3_VM -Dinline= -I../cgame_common/vmlibc -I../cgame_common
qvm: $(B)/oe/vm/oegame.qvm $(B)/oe2/vm/oegame.qvm $(B)/base/vm/ui.qvm
QVM_DEPS = $(T)/q3lcc $(T)/q3rcc $(T)/q3cpp $(T)/q3asm $(GAME_COMMON) src/cgame_common/vm_libc.c src/cgame_common/oe_syscalls.asm
$(B)/oe/vm/oegame.qvm: $(QVM_DEPS) $(wildcard src/cgame_oe/*.c src/cgame_oe/*.h)
$(B)/oe2/vm/oegame.qvm: $(QVM_DEPS) $(wildcard src/cgame_oe2/*.c src/cgame_oe2/*.h)
$(B)/%/vm/oegame.qvm:
	@mkdir -p $(dir $@) $(B)/obj/qvm/$*
	@set -e; files=""; cd src/cgame_$*; \
	for f in ../cgame_common/oe_game.c *.c ../cgame_common/brew_shim.c ../cgame_common/oe_hw.c ../cgame_common/oe_options.c ../cgame_common/oe_export.c ../cgame_common/moddata.c ../cgame_common/vm_libc.c; do \
		o=$(CURDIR)/$(B)/obj/qvm/$*/$$(basename $$f .c).asm; \
		$(CURDIR)/$(T)/q3lcc $(LCCFLAGS) -o $$o $$f 2>&1 | grep -v warning || true; \
		test -s $$o; files="$$files $$o"; \
	done; \
	$(CURDIR)/$(T)/q3asm -o $(CURDIR)/$@ $$files ../cgame_common/oe_syscalls.asm

$(B)/base/vm/ui.qvm: $(T)/q3lcc $(T)/q3rcc $(T)/q3cpp $(T)/q3asm $(wildcard src/ui/*.c src/ui/*.h) src/ui/ui_syscalls.asm src/cgame_common/vm_libc.c
	@mkdir -p $(dir $@) $(B)/obj/qvm/ui
	@set -e; files=""; cd src/ui; \
	for f in ui_main.c ui_menu.c ui_parse.c ../cgame_common/vm_libc.c; do \
		o=$(CURDIR)/$(B)/obj/qvm/ui/$$(basename $$f .c).asm; \
		$(CURDIR)/$(T)/q3lcc $(LCCFLAGS) -o $$o $$f 2>&1 | grep -v warning || true; \
		test -s $$o; files="$$files $$o"; \
	done; \
	$(CURDIR)/$(T)/q3asm -o $(CURDIR)/$@ $$files ui_syscalls.asm

clean:
	rm -rf $(B)/oe-headless $(B)/oe $(B)/tools $(B)/obj $(B)/oe/vm $(B)/oe2/vm $(B)/oe/*.so $(B)/oe2/*.so $(B)/base/vm $(B)/base/*.so

.PHONY: all headless client so tools qvm clean
