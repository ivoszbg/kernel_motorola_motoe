#!/bin/bash
make clean
make mrproper
make condor_clarity_defconfig
make CONFIG_NO_ERROR_ON_MISMATCH=y -j$(nproc --all)
