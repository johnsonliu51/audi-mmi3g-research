#!/bin/sh
# Build with GNU cc1/as/ld components; no qcc license alteration.
# QNX_HOST and QNX_TARGET must reference an installed/supplied SH SDK.
set -eu
: "${QNX_HOST:?Set QNX_HOST}"
: "${QNX_TARGET:?Set QNX_TARGET}"
source_root=${1:?source root required}
output=${2:?output directory required}
mkdir -p "$output"
cc="$QNX_HOST/usr/lib/gcc-lib/sh-unknown-nto-qnx6.3.0/3.3.5/cc1"
assembler="$QNX_HOST/usr/bin/sh-unknown-nto-qnx6.3.0-as"
linker="$QNX_HOST/usr/bin/sh-unknown-nto-qnx6.3.0-ld"
gccdir="$QNX_HOST/usr/lib/gcc-lib/sh-unknown-nto-qnx6.3.0/3.3.5"
libdir="$QNX_TARGET/shle/lib"
compile() {
    "$cc" -quiet -m4 -ml -fno-delayed-branch -std=c99 -Wall -W -Werror -O2 \
      -nostdinc -D__LANGUAGE_C -D_LANGUAGE_C -D__QNX__ -D__QNXNTO__ \
      -D__GNUC__=3 -D__GNUC_MINOR__=3 -D__GNUC_PATCHLEVEL__=5 \
      -D__unix__ -D__unix -D__ELF__ -D__SH__ -D__SH4__ -D__sh__ -D__LITTLEENDIAN__ \
      -I"$gccdir/include" -I"$QNX_TARGET/usr/include" -I"$source_root/src" \
      "$1" -o "$output/$2.s"
    "$assembler" -little "$output/$2.s" -o "$output/$2.o"
}
for module in iap2_csm ncm_ntb16 usbmux_codec lockdown_channel avc_nal usbmux_session usbmux_host iap2_link; do
    compile "$source_root/src/protocol/$module.c" "$module"
done
rm -f "$output/libmmi_carplay_core.a"
"$QNX_HOST/usr/bin/ntomulti-ar" rcs "$output/libmmi_carplay_core.a" \
  "$output/iap2_csm.o" "$output/ncm_ntb16.o" "$output/usbmux_codec.o" \
  "$output/lockdown_channel.o" "$output/avc_nal.o" "$output/usbmux_session.o" \
  "$output/usbmux_host.o" "$output/iap2_link.o"
compile "$source_root/src/qnx_runtime_check.c" qnx_runtime_check
"$linker" -b elf32-shl -u_start --small-stubs -z now -EL -m shlelf_nto \
  --dynamic-linker /usr/lib/ldqnx.so.2 -nostdlib \
  -rpath-link "$libdir:$QNX_TARGET/shle/usr/lib" \
  "$libdir/crt1.o" "$libdir/crti.o" "$libdir/crtbegin.o" \
  "$output/qnx_runtime_check.o" "$output/iap2_link.o" \
  "$gccdir/libgcc.a" "$libdir/libc.so.2" -dn -Bstatic "$libdir/libc.a" \
  "$gccdir/libgcc.a" "$libdir/crtend.o" "$libdir/crtn.o" \
  -o "$output/mmi-runtime-check"
echo 'Built QNX SH candidate; target ABI/runtime and CarPlay are NOT validated.'
