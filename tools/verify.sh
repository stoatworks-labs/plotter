#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one command.
#
#     tools/verify.sh
#
# ---------------------------------------------------------------- the point
#
# Half of this file checks things the RELEASE job checks. That is deliberate,
# and it is the fleet's most expensive lesson: a check that only ever runs in
# CI, after a tag, is a check that will catch you after the tag -- and the fix
# for a bad tag is to re-point it, which strands the release.
#
# What each check answers that none of the others can:
#
#   shaders     does every shader compile, through a real GLSL compiler,
#               before a host has to find out -- the exact strings the plugin
#               hands to glCompileShader, dumped by the harness, including the
#               ink pass that is assembled at run time -- and does none of
#               them use a GLSL 4.10 reserved word (`packed` compiles here and
#               not on Mesa)
#   offline     --names   nothing the host will silently truncate, no name twice
#               --plan    the planner against its closed forms, the machine's
#                         clock and grid, the pen sorting, the pen choice
#   physics     every GL check, at TWO rasters: 320x180, which is what CI
#               uses, and 1280x720. A check that only holds at one raster is a
#               check on the rasteriser, not the plugin.
#               --trapezoid  a straight stroke takes L/v + v/a or 2 sqrt(L/a),
#                            from the ink on the sheet and from the frames
#               --ink        density along a stroke follows 1/v
#               --budget     after t seconds, exactly the strokes the profile
#                            predicts are drawn, and no others
#               --steps      a coarse Step Size is a staircase of k steps by one
#               --pens       pen changes = distinct pens with Optimise; 8 without
#               --persist    ink stays; a resize resamples it; New Sheet clears it
#               --trace      a filled square through the detect passes is one
#                            closed stroke, and gets drawn
#               --negative   every one of those FAILS on a perturbed plugin
#   pipe        the fleet's --pipe contract: whole frames only, a cue naming
#               no parameter refused, exit 1 -- not SIGPIPE 141 -- on a failed
#               render and on a closed stdout, the last proved with `| head -c 1`
#   sweep       no control is silently dead
#   bench       the render cost, for the record. Not pass/fail.
#   binary      universal, exports plugMain, the plist names a binary that is
#               really there, and the ad-hoc codesign the release job runs
#               succeeds
#   oxbow       instantiation and real frames through a real FFGL host, which
#               nothing else here reaches
#
set -uo pipefail

cd "$(dirname "$0")/.."

UNIVERSAL="${UNIVERSAL:-build-universal}"
failures=()

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures+=("$1"); }

#---------------------------------------------------------------------------
# A FRESH universal build, and the build directory is deleted first.
#
# `cmake -B build` on an existing tree re-uses the cache, and the cache is
# exactly where the architecture list lives. A developer who configured once
# with -DCMAKE_OSX_ARCHITECTURES=arm64 for a fast iteration loop -- which is
# the documented way to work in CLAUDE.md -- leaves a tree where this script
# happily rebuilds, finds a single-architecture binary, and reports it as a
# defect in the source.
#---------------------------------------------------------------------------
step "build (fresh, universal)"
rm -rf "$UNIVERSAL"
if cmake -B "$UNIVERSAL" -DCMAKE_BUILD_TYPE=Release >/tmp/plotter-configure.log 2>&1 \
   && cmake --build "$UNIVERSAL" --parallel >/tmp/plotter-build.log 2>&1; then
	pass "configured and built universal"
else
	fail "build failed -- see /tmp/plotter-build.log"
	tail -25 /tmp/plotter-build.log
	printf '\n\033[31mstopping: nothing below can run\033[0m\n'
	exit 1
fi

PLTEST="$UNIVERSAL/pltest"

step "shaders"
if out=$( tools/check-shaders.sh "$PLTEST" 2>&1 ); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
elif [ $? -eq 3 ]; then
	printf '   skipped the compile: %s\n' "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "a shader does not compile, or uses a reserved word"
	printf '%s\n' "$out" | sed 's/^/      /'
fi

step "offline (no GL)"
for check in names plan; do
	if out=$( "$PLTEST" --$check 2>&1 ); then
		pass "pltest --$check: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
	else
		fail "pltest --$check"
		printf '%s\n' "$out" | sed 's/^/      /'
	fi
done

for size in 320x180 1280x720; do
	step "physics at $size"
	for check in trapezoid ink budget steps pens persist trace negative; do
		if out=$( "$PLTEST" --$check --size $size 2>&1 ); then
			pass "pltest --$check: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
		else
			fail "pltest --$check at $size"
			printf '%s\n' "$out" | grep -v '^  t ' | sed 's/^/      /'
		fi
	done
done

#---------------------------------------------------------------------------
# --pipe, in the fleet's frame format. Two and a half frames in must be
# exactly two frames out and a clean exit -- a partial frame is the end of the
# stream, never a frame -- a cue naming no parameter must be refused rather
# than silently doing nothing to a take, a failed render must stop the stream
# with exit 1, and a reader that goes away must be a failure, not a render
# into nothing.
#---------------------------------------------------------------------------
step "pipe"
frame=$(( 64 * 36 * 4 ))
raw=$( mktemp ); many=$( mktemp ); cues=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
head -c $(( frame * 40 )) /dev/zero > "$many"

got=$( "$PLTEST" --pipe --size 64x36 < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi

# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever pltest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
"$PLTEST" --pipe --size 64x36 --script "$cues" < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then
	pass "a cue naming no parameter is refused (exit 2)"
else
	fail "a cue naming no parameter gave exit $status, not 2"
fi

# A stepped cue: Paper is an option, so between a key at frame 0 (Alpha) and
# one at frame 2 (White) frame 1 must still be Alpha. A ramp would put frame 1
# at 2, Ghost, which is opaque; Alpha paper is transparent where nothing is
# inked, and on blank input nothing is. The alpha byte of frame 1's first
# pixel tells them apart: 0 stepped, 255 ramped. The carriage is hidden: its
# ring at the carousel feathers into that corner pixel at 36 rows.
printf '0 Paper 4\n2 Paper 0\n' > "$cues"
alpha=$( "$PLTEST" --pipe --size 64x36 --set "Show Carriage=0" --script "$cues" < "$raw" 2>/dev/null | tail -c +$(( frame + 4 )) | head -c 1 | od -An -tu1 | tr -d ' ' )
if [ "$alpha" = "0" ]; then
	pass "an option cue steps: frame 1 between Alpha@0 and White@2 is still Alpha (alpha 0)"
else
	fail "an option cue ramped: frame 1 between Alpha@0 and White@2 has alpha '$alpha', not 0"
fi

# A failed render stops the stream with exit 1 and nothing after it. The
# failure is injected by the harness (--fail-render-at), because the plugin
# only fails on input no ffmpeg would send.
got=$( "$PLTEST" --pipe --size 64x36 --fail-render-at 1 < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ] && [ "$got" = "$frame" ]; then
	pass "a failed render at frame 1: exit 1, one frame out"
else
	fail "a failed render at frame 1 gave exit $status and $got bytes (want 1 and $frame)"
fi

# A reader that takes one byte and goes away: forty frames is far more than a
# pipe buffer holds, so a write after head leaves must fail. Exit 1, said on
# stderr -- not the 141 of a process SIGPIPE killed before it could say anything.
"$PLTEST" --pipe --size 64x36 < "$many" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ]; then
	pass "a closed stdout (| head -c 1): exit 1"
else
	fail "a closed stdout gave exit $status, not 1"
fi
rm -f "$raw" "$many" "$cues"

step "sweep: no control silently dead"
if out=$( python3 tools/sweep.py --binary "$PLTEST" 2>/dev/null ); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "dead controls -- see below"
	printf '%s\n' "$out" | grep -E 'DEAD|changed nothing' | sed 's/^/      /'
fi

# CI's exact GPU commands again, on Apple's SOFTWARE renderer, which is what
# GitHub's macOS runners have. It is not repeatable at the last bit (repousse's
# resize check failed CI by one ulp), and this is where a check that asserts
# exactness on this Mac's GPU is found before CI finds it.
step "software renderer (CI's, at 320x180)"
for check in trapezoid ink budget steps pens persist trace negative; do
	if out=$( PLTEST_RENDERER=software "$PLTEST" --$check --size 320x180 2>&1 ); then
		pass "pltest --$check (software): $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
	else
		fail "pltest --$check on the software renderer -- run: PLTEST_RENDERER=software $PLTEST --$check --size 320x180"
		printf '%s\n' "$out" | grep -v '^  t ' | sed 's/^/      /'
	fi
done

step "bench: the render cost, for the record"
"$PLTEST" --bench --frames 60 2>&1 | sed -n '5,7p' | sed 's/^/   /'

BUNDLE="$UNIVERSAL/Plotter.bundle"
BIN="$BUNDLE/Contents/MacOS/Plotter"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "binary"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o
	# pipefail`: grep exits at once, nm takes SIGPIPE, and the pipeline reports
	# failure. It is output-size dependent, so it fires on the bigger binary
	# first and looks intermittent. Capture and match with `case` -- not a
	# pipeline anywhere.
	symbols=$( nm -gU "$BIN" 2>/dev/null || true )
	case "$symbols" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle would load and contain no plugins" ;;
	esac

	archs=$( lipo -archs "$BIN" 2>/dev/null )
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present ($archs)" ;; *) fail "NOT universal (got: $archs)" ;; esac

	exe=$( /usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null )
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign fails after the tag"
	fi

	ident=$( /usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null )
	if [ "$ident" = "com.stoatworks.ffgl.plotter" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident', not com.stoatworks.ffgl.plotter"
	fi

	step "codesign (the exact command the release job runs, on a copy)"
	tmp=$( mktemp -d )
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Plotter.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs"
	else
		fail "ad-hoc signing failed -- the failure that never mentions the plist"
	fi
	rm -rf "$tmp"

	step "oxbow: a real FFGL host loads it"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$( "$OXBOW" probe "$BUNDLE" 2>&1 )
		case "$probe" in
			*"SW Plotter"*) pass "name is SW Plotter" ;;
			*) fail "oxbow does not see the name: $( printf '%s' "$probe" | head -3 )" ;;
		esac
		case "$probe" in *"PL01"*) pass "id is PL01" ;; *) fail "id is not PL01" ;; esac
		case "$probe" in *"type:        effect"*) pass "type is effect" ;; *) fail "type is not effect" ;; esac

		# An effect needs an input, and oxbow's selftest feeds it one.
		self=$( "$OXBOW" selftest "$BUNDLE" 2>&1 )
		case "$self" in
			*"FF_INSTANTIATE_GL failed"*) fail "instantiation failed -- see: $OXBOW selftest $BUNDLE" ;;
			*PASS*) pass "instantiates and renders in a host" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if (( ${#failures[@]} == 0 )); then
	printf '\033[32mall checks passed\033[0m\n'
	exit 0
fi
printf '\033[31mFAILURES:\033[0m\n'
printf '  %s\n' "${failures[@]}"
exit 1
