#!/system/bin/sh
# Switch USB gadget to "HID keyboard + adb" and start hidkbd.
# usage: su -c sh hid_start.sh
D=/data/local/tmp/hidkbd
G=/config/usb_gadget/g1
C=$G/configs/b.1
FFS=/dev/usb-ffs/hid
UDC_NAME=$(getprop sys.usb.controller); [ -n "$UDC_NAME" ] || UDC_NAME=$(ls /sys/class/udc | head -1)
# input device that has KEY_VOLUMEDOWN (abort button)
KEYDEV=/dev/input/$(grep -A4 -E 'Name="[^"]*gpio[-_]keys?"' /proc/bus/input/devices | grep -o 'event[0-9]*' | head -1)

# pid file may be stale (e.g. after reboot): check that the pid really is hidkbd
running() { p=$(cat $D/pid 2>/dev/null); [ -n "$p" ] && grep -q hidkbd /proc/$p/cmdline 2>/dev/null; }
hid_active() { [ -L $C/f1 ] && [ "$(basename "$(readlink $C/f1)")" = ffs.hid ] && [ -n "$(cat $G/UDC)" ]; }

if running; then
  if hid_active; then echo "already running (pid $(cat $D/pid)); USB is in HID mode"; exit 0; fi
  echo "hidkbd running but USB is not in HID mode: restarting"
  kill "$(cat $D/pid)"; sleep 0.3
fi
rm -f $D/pid

# remember original gadget state (not when the gadget is already ours)
if [ ! -f $D/orig.sh ] && [ "$(cat $G/idVendor)" != 0x1d6b ]; then
  {
    echo "VID=$(cat $G/idVendor)"
    echo "PID=$(cat $G/idProduct)"
    echo "CFG='$(cat $C/strings/0x409/configuration)'"
    echo "LINKS='$(for f in $C/f*; do [ -L $f ] && echo "$(basename $f):$(basename "$(readlink $f)")"; done | tr '\n' ' ')'"
  } > $D/orig.sh
fi

mkdir -p $G/functions/ffs.hid $FFS
grep -q " $FFS functionfs" /proc/mounts || mount -t functionfs hid $FFS || exit 1
[ -p $D/ctl ] || mkfifo $D/ctl

$D/hidkbd $FFS $D/ctl $D/status $KEYDEV > $D/log 2>&1 &
echo $! > $D/pid
i=0
while ! grep -q ready $D/log; do
  i=$((i+1)); [ $i -gt 50 ] && { echo "hidkbd failed:"; cat $D/log; exit 1; }
  sleep 0.1
done

echo "" > $G/UDC
for f in $C/f*; do [ -L $f ] && rm $f; done
ln -s $G/functions/ffs.hid $C/f1
ln -s $G/functions/ffs.adb $C/f2
echo 0x1d6b > $G/idVendor
echo 0x0104 > $G/idProduct
echo "hid_adb" > $C/strings/0x409/configuration
echo $UDC_NAME > $G/UDC
echo "started (abort key device: $KEYDEV)"
