
#!/usr/bin/env bash
# Re-exec with bash when invoked as "sh build.sh".
if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

if [ ! -f dist/ABL.efi ]; then
  echo "dist/ABL.efi not found. Run ./patch.sh first (or use ./auto.sh)." >&2
  exit 1
fi

xxd -i dist/ABL.efi > edk2/QcomModulePkg/Include/Library/ABL.h

if [ -d ./Conf ]; then
  rm -rf ./edk2/Conf
  cp -r ./Conf ./edk2/
else
  mkdir -p ./edk2/Conf
fi

cd edk2
source edksetup.sh
make BOARD_BOOTLOADER_PRODUCT_NAME=canoe TARGET_ARCHITECTURE=AARCH64 TARGET=RELEASE \
  CLANG_BIN=/usr/bin/ CLANG_PREFIX=aarch64-linux-gnu- VERIFIED_BOOT_ENABLED=1 \
  VERIFIED_BOOT_LE=0 AB_RETRYCOUNT_DISABLE=0 TARGET_BOARD_TYPE_AUTO=0 \
  BUILD_USES_RECOVERY_AS_BOOT=0 DISABLE_PARALLEL_DOWNLOAD_FLASH=0 PVMFW_BCC_ENABLED=-DPVMFW_BCC\
  REMOVE_CARVEOUT_REGION=1 QSPA_BOOTCONFIG_ENABLE=1 USER_BUILD_VARIANT=0 \
  PREBUILT_HOST_TOOLS="BUILD_CC=clang BUILD_CXX=clang++ LDPATH=-fuse-ld=lld BUILD_AR=llvm-ar"
cd ../
cp edk2/Build/RELEASE_CLANG35/AARCH64/LinuxLoader.efi ./dist/ABL_with_superfastboot.efi
if [ -f ./dist/patch_log.txt ]; then
  cat ./dist/patch_log.txt
fi
ls -l ./dist