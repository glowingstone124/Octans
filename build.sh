#!/usr/bin/env bash
set -e
: "${LAMP_CLANG:?Error: LAMP_CLANG is not defined}"
: "${LAMP_LD:?Error: LAMP_LD is not defined}"

if ! command -v "$LAMP_CLANG" >/dev/null 2>&1; then
  echo "Error: LAMP_CLANG executable not found: $LAMP_CLANG"
  exit 1
fi

if ! command -v "$LAMP_LD" >/dev/null 2>&1; then
  echo "Error: LAMP_LD executable not found: $LAMP_LD"
  exit 1
fi

mkdir -p build-kernel
objects=()

for f in kernel/src/*.c kernel/src/net/*.c; do
  opt_level="${LAMP_KERNEL_OPT_LEVEL:--O2}"
  # Optimized user_exec.c currently makes the Lamp-target build return -EIO
  # while loading /bin/sh. Keep this one translation unit conservative until
  # the backend/IR issue is isolated; compiling the rest of the kernel at -O2
  # cuts boot-time instruction count sharply.
  if [[ "$f" == "kernel/src/user_exec.c" ]]; then
    opt_level="${LAMP_USER_EXEC_OPT_LEVEL:--O0}"
  fi
  object="build-kernel/$(basename "$f" .c).o"
  "$LAMP_CLANG" --target=lamp-unknown-unknown \
    -ffreestanding -fno-builtin -fno-stack-protector -fomit-frame-pointer \
    -fno-optimize-sibling-calls "$opt_level" \
    ${LAMP_KERNEL_CFLAGS:-} \
    -Ikernel/include -c "$f" \
    -o "$object"
  objects+=("$object")
done

"$LAMP_LD" -T kernel/linker.ld -e kernel_entry \
  "${objects[@]}" -o build-kernel/kernel.elf

test -f disk.img || truncate -s 512M disk.img

dd if=build-kernel/kernel.elf of=disk.img \
   bs=512 seek=1 conv=notrunc
