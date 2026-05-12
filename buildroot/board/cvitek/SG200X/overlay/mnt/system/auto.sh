#!/bin/sh
${CVI_SHOPTS}

export LD_LIBRARY_PATH="/lib:/lib/3rd:/lib/arm-linux-gnueabihf:/usr/lib:/usr/local/lib:/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:/mnt/data/lib"
export PATH="/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin:/mnt/system/usr/bin:/mnt/system/usr/sbin:/mnt/data/bin:/mnt/data/sbin"
AIKB_LCD_INPUT="/tmp/aikb_lcd_ui.in"
AIKB_LCD_CTRL="/tmp/aikb_lcd_ui.ctrl"
AIKB_PET_EVENTS="/tmp/aikb_pet_events.in"
AIKB_LCD_SPLASH="/mnt/system/usr/share/aikb/splash.argb"
AIKB_LCD_BOOT_ANIM="/mnt/system/usr/share/aikb/boot_anim.bin"
AIKB_LCD_WAIT_ANIM="/mnt/system/usr/share/aikb/wait_cycle.bin"

prepare_aikb_lcd_input()
{
   for f in "${AIKB_LCD_INPUT}" "${AIKB_LCD_CTRL}" "${AIKB_PET_EVENTS}"; do
      if [ -e "$f" ] && [ ! -p "$f" ]; then
         rm -f "$f"
      fi
      if [ ! -p "$f" ]; then
         mkfifo "$f" 2>/dev/null || true
         chmod 600 "$f" 2>/dev/null || true
      fi
   done
}

start_aikb_hid_input()
{
   HID_INPUT="/mnt/system/usr/bin/aikb_hid_input"
   HID_LOG="/tmp/aikb_hid_input.log"

   if pidof aikb_hid_input >/dev/null 2>&1; then
      echo "$(date '+%H:%M:%S') aikb_hid_input already running: $(pidof aikb_hid_input)" >> "${HID_LOG}"
      return 0
   fi

   : > "${HID_LOG}"
   echo "$(date '+%H:%M:%S') start aikb_hid_input" >> "${HID_LOG}"

   if [ ! -x "${HID_INPUT}" ]; then
      echo "$(date '+%H:%M:%S') aikb_hid_input not executable: ${HID_INPUT}" >> "${HID_LOG}"
      return 0
   fi

   prepare_aikb_lcd_input

   "${HID_INPUT}" --hid /dev/hidg0 --screen-out "${AIKB_LCD_INPUT}" --ctrl-out "${AIKB_LCD_CTRL}" --event-out "${AIKB_PET_EVENTS}" >> "${HID_LOG}" 2>&1 &
   HID_PID=$!
   sleep 1
   if kill -0 "${HID_PID}" >/dev/null 2>&1; then
      echo "$(date '+%H:%M:%S') aikb_hid_input pid=${HID_PID}" >> "${HID_LOG}"
   else
      echo "$(date '+%H:%M:%S') aikb_hid_input exited during startup" >> "${HID_LOG}"
   fi
}

start_aikb_lcd_ui()
{
   LCD_UI="/mnt/system/usr/bin/aikb_lcd_ui"
   LCD_LOG="/tmp/aikb_lcd_ui.log"

   if pidof aikb_lcd_ui >/dev/null 2>&1; then
      echo "$(date '+%H:%M:%S') aikb_lcd_ui already running: $(pidof aikb_lcd_ui)" >> "${LCD_LOG}"
      return 0
   fi

   : > "${LCD_LOG}"
   echo "$(date '+%H:%M:%S') aikb auto.sh start" >> "${LCD_LOG}"

   if [ ! -f "/tmp/evb_init" ]; then
      echo 1 > /tmp/evb_init
   else
      echo "$(date '+%H:%M:%S') /tmp/evb_init already exists; continue" >> "${LCD_LOG}"
   fi

   if [ ! -x "${LCD_UI}" ]; then
      echo "$(date '+%H:%M:%S') aikb_lcd_ui not executable: ${LCD_UI}" >> "${LCD_LOG}"
      return 0
   fi

   prepare_aikb_lcd_input

   if [ ! -e "/dev/fb0" ] && [ -f "/mnt/system/ko/loadsystemko.sh" ]; then
      echo "$(date '+%H:%M:%S') /dev/fb0 missing; retry loadsystemko.sh" >> "${LCD_LOG}"
      sh /mnt/system/ko/loadsystemko.sh >> "${LCD_LOG}" 2>&1
   fi

   for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 \
            16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
      if [ -e "/dev/fb0" ]; then
         break
      fi
      sleep 1
   done

   if [ ! -e "/dev/fb0" ]; then
      echo "$(date '+%H:%M:%S') /dev/fb0 not ready" >> "${LCD_LOG}"
      cat /proc/fb >> "${LCD_LOG}" 2>&1
      cat /proc/modules >> "${LCD_LOG}" 2>&1
      return 0
   fi

   echo "$(date '+%H:%M:%S') start aikb_lcd_ui; fb=$(cat /proc/fb 2>/dev/null)" >> "${LCD_LOG}"
   LCD_SPLASH_ARG=""
   if [ -f "${AIKB_LCD_SPLASH}" ]; then
      LCD_SPLASH_ARG="--splash ${AIKB_LCD_SPLASH}"
   fi
   LCD_BOOT_ANIM_ARG=""
   if [ -f "${AIKB_LCD_BOOT_ANIM}" ]; then
      LCD_BOOT_ANIM_ARG="--boot-anim ${AIKB_LCD_BOOT_ANIM}"
   fi
   LCD_WAIT_ANIM_ARG=""
   if [ -f "${AIKB_LCD_WAIT_ANIM}" ]; then
      LCD_WAIT_ANIM_ARG="--wait-anim ${AIKB_LCD_WAIT_ANIM}"
   fi
   LCD_VIEW="pet"
   LCD_EVENT_ARG="--event-input ${AIKB_PET_EVENTS}"
   if [ "${AIKB_VIEW}" = "terminal" ] || [ "${AIKB_VIEW}" = "dashboard" ]; then
      LCD_VIEW="${AIKB_VIEW}"
      LCD_EVENT_ARG=""
   else
      LCD_VIEW="pet"
      LCD_EVENT_ARG="--event-input ${AIKB_PET_EVENTS}"
      LCD_BOOT_ANIM_ARG=""
      LCD_WAIT_ANIM_ARG=""
      LCD_SPLASH_ARG=""
   fi
   "${LCD_UI}" --fb /dev/fb0 --input "${AIKB_LCD_INPUT}" --ctrl "${AIKB_LCD_CTRL}" ${LCD_EVENT_ARG} ${LCD_BOOT_ANIM_ARG} ${LCD_WAIT_ANIM_ARG} ${LCD_SPLASH_ARG} --rotate auto --view "${LCD_VIEW}" >> "${LCD_LOG}" 2>&1 &
   LCD_PID=$!
   sleep 1
   if kill -0 "${LCD_PID}" >/dev/null 2>&1; then
      echo "$(date '+%H:%M:%S') aikb_lcd_ui pid=${LCD_PID}" >> "${LCD_LOG}"
   else
      echo "$(date '+%H:%M:%S') aikb_lcd_ui exited during startup" >> "${LCD_LOG}"
   fi
}

# LCD first so the boot animation / splash blits before any visible green
# panel-init pattern. start_aikb_hid_input's internal `sleep 1` used to delay
# the LCD startup by ~1s, which was the visible green window.
start_aikb_lcd_ui
start_aikb_hid_input
