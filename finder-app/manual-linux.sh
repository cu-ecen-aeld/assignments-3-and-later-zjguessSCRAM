#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here (COMPLETED)
    #Completely cleans the kernel source tree. Ensures our kernel build is fully reproducible and note affected by previous builds.
    make ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE} mrproper
    
    #Generates a default kernel configuration for the aarch64-none-linux-gnus-modules
    make ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE} defconfig
    
    #Uncompressed ELF Kernel binary
    make -j4 ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE} all
    
    #Compiles kernel loadable modules
    #make ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE} modules
    
    #Builds device tree vinaries describing the hardware.
    make ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE} dtbs
    
fi

echo "Adding the Image in outdir"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
#bin: Essential Binaries: Contains essential user commands. BusyBox binaries will go here, so scrips and shell commands can execute. If missing, no shell, no commands, the system is effectively useless.
#dev: Device Nodes: Special files that represent devices. If missing, Kernel panic, or scripts fail if they attempt to write.
#etc: Configuration Files: Stores system-wide configuration files such as passwd or hostname. Scripts and apps read config. Scripts that rely on configuration files will fail
#home: User scripts and applications: Store your personal files, scripts and assignment programs such as finder.sh, writer, autorun-qemu.sh. Keeps user files seperate from system binaries. Applications may not run or be found by QEMU/init scripts.
#Lib & LIB64: Shared Libraries: This is where dynamically linked programs get their libraries so when BusyBox or your writer app is dynamically linked, they rely on shared libraries. Dynamic programs fail to execute with "cannot load shared library"
#proc: Kernel Info: Virtual filesystem exposing runtime kernel information. Multiple utilities in BusyBox read /proc. If missing, commands that rely on /proc will fail.
#sbin: System Binaries: Contains essential system administraion commands, like mount, ifconfig, init. Commands required to configure the system boot are stored here. If missing some scripts or kernel init may fail to find commands that are expected in /sbin
#sys: Device Information: Sysfs, exposes devices, drivers, and hardware info. Some scripts or tools query /sys/class for device info. 
#tmp: Temporary Files: Scratch place for scripts, programs, logs. Your scripts may create temporary files for intermediate data. Standard linux programs assume /tmp exists. Programs fail or cannot write temporary files.
#usr: Secondary binaries, libraries: Contains non essential user and system binaries, often "additional" utlities".Scripts that expect commands here will fail.
#var: Variable files and logs: Persistent runtime files. Logging fails if missing.

mkdir -p rootfs/{bin,dev,etc,home,lib,lib64,proc,sbin,sys,tmp,usr,var}
mkdir -p rootfs/usr/{bin,lib,sbin}
mkdir -p rootfs/var/log

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # TODO:  Configure busybox
    make distclean
    make defconfig
else
    cd busybox
fi

# TODO: Make and install busybox
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} CONFIG_PREFIX=${OUTDIR}/rootfs install

echo "Library dependencies"
${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library"

# TODO: Add library dependencies to rootfs
cd ${OUTDIR}/rootfs
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)
cp -a $SYSROOT/lib/ld-linux-aarch64.so.1 lib/
cp -a $SYSROOT/lib64/libc.so.6 lib64/
cp -a $SYSROOT/lib64/libm.so.5 lib64/
cp -a $SYSROOT/lib64/libresolv.so.2 lib64/

# TODO: Make device nodes
sudo mknod -m 666 dev/null c 1 3
sudo mknod -m 600 dev/console c 5 1

# TODO: Clean and build the writer utility
make clean
make CROSS_COMPILE=${CROSS_COMPILE}

# TODO: Copy the finder related scripts and executable to the /home directory on the target rootfs
mkdir -p ${OUTDIR}/rootfs/home
cp writer ${OUTDIR}/rootfs/home/
cp finder.sh finder-test.sh ${OUTDIR}/rootfs/home/
mkdir -p ${OUTDIR}/rootfs/home/conf
cp conf/username.txt conf/assignment.txt $OUTDIR/rootfs/home/conf/
cp autorun-qemo.sh ${OUTDIR}/rootfs/home/

# TODO: Chown the root directory
sudo chown -R root:root ${OUTDIR}/rootfs

# TODO: Create initramfs.cpio.gz
cd ${OUTDIR}/rootfs
find . | cpio -H newc -ov --owner root:root | gzip > ${OUTDIR}/initramfs.cpio.gz

exit 0
