#!/bin/bash
# Stream a PC install's addon paks (every .pk3 but the retail and Trinity ones) into ~/.trinity/{baseq3,missionpack} on the Frame.
# Only paks the Frame lacks are sent, as one tar over ssh.sh: rsync with sshpass fails its password intermittently, a piped ssh does not.
# Usage: setsid -f misc/frame/sync-addon-paks.sh /mnt/d/Games/Trinity (detached, since it outlives a wsl.exe call); logs to ~/frame-sync-paks.log
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
exec > ~/frame-sync-paks.log 2>&1
cd "$1"
# name and size, so a pak cut short by an interrupted copy is sent again
"$HERE/ssh.sh" 'cd ~/.trinity && stat -c "%s %n" baseq3/*.pk3 missionpack/*.pk3 2>/dev/null; true' | sort -k2 > /tmp/frame-have
find baseq3 missionpack -maxdepth 1 -name '*.pk3' ! -name 'pak[0-8].pk3' ! -name 'pak8t.pk3' ! -name 'pak3t.pk3' \
	! -name 'zzz-trinity-announcer.pk3' ! -name 'zzz-trinity-bots.pk3' -printf '%s %p\n' | sort -k2 > /tmp/frame-want
comm -23 /tmp/frame-want /tmp/frame-have | cut -d' ' -f2- > /tmp/frame-missing
echo "$(wc -l < /tmp/frame-missing) paks to send, started $(date +%T)"
if [ -s /tmp/frame-missing ]; then
	tar -T /tmp/frame-missing -cf - | "$HERE/ssh.sh" 'tar -C ~/.trinity -xf - && chmod 644 ~/.trinity/baseq3/*.pk3 ~/.trinity/missionpack/*.pk3'
fi
echo "SYNCED $(date +%T)"
