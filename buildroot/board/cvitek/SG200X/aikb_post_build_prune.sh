#!/bin/sh
set -eu

TARGET_DIR="${1:?target directory required}"

rm_rf()
{
	for path in "$@"; do
		rm -rf "${TARGET_DIR}${path}"
	done
}

rm_glob()
{
	for pattern in "$@"; do
		for path in ${TARGET_DIR}${pattern}; do
			[ -e "$path" ] || continue
			rm -rf "$path"
		done
	done
}

# AIKB runtime is C/busybox based. These SDK/demo/debug payloads dominate
# the rootfs but are not used by /mnt/system/auto.sh.
rm_rf \
	/usr/lib/python3.11 \
	/usr/lib/qt \
	/usr/lib/metatypes \
	/usr/share/gdb \
	/usr/share/vim \
	/usr/share/cursors \
	/usr/share/fonts \
	/usr/share/icons \
	/usr/share/applications \
	/usr/share/pixmaps \
	/usr/share/misc \
	/etc/udev/hwdb.bin \
	/etc/udev/hwdb.d

rm_glob \
	/usr/bin/python \
	/usr/bin/python3 \
	/usr/bin/python3.* \
	/usr/bin/ipython \
	/usr/bin/ipython3 \
	/usr/bin/gdb \
	/usr/bin/gdbserver \
	/usr/bin/vim \
	/usr/bin/*.cvimodel \
	/usr/bin/nn_* \
	/usr/bin/ffmpeg \
	/usr/bin/ffprobe \
	/usr/bin/stress-ng \
	/usr/bin/strace \
	/usr/bin/tcpdump \
	/usr/lib/libpython3.11* \
	/usr/lib/libQt5* \
	/usr/lib/libopencv_* \
	/usr/lib/libavcodec* \
	/usr/lib/libavfilter* \
	/usr/lib/libavformat* \
	/usr/lib/libavutil* \
	/usr/lib/libswresample* \
	/usr/lib/libswscale*
