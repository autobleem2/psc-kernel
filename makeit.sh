export  ARCH=arm
export  KBUILD_DEFCONFIG=autobleem_defconfig
export  CROSS_COMPILE=arm-unknown-linux-gnueabihf-


# Build kernel
#make clean
make defconfig KBUILD_DEFCONFIG="${KBUILD_DEFCONFIG}"
make oldconfig
make menuconfig
make prepare

make
make modules_install
make firmware_install
#    - "# Package with U-Boot"
#    - cd "arch/${ARCH}/boot"
#    - lz4 -c1 Image Image.lz4
#    - cp "${CI_PROJECT_DIR}/sup_files/orig.dtb" .
#    - cp "${CI_PROJECT_DIR}/sup_files/kernel_fdt_${ARCH}.its" .
#    - mkimage -f "kernel_fdt_${ARCH}.its" "kernel_${ARCH}.itb"
#    - mkdir -p "${CI_PROJECT_DIR}/build_output"
#    - cp "kernel_${ARCH}.itb" "${CI_PROJECT_DIR}/build_output"
#    - mv "kernel_${ARCH}.itb" boot.img
#    - "# Generate signed package"
#    - zip LBOOT.ZIP boot.img
#    - mkdir "${CI_PROJECT_DIR}/build_output/dev" "${CI_PROJECT_DIR}/build_output/release"
#    - /opt/UpgradeSign/build/UpgradeSign sign LBOOT.ZIP "${CI_PROJECT_DIR}/build_output/dev/LBOOT.EPB" "${SIGNING_KEY_PATH_DEBUG}"
#    - /opt/UpgradeSign/build/UpgradeSign sign LBOOT.ZIP "${CI_PROJECT_DIR}/build_output/release/LBOOT.EPB" "${SIGNING_KEY_PATH_RELEASE}"
#    - md5sum -t boot.img > "${CI_PROJECT_DIR}/build_output/release/project_eris_kernel_v${CI_PIPELINE_IID}.md5"
#    - mv boot.img "${CI_PROJECT_DIR}/build_output/release/project_eris_kernel_v${CI_PIPELINE_IID}.img"

