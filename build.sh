#!/bin/bash
make clean
make mrproper
make cm_condor_defconfig
make CONFIG_NO_ERROR_ON_MISMATCH=y -j$(nproc --all)
