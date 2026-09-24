#!/usr/bin/env bash
#
# Every shader the plugin compiles, through a real GLSL compiler.
#
#     tools/check-shaders.sh build/pltest
#
# The harness dumps the exact strings the plugin hands to glCompileShader --
# including the ink pass, which is assembled at run time from a constants
# block and a body and so exists in no file on disk -- and glslc compiles each
# one. No re-transcription, no extraction regex to go stale.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V,
# which demands an explicit layout( location ) on every uniform and varying.
# Those are Vulkan rules and not GLSL ones, and without the flag every shader
# "fails" for reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it exits
# 3 ("skipped") rather than failing; verify.sh reports that as a skip.
#
# It also greps every dumped shader for a GLSL 4.10 reserved word used as an
# identifier. Mesa's llvmpipe (the Arena gate) refuses `packed`; Apple's
# compiler and glslc accept it, so a compile alone would not catch it.
set -uo pipefail

PLTEST="${1:-build/pltest}"
[ -x "$PLTEST" ] || { echo "no harness at $PLTEST"; exit 2; }

dir="$( mktemp -d )"
trap 'rm -rf "$dir"' EXIT

"$PLTEST" --dump-shaders "$dir" >/dev/null || { echo "could not dump the shaders"; exit 1; }

n=0; bad=0
for shader in "$dir"/*.vert "$dir"/*.frag; do
	[ -e "$shader" ] || continue
	n=$(( n + 1 ))
	# A reserved word as an identifier: a word boundary either side, and not
	# inside a comment. `input`/`output` appear in comments legitimately.
	if sed 's|//.*$||' "$shader" | grep -nwE 'patch|sample|input|output|filter|common|active|half|packed' >"$dir/reserved" 2>/dev/null; then
		printf '   %s uses a GLSL reserved word as an identifier:\n' "$( basename "$shader" )"
		sed 's|^|      |' "$dir/reserved"
		bad=$(( bad + 1 ))
		continue
	fi
	if ! command -v glslc >/dev/null 2>&1; then
		continue
	fi
	if ! glslc --target-env=opengl4.5 -fauto-map-locations "$shader" -o /dev/null 2>"$dir/err"; then
		printf '   %s does not compile\n' "$( basename "$shader" )"
		sed "s|$dir/||; s|^|      |" "$dir/err"
		bad=$(( bad + 1 ))
	fi
done

if [ "$n" -eq 0 ]; then
	# No shaders at all is a FAILURE, not a pass: a check that silently looks
	# at nothing is worse than no check.
	echo "no shaders were dumped"
	exit 1
fi
if ! command -v glslc >/dev/null 2>&1; then
	echo "$n shaders free of reserved words; glslc not installed (brew install shaderc), compile skipped"
	exit $(( bad == 0 ? 3 : 1 ))
fi
[ "$bad" -eq 0 ] && echo "$n shaders, all compile, none uses a reserved word"
exit "$bad"
