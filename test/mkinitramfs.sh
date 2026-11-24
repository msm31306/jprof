#!/bin/sh
# Build a tiny busybox initramfs containing the module and test binaries.
set -e
KDIR=$1; OUT=$2; R=$(mktemp -d)
mkdir -p $R/bin $R/proc $R/sys $R/dev $R/tmp
cp /usr/bin/busybox $R/bin/busybox
for a in sh mount insmod rmmod ls cat sleep kill dmesg grep head poweroff echo; do ln -s busybox $R/bin/$a; done
cp ../kernel/jprof.ko ../demo/demo jprof_test $R/
cp init.sh $R/init; chmod +x $R/init
(cd $R && find . | cpio -o -H newc 2>/dev/null | gzip) > $OUT
rm -rf $R
