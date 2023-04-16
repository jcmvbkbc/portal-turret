#!/bin/bash -ex

IMAGE=image.fat
SRC=audio
CODE=sw/idf/examples/turret

[ -d $SRC -a -d $CODE ] || exit 1
DST=`mktemp -d`
( cd $SRC ; find -type d ) | ( cd "$DST" ; xargs mkdir -p )
( cd $CODE ; git grep -h -o '/audio/.*mp3' | sort -u | sed 's,/audio/,,' ) | xargs -I{} ffmpeg -i $SRC/\{} -ar 22050 -f s8 "$DST/{}.s8"
rm -f $IMAGE
dd if=/dev/zero of=$IMAGE bs=4096 count=$(( 40 + `du -B 4096 -s "$DST" | cut -f 1` * 3 / 2 ))
/sbin/mkfs.vfat -S 4096 $IMAGE
TMP=`mktemp -d`
sudo mount $IMAGE "$TMP"
tar -C "$DST" -c . | sudo tar -x --no-same-owner --no-same-permissions -C "$TMP"
sudo umount "$TMP"
rm -rf "$DST"
rm -rf "$TMP"

#find -type f -name '*.mp3' -print0 | xargs -0 -I{} ffmpeg -i \{} -ar 22050 -f s8 \{}.s8
# parttool.py -p /dev/ttyUSB0 write_partition --partition-name storage --input image.fat
