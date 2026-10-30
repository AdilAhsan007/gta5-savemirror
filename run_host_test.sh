#!/bin/bash
# Simulates a PS4 file tree under /tmp/fake and drives the real main.c logic.
set -e
cd "$(dirname "$0")"
${CC:-cc} -DHOST_TEST -I. -Wall -Wno-format-truncation -O1 -DROOT='"/tmp/fake"' -DPOLL_OVERRIDE=1 \
  -DSTABLE_IDLE_SECS=2 -DSTABLE_LIVE_SECS=3 -DKEEP_RECENT=3 -DMIN_FREE_MB=1 ../main.c -o host_savemirror
F=/tmp/fake; rm -rf $F
U=$F/user/home/1a2b3c4d/savedata/CUSA00419
mkdir -p $U $F/mnt/usb1/SaveMirror $F/data
touch $F/mnt/usb1/SaveMirror/ENABLE_SAVEMIRROR.txt
head -c 3000000 /dev/urandom > $U/sdimg_SGTA50000; head -c 96 /dev/urandom > $U/SGTA50000.bin
ORIG=$(cat $U/* | sha256sum)
./host_savemirror > out.log 2>&1 & PID=$!
sleep 5                                     # boot: idle baseline snapshot
mkdir -p $F/mnt/sandbox/CUSA00419_000/savedata0   # "game launched"
echo decrypted > $F/mnt/sandbox/CUSA00419_000/savedata0/SGTA50000
for i in 1 2 3 4 5; do                      # five "autosaves"
  head -c 3000000 /dev/urandom > $U/sdimg_SGTA50000; sleep 6
done
# a save that keeps changing every second must NOT be copied
for i in $(seq 1 6); do head -c 3000000 /dev/urandom > $U/sdimg_SGTA50000; sleep 1; done
rmdir_ok=$(rm -rf $F/mnt/sandbox/CUSA00419_000)   # "game closed"
sleep 5
# drive moves from usb1 to usb0 (re-plug / reboot)
mv $F/mnt/usb1 $F/mnt/usb0
head -c 3000000 /dev/urandom > $U/sdimg_SGTA50000; sleep 5
touch $F/mnt/usb0/SaveMirror/STOP.txt; sleep 2
wait $PID || true
echo "== notifications"; cat out.log
echo "== snapshots"; ls $F/mnt/usb0/SaveMirror/CUSA00419/1a2b3c4d/
echo "== one manifest"; cat "$(ls -d $F/mnt/usb0/SaveMirror/CUSA00419/1a2b3c4d/* | tail -1)/MANIFEST.txt"
echo "== verify every snapshot's files against its manifest"
for d in $F/mnt/usb0/SaveMirror/CUSA00419/1a2b3c4d/*/; do
  n=$(grep -c "^raw/" $d/MANIFEST.txt); m=$(ls $d/raw | wc -l); echo "$(basename $d): manifest=$n files=$m"
done
echo "== original untouched besides our simulated game writes:"; ls -la $U
