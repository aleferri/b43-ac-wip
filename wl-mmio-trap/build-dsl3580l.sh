PATH="/opt/toolchains/uclibc-crosstools-gcc-4.4.2-1/usr/bin/:$PATH"
KDIR=/home/alessio/DSL-3580_EU_1.00_10232013_GPL/bcm963xx_router/kernel/linux/

make KDIR=$KDIR ARCH=mips CROSS_COMPILE=mips-linux-
