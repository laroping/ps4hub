#!/usr/bin/env bash
# Builds ps4hub.pkg. Uses GNU make when available; otherwise runs the
# same steps as the Makefile directly (Git Bash on Windows ships without make).
set -euo pipefail
cd "$(dirname "$0")"

if command -v make >/dev/null 2>&1; then
    exec make "$@"
fi

TITLE="PS4Hub"; VERSION="1.29"; TITLE_ID="CPAN00001"
CONTENT_ID="IV0000-CPAN00001_00-CONTROLPANEL0000"
TC=.toolchain/OpenOrbis/PS4Toolchain; LLVM=.toolchain/llvm/bin; GH=.toolchain/goldhen_sdk
OOBIN=$TC/bin/windows; BUILD=build
export DOTNET_ROLL_FORWARD=Major   # PkgTool targets .NET Core 3.0; run it on any newer runtime
export OO_PS4_TOOLCHAIN="$(cygpath -w "$PWD/$TC" 2>/dev/null || echo "$PWD/$TC")"
mkdir -p $BUILD/app $BUILD/imgui $BUILD/sqlite $BUILD/goldhen $BUILD/ntfs
NTFS_DIR=third_party/ntfs-3g

if [[ "${1:-}" == "clean" ]]; then
    rm -rf $BUILD eboot.bin project.gp4 ps4hub.pkg sce_sys/param.sfo; exit 0
fi

TARGET="--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -march=btver2"
SYSINC="-isysroot $TC -isystem $TC/include"
DEFS="-D__PS4__=1 -DORBIS=1 -D__ORBIS__=1"
INCS="-Isrc -Ithird_party/imgui -Ithird_party/sqlite -I$GH/include -I$NTFS_DIR/include -I$NTFS_DIR"
CFLAGS="$TARGET $SYSINC $DEFS $INCS -O2 -c"
CXXFLAGS="$TARGET $SYSINC -isystem $TC/include/c++/v1 $DEFS $INCS -std=gnu++17 -O2 -Wall -Wno-unused-function -c -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEBUG_TOOLS"
SQLITE_DEFS="-DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DHAVE_USLEEP=1 -Wno-everything"

newer() { [[ ! -f "$2" || "$1" -nt "$2" ]]; }

OBJS=()
for f in src/*.cpp; do
    o=$BUILD/app/$(basename "${f%.cpp}").o; OBJS+=("$o")
    if newer "$f" "$o" || [[ -n "$(find src -name '*.h' -newer "$o" 2>/dev/null)" ]]; then
        echo "CXX $f"; $LLVM/clang++.exe $CXXFLAGS -o "$o" "$f"
    fi
done
for n in imgui imgui_draw imgui_tables imgui_widgets; do
    o=$BUILD/imgui/$n.o; OBJS+=("$o")
    newer third_party/imgui/$n.cpp "$o" && { echo "CXX $n.cpp"; $LLVM/clang++.exe $CXXFLAGS -o "$o" third_party/imgui/$n.cpp; }
done
o=$BUILD/sqlite/sqlite3.o; OBJS+=("$o")
newer third_party/sqlite/sqlite3.c "$o" && { echo "CC  sqlite3.c"; $LLVM/clang.exe $CFLAGS $SQLITE_DEFS -o "$o" third_party/sqlite/sqlite3.c; }
for n in GoldHEN Syscall; do
    o=$BUILD/goldhen/$n.o; OBJS+=("$o")
    newer $GH/source/$n.c "$o" && { echo "CC  $n.c"; $LLVM/clang.exe $CFLAGS -std=c11 -D__BSD_VISIBLE=1 -D_BSD_SOURCE=1 -o "$o" $GH/source/$n.c; }
done

NTFS_DEFS="-DHAVE_CONFIG_H -include $NTFS_DIR/ps4_config.h -I$NTFS_DIR/include/ntfs-3g -D__BSD_VISIBLE=1 -D_BSD_SOURCE=1 -DNTFS_DISABLE_DEBUG_LOGGING -Wno-everything"
for n in acls attrib attrlist bitmap bootsect cache collate compat compress debug device dir ea efs index inode ioctl lcnalloc logfile logging mft misc mst object_id realpath reparse runlist security unistr unix_io volume xattrs; do
    src=$NTFS_DIR/libntfs-3g/$n.c; o=$BUILD/ntfs/$n.o; OBJS+=("$o")
    newer "$src" "$o" && { echo "CC  ntfs-3g/$n.c"; $LLVM/clang.exe $CFLAGS -std=gnu99 $NTFS_DEFS -o "$o" "$src"; }
done

echo "LD  ps4hub.elf"
$LLVM/ld.lld.exe -m elf_x86_64 -pie --script $TC/link.x --eh-frame-hdr -L$TC/lib $TC/lib/crt1.o "${OBJS[@]}" \
    -lc -lkernel -lc++ -lSDL2 -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad -lSceSysmodule -lSceSystemService -lSceNet -lSceHttp -lSceSsl \
    -o $BUILD/ps4hub.elf

$OOBIN/create-fself.exe -in=$BUILD/ps4hub.elf -out=$BUILD/ps4hub.oelf --eboot eboot.bin --paid 0x3800000000000011

[[ -f sce_sys/icon0.png ]] || python tools/make_icon.py sce_sys/icon0.png

P=$OOBIN/PkgTool.Core.exe; SFO=sce_sys/param.sfo
$P sfo_new $SFO
$P sfo_setentry $SFO APP_TYPE --type Integer --maxsize 4 --value 1
$P sfo_setentry $SFO APP_VER --type Utf8 --maxsize 8 --value "$VERSION"
$P sfo_setentry $SFO ATTRIBUTE --type Integer --maxsize 4 --value 256
$P sfo_setentry $SFO CATEGORY --type Utf8 --maxsize 4 --value gd
$P sfo_setentry $SFO CONTENT_ID --type Utf8 --maxsize 48 --value "$CONTENT_ID"
$P sfo_setentry $SFO DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
$P sfo_setentry $SFO SYSTEM_VER --type Integer --maxsize 4 --value 0
$P sfo_setentry $SFO TITLE --type Utf8 --maxsize 128 --value "$TITLE"
$P sfo_setentry $SFO TITLE_ID --type Utf8 --maxsize 12 --value "$TITLE_ID"
$P sfo_setentry $SFO VERSION --type Utf8 --maxsize 8 --value "$VERSION"

# project.gp4 is maintained by hand (includes autoloader & PS4Webkit assets
# with their full sub-directory tree).  Only regenerate when it does not exist.
if [[ ! -f project.gp4 ]]; then
    AL_FILES="$(find assets/autoloader -type f 2>/dev/null | tr '\n' ' ')"
    FILES="eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png $(ls sce_module/*.prx | tr '\n' ' ')$(ls assets/fonts/* | tr '\n' ' ')assets/PS4Webkit.png ${AL_FILES}"
    $OOBIN/create-gp4.exe -out project.gp4 --content-id=$CONTENT_ID --files "$FILES"
fi
$P pkg_build project.gp4 $BUILD
mv -f $BUILD/$CONTENT_ID.pkg ps4hub.pkg
ls -l ps4hub.pkg
