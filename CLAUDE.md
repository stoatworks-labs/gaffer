# gaffer

A simulated lens on a depth map, driven by the music: bass rattles the rig, and
the focus racks. FFGL effect for Resolume Arena/Avenue and an OpenFX effect for
Resolve/Nuke/Natron/Vegas. C++/GLSL, CMake MODULE → universal `.bundle` (macOS)
+ Windows `.dll`. Public MIT repo.

Read `AGENTS.md` before changing the gather or the lens maths.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/gftest --out /tmp/frame.png`
- Render it MOVING (both halves of this plugin are temporal — a single frame
  cannot tell a Sweep from a Stutter):
  `./build/gftest --out /tmp/frame.png --frames 60 --audio`
- Watch what the rack and the rig are doing: add `--trace`
- List parameters: `./build/gftest --list`

## Verify
- Everything (~8s): `tools/verify.sh`
- No GPU needed: `./build/gftest --focus`, `--rattle`, `--presets`
- The lens, measured: `./build/gftest --bokeh --set "Focus=0.2" --set "Aperture=0.8"`
- The iris shape: `./build/gftest --iris --block 10 --depth-level 0.98`
- Both nulls, byte for byte: `./build/gftest --null`
- No dead controls: `python3 tools/sweep.py`

## OpenFX build
- `source/ofx/GafferOFX.cpp` → `build/Gaffer.ofx.bundle` (target `GafferOFX`,
  `-DBUILD_OFX=OFF` to skip). It links `Lens.cpp`, `Rack.cpp`, `Rattle.cpp` and
  `Controls.cpp` directly — the model has one home — but mirrors the fragment
  shader's pixel machinery (camera transform, depth field, stratified gather,
  coverage rule, three-layer composite) on the CPU. Change the shader's pixel
  machinery, change this too.
- Two things FFGL supplies and OFX does not, handled rather than dropped:
  **no transport**, so there is a `Tempo` control here only; **no audio**, so
  the rig is driven by a `Kick` pulse on a division of that tempo.
- A host renders frames in any order, so nothing accumulates: Off/Pull/Sweep/
  Stutter go through `Rack::EvaluateStateless`, and Follow and the rig are
  simulated over a bounded look-back ending at the frame being rendered.
- OFX SDK subset (BSD-3) vendored under `external/openfx`.
- Smoke test: `../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.gaffer`
- Null proof (**no `quality=0` caveat here**, unlike the sibling plugin — the
  gather rejects rather than resamples, so the nulls hold at every Quality):
  `--set aperture=0 --set breathing=0` → 0 bytes differ, and
  `--set aperture=0.6 --set breathing=0 --set depthGain=0.25 --set focus=0.5`
  likewise.
- Preset proof: `--edit preset=N` (a real user edit, so the preset logic runs)
  must be byte-identical to the same values set by hand, and must NOT match the
  null.
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins`.

## Notes
- One shader pass, no intermediate buffers. Both moving halves resolve to
  numbers on the CPU; the GPU is handed a focal plane, a shift, a roll and a
  scale.
- The lens is written in **disparity** because that is what the thin-lens
  equation says: `coc = K(d - focus)`, linear in the difference of reciprocal
  depths. `K` and the frame scale share the denominator `1 - phi*focus`, so
  shallower depth of field up close and focus breathing are one fact. `phi < 1`
  by construction — there is no divide-by-zero guard and there should not be one.
- **Every weight in the gather is an area over an area.** Do not tune one. The
  result is independent of the Quality setting because of it.
- Near/Same/Far are split against the **receiving pixel's own surface**, not
  against the focal plane. `sameW` clamped to 1 IS the surface's opacity.
- `half` is a GLSL reserved word. So are `smooth`, `flat`, `input`, `output`,
  `sample`, `filter`. Shader errors surface only at runtime, in the diagnostics
  log.
- The iris polygon is evaluated at `ang + pi` and the **same** value used in the
  coverage test — otherwise the corners are placed and immediately rejected, and
  a hexagon renders a circle.
- The coverage ramp runs outward from `rad == dist`, not centred on it. Centred,
  a fully focused frame is not byte-identical to its input.
- The rig is driven by the **change** in the envelope, not its level. Onset
  detection deliberately still runs with Drive at zero, so "pull focus on the
  kick, camera still" works.
- `Rattle` substeps at 1/480 s and clamps the elapsed TIME, not the step count.
  Each axis has its own frequency and must be integrated at it.
- Resolume sends `SetTime` in **milliseconds**. `Clock` measures the unit rather
  than assuming it, and runs on the wall clock until four frames agree.
- All host parameters are 0..1 and mapped internally. `SetParamInfo` clamps a
  standard default into 0..1 before `SetParamRange` can widen it.
- `SetTextParameter` must be overridden for the About block or
  `FF_INSTANTIATE_GL` fails for the whole plugin, invisibly to the harness.
- Geometry is in output picture space; the rect mapping and `MaxUV` are applied
  only at the fetch, and every fetch stays half a texel inside **its own rect**
  — in the Split modes the picture's inside edge is the depth map's outside edge.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It exists for the one failure that actually
happens: a shader that will not compile, which otherwise looks like "the effect
does nothing" with no message anywhere. It logs the GL vendor/renderer/version
next to it, and one line at frame 60 with the host's raw clock, the unit
inferred from it, the frame time and the transport. `~/Library/Logs/gaffer/`, or
`GAFFER_LOG_DIR`.
