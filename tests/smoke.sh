#!/bin/sh
# deskctl smoke test -- proves screenshot, pointer and keyboard really work.
#
# Needs a running Hyprland (or wlroots) session. Exits 77 (skip) when there is
# no usable graphical session, so it is safe in CI.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

skip() { printf 'SKIP: %s\n' "$1"; exit 77; }
fail() { printf 'FAIL: %s\n' "$1"; exit 1; }
pass() { printf 'ok   %s\n' "$1"; }

[ -x build/wayinput ] || fail "build/wayinput missing, run make first"
[ -n "${WAYLAND_DISPLAY:-}" ] || skip "no Wayland session"
[ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ] || skip "not Hyprland"

TMP="$ROOT/.tmp"
mkdir -p "$TMP"

# ---- 1. output geometry -------------------------------------------------
screen=$(./build/wayinput screen) || fail "wayinput screen failed"
case "$screen" in
    [0-9]*x[0-9]*) pass "screen geometry $screen" ;;
    *) fail "unexpected screen geometry: $screen" ;;
esac
w=${screen%x*}; h=${screen#*x}
[ "$w" -gt 0 ] && [ "$h" -gt 0 ] || fail "non-positive screen size"

# ---- 2. pointer ---------------------------------------------------------
./build/wayinput move 12 34 || fail "pointer move failed"
sleep 0.3
pos=$(hyprctl cursorpos | tr -d ' ')
[ "$pos" = "12,34" ] || fail "cursor at $pos, expected 12,34"
pass "pointer moves to absolute coordinates"

# ---- 3. keyboard, inside a throwaway window -----------------------------
RESULTS="$TMP/typed.txt"
SPAWN="$TMP/smoke-spawn.sh"
rm -f "$RESULTS"
cat > "$SPAWN" <<EOF
#!/bin/sh
alacritty --class DeskctlSmoke -e sh -c 'IFS= read -r line; printf "%s" "\$line" > "$RESULTS"; sleep 20'
EOF
chmod +x "$SPAWN"

cleanup() {
    hyprctl dispatch 'hl.dsp.window.close({ window = "class:DeskctlSmoke" })' >/dev/null 2>&1 || true
    hyprctl dispatch 'killwindow class:DeskctlSmoke' >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

if hyprctl eval "hl.exec_cmd(\"$SPAWN\")" >/dev/null 2>&1; then
    :
elif hyprctl dispatch exec "$SPAWN" >/dev/null 2>&1; then
    :
else
    skip "cannot spawn a test window (hyprctl exec unsupported)"
fi

i=0
while [ $i -lt 50 ]; do
    if ./deskctl windows 2>/dev/null | grep -q DeskctlSmoke; then break; fi
    i=$((i + 1)); sleep 0.2
done
[ $i -lt 50 ] || fail "test window never appeared"

./deskctl focus DeskctlSmoke >/dev/null || fail "could not focus test window"
./deskctl type --window DeskctlSmoke "smoke: Hello, World! 42" >/dev/null || fail "typing failed"
./deskctl key --window DeskctlSmoke Return >/dev/null || fail "Return failed"
sleep 0.8

[ -f "$RESULTS" ] || fail "test window received no input"
got=$(cat "$RESULTS")
want="smoke: Hello, World! 42"
[ "$got" = "$want" ] || fail "typed '$got', expected '$want'"
pass "keyboard types text (shift, symbols, space) into the focused window"

# ---- 4. screenshot ------------------------------------------------------
./deskctl shot -o "$TMP/smoke.png" | grep -q '"ok": true' || fail "screenshot failed"
[ -s "$TMP/smoke.png" ] || fail "screenshot file empty"
pass "screenshot captured"

printf '\nall good\n'
