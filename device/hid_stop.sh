#!/system/bin/sh
# Stop hidkbd and restore the original USB gadget configuration.
D=/data/local/tmp/hidkbd
G=/config/usb_gadget/g1
C=$G/configs/b.1
FFS=/dev/usb-ffs/hid
UDC_NAME=$(getprop sys.usb.controller); [ -n "$UDC_NAME" ] || UDC_NAME=$(ls /sys/class/udc | head -1)

echo "" > $G/UDC
for f in $C/f*; do [ -L $f ] && rm $f; done
if [ -f $D/orig.sh ]; then
  . $D/orig.sh
  for l in $LINKS; do ln -s $G/functions/${l#*:} $C/${l%%:*}; done
  echo $VID > $G/idVendor
  echo $PID > $G/idProduct
  echo "$CFG" > $C/strings/0x409/configuration
  rm $D/orig.sh
fi
p=$(cat $D/pid 2>/dev/null)
[ -n "$p" ] && grep -q hidkbd /proc/$p/cmdline 2>/dev/null && kill "$p"; rm -f $D/pid
echo $UDC_NAME > $G/UDC
sleep 0.2
umount $FFS 2>/dev/null
rmdir $G/functions/ffs.hid 2>/dev/null
echo "restored: $(cat $G/idVendor):$(cat $G/idProduct) $(ls $C | grep '^f' | tr '\n' ' ')"
