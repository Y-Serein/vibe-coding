#!/bin/sh
${CVI_SHOPTS}

KO_DIR="/mnt/system/ko"

load_mod()
{
	mod="$1"
	shift
	if [ ! -f "${KO_DIR}/${mod}.ko" ]; then
		echo "missing ${KO_DIR}/${mod}.ko"
		return 0
	fi
	insmod "${KO_DIR}/${mod}.ko" "$@" 2>/dev/null || true
}

# Video path needs VO/VDEC/MIPI TX, but deliberately skips soph_fb so
# framebuffer users do not keep the panel in the fb display path.
load_mod soph_sys
load_mod soph_base
load_mod soph_rtos_cmdqu
load_mod soph_fast_image
load_mod soph_mipi_rx
load_mod soph_snsr_i2c
load_mod soph_vi
load_mod soph_vpss
load_mod soph_dwa
load_mod soph_vo
load_mod soph_mipi_tx
load_mod soph_rgn

load_mod soph_clock_cooling
load_mod soph_tpu
load_mod soph_vcodec
load_mod soph_jpeg
load_mod soph_vc_driver MaxVencChnNum=9 MaxVdecChnNum=9
load_mod soph_ive

if [ -w /proc/sys/vm/drop_caches ]; then
	echo 3 >/proc/sys/vm/drop_caches
fi
dmesg -n 4 2>/dev/null || true

exit 0
