#!/bin/bash
set -e
DEV=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
CLANG=$DEV/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang
SDK=${SDK:-$HOME/Source/MacKernelSDK}
BID=org.local.XHCIPeek; VER=1.0.0
OUT=$PWD/build/XHCIPeek.kext
rm -rf build; mkdir -p build "$OUT/Contents/MacOS"
COMMON="-arch x86_64 -mmacosx-version-min=12.0 -mkernel -fno-builtin -fno-common -fno-exceptions -fno-rtti
  -fno-asynchronous-unwind-tables -fno-stack-protector -Os -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
  -nostdinc -I$SDK/Headers -isystem $SDK/Headers"
"$CLANG"++ $COMMON -fapple-kext -std=gnu++14 -nostdinc++ -Wno-unknown-warning-option -c XHCIPeek.cpp -o build/XHCIPeek.o
cat > build/kmod_info.c <<KMOD
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($BID, "$VER", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
KMOD
"$CLANG" $COMMON -c build/kmod_info.c -o build/kmod_info.o
"$CLANG"++ -arch x86_64 -mmacosx-version-min=12.0 -nostdlib -Xlinker -kext -Xlinker -export_dynamic -Xlinker -no_data_const -static \
  -L"$SDK/Library/x86_64" build/XHCIPeek.o build/kmod_info.o -lkmod -o "$OUT/Contents/MacOS/XHCIPeek"
cp Info.plist "$OUT/Contents/Info.plist"
echo built
