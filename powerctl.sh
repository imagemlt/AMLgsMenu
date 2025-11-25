#!/bin/sh

# Disable system autosleep if exposed
if [ -w /sys/power/autosleep ]; then
  echo "" > /sys/power/autosleep 2>/dev/null
fi

# Keep USB devices awake
if [ -w /sys/module/usbcore/parameters/autosuspend ]; then
  echo -1 > /sys/module/usbcore/parameters/autosuspend 2>/dev/null
fi
for f in /sys/bus/usb/devices/*/power/control; do
  [ -w "$f" ] && echo on > "$f" 2>/dev/null
done

# Set CPU governor to performance
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  [ -w "$g" ] && echo performance > "$g" 2>/dev/null
done

# Optionally disable deep C-states (uncomment if needed; increases power)
# for s in /sys/devices/system/cpu/cpuidle/state*/disable; do
#   [ -w "$s" ] && echo 1 > "$s" 2>/dev/null
# done

# Disable HDMI CEC sleep triggers if available
if [ -w /sys/class/amhdmitx/amhdmitx0/cec_config ]; then
  echo 0 > /sys/class/amhdmitx/amhdmitx0/cec_config 2>/dev/null
fi

# Disable Wi-Fi powersave (best-effort; may vary by driver)
if command -v iw >/dev/null 2>&1; then
  for dev in $(iw dev | awk '/Interface/ {print $2}'); do
    iw dev "$dev" set power_save off 2>/dev/null
  done
fi

exit 0
