#!/bin/sh -e

size_append(){
  printf "%.8x" "$(stat -c "%s" "$1")" | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/g' | xxd -r -p
}

rm -rf Image.lz4
rm -rf Image
cp ../arch/arm/boot/Image ./

lz4 -lf9 "Image" "Image.lz4" 
size_append "Image" >> "Image.lz4"
mkimage -f kernel.its boot.img
