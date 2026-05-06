#!/bin/sh
${CVI_SHOPTS}

export LD_LIBRARY_PATH="/lib:/lib/3rd:/lib/arm-linux-gnueabihf:/usr/lib:/usr/local/lib:/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:/mnt/data/lib"
export PATH="/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin:/mnt/system/usr/bin:/mnt/system/usr/sbin:/mnt/data/bin:/mnt/data/sbin"

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
   "${LCD_UI}" --fb /dev/fb0 --input /dev/ttyGS0 --rotate auto >> "${LCD_LOG}" 2>&1 &
   LCD_PID=$!
   sleep 1
   if kill -0 "${LCD_PID}" >/dev/null 2>&1; then
      echo "$(date '+%H:%M:%S') aikb_lcd_ui pid=${LCD_PID}" >> "${LCD_LOG}"
   else
      echo "$(date '+%H:%M:%S') aikb_lcd_ui exited during startup" >> "${LCD_LOG}"
   fi
}

start_aikb_lcd_ui
