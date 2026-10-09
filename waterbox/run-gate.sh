#!/bin/sh
# The Azahar core's gate.
#
# Written against ~/chimera/docs/gates.md, and the ways a gate has already been
# watched to go green on a broken thing:
#
#   * a DEAD machine finishes instantly and every digest agrees with itself,
#     so the machine legs assert the picture is alive first;
#   * an END-STATE digest misses a machine that ran differently and arrived
#     at the same place, so flavors are compared as a per-frame digest STREAM;
#   * a guest build that failed was silently packaged as the PREVIOUS binary,
#     so the package leg checks core.wbx is newer than its sources;
#   * a comparison that cannot fail proves nothing, so every leg that says
#     "the same" or "it reached the machine" runs a negative control beside it.
#
# The machine legs need games, which cannot ship: put decrypted dumps (or links
# to them) in tests/roms-local, or point AZAHAR_ROMS at a folder holding them.
# The names the legs look for: darkwitch.cci, drancia.cci, cars2.3ds,
# mlss.3ds. A game that is not there is SKIPPED, visibly; a gate that ran no
# machine at all still proves the build and the declarations.
#
# Usage: ./run-gate.sh [-r <chimera root>] [-m <minibox dir>] [-q]
#        -q: skip the rebuild (the binaries in build/ and waterbox/bin are used)
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
chimera_root="${CHIMERA_ROOT:-$HOME/chimera}"
mb="${MINIBOX_DIR:-$chimera_root/extern/chimera-common-minibox}"
roms="${AZAHAR_ROMS:-$root/tests/roms-local}"
quick=0
while getopts "r:m:q" opt; do
	case "$opt" in
		r) chimera_root="$OPTARG" ;;
		m) mb="$OPTARG" ;;
		q) quick=1 ;;
		*) exit 2 ;;
	esac
done

work="$root/build/gate"
rm -rf "$work"; mkdir -p "$work"
native="$here/obj-native/run-native"
wbxrun="$here/bin/run-wbx"
core="$here/bin/core.wbx"
run="$chimera_root/build/meson-linux/chimera-run"
pkg="$root/build/package/azahar.chimeraCore"

pass=0; fail=0; skip=0
report() {
	case "$1" in
		PASS) pass=$((pass+1)) ;;
		FAIL) fail=$((fail+1)) ;;
		SKIP) skip=$((skip+1)) ;;
	esac
	printf '%-6s %-52s %s\n' "$1" "$2" "${3:-}"
}
finish() {
	printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
	[ "$pass" -eq 0 ] && { echo "NOTHING RAN"; exit 1; }
	[ "$fail" -eq 0 ] || exit 1
	exit 0
}
stream() { awk '/^frame/' "$1"; }
field() { echo "$1" | awk -v f="$2" '{for (i=1;i<=NF;i++) if ($i==f) {print $(i+1); exit}}'; }
last() { stream "$1" | tail -1; }
# alive <file>: the run reported at least one frame. A comparison of two
# streams must ask this first: two runs that both died at Init print the same
# nothing (or the same host chatter), and "the same" would then pass on a
# machine that never started - which is what the savestate legs did the day
# the sandbox's memory layout fell behind the package's (2026-10-09).
alive() { [ "$(stream "$1" | wc -l)" -ge 1 ]; }
# same <a> <b>: the two runs reported the same frames. What is compared is the
# stream and not the file: the sandbox's host says a line of its own on the
# same descriptor the first time the guest allocates invisible memory, and a
# run in two hosts says it twice.
same() { [ "$(stream "$1")" = "$(stream "$2")" ]; }

# ---------------------------------------------------------------- 1. build
if [ "$quick" -eq 0 ]; then
	if sh "$here/build-native.sh" > "$work/native.log" 2>&1 && make -C "$here" -f native.mk MB="$mb" > "$work/native-mk.log" 2>&1; then
		report PASS "the native reference and harness build"
	else
		report FAIL "the native reference and harness build" "see build/gate/native.log"
	fi
	if sh "$here/build-package.sh" -r "$chimera_root" -m "$mb" -o "$root/build/package" > "$work/package.log" 2>&1; then
		report PASS "package builds" "$(grep -o 'sha1 [0-9a-f]*' "$work/package.log")"
	else
		report FAIL "package builds" "see build/gate/package.log"
	fi
else
	report SKIP "the builds (-q)" "using what build/ and waterbox/bin hold"
fi
stale=""
for src in "$here"/azahar-driver.cpp "$here"/wbx-entry.cpp "$here"/chimera-fs.cpp "$here"/zip-read.cpp "$here"/guest-syscalls.cpp; do
	[ "$core" -nt "$src" ] || stale="$stale $(basename "$src")"
done
if [ -f "$core" ] && [ -z "$stale" ]; then
	report PASS "core.wbx is not stale"
else
	report FAIL "core.wbx is not stale" "older than:$stale"
fi

# The declarations are what gen-config.py writes: nobody hand-edits one of the
# three files and leaves the generator (and the panel it mirrors) behind.
mkdir -p "$work/gen"
python3 "$here/gen-config.py" "$work/gen"
drift=""
for f in waterbox.config file_slots.json default_keybinds.json; do
	cmp -s "$work/gen/$f" "$here/$f" || drift="$drift $f"
done
if [ -z "$drift" ]; then
	report PASS "the declarations are gen-config.py's"
else
	report FAIL "the declarations are gen-config.py's" "differs:$drift (run waterbox/gen-config.py)"
fi

# No Nintendo key in the package: Azahar's built-in key blob is compiled out.
# The control: the same symbol listing names the core's own hook, so an empty
# grep means "not there", not "nm read nothing".
if grep -q -- '-DENABLE_BUILTIN_KEYBLOB=OFF' "$here/configure-flags.sh" \
	&& ! nm "$core" 2>/dev/null | grep -q 'default_keys_enc' \
	&& nm "$core" 2>/dev/null | grep -q 'Chimera_InputRead'; then
	report PASS "the core carries no key blob" "control: the core's own symbols are listed"
else
	report FAIL "the core carries no key blob"
fi

# ------------------------------------------------------------ 2. machines
# workdir <dir> <game> [settings json]: the files a project would mount
workdir() {
	d="$1"; rm -rf "$d"; mkdir -p "$d"
	ln -s "$roms/$2" "$d/$2"
	printf '{"game":["%s"]}' "$2" > "$d/slots"
	[ -n "${3:-}" ] && printf '%s' "$3" > "$d/settings"
	true
}

# the panel the core binds is the panel the package declares
panelcheck() {
	python3 - "$here/waterbox.config" "$1" <<'PY'
import json, re, sys
cfg = json.load(open(sys.argv[1]))
lines = open(sys.argv[2]).read().splitlines()
got = [re.match(r"panel \d+ '(.*)' ", l).group(1) for l in lines if l.startswith("panel ")]
axes = [l for l in lines if l.startswith("axis ")]
want = cfg["input"]["buttons"]
if got != want:
    sys.exit(f"core: {got}\ndeclared: {want}")
if len(axes) != len(cfg["input"]["axes"]):
    sys.exit(f"core has {len(axes)} axes, the package declares {len(cfg['input']['axes'])}")
live = sum(1 for l in lines if l.startswith("panel ") and l.endswith("active"))
print(f"{len(got)} buttons ({live} live), {len(axes)} axes")
PY
}

ran_any=0
for game in darkwitch.cci drancia.cci mlss.3ds cars2.3ds; do
	name="${game%.*}"
	if [ ! -f "$roms/$game" ]; then
		report SKIP "$name: every machine leg" "no $game in $roms"
		continue
	fi
	ran_any=1
	w="$work/$name"
	workdir "$w" "$game"

	"$wbxrun" "$core" "$w" --frames 1 --list-panel > "$work/$name.panel" 2>&1 || true
	if out="$(panelcheck "$work/$name.panel" 2>&1)"; then
		report PASS "$name: the panel is the declared one" "$out"
	else
		report FAIL "$name: the panel is the declared one" "$(echo "$out" | head -1)"
	fi

	# native == sandbox, over an exercised run, as a stream; the native run
	# twice (deterministic), and idle (the input reaches the machine - the
	# control that says the stream comparison can tell two runs apart)
	"$native" "$w" --frames 600 --report 30 --exercise > "$work/$name.n" 2>"$work/$name.ne" &
	"$native" "$w" --frames 600 --report 30 --exercise > "$work/$name.n2" 2>/dev/null &
	"$native" "$w" --frames 600 --report 30 > "$work/$name.ni" 2>/dev/null &
	MALLOC_PERTURB_=85 "$native" "$w" --frames 600 --report 30 --exercise > "$work/$name.nh" 2>/dev/null &
	"$wbxrun" "$core" "$w" --frames 600 --report 30 --exercise > "$work/$name.w" 2>"$work/$name.we" &
	wait
	pics="$(stream "$work/$name.n" | awk '{print $7}' | sort -u | wc -l)"
	if [ "$(stream "$work/$name.n" | wc -l)" -ne 20 ] || [ "$pics" -lt 3 ]; then
		report FAIL "$name: the machine is alive" "$(stream "$work/$name.n" | wc -l) reports, $pics distinct pictures"
	else
		report PASS "$name: the machine is alive" "$pics distinct pictures in 600 frames"
	fi
	if alive "$work/$name.n" && same "$work/$name.n" "$work/$name.n2"; then
		report PASS "$name: native is deterministic (600 frames)"
	else
		report FAIL "$name: native is deterministic (600 frames)"
	fi
	# the machine never reads the host's heap: glibc fills every fresh block
	# with a pattern under MALLOC_PERTURB_, and an emulator that copies an
	# uninitialised host byte into the guest digests differently (patch 0013
	# was found this way: Cars 2 differed from frame 27)
	if alive "$work/$name.nh" && same "$work/$name.n" "$work/$name.nh"; then
		report PASS "$name: nothing the host's heap held reaches the machine"
	else
		report FAIL "$name: nothing the host's heap held reaches the machine" \
			"first difference: $(diff "$work/$name.n" "$work/$name.nh" 2>&1 | awk 'NR==2' | cut -c1-60)"
	fi
	if alive "$work/$name.n" && same "$work/$name.n" "$work/$name.w"; then
		report PASS "$name: native == sandbox (600 frames, exercised)"
	else
		report FAIL "$name: native == sandbox (600 frames, exercised)" \
			"first difference: $(diff "$work/$name.n" "$work/$name.w" 2>&1 | awk 'NR==2' | cut -c1-60)"
	fi
	if alive "$work/$name.ni" && alive "$work/$name.n" && ! same "$work/$name.ni" "$work/$name.n"; then
		report PASS "$name: the input reaches the machine" "negative control: an idle run digests differently"
	else
		report FAIL "$name: the input reaches the machine" "an exercised run digests like an idle one"
	fi

	# lag: both directions in one run - the frames before the game has its
	# controls mapped are lag, and once it reads them some frames are not
	first="$(stream "$work/$name.n" | head -1)"; end="$(last "$work/$name.n")"
	lag0="$(field "$first" lag)"; lagN="$(field "$end" lag)"
	if [ -n "$lag0" ] && [ "$lag0" -gt 0 ] && [ "$lagN" -lt 600 ]; then
		report PASS "$name: lag frames are counted" "$lag0 of the first 30 (booting), $lagN of 600"
	else
		report FAIL "$name: lag frames are counted" "first 30: $lag0, all 600: $lagN"
	fi

	# savestates: around every frame, and into a new host. The exercised
	# stream above is the reference; the controls are the input leg's.
	"$wbxrun" "$core" "$w" --frames 300 --report 30 --exercise > "$work/$name.p" 2>/dev/null &
	"$wbxrun" "$core" "$w" --frames 300 --report 30 --exercise --rerecord > "$work/$name.r" 2>"$work/$name.re" &
	"$wbxrun" "$core" "$w" --frames 300 --report 30 --exercise --session > "$work/$name.s" 2>/dev/null &
	wait
	if alive "$work/$name.p" && same "$work/$name.p" "$work/$name.r"; then
		report PASS "$name: save+load around every frame changes nothing" "$(grep -o 'stateBytes=[0-9]*' "$work/$name.re")"
	else
		report FAIL "$name: save+load around every frame changes nothing"
	fi
	if alive "$work/$name.p" && same "$work/$name.p" "$work/$name.s"; then
		report PASS "$name: a state reopens in a new host"
	else
		report FAIL "$name: a state reopens in a new host"
	fi
done

# ------------------------------------------ 3. input, touch, save data (Drancia)
# Drancia Saga's title menu reads the D-pad and the touch screen: at frame
# 700 the menu is up. Down moves the cursor; a touch of the bottom screen
# reaches the machine; the SAME touch on the top screen - outside the touch
# panel - must reach nothing (the negative control, and the rule the Touch
# axes declare).
if [ -f "$roms/drancia.cci" ]; then
	w="$work/drancia"
	"$native" "$w" --frames 760 --report 760 > "$work/in.idle" 2>/dev/null &
	"$native" "$w" --frames 760 --report 760 --press 5:700:6 > "$work/in.down" 2>/dev/null &
	"$native" "$w" --frames 760 --report 760 --press 14:700:6 --axis 4:32768:700:6 --axis 5:45466:700:6 > "$work/in.touch" 2>/dev/null &
	"$native" "$w" --frames 760 --report 760 --press 14:700:6 --axis 4:32768:700:6 --axis 5:16384:700:6 > "$work/in.touchtop" 2>/dev/null &
	wait
	idle="$(last "$work/in.idle")"
	if [ -n "$idle" ] && [ "$(field "$idle" vid)" != "" ] && \
		[ "$(field "$(last "$work/in.down")" 400x480)" != "$(field "$idle" 400x480)" ]; then
		report PASS "drancia: Down moves the menu cursor" "the picture changes, against an idle run"
	else
		report FAIL "drancia: Down moves the menu cursor"
	fi
	if [ "$(field "$(last "$work/in.touch")" ram)" != "$(field "$idle" ram)" ] && \
		[ "$(last "$work/in.touchtop")" = "$idle" ]; then
		report PASS "drancia: a touch reaches the machine" "on the bottom screen it does; on the top screen it is nothing"
	else
		report FAIL "drancia: a touch reaches the machine" \
			"bottom: $(field "$(last "$work/in.touch")" ram) top: $(field "$(last "$work/in.touchtop")" ram) idle: $(field "$idle" ram)"
	fi

	# save data: what the game writes comes out; that zip goes back in and
	# comes out the same; the machine that starts from it is not the one that
	# starts from nothing; and a file that is not a save zip is refused
	rm -rf "$work/sd1" "$work/sd2"
	"$native" "$w" --frames 400 --report 400 --savedata-out "$work/sd1" > /dev/null 2>&1 || true
	n1="$(find "$work/sd1" -type f 2>/dev/null | wc -l)"
	(cd "$work/sd1" && python3 -c "
import os, zipfile
with zipfile.ZipFile('$work/save.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for r, d, fs in sorted(os.walk('sdmc')):
        for f in sorted(fs):
            z.write(os.path.join(r, f))
") 2>/dev/null || true
	ws="$work/drancia-save"
	workdir "$ws" drancia.cci
	cp "$work/save.zip" "$ws/save.zip" 2>/dev/null || true
	printf '{"game":["drancia.cci"],"savedata":["save.zip"]}' > "$ws/slots"
	"$native" "$ws" --frames 1 --report 1 --savedata-out "$work/sd2" > /dev/null 2>&1 || true
	if [ "$n1" -gt 0 ] && diff -r "$work/sd1" "$work/sd2" > /dev/null 2>&1; then
		report PASS "drancia: save data goes out and back in unchanged" "$n1 files under sdmc/"
	else
		report FAIL "drancia: save data goes out and back in unchanged" "$n1 files out"
	fi
	"$native" "$ws" --frames 120 --report 120 > "$work/sv.with" 2>/dev/null || true
	"$native" "$w" --frames 120 --report 120 > "$work/sv.without" 2>/dev/null || true
	if alive "$work/sv.with" && alive "$work/sv.without" && [ "$(field "$(last "$work/sv.with")" ram)" != "$(field "$(last "$work/sv.without")" ram)" ]; then
		report PASS "drancia: the game starts from its save data" "against the same boot without it"
	else
		report FAIL "drancia: the game starts from its save data"
	fi
	printf 'not a zip' > "$ws/save.zip"
	if "$native" "$ws" --frames 1 --report 1 > "$work/sv.bad" 2>&1; then
		report FAIL "drancia: a file that is not a save zip is refused" "it booted"
	elif grep -q "is not a save data zip" "$work/sv.bad"; then
		report PASS "drancia: a file that is not a save zip is refused" "$(grep -o 'is not a save data zip.*' "$work/sv.bad" | head -1)"
	else
		report FAIL "drancia: a file that is not a save zip is refused" "$(tail -1 "$work/sv.bad")"
	fi

	# settings that are the machine: the clock at power-on changes what the
	# console computes; the default spelled out changes nothing (control)
	printf '{"rtc_start":946684800}' > "$w/settings"
	"$native" "$w" --frames 120 --report 120 > "$work/rtc.default" 2>/dev/null || true
	printf '{"rtc_start":1262304000}' > "$w/settings"
	"$native" "$w" --frames 120 --report 120 > "$work/rtc.2010" 2>/dev/null || true
	rm -f "$w/settings"
	if [ "$(last "$work/rtc.default")" = "$(last "$work/sv.without")" ] && \
		[ "$(field "$(last "$work/rtc.2010")" ram)" != "$(field "$(last "$work/sv.without")" ram)" ]; then
		report PASS "drancia: the clock at power-on is part of the machine" "2010 differs; the default spelled out does not"
	else
		report FAIL "drancia: the clock at power-on is part of the machine"
	fi
	# the model: an old 3DS has no ZL, ZR or C-Stick
	printf '{"model":"old3ds"}' > "$w/settings"
	"$native" "$w" --frames 1 --report 1 --list-panel > "$work/old.panel" 2>/dev/null || true
	rm -f "$w/settings"
	if grep -q "panel 12 'ZL' -" "$work/old.panel" && grep -q "axis 2 -" "$work/old.panel" && \
		grep -q "panel 12 'ZL' active" "$work/drancia.panel"; then
		report PASS "drancia: an old 3DS has no ZL, ZR or C-Stick" "a New 3DS has them"
	else
		report FAIL "drancia: an old 3DS has no ZL, ZR or C-Stick"
	fi
else
	report SKIP "drancia: input, touch, save data and settings legs" "no drancia.cci in $roms"
fi

# -------------------------------------------------- 4. content the core refuses
# Cars 2's dump says "encrypted" in its NCCH header but holds plaintext; it
# boots above. A dump whose ExeFS really is encrypted must still be refused:
# made here from Cars 2's first 8 MiB with the ExeFS header scrambled (the
# rest of the file sparse, so its size is still a cartridge's).
if [ -f "$roms/cars2.3ds" ]; then
	wf="$work/fake"
	rm -rf "$wf"; mkdir -p "$wf"
	python3 - "$roms/cars2.3ds" "$wf/fake.3ds" <<'PY'
import os, struct, sys
src = open(sys.argv[1], 'rb').read(8 << 20)
off = struct.unpack_from('<I', src, 0x120)[0] * 0x200
exefs = off + struct.unpack_from('<I', src, off + 0x1A0)[0] * 0x200
out = bytearray(src)
for i in range(0x200):
    out[exefs + i] = (i * 151 + 89) & 0xFF
with open(sys.argv[2], 'wb') as f:
    f.write(out)
    f.truncate(os.path.getsize(sys.argv[1]))  # sparse: the rest reads as zeros
PY
	printf '{"game":["fake.3ds"]}' > "$wf/slots"
	if "$native" "$wf" --frames 1 > "$work/fake.out" 2>&1; then
		report FAIL "an encrypted dump is refused" "it booted"
	elif grep -q "the game is encrypted" "$work/fake.out"; then
		report PASS "an encrypted dump is refused" "and Cars 2's plaintext one is not (above)"
	else
		report FAIL "an encrypted dump is refused" "$(tail -1 "$work/fake.out")"
	fi
else
	report SKIP "an encrypted dump is refused" "no cars2.3ds to make one from"
fi

# -------------------------------------------------------------- 5. the engine
# The package through chimera-run, a movie of the live panel: Drancia's menu,
# Down pressed at 700, and a picture taken at the end.
if [ -f "$roms/drancia.cci" ] && [ -x "$run" ] && [ -f "$pkg" ]; then
	python3 - "$work/engine.movie" <<'PY'
import sys
lines = []
for f in range(760):
    b = ['.'] * 15
    if 699 <= f < 705:
        b[5] = 'D'
    lines.append('|' + ''.join('%5d,' % v for v in (0, 0, 0, 0, 32768, 32768)) + ''.join(b) + '|')
open(sys.argv[1], 'w').write('\n'.join(lines) + '\n')
PY
	if "$run" "$pkg" "$roms/drancia.cci" "$work/engine.movie" --screenshot 759="$work/engine.tga" \
		> "$work/engine.out" 2>&1 && grep -q '^frames=760' "$work/engine.out" && [ -s "$work/engine.tga" ]; then
		report PASS "drancia: 760 frames in the engine" "a Down press at 700, through a movie"
	else
		report FAIL "drancia: 760 frames in the engine" "see build/gate/engine.out"
	fi
else
	report SKIP "the engine leg" "no drancia.cci, chimera-run or package"
fi

# ------------------------------------------------ 6. the GPU bridge (opengl-hw)
# The OpenGL renderer through miniBox's GPU bridge, on the headless EGL context
# the host half makes (Mesa's llvmpipe here, which is what makes the legs
# repeatable on any machine). Both flavors go through the same generated
# wrappers and dispatcher; they differ only by the sandbox.
gpurun() { CHIMERA_GPU=1 "$@"; }
for game in darkwitch.cci drancia.cci; do
	name="${game%.*}"
	if [ ! -f "$roms/$game" ]; then
		report SKIP "$name: the GPU legs" "no $game in $roms"
		continue
	fi
	g="$work/gl-$name"
	workdir "$g" "$game" '{"renderer":"opengl-hw"}'
	gpurun "$native" "$g" --frames 600 --report 30 --exercise > "$work/gl-$name.n" 2>"$work/gl-$name.ne" &
	gpurun "$wbxrun" "$core" "$g" --frames 600 --report 30 --exercise > "$work/gl-$name.w" 2>"$work/gl-$name.we" &
	# no bridge handed over: the same settings fall back to the software renderer
	"$native" "$g" --frames 600 --report 30 --exercise > "$work/gl-$name.nobridge" 2>"$work/gl-$name.nbe" &
	wait
	pics="$(stream "$work/gl-$name.n" | awk '{print $7}' | sort -u | wc -l)"
	if grep -q "^gpu bridge: .*Core Profile" "$work/gl-$name.we" && ! grep -q "software renderer" "$work/gl-$name.we" \
		&& [ "$pics" -ge 3 ] && ! same "$work/gl-$name.n" "$work/$name.n"; then
		report PASS "$name: the GPU draws it (opengl-hw)" "$pics pictures, and not the software renderer's stream"
	else
		report FAIL "$name: the GPU draws it (opengl-hw)" "$pics pictures; see build/gate/gl-$name.we"
	fi
	if alive "$work/gl-$name.n" && same "$work/gl-$name.n" "$work/gl-$name.w"; then
		report PASS "$name: GPU native == sandbox (600 frames, exercised)"
	else
		report FAIL "$name: GPU native == sandbox (600 frames, exercised)" \
			"first difference: $(diff "$work/gl-$name.n" "$work/gl-$name.w" 2>&1 | awk 'NR==2' | cut -c1-60)"
	fi
	if alive "$work/$name.n" && same "$work/gl-$name.nobridge" "$work/$name.n" && grep -q "no GPU bridge" "$work/gl-$name.nbe"; then
		report PASS "$name: with no bridge, opengl-hw draws in software" "and says so"
	else
		report FAIL "$name: with no bridge, opengl-hw draws in software"
	fi
	# a load moves the context id (the host mints a new one, as chimera does):
	# the renderer is thrown away and made again from the console's memory
	gpurun "$wbxrun" "$core" "$g" --frames 300 --report 30 --exercise > "$work/gl-$name.p" 2>/dev/null &
	gpurun "$wbxrun" "$core" "$g" --frames 300 --report 30 --exercise --rerecord > "$work/gl-$name.r" 2>"$work/gl-$name.re" &
	gpurun "$wbxrun" "$core" "$g" --frames 300 --report 30 --exercise --session > "$work/gl-$name.s" 2>"$work/gl-$name.se" &
	wait
	if alive "$work/gl-$name.p" && same "$work/gl-$name.p" "$work/gl-$name.r"; then
		report PASS "$name: GPU: a rebuild after every frame's load changes nothing" "300 frames"
	else
		report FAIL "$name: GPU: a rebuild after every frame's load changes nothing"
	fi
	if alive "$work/gl-$name.p" && same "$work/gl-$name.p" "$work/gl-$name.s"; then
		report PASS "$name: GPU: a state reopens in a new host and draws on"
	else
		report FAIL "$name: GPU: a state reopens in a new host and draws on"
	fi
	if grep -q "no case for" "$work/gl-$name.we" "$work/gl-$name.re" "$work/gl-$name.se"; then
		report FAIL "$name: GPU: every call the renderer makes is answered" "$(grep -h 'no case for' "$work/gl-$name.we" "$work/gl-$name.re" | head -1)"
	else
		report PASS "$name: GPU: every call the renderer makes is answered"
	fi
done
if [ -f "$roms/drancia.cci" ] && [ -x "$run" ] && [ -f "$pkg" ]; then
	if "$run" "$pkg" "$roms/drancia.cci" "$work/engine.movie" --gpu --settings '{"renderer":"opengl-hw"}' \
		--screenshot 759="$work/engine-gl.tga" > "$work/engine-gl.out" 2>&1 \
		&& grep -q '^frames=760' "$work/engine-gl.out" && [ -s "$work/engine-gl.tga" ]; then
		report PASS "drancia: 760 frames in the engine on the GPU" "$(grep -o 'Core Profile.*' "$work/engine-gl.out" | head -1 | cut -c1-40)"
	else
		report FAIL "drancia: 760 frames in the engine on the GPU" "see build/gate/engine-gl.out"
	fi
fi

# ------------------------------------- 7. the picture's shape, and the user name
# chimera#223. Screen Layout, Swap Screens, Upright Screens, Large Screen
# Proportion and Linear Filtering are declared the picture only, and a
# declaration like that is a claim about memory: every run below must leave
# the RAM the stacked run leaves, while the picture changes. Azahar's
# internal resolution, its texture filters and its forced texture sampling
# were measured the same way, changed the RAM, and are not offered.
#
# pic <stream line> -> "WxH hash"
pic() { echo "$1" | awk '{for (i=1;i<=NF;i++) if ($i=="vid") {print $(i+1), $(i+2); exit}}'; }
if [ -f "$roms/drancia.cci" ]; then
	# lay <name> <settings> [harness arguments]: 760 frames of the title menu
	lay() {
		n="$1"; workdir "$work/lay-$n" drancia.cci "$2"; shift 2
		"$native" "$work/lay-$n" --frames 760 --report 760 "$@" > "$work/lay-$n.n" 2>"$work/lay-$n.ne"
	}
	lay stacked '{}' &
	lay side '{"layout":"side-by-side"}' &
	lay large '{"layout":"large"}' &
	lay bottom '{"layout":"single","swap_screens":true}' &
	wait
	lay upright '{"upright":true}' &
	lay large2 '{"layout":"large","large_screen_proportion":2}' &
	# The menu's touch (section 3): the middle of the bottom screen, 93 rows
	# down. Stacked that is the picture's (200, 333); side by side the bottom
	# screen starts at x 400, so it is (560, 93) of 720x240. The Touch axes are
	# a place in the picture, so the two runs name the same place on the panel
	# and must leave the same machine.
	lay touch-stacked '{}' --press 14:700:6 --axis 4:32768:700:6 --axis 5:45466:700:6 &
	lay touch-side '{"layout":"side-by-side"}' --press 14:700:6 --axis 4:50973:700:6 --axis 5:25396:700:6 &
	wait
	base="$(last "$work/lay-stacked.n")"
	bad=""
	for want in "side 720x240" "large 480x240" "bottom 320x240" "upright 480x400" "large2 560x240"; do
		n="${want% *}"; size="${want#* }"
		line="$(last "$work/lay-$n.n")"
		[ -n "$line" ] && [ "$(field "$line" ram)" = "$(field "$base" ram)" ] \
			&& [ "$(field "$line" vid)" = "$size" ] && [ "$(pic "$line")" != "$(pic "$base")" ] \
			|| bad="$bad $n($(field "$line" vid) ram $(field "$line" ram))"
	done
	if [ -n "$base" ] && [ "$(field "$base" vid)" = "400x480" ] && [ -z "$bad" ]; then
		report PASS "drancia: a layout is the picture only (software)" "five layouts, each its own size, the RAM the stacked run's"
	else
		report FAIL "drancia: a layout is the picture only (software)" "stacked ram $(field "$base" ram);$bad"
	fi
	ts="$(last "$work/lay-touch-stacked.n")"; td="$(last "$work/lay-touch-side.n")"
	if [ -n "$ts" ] && [ "$(field "$ts" ram)" = "$(field "$td" ram)" ] && [ "$(field "$ts" ram)" != "$(field "$base" ram)" ]; then
		report PASS "drancia: a touch is a place in the picture" "the same point of the bottom screen, stacked and side by side, leaves the same machine"
	else
		report FAIL "drancia: a touch is a place in the picture" "stacked $(field "$ts" ram) side $(field "$td" ram) idle $(field "$base" ram)"
	fi
	"$wbxrun" "$core" "$work/lay-upright" --frames 760 --report 760 > "$work/lay-upright.w" 2>/dev/null
	if alive "$work/lay-upright.n" && same "$work/lay-upright.n" "$work/lay-upright.w"; then
		report PASS "drancia: a layout, native == sandbox" "upright, 760 frames"
	else
		report FAIL "drancia: a layout, native == sandbox" "$(last "$work/lay-upright.w" | cut -c1-70)"
	fi
	# a value this build does not know is refused, not defaulted
	lay nolayout '{"layout":"diagonal"}' || true
	lay longname '{"username":"elevenchars"}' || true
	if grep -q "no such screen layout: diagonal" "$work/lay-nolayout.ne" && [ ! -s "$work/lay-nolayout.n" ] \
		&& grep -q "one to ten characters" "$work/lay-longname.ne" && [ ! -s "$work/lay-longname.n" ]; then
		report PASS "drancia: a layout or a name it cannot give is refused" "and says which"
	else
		report FAIL "drancia: a layout or a name it cannot give is refused" "$(tail -1 "$work/lay-nolayout.ne" | cut -c1-60)"
	fi
else
	report SKIP "drancia: the layout legs" "no drancia.cci in $roms"
fi

# The user name is the console's own setting, and Cars 2 reads it: a name
# changes that game's memory, and Azahar's default spelled out does not.
if [ -f "$roms/cars2.3ds" ]; then
	for u in none:'{}' azahar:'{"username":"AZAHAR"}' chimera:'{"username":"Chimera"}'; do
		n="${u%%:*}"
		workdir "$work/name-$n" cars2.3ds "${u#*:}"
		"$native" "$work/name-$n" --frames 600 --report 600 > "$work/name-$n.n" 2>/dev/null &
	done
	wait
	none="$(field "$(last "$work/name-none.n")" ram)"
	if [ -n "$none" ] && [ "$(field "$(last "$work/name-azahar.n")" ram)" = "$none" ] \
		&& [ "$(field "$(last "$work/name-chimera.n")" ram)" != "$none" ]; then
		report PASS "cars2: the user name is part of the machine" "another name changes the RAM; the default spelled out does not"
	else
		report FAIL "cars2: the user name is part of the machine" "default $none AZAHAR $(field "$(last "$work/name-azahar.n")" ram) Chimera $(field "$(last "$work/name-chimera.n")" ram)"
	fi
else
	report SKIP "cars2: the user name is part of the machine" "no cars2.3ds in $roms"
fi

# The same claims of the OpenGL renderer, which draws the layout itself.
# (Internal resolution is not here because it is not offered: measured with
# this leg's own runs at 2x, the RAM differed from 1x and - what rules it out -
# a load around every frame gave another run than no load. docs/PLAN.md.)
if [ -f "$roms/darkwitch.cci" ]; then
	glay() {
		n="$1"; workdir "$work/glay-$n" darkwitch.cci "$2"; shift 2
		gpurun "$native" "$work/glay-$n" --frames 900 --report 900 "$@" > "$work/glay-$n.n" 2>"$work/glay-$n.ne"
	}
	glay stacked '{"renderer":"opengl-hw"}' &
	glay large '{"renderer":"opengl-hw","layout":"large","swap_screens":true}' &
	glay nearest '{"renderer":"opengl-hw","layout":"large","swap_screens":true,"linear_filter":false}' &
	wait
	g1="$(last "$work/glay-stacked.n")"; gl="$(last "$work/glay-large.n")"; gn="$(last "$work/glay-nearest.n")"
	if grep -q "^gpu bridge: .*Core Profile" "$work/glay-large.ne" && [ -n "$g1" ] \
		&& [ "$(field "$gl" ram)" = "$(field "$g1" ram)" ] && [ "$(field "$gn" ram)" = "$(field "$g1" ram)" ] \
		&& [ "$(field "$gl" vid)" = "420x240" ] && [ "$(pic "$gl")" != "$(pic "$gn")" ]; then
		report PASS "darkwitch: GPU: a layout and its filter are the picture only" "large and swapped, 420x240; linear filtering off changes the picture and not the RAM"
	else
		report FAIL "darkwitch: GPU: a layout and its filter are the picture only" "ram $(field "$g1" ram) / $(field "$gl" ram) / $(field "$gn" ram), $(field "$gl" vid)"
	fi
	# and a load does not undo it: the renderer made again after every frame's
	# load draws into the same layout
	g="$work/glay-large"
	gpurun "$wbxrun" "$core" "$g" --frames 300 --report 30 --exercise > "$work/glay.p" 2>/dev/null &
	gpurun "$wbxrun" "$core" "$g" --frames 300 --report 30 --exercise --rerecord > "$work/glay.r" 2>/dev/null &
	wait
	if alive "$work/glay.p" && same "$work/glay.p" "$work/glay.r" && [ "$(field "$(last "$work/glay.p")" vid)" = "420x240" ]; then
		report PASS "darkwitch: GPU: a layout, a load every frame changes nothing" "large and swapped, 300 frames in the sandbox"
	else
		report FAIL "darkwitch: GPU: a layout, a load every frame changes nothing"
	fi
else
	report SKIP "darkwitch: the GPU's layout legs" "no darkwitch.cci in $roms"
fi

[ "$ran_any" -eq 1 ] || echo "no game in $roms: the machine legs were skipped (see the header of this script)"
finish
