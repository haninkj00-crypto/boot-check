NEW_IMG='boot-check.img'
PART="$HOME/START_MBR.img@@1048576"

mcopy -o -i "$NEW_IMG" ::START.COM /tmp/START.COM
mcopy -o -i "$PART" /tmp/START.COM ::START.COM
rm -f /tmp/START.COM

TMP=$(mktemp)
dd if=/dev/zero of="$TMP" bs=61440 count=1 status=none
mcopy -o -i "$PART" "$TMP" ::RESULT.TXT
rm -f "$TMP"

qemu-system-i386 \
  -drive file="$HOME/START_MBR.img",format=raw,if=ide,index=0 \
  -boot c \
  -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0

mtype -i "$PART" ::RESULT.TXT
