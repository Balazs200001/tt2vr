#!/usr/bin/env bash
# Test-mode helper (PLAN Decisions log 2026-10-21): drive TT2 without the headset.
#   tools/tt2vr-test.sh launch           start TT2 through Steam with -tt2vr-test (simulated headset, virtual pad) and
#                                        wait until the mod answers remote commands
#   tools/tt2vr-test.sh cmd "<lines>"    send a command file (one command per line), wait for its .done, print it
#   tools/tt2vr-test.sh ride             from the main menu (Single-player highlighted): Time Attack, Isle of Man,
#                                        default bike, Start; ends riding in the helmet camera (about 50 s)
#   tools/tt2vr-test.sh restart          while riding: pause > Restart > Yes; back on the start line (about 20 s)
#   tools/tt2vr-test.sh quit [force]     close the game (WM_CLOSE, waits 30 s); force: kill it if it is still open
#                                        (never while the autosave icon shows: it can corrupt the save)
#   tools/tt2vr-test.sh window <png>     capture the game window as the desktop shows it (after the mirror), with the
#                                        window and monitor geometry (tools/tt2vr-window.ps1)
#   tools/tt2vr-test.sh running          exit status 0 while TT2.exe runs
# Needs TT2_GAME_DIR (as for the deploy target). Optional: STEAM_EXE, TT2VR_CMD_TIMEOUT (seconds, default 90).
set -euo pipefail

: "${TT2_GAME_DIR:?set TT2_GAME_DIR to the game folder}"
COMMANDS="$TT2_GAME_DIR/tt2vr/commands"
LOG="$TT2_GAME_DIR/tt2vr/logs/tt2vr.log"
APP_ID=1082180

running() { tasklist //FI "IMAGENAME eq TT2.exe" 2>/dev/null | grep -qi "TT2.exe"; }

steam_exe() {
    if [ -n "${STEAM_EXE:-}" ]; then echo "$STEAM_EXE"; return; fi
    reg query 'HKCU\Software\Valve\Steam' //v SteamExe 2>/dev/null | sed -n 's/.*REG_SZ *//p' | tr -d '\r'
}

# send <text> [timeout]: one request file, waits for <id>.done and prints it.
send() {
    local id="t$(date +%s%N)" timeout="${2:-${TT2VR_CMD_TIMEOUT:-90}}"
    mkdir -p "$COMMANDS"
    printf '%s\n' "$1" > "${COMMANDS:?}/${id:?}.tmp"
    mv "${COMMANDS:?}/${id:?}.tmp" "${COMMANDS:?}/${id:?}.txt"
    local i=0
    while [ ! -f "${COMMANDS:?}/${id:?}.done" ]; do
        if ! running; then echo "FAILED: TT2.exe is not running"; rm -f "${COMMANDS:?}/${id:?}.txt"; return 1; fi
        if [ $i -ge $((timeout * 4)) ]; then echo "FAILED: no answer after ${timeout} s"; return 1; fi
        sleep 0.25; i=$((i + 1))
    done
    cat "${COMMANDS:?}/${id:?}.done"
    rm -f "${COMMANDS:?}/${id:?}.done"
}

case "${1:-}" in
launch)
    if running; then echo "TT2.exe is already running"; exit 1; fi
    steam="$(steam_exe)"
    [ -n "$steam" ] || { echo "Steam not found (set STEAM_EXE)"; exit 1; }
    "$steam" -applaunch "$APP_ID" -tt2vr-test >/dev/null 2>&1 &
    for _ in $(seq 1 240); do running && break; sleep 0.5; done
    running || { echo "FAILED: TT2.exe did not start"; exit 1; }
    # The mod polls commands once its device exists; "camera" answers right away.
    send "camera" 180 >/dev/null || { echo "FAILED: the mod does not answer"; exit 1; }
    if grep -q "TEST MODE (-tt2vr-test)" "$LOG"; then echo "launched in test mode"; else
        echo "WARNING: launched, but the log has no TEST MODE line (was -tt2vr-test passed?)"; fi
    ;;
cmd)
    send "${2:?usage: cmd \"<lines>\"}" "${3:-}"
    ;;
ride)
    # Menu path measured 2026-10-21 (screens s03..s09): Single-player > Time Attack (2 down) > Isle of Man >
    # Snaefell settings > bike > loading (~20 s) > Start > riding (~12 s).
    send "pad a wait:2000 down wait:400 down wait:400 a wait:2500 a wait:2500 a wait:3000 a wait:20000 a wait:12000" 120
    send "screenshot ride"
    ;;
restart)
    send "pad start wait:1500 down wait:500 a wait:2000 right wait:400 a wait:15000" 60
    ;;
quit)
    running || { echo "not running"; exit 0; }
    taskkill //IM TT2.exe >/dev/null 2>&1 || true
    for _ in $(seq 1 60); do running || { echo "closed"; exit 0; }; sleep 0.5; done
    if [ "${2:-}" = "force" ]; then taskkill //F //IM TT2.exe >/dev/null 2>&1 || true; echo "killed"; exit 0; fi
    echo "still running after 30 s (WM_CLOSE ignored?); use the menus or: quit force"; exit 1
    ;;
window)
    powershell -NoProfile -ExecutionPolicy Bypass -File "$(dirname "$0")/tt2vr-window.ps1" "${2:?usage: window <png>}"
    ;;
running)
    running
    ;;
*)
    sed -n '2,14p' "$0"
    exit 2
    ;;
esac
