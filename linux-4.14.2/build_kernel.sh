#cp config_for_iTop-4412_scp .config
export ARCH=arm

#make iTop-4412_scp_defconfig

make uImage LOADADDR=0x40007000 -j8

make dtbs
