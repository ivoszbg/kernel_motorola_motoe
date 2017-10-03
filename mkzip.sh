#!/bin/bash

cat ./arch/arm/boot/zImage ./arch/arm/boot/msm8610-condor-p2b.dtb > ./clarity-N-condor/boot/zImage-dtb

# get modules into one place
find -name "*.ko" -exec cp {} ./clarity-N-condor/system/lib/modules \;
sleep 2

cd ./clarity-N-condor

DATE=`date +%d-%m-%Y`;
zip -r ../release/Clarity-N-$DATE.zip *

