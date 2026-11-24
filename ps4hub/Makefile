# ---------------------------------------------------------------------------
# PS4Hub (CPAN00001) - OpenOrbis PS4 build, Windows host.
# Run from Git Bash / MSYS2 with GNU make:   make        (or ./build.sh)
# Everything comes from .toolchain/; OO_PS4_TOOLCHAIN is exported automatically.
# ---------------------------------------------------------------------------

TITLE       := PS4Hub
VERSION     := 1.27
TITLE_ID    := CPAN00001
CONTENT_ID  := IV0000-CPAN00001_00-CONTROLPANEL0000
OUTPUT_PKG  := ps4hub.pkg

TOOLCHAIN   := .toolchain/OpenOrbis/PS4Toolchain
LLVM        := .toolchain/llvm/bin
GOLDHEN     := .toolchain/goldhen_sdk
OOBIN       := $(TOOLCHAIN)/bin/windows

# create-fself / PkgTool look the toolchain up through this variable.
# PkgTool.Core targets .NET Core 3.0; allow it to run on a newer runtime.
export DOTNET_ROLL_FORWARD := Major
export OO_PS4_TOOLCHAIN := $(shell cygpath -w "$(abspath $(TOOLCHAIN))" 2>/dev/null || echo "$(abspath $(TOOLCHAIN))")

CC          := $(LLVM)/clang.exe
CXX         := $(LLVM)/clang++.exe
LD          := $(LLVM)/ld.lld.exe
PKGTOOL     := $(OOBIN)/PkgTool.Core.exe
CREATE_GP4  := $(OOBIN)/create-gp4.exe
CREATE_FSELF:= $(OOBIN)/create-fself.exe

BUILD       := build
ELF         := $(BUILD)/ps4hub.elf
OELF        := $(BUILD)/ps4hub.oelf

LIBS        := -lc -lkernel -lc++ -lSDL2 -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad \
               -lSceSysmodule -lSceSystemService -lSceNet -lSceHttp -lSceSsl

# Sources ---------------------------------------------------------------------
APP_SRC     := $(wildcard src/*.cpp)
IMGUI_SRC   := third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp \
               third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
SQLITE_SRC  := third_party/sqlite/sqlite3.c
GOLDHEN_SRC := $(GOLDHEN)/source/GoldHEN.c $(GOLDHEN)/source/Syscall.c

# ntfs-3g: only the files we need for read-only mounting + reading
NTFS_DIR    := third_party/ntfs-3g
NTFS_FILES  := acls attrib attrlist bitmap bootsect cache collate compat compress \
               debug device dir ea efs index inode ioctl lcnalloc logfile logging \
               mft misc mst object_id realpath reparse runlist security unistr \
               unix_io volume xattrs
NTFS_SRC    := $(addprefix $(NTFS_DIR)/libntfs-3g/,$(addsuffix .c,$(NTFS_FILES)))

OBJS        := $(patsubst src/%.cpp,$(BUILD)/app/%.o,$(APP_SRC)) \
               $(patsubst third_party/imgui/%.cpp,$(BUILD)/imgui/%.o,$(IMGUI_SRC)) \
               $(BUILD)/sqlite/sqlite3.o \
               $(patsubst $(GOLDHEN)/source/%.c,$(BUILD)/goldhen/%.o,$(GOLDHEN_SRC)) \
               $(patsubst $(NTFS_DIR)/libntfs-3g/%.c,$(BUILD)/ntfs/%.o,$(NTFS_SRC))

# Flags -----------------------------------------------------------------------
TARGET      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -march=btver2
SYSINC      := -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include
DEFS        := -D__PS4__=1 -DORBIS=1 -D__ORBIS__=1
INCS        := -Isrc -Ithird_party/imgui -Ithird_party/sqlite -I$(GOLDHEN)/include \
               -I$(NTFS_DIR)/include -I$(NTFS_DIR)

CFLAGS      := $(TARGET) $(SYSINC) $(DEFS) $(INCS) -O2 -c
CXXFLAGS    := $(TARGET) $(SYSINC) -isystem $(TOOLCHAIN)/include/c++/v1 $(DEFS) $(INCS) \
               -std=gnu++17 -O2 -Wall -Wno-unused-function -c \
               -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEBUG_TOOLS
SQLITE_DEFS := -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 \
               -DSQLITE_DQS=0 -DHAVE_USLEEP=1 -Wno-everything
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib

# Package contents --------------------------------------------------------------
SCE_FILES   := sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png
MODULES     := $(wildcard sce_module/*.prx)
ASSETS      := $(wildcard assets/fonts/*) $(wildcard assets/*.png) \
               $(shell find assets/autoloader -type f 2>/dev/null)

.PHONY: all clean eboot

all: $(OUTPUT_PKG)

$(OUTPUT_PKG): project.gp4
	$(PKGTOOL) pkg_build project.gp4 $(BUILD)
	mv -f $(BUILD)/$(CONTENT_ID).pkg $@
	@echo "Built $@"

# project.gp4 is maintained by hand (includes autoloader & PS4Webkit assets
# with their full sub-directory tree).  Only regenerate when it does not exist.
project.gp4: eboot.bin $(SCE_FILES) $(MODULES) $(ASSETS)
	@if [ ! -f $@ ]; then \
	  $(CREATE_GP4) -out $@ --content-id=$(CONTENT_ID) --files "eboot.bin $(SCE_FILES) $(MODULES) $(ASSETS)"; \
	fi

eboot: eboot.bin

eboot.bin: $(ELF)
	$(CREATE_FSELF) -in=$(ELF) -out=$(OELF) --eboot eboot.bin --paid 0x3800000000000011

$(ELF): $(OBJS)
	$(LD) $(LDFLAGS) $(TOOLCHAIN)/lib/crt1.o $(OBJS) $(LIBS) -o $@

sce_sys/param.sfo: Makefile
	$(PKGTOOL) sfo_new $@
	$(PKGTOOL) sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(PKGTOOL) sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(PKGTOOL) sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 256
	$(PKGTOOL) sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(PKGTOOL) sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(PKGTOOL) sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(PKGTOOL) sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(PKGTOOL) sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

# Only generated when missing, so a custom icon is never overwritten.
sce_sys/icon0.png:
	python tools/make_icon.py $@

# Object rules ------------------------------------------------------------------
$(BUILD)/app/%.o: src/%.cpp $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BUILD)/imgui/%.o: third_party/imgui/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BUILD)/sqlite/sqlite3.o: $(SQLITE_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SQLITE_DEFS) -o $@ $<

$(BUILD)/goldhen/%.o: $(GOLDHEN)/source/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=c11 -D__BSD_VISIBLE=1 -D_BSD_SOURCE=1 -o $@ $<

NTFS_DEFS   := -DHAVE_CONFIG_H -include $(NTFS_DIR)/ps4_config.h -I$(NTFS_DIR)/include/ntfs-3g \
               -D__BSD_VISIBLE=1 -D_BSD_SOURCE=1 -DNTFS_DISABLE_DEBUG_LOGGING -Wno-everything

$(BUILD)/ntfs/%.o: $(NTFS_DIR)/libntfs-3g/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=gnu99 $(NTFS_DEFS) -o $@ $<

clean:
	rm -rf $(BUILD) eboot.bin project.gp4 $(OUTPUT_PKG) sce_sys/param.sfo
