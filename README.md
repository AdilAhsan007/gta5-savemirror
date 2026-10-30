# GTA V Save Mirror (gta5_savemirror.elf)

Offline, automatic, read-only backups of GTA V (CUSA00419) saves to your
exFAT USB drive, for GoldHEN v2.4b18.10+ (you have v2.4b18.12, FW 13.52).

Status, honestly:
- Compiled with the ps4-payload-dev SDK (git master, Sep 2026, which carries
  13.50/13.52 kernel offsets). The included .elf is a real build.
- The backup logic passed a simulated end-to-end test on Linux
  (test/run_host_test.sh).
- It has NOT been run on a real PS4 yet. Do the safe test below first.

## What it does
Every 15 s it looks at /user/home/<user>/savedata/CUSA00419/ (the raw,
encrypted save files). When they have changed and then stayed unchanged for
30 s (game closed) or 90 s (game running), it copies them to
<USB>/SaveMirror/CUSA00419/<user>/<date>_<time>_<idle|live>/raw/,
checks each copy byte-for-byte via CRC32, re-checks the source did not change
during the copy, writes MANIFEST.txt, and only then renames the folder from
.partial to its final name. It never writes to the console's save folder,
never touches PS4/APOLLO, and never reuses a folder name.

While the game runs it also tries to copy the decrypted files the game has
mounted (/mnt/sandbox/CUSA00419_000/savedata0) into decrypted/. That part is
best effort and unverified.

Retention: newest 40 snapshots + newest snapshot of each of the last 90 days.
Stops writing if the drive has < 2 GB free.

## Install (one time)
1. On the Mac, copy usb_template/SaveMirror/ to the ROOT of the exFAT drive
   (so the drive has SaveMirror/ENABLE_SAVEMIRROR.txt). Only a drive with this
   file is ever written to.
2. Copy gta5_savemirror.elf to /data/payloads/ on the PS4 (GoldHEN FTP, port
   2121, or an on-console file manager). Create the folder if missing.
3. Safe test first (step "Test" below), then enable AutoRun: GoldHEN menu ->
   Utilities -> PayLoader LaunchPad -> highlight gta5_savemirror.elf -> Square.
   That writes /data/GoldHEN/payloads.ini (see payloads.ini.example).
   GoldHEN -> Server Settings -> PayLoader must be enabled.

## Test
1. Launch it once from the LaunchPad (do not add to AutoRun yet).
   Expect: "GTA V Save Mirror running -> /mnt/usbN".
2. Wait about a minute without starting GTA V. Expect "GTA V save backed up
   (game closed)" and a folder ..._idle under SaveMirror/CUSA00419/<user>/.
3. Play until GTA V autosaves; about 90 s later expect "GTA V save backed up".
4. On the Mac open SaveMirror/savemirror.log and one MANIFEST.txt.
   File sizes in MANIFEST must match the files in raw/.
5. Only then add it to AutoRun.

## Stop / uninstall
- Instant stop: put an empty STOP.txt in SaveMirror/ on the USB (it exits
  within 15 s; delete STOP.txt afterwards or it will stop again next boot).
- Hide notifications: put an empty QUIET.txt in SaveMirror/.
- Uninstall: LaunchPad -> remove from AutoRun (or delete the line in
  /data/GoldHEN/payloads.ini), delete /data/payloads/gta5_savemirror.elf and
  /data/savemirror/. Your backups on USB stay where they are.

## Restoring (manual, deliberate)
Raw snapshots restore only to the SAME console and SAME user. With GTA V
closed, first take an Apollo backup of the current save, then copy the files
from a snapshot's raw/ back into /user/home/<user>/savedata/CUSA00419/
(GoldHEN FTP). Prefer an ..._idle snapshot. This restore path is untested;
your Apollo "Copy save game to USB" backups remain the verified way back.

## Build it yourself (macOS)
./build_mac.sh   (Homebrew llvm@18 + ps4-payload-dev SDK release zip)
License: GPLv3+ (because the SDK runtime is GPLv3+).
