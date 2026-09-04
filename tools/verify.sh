#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   --focus        the focus puller against the properties it claims. No GL:
#                  where the focal plane is, is a number, and a number can be
#                  checked over ten thousand frames in a few milliseconds.
#   --rattle       the rig, likewise -- including that no input at all can make
#                  an oscillator run away, which is the one that matters on a
#                  show.
#   --presets      every factory preset survives every host behaviour. The
#                  parameter plumbing, which is the half of a plugin that
#                  external users actually get stuck on.
#   --null         the two nulls, byte for byte. Aperture closed is the
#                  identity, and so is a frame with the focal plane exactly on
#                  a flat depth field -- the second one with the whole gather
#                  running, which is what makes it worth having.
#   --bokeh        an impulse through the lens, measured against Lens.cpp: the
#                  radius, the roundness, and the light it neither created nor
#                  destroyed. The only thing checking that the GLSL copy of the
#                  lens maths still agrees with the C++ one.
#   --iris         the bokeh's rotational symmetry against the blade count.
#   sweep.py       that no control is silently dead.
#   registration   that the bundle contains a plugin at all.
#   OFX plist      that the OpenFX bundle will codesign -- the release step, run
#                  here where it is cheap instead of after the tag.
#   lipo           that the macOS build really is universal.
#
# The bokeh check declines to answer where the picture cannot support the
# question -- a disc too small to measure against the impulse's own width, or
# too big to stay in frame. Those are reported as SKIP, not as passes, and a run
# made entirely of skips is treated as a failure.
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build}"
failures=0

step() {
	printf '\n\033[1m== %s\033[0m\n' "$1"
}

if [ ! -x "$BUILD/gftest" ]; then
	echo "$BUILD/gftest not found. Run:"
	echo "  cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

# ---------------------------------------------------------------------------
# The half with no pixels first: it needs no GPU, it takes no time, and it is
# where the two things this plugin is actually FOR are implemented.
# ---------------------------------------------------------------------------
for check in focus rattle presets; do
	step "$check"
	if "$BUILD/gftest" "--$check"; then
		:
	else
		printf '\033[31mFAILED: %s\033[0m\n' "$check"
		failures=$((failures + 1))
	fi
done

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders: every one through a real GLSL compiler"
if ! shaders_compile; then
	failures=$(( failures + 1 ))
fi

# ---------------------------------------------------------------------------
# The nulls. Nothing here needs interpreting: either the bytes match or they do
# not.
# ---------------------------------------------------------------------------
step "nulls: a closed aperture, and a frame entirely in focus, are the identity"
if "$BUILD/gftest" --null; then
	:
else
	printf '\033[31mFAILED: a null is not exact\033[0m\n'
	failures=$((failures + 1))
fi

# ---------------------------------------------------------------------------
# The lens, measured. This is the check that guards the duplication: the circle
# of confusion is written once in Lens.cpp and once in GLSL, and two copies of
# one formula drift.
# ---------------------------------------------------------------------------
step "bokeh: the disc the GPU drew against the circle of confusion Lens.cpp predicts"
bokeh_pass=0; bokeh_fail=0; bokeh_skip=0
for focus in 0.00 0.25 0.50 0.75 1.00; do
	for level in 0.10 0.35 0.65 0.95; do
		for focal in 0.0 0.5 0.9; do
			"$BUILD/gftest" --bokeh --block 12 --depth-level "$level" \
				--set "Focus=$focus" --set "Aperture=1.0" --set "Focal Length=$focal" \
				>/dev/null 2>&1
			case $? in
				0) bokeh_pass=$((bokeh_pass + 1)) ;;
				2) bokeh_skip=$((bokeh_skip + 1)) ;;
				*)
					bokeh_fail=$((bokeh_fail + 1))
					printf '\033[31mFAILED: bokeh focus=%s level=%s focal=%s\033[0m\n' \
						"$focus" "$level" "$focal"
					failures=$((failures + 1))
					;;
			esac
		done
	done
done
printf '   %d passed, %d failed, %d not measurable at those settings\n' \
	"$bokeh_pass" "$bokeh_fail" "$bokeh_skip"
if [ "$bokeh_pass" -eq 0 ]; then
	printf '\033[31mFAILED: the bokeh check never actually ran\033[0m\n'
	failures=$((failures + 1))
fi

step "iris: the bokeh has the rotational symmetry the blade count claims"
if "$BUILD/gftest" --iris --block 10 --depth-level 0.98; then
	:
else
	printf '\033[31mFAILED: the aperture shape is not what Blades says it is\033[0m\n'
	failures=$((failures + 1))
fi

# ---------------------------------------------------------------------------
# The demo's copy of the shader.
#
# demo/plugin.js cannot include a C++ file, so the browser demo carries a second
# copy of the GLSL and nothing about the build says a word when they diverge.
# ---------------------------------------------------------------------------
if [ -f demo/tools/check_shaders.py ]; then
	step "the demo runs the plugin's shader"
	if python3 demo/tools/check_shaders.py; then
		:
	else
		printf '\033[31mFAILED: demo/plugin.js has drifted from source/Shaders.cpp\033[0m\n'
		failures=$((failures + 1))
	fi
fi

# ---------------------------------------------------------------------------
# A dead control is invisible to the compiler.
# ---------------------------------------------------------------------------
step "sweep: no control silently dead"
if python3 tools/sweep.py > "${TMPDIR:-/tmp}/gaffer-sweep.txt" 2>&1; then
	tail -1 "${TMPDIR:-/tmp}/gaffer-sweep.txt"
else
	printf '\033[31mFAILED: dead controls, see %sgaffer-sweep.txt\033[0m\n' "${TMPDIR:-/tmp}/"
	tail -4 "${TMPDIR:-/tmp}/gaffer-sweep.txt"
	failures=$((failures + 1))
fi

# ---------------------------------------------------------------------------
# Registration.
#
# The failure this catches is specific and silent: CFFGLPluginInfo registers
# itself from a file-scope constructor and nothing references it by name, so a
# linker that drops the translation unit gives a bundle which loads, exports
# plugMain, and reports that it contains no plugins. Resolume shows an empty
# effects list and no error.
#
# Read once into variables rather than piping into `grep -q`. `grep -q` exits
# the instant it matches, which closes the pipe under the still-running nm or
# strings; they take SIGPIPE and exit 141, and with `set -o pipefail` the
# pipeline is then a failure however well the grep went.
# ---------------------------------------------------------------------------
step "the bundle contains its plugin"
binary="$BUILD/Gaffer.bundle/Contents/MacOS/Gaffer"
if [ ! -f "$binary" ]; then
	printf '\033[31mFAILED: %s not built\033[0m\n' "$binary"
	failures=$((failures + 1))
else
	symbols=$(nm -gU "$binary" 2>/dev/null)
	literals=$(strings "$binary" 2>/dev/null)

	if ! grep -q plugMain <<<"$symbols"; then
		printf '\033[31mFAILED: Gaffer exports no plugMain\033[0m\n'
		failures=$((failures + 1))
	elif ! grep -qx "GF01" <<<"$literals"; then
		printf '\033[31mFAILED: Gaffer does not carry its own id GF01\033[0m\n'
		failures=$((failures + 1))
	else
		printf 'ok   Gaffer exports plugMain and carries GF01\n'
	fi
fi

# ---------------------------------------------------------------------------
# The OpenFX bundle's plist.
#
# This check exists because it went wrong elsewhere in the fleet.
# cmake/InfoOFX.plist.in is one of the files copied from repo to repo when a new
# plugin starts, and the version it was copied from had the PREVIOUS plugin's
# name hardcoded into CFBundleExecutable. NOTHING caught it: the bundle
# assembles, the binary is universal, the OFX entry point exports, and a probe
# host loads it and renders a correct frame. It fails only at release time, in
# codesign, with a message that names a "subcomponent" and never mentions the
# plist.
#
# So the check is the release step itself, run here where it is cheap. On a copy
# of the bundle, so a verify run never leaves a signature on the build tree that
# the release job did not put there.
# ---------------------------------------------------------------------------
if [ -d "$BUILD/Gaffer.ofx.bundle" ]; then
	step "the OpenFX bundle signs"

	plist="$BUILD/Gaffer.ofx.bundle/Contents/Info.plist"
	named=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$plist" 2>/dev/null)
	if [ ! -f "$BUILD/Gaffer.ofx.bundle/Contents/MacOS/$named" ]; then
		printf '\033[31mFAILED: Info.plist names "%s", which is not in Contents/MacOS\033[0m\n' "$named"
		failures=$((failures + 1))
	else
		scratch="${TMPDIR:-/tmp}/gaffer-signcheck.ofx.bundle"
		rm -rf "$scratch"
		cp -R "$BUILD/Gaffer.ofx.bundle" "$scratch"
		if codesign --force --sign - --timestamp=none "$scratch" >/dev/null 2>&1; then
			printf 'ok   CFBundleExecutable is %s, and the bundle ad-hoc signs\n' "$named"
		else
			printf '\033[31mFAILED: the OpenFX bundle will not codesign\033[0m\n'
			codesign --force --sign - --timestamp=none "$scratch" 2>&1 | sed 's/^/       /'
			failures=$((failures + 1))
		fi
		rm -rf "$scratch"
	fi

	ofxSymbols=$(nm -gU "$BUILD/Gaffer.ofx.bundle/Contents/MacOS/Gaffer.ofx" 2>/dev/null)
	if ! grep -q OfxGetPlugin <<<"$ofxSymbols"; then
		printf '\033[31mFAILED: the OpenFX bundle exports no OfxGetPlugin\033[0m\n'
		failures=$((failures + 1))
	else
		printf 'ok   the OpenFX bundle exports OfxGetPlugin\n'
	fi
fi

# ---------------------------------------------------------------------------
# Universal.
#
# CMake latches CMAKE_OSX_ARCHITECTURES when the first target is created, so
# setting it late is silently ignored and the build log still says success. The
# only honest answer comes from lipo. Skipped when the developer asked for a
# single-architecture build on purpose.
# ---------------------------------------------------------------------------
step "the macOS build is universal"
if grep -q "CMAKE_OSX_ARCHITECTURES:.*arm64;x86_64" "$BUILD/CMakeCache.txt" 2>/dev/null; then
	for candidate in "$BUILD/Gaffer.bundle/Contents/MacOS/Gaffer" \
	                 "$BUILD/Gaffer.ofx.bundle/Contents/MacOS/Gaffer.ofx"; do
		[ -f "$candidate" ] || continue
		arches=$(lipo -archs "$candidate" 2>/dev/null)
		case "$arches" in
			*arm64*x86_64* | *x86_64*arm64*)
				printf 'ok   %s: %s\n' "$(basename "$candidate")" "$arches" ;;
			*)
				printf '\033[31mFAILED: %s is %s, not universal\033[0m\n' \
					"$(basename "$candidate")" "${arches:-missing}"
				failures=$((failures + 1)) ;;
		esac
	done
else
	echo "skipped: this build was configured for one architecture"
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit "$failures"
