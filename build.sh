#!/bin/bash
set -e

echo "⏰ Prepare timezone to Asia/Jakarta"
sudo rm -f /etc/localtime
sudo ln -s /usr/share/zoneinfo/Asia/Jakarta /etc/localtime

echo "📦 Install dependencies"
sudo apt update -y
sudo apt install -y bc cpio flex bison aptitude git python-is-python3 tar aria2 perl wget curl lz4 libssl-dev

echo "🔧 Clone Toolchains"
mkdir -p clang
cd clang
curl -LO "https://raw.githubusercontent.com/Neutron-Toolchains/antman/main/antman"
chmod +x antman
./antman -S
./antman --patch=glibc
cd ..

git clone --depth=1 -b main https://github.com/greenforce-project/gcc-arm64 gcc64
git clone --depth=1 -b main https://github.com/greenforce-project/gcc-arm gcc32

echo "⚙️ Setup environment variables"
export BUILD_TIME=$(TZ=Asia/Jakarta date '+%d%m%Y-%H%M')
export CLANG_PATH="$PWD/clang"
export GCC64_PATH="$PWD/gcc64"
export GCC32_PATH="$PWD/gcc32"

echo "📥 Clone kernelSU-Next"
curl -LSs "https://raw.githubusercontent.com/Mr-Morat/KernelSU-Next/susfs/kernel/setup.sh" | bash -s susfs

echo "🛠️ Build Kernel"
export ARCH=arm64
export PATH="$CLANG_PATH/bin:$GCC64_PATH/bin:$GCC32_PATH/bin:$PATH"
export KBUILD_BUILD_USER=Brutalist
export KBUILD_BUILD_HOST=MoratRealm
export KBUILD_COMPILER_STRING="$CLANG_PATH/clang"

make O=out ARCH=arm64 sweet_defconfig

make -j$(nproc --all) O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 CC=clang \
  CLANG_TRIPLE=$CLANG_PATH/aarch64-linux-gnu- \
  CROSS_COMPILE=$GCC64_PATH/bin/aarch64-elf- \
  CROSS_COMPILE_ARM32=$GCC32_PATH/bin/arm-eabi-

mv out/.config out/sweet_defconfig.txt

echo "💾 Prepare flashable zip with Anykernel3"
git clone --depth=1 -b main https://github.com/Mr-Morat/anykernel3 AnyKernel3

cp out/arch/arm64/boot/Image.gz AnyKernel3/Image.gz
cp out/arch/arm64/boot/dtbo.img AnyKernel3/dtbo.img
cp out/arch/arm64/boot/dtb.img AnyKernel3/dtb.img

cd AnyKernel3
zip -r "../TobrutExotic-AOSP-${BUILD_TIME}.zip" *

echo "✅ Build completed: TobrutExotic-AOSP-${BUILD_TIME}.zip"