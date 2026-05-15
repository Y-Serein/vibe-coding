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
AIKB_VIDEO_FILE="/mnt/system/usr/share/aikb/video/asking.264"
AIKB_VIDEO_ASKING="${AIKB_VIDEO_ASKING:-/mnt/system/usr/share/aikb/video/asking.264}"
AIKB_VIDEO_RUNNING="${AIKB_VIDEO_RUNNING:-/mnt/system/usr/share/aikb/video/running.264}"
AIKB_VIDEO_VOICE="${AIKB_VIDEO_VOICE:-/mnt/system/usr/share/aikb/video/voice.264}"
AIKB_VIDEO_SLEEP="${AIKB_VIDEO_SLEEP:-/mnt/system/usr/share/aikb/video/sleep.264}"
AIKB_VIDEO_REJECT="${AIKB_VIDEO_REJECT:-/mnt/system/usr/share/aikb/video/reject.264}"
AIKB_VIDEO_SESSION="${AIKB_VIDEO_SESSION:-/mnt/system/usr/share/aikb/video/session.264}"
AIKB_VIDEO_REVIEW="${AIKB_VIDEO_REVIEW:-/mnt/system/usr/share/aikb/video/review.264}"
AIKB_VIDEO_VOTE_REVIEW="${AIKB_VIDEO_VOTE_REVIEW:-/mnt/system/usr/share/aikb/video/vote_review.264}"
AIKB_VIDEO_FIX="${AIKB_VIDEO_FIX:-/mnt/system/usr/share/aikb/video/fix.264}"
AIKB_VIDEO_COMMIT="${AIKB_VIDEO_COMMIT:-/mnt/system/usr/share/aikb/video/commit.264}"
AIKB_VIDEO_AGENT="${AIKB_VIDEO_AGENT:-/mnt/system/usr/share/aikb/video/agent.264}"
AIKB_VIDEO_AGENT_MODEL="${AIKB_VIDEO_AGENT_MODEL:-/mnt/system/usr/share/aikb/video/agent_model.264}"
AIKB_VIDEO_MULTI="${AIKB_VIDEO_MULTI:-/mnt/system/usr/share/aikb/video/multi.264}"
AIKB_VIDEO_BACK="${AIKB_VIDEO_BACK:-/mnt/system/usr/share/aikb/video/back.264}"
AIKB_VIDEO_MODE="${AIKB_VIDEO_MODE:-/mnt/system/usr/share/aikb/video/mode.264}"
AIKB_VIDEO_MENU_DEBUG="${AIKB_VIDEO_MENU_DEBUG:-/mnt/system/usr/share/aikb/video/menu_debug.264}"
AIKB_VIDEO_CONFIRM="${AIKB_VIDEO_CONFIRM:-/mnt/system/usr/share/aikb/video/confirm.264}"
AIKB_VIDEO_SELECT="${AIKB_VIDEO_SELECT:-${AIKB_VIDEO_CONFIRM}}"
AIKB_VIDEO_KEY0="${AIKB_VIDEO_KEY0:-${AIKB_VIDEO_REJECT}}"
AIKB_VIDEO_KEY1="${AIKB_VIDEO_KEY1:-${AIKB_VIDEO_VOICE}}"
AIKB_VIDEO_KEY2="${AIKB_VIDEO_KEY2:-${AIKB_VIDEO_SESSION}}"
AIKB_VIDEO_KEY3="${AIKB_VIDEO_KEY3:-${AIKB_VIDEO_VOTE_REVIEW}}"
AIKB_VIDEO_KEY4="${AIKB_VIDEO_KEY4:-${AIKB_VIDEO_AGENT_MODEL}}"
AIKB_VIDEO_KEY5="${AIKB_VIDEO_KEY5:-${AIKB_VIDEO_MULTI}}"
AIKB_VIDEO_KEY6="${AIKB_VIDEO_KEY6:-${AIKB_VIDEO_CONFIRM}}"
AIKB_VIDEO_KEY7="${AIKB_VIDEO_KEY7:-${AIKB_VIDEO_MENU_DEBUG}}"
AIKB_VIDEO_KO_LOADER="/mnt/system/ko/loadvideoko.sh"
AIKB_VIDEO_PANEL="GC9503_CXW034"
AIKB_VIDEO_CONTROLLER_PID="/tmp/aikb_video_controller.pid"
AIKB_VIDEO_CURRENT="/tmp/aikb_video_current"
AIKB_VIDEO_CTRL="/tmp/aikb_video_ctrl"

prepare_aikb_lcd_input()
{
   for f in "${AIKB_LCD_INPUT}" "${AIKB_LCD_CTRL}" "${AIKB_PET_EVENTS}" "${AIKB_VIDEO_CTRL}"; do
      if [ -e "$f" ] && [ ! -p "$f" ]; then
         rm -f "$f"
      fi
      if [ ! -p "$f" ]; then
         mkfifo "$f" 2>/dev/null || true
         chmod 600 "$f" 2>/dev/null || true
      fi
   done
}

start_aikb_fb_keeper()
{
   if [ ! -e /dev/fb0 ]; then
      return 0
   fi
   if [ -f /tmp/aikb_fb_keeper.pid ] &&
      kill -0 "$(cat /tmp/aikb_fb_keeper.pid)" >/dev/null 2>&1; then
      return 0
   fi
   (
      exec 9<>/dev/fb0 || exit 0
      while true; do
         sleep 65535
      done
   ) &
   echo "$!" >/tmp/aikb_fb_keeper.pid
}

stop_aikb_fb_keeper()
{
   if [ -f /tmp/aikb_fb_keeper.pid ]; then
      kill "$(cat /tmp/aikb_fb_keeper.pid)" >/dev/null 2>&1 || true
      rm -f /tmp/aikb_fb_keeper.pid
   fi
}

aikb_video_file_for_event()
{
   case "$1" in
      "KEY 0 DOWN")
         printf '%s\n' "${AIKB_VIDEO_KEY0}"
         ;;
      "KEY 1 DOWN")
         printf '%s\n' "${AIKB_VIDEO_KEY1}"
         ;;
      "KEY 2 DOWN")
         printf '%s\n' "${AIKB_VIDEO_KEY2}"
         ;;
      "KEY 3 DOWN")
         printf '%s\n' "${AIKB_VIDEO_KEY3}"
         ;;
      "KEY 4 DOWN")
         printf '%s\n' "${AIKB_VIDEO_KEY4}"
         ;;
      "KEY 5 DOWN")
         printf '%s\n' "${AIKB_VIDEO_KEY5}"
         ;;
      "KEY 6 DOWN")
         printf '%s\n' "${AIKB_VIDEO_KEY6}"
         ;;
      "KEY 7 DOWN")
         printf '%s\n' "${AIKB_VIDEO_KEY7}"
         ;;
      "ENC_BTN DOWN")
         printf '%s\n' "${AIKB_VIDEO_SELECT}"
         ;;
      *)
         printf '%s\n' ""
         ;;
   esac
}

launch_aikb_video_file()
{
   VIDEO_PLAYER="/mnt/system/usr/bin/sample_vdecvo"
   VIDEO_LOG="/tmp/aikb_video_player.log"
   VIDEO_FILE="$1"
   VIDEO_REASON="$2"

   if [ ! -f "${VIDEO_FILE}" ]; then
      echo "$(date '+%H:%M:%S') missing ${VIDEO_REASON} video: ${VIDEO_FILE}; fallback ${AIKB_VIDEO_FILE}" >> "${VIDEO_LOG}"
      VIDEO_FILE="${AIKB_VIDEO_FILE}"
   fi
   if [ ! -x "${VIDEO_PLAYER}" ]; then
      echo "$(date '+%H:%M:%S') sample_vdecvo not executable: ${VIDEO_PLAYER}" >> "${VIDEO_LOG}"
      return 1
   fi
   if [ ! -f "${VIDEO_FILE}" ]; then
      echo "$(date '+%H:%M:%S') video not found: ${VIDEO_FILE}" >> "${VIDEO_LOG}"
      return 1
   fi

   if pidof sample_vdecvo >/dev/null 2>&1 &&
      [ -f "${AIKB_VIDEO_CURRENT}" ] &&
      [ "$(cat "${AIKB_VIDEO_CURRENT}" 2>/dev/null)" = "${VIDEO_FILE}" ]; then
      echo "$(date '+%H:%M:%S') already playing ${VIDEO_REASON}: ${VIDEO_FILE}" >> "${VIDEO_LOG}"
      return 0
   fi

   if pidof sample_vdecvo >/dev/null 2>&1 && [ -p "${AIKB_VIDEO_CTRL}" ]; then
      if printf 'file %s\n' "${VIDEO_FILE}" > "${AIKB_VIDEO_CTRL}"; then
         printf '%s\n' "${VIDEO_FILE}" > "${AIKB_VIDEO_CURRENT}"
         echo "$(date '+%H:%M:%S') switch ${VIDEO_REASON}: ${VIDEO_FILE}" >> "${VIDEO_LOG}"
         return 0
      fi
      echo "$(date '+%H:%M:%S') video ctrl write failed; restart player" >> "${VIDEO_LOG}"
   fi

   killall sample_vdecvo >/dev/null 2>&1 || true
   if command -v usleep >/dev/null 2>&1; then
      for i in 1 2 3 4 5 6 7 8 9 10; do
         if ! pidof sample_vdecvo >/dev/null 2>&1; then
            break
         fi
         usleep 100000
      done
   fi
   echo "$(date '+%H:%M:%S') play ${VIDEO_REASON}: ${VIDEO_FILE}" >> "${VIDEO_LOG}"
   "${VIDEO_PLAYER}" 1 "${VIDEO_FILE}" </dev/null >> "${VIDEO_LOG}" 2>&1 &
   VIDEO_PID=$!
   if command -v usleep >/dev/null 2>&1; then
      usleep 200000
   fi
   if kill -0 "${VIDEO_PID}" >/dev/null 2>&1; then
      printf '%s\n' "${VIDEO_FILE}" > "${AIKB_VIDEO_CURRENT}"
      echo "$(date '+%H:%M:%S') sample_vdecvo pid=${VIDEO_PID}" >> "${VIDEO_LOG}"
      return 0
   fi
   echo "$(date '+%H:%M:%S') sample_vdecvo exited during startup" >> "${VIDEO_LOG}"
   return 1
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

   killall sample_vdecvo >/dev/null 2>&1 || true

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
   start_aikb_fb_keeper

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

start_aikb_video_player()
{
   VIDEO_LOG="/tmp/aikb_video_player.log"

   if pidof sample_vdecvo >/dev/null 2>&1; then
      echo "$(date '+%H:%M:%S') sample_vdecvo already running: $(pidof sample_vdecvo)" >> "${VIDEO_LOG}"
      return 0
   fi

   : > "${VIDEO_LOG}"
   echo "$(date '+%H:%M:%S') start sample_vdecvo ${AIKB_VIDEO_FILE}" >> "${VIDEO_LOG}"

   killall aikb_lcd_ui >/dev/null 2>&1 || true
   stop_aikb_fb_keeper
   rmmod soph_fb >/dev/null 2>&1 || true

   if [ -x "${AIKB_VIDEO_KO_LOADER}" ]; then
      echo "$(date '+%H:%M:%S') load video ko: ${AIKB_VIDEO_KO_LOADER}" >> "${VIDEO_LOG}"
      sh "${AIKB_VIDEO_KO_LOADER}" >> "${VIDEO_LOG}" 2>&1
   else
      echo "$(date '+%H:%M:%S') video ko loader not executable: ${AIKB_VIDEO_KO_LOADER}" >> "${VIDEO_LOG}"
   fi

   for i in 1 2 3 4 5; do
      if [ -e "/dev/mipi-tx" ]; then
         break
      fi
      sleep 1
   done

   if [ ! -e "/dev/mipi-tx" ]; then
      echo "$(date '+%H:%M:%S') /dev/mipi-tx not ready" >> "${VIDEO_LOG}"
      cat /proc/modules >> "${VIDEO_LOG}" 2>&1
      return 1
   fi

   echo "$(date '+%H:%M:%S') init dsi panel ${AIKB_VIDEO_PANEL}" >> "${VIDEO_LOG}"
   /mnt/system/usr/bin/sample_dsi --panel="${AIKB_VIDEO_PANEL}" >> "${VIDEO_LOG}" 2>&1 || {
      echo "$(date '+%H:%M:%S') sample_dsi failed" >> "${VIDEO_LOG}"
      return 1
   }

   launch_aikb_video_file "${AIKB_VIDEO_FILE}" "boot"
   if [ "$?" -eq 0 ]; then
      return 0
   fi
   start_aikb_fb_keeper
   return 1
}

start_aikb_video_controller()
{
   VIDEO_LOG="/tmp/aikb_video_player.log"

   prepare_aikb_lcd_input

   if [ -f "${AIKB_VIDEO_CONTROLLER_PID}" ] &&
      kill -0 "$(cat "${AIKB_VIDEO_CONTROLLER_PID}")" >/dev/null 2>&1; then
      echo "$(date '+%H:%M:%S') video controller already running: $(cat "${AIKB_VIDEO_CONTROLLER_PID}")" >> "${VIDEO_LOG}"
      return 0
   fi

   (
      while true; do
         while read EVENT_LINE; do
            VIDEO_FILE="$(aikb_video_file_for_event "${EVENT_LINE}")"
            if [ -n "${VIDEO_FILE}" ]; then
               echo "$(date '+%H:%M:%S') video event: ${EVENT_LINE}" >> "${VIDEO_LOG}"
               launch_aikb_video_file "${VIDEO_FILE}" "${EVENT_LINE}"
            fi
         done < "${AIKB_PET_EVENTS}"
         sleep 1
      done
   ) &
   echo "$!" > "${AIKB_VIDEO_CONTROLLER_PID}"
   echo "$(date '+%H:%M:%S') video controller pid=$!" >> "${VIDEO_LOG}"
}

# LCD first so the boot animation / splash blits before any visible green
# panel-init pattern. start_aikb_hid_input's internal `sleep 1` used to delay
# the LCD startup by ~1s, which was the visible green window.
case "${AIKB_VIEW:-video}" in
   video)
      if start_aikb_video_player; then
         start_aikb_video_controller
      else
         start_aikb_lcd_ui
      fi
      ;;
   *)
      start_aikb_lcd_ui
      ;;
esac
start_aikb_hid_input
