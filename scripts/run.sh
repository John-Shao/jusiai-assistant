#!/bin/sh
# JuSi AI Assistant — RV1126B board launcher.
#
# Deployed next to the executable. The board's stock camera firmware holds
# the resources this app needs and actively defends them:
#
#   camera_core_d  — holds /dev/video-camera0 AND the ALSA capture device
#                    (/dev/snd/pcmC0D0c); runs the in-process ISP 3A.
#   camera_ui_d    — the stock on-screen UI.
#   watchdog_d     — restarts camera_core_d / camera_ui_d ~5 s after they die.
#
# A plain `kill` is futile: watchdog_d brings camera_core_d straight back and
# it re-grabs the microphone, so the AI never hears the user. We stop the
# whole stack through its init.d scripts, watchdog FIRST (its own script
# documents this order) — then the daemons stay down and a clean SIGTERM lets
# camera_core_d release the ISP / camera / ALSA gracefully.
DIR="$(cd "$(dirname "$0")" && pwd)"

stop_stock_stack() {
  # Order matters: stop the watchdog before its targets, else it restarts
  # them as we shut them down.
  [ -x /etc/init.d/S97watchdog ]      && /etc/init.d/S97watchdog stop
  [ -x /etc/init.d/S96camera-ui-d ]   && /etc/init.d/S96camera-ui-d stop
  [ -x /etc/init.d/S95camera-core-d ] && /etc/init.d/S95camera-core-d stop
  # Fallback for boards without the init.d scripts (same order).
  for d in watchdog_d camera_ui_d camera_core_d; do
    p=$(pidof "$d" 2>/dev/null)
    [ -n "$p" ] && kill $p 2>/dev/null && echo "[run] killed $d ($p)"
  done
  # Let camera_core_d's SIGTERM handler finish releasing ISP / VENC / ALSA.
  sleep 1
}

stop_stock_stack
echo "[run] stock camera stack stopped"

# A Wayland compositor (weston), if present, would hold the DRM/framebuffer.
wpid=$(pidof weston 2>/dev/null)
[ -n "$wpid" ] && kill $wpid 2>/dev/null && echo "[run] stopped weston"

# Stopping camera_core_d also stopped its in-process ISP 3A tuning. Start the
# standalone 3A server so the camera image is auto-exposed / white-balanced —
# without it the V4L2 frames come out dark grey.
if command -v rkaiq_3A_server >/dev/null 2>&1 \
   && ! pidof rkaiq_3A_server >/dev/null 2>&1; then
  rkaiq_3A_server >/dev/null 2>&1 &
  echo "[run] started rkaiq_3A_server"
  sleep 2
fi

# The ES8389 codec mic path (PGA / ADC volume / ALC-off) is configured by the
# app itself at start-up via libasound — no amixer needed.
exec "$DIR/jusiai-assistant" "$@"
