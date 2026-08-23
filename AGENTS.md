# AGENTS.md — bringing an LLM up to speed on gaffer

Orientation for an AI assistant (or a new human) picking this project up cold.
`CLAUDE.md` holds the short command reference; this file explains the model and
the traps.

---

## 1. What this is

A **simulated lens on a depth map, with the music holding it**, as an FFGL
effect for Resolume Arena / Avenue and an OpenFX effect for Resolve, Nuke,
Natron and Vegas. C++/GLSL, CMake, public MIT.

There are two things it does, and one machine underneath both:

- **Rattle.** A camera in front of a loud PA does not move like the music. It
  moves like a camera. The pressure hits a body on a head on a set of legs, and
  that assembly answers at *its* frequency and keeps answering after the hit —
  and the lens elements are part of it, so the frame moves and the focus comes
  off at the same time.
- **Rack.** A focus puller with two marks taped on the barrel. Follow, Pull,
  Sweep and Stutter are four things one hand can do, and the fast end of Sweep
  is the frame polling through every plane in the picture several times a
  second.

The one idea to internalise before changing anything:

> **Both halves resolve to numbers on the CPU before the shader is told
> anything.** The GPU is handed a focal plane, a shift, a roll and a scale, and
> never learns that a rig or a focus puller exists. That is what makes the
> interesting half of this plugin testable without a GPU at all.

### Written down

A thin lens of focal length `f` focused at `z_f` puts its sensor at image
distance `v(z_f) = f z_f/(z_f - f)`. An object at `z` images at `v(z)`, so on
the sensor it is a disc of diameter `c = A |v(z) - v(z_f)| / v(z)`. Substitute
and every term in `z` collapses:

```
c = A f z_f / (z_f - f) * | 1/z - 1/z_f |
```

**The blur is linear in the difference of RECIPROCAL depths.** That is not an
approximation made for the shader's convenience — it is what the thin-lens
equation says, and the reciprocal of depth is disparity, which is what a depth
map holds. With the field normalised into 0..1 (1 nearest):

```
coc(d) = K * ( d - focus )
```

signed, because which side of the focal plane a surface sits on decides whether
it occludes what is behind it.

### One number does two jobs

Write `phi = f / z_near`. Then, up to a constant,

```
K = A phi / ( 1 - phi*focus )        v = f / ( 1 - phi*focus )
```

— the same denominator. `v` is the image distance, and the image distance sets
the frame's scale. So **depth of field collapsing as you focus closer** and
**the frame creeping as you rack** are one fact about where the sensor is, not
two effects arranged next to each other. `Breathing` exists only because real
lenses differ in how much of it they show.

`phi` is bounded below 1 by construction (`kMaxPhi`) and `focus` is 0..1, so the
denominator is unconditionally positive. **There is no divide-by-zero guard in
`Lens.cpp` and there must not be one** — the pole is a lens focused inside its
own focal length, which the parameter mapping cannot reach. If you find yourself
wanting to add a guard, something upstream has stopped holding an invariant and
*that* is the bug.

## 2. Where the depth comes from

A video clip has no depth, so this is the honest part.

```
Radial   an invented field: near on the axis, far at the corners
Luma     the clip's own brightness read as a depth map
Alpha    the clip's alpha read as a depth map
Split H  side by side: picture in the left half, depth in the right
Split V  over and under: picture on top, depth beneath
```

They are not five settings of one thing. Radial *invents* a field, so it works
on any footage at all and what it produces is the look of a shallow lens rather
than the lens. Luma and Alpha read a field out of the picture, which is either
the real thing or a mistake depending on the clip. **The Split modes are the
only two in which the picture and the depth are independent** — which is why
they are the only ones that can carry a real depth pass at full resolution, and
also why the offline harness measures the lens through Split H: it is the only
configuration in which an impulse can be put through a *known* depth.

All five end as a disparity in 0..1 and nothing downstream cares which.

## 3. The shape of it

One shader pass. Nothing accumulates on the GPU between frames and no pixel
depends on any other, so there are no intermediate buffers at all — which
sidesteps both `FFGLFBO` bugs in the fleet's SDK notes without having to think
about them.

```
source/Lens.{h,cpp}       the lens: circle of confusion, image distance,
                          breathing, and every 0..1 -> physical mapping the
                          shader mirrors
source/Rack.{h,cpp}       the focus puller: five modes, a cue grid, and a
                          stateless evaluator for hosts that render out of order
source/Rattle.{h,cpp}     the rig: four damped oscillators and an onset detector
source/Audio.{h,cpp}      the host's 64 FFT bins, smoothed and folded to a band
source/Clock.{h,cpp}      what time it is, and how long the last frame was
source/Controls.{h,cpp}   the mappings with no GLSL counterpart
source/Shaders.{h,cpp}    the GLSL: the camera transform, the depth field, and
                          the gather
source/Gaffer.{h,cpp}     FFGL host glue and the parameter declarations
source/ofx/GafferOFX.cpp  the OpenFX build: links the model, mirrors the pixel
                          machinery on the CPU
source/Presets.h          one preset table, read by BOTH builds
source/Diag.{h,cpp}       a log file, for the shader that will not compile
tools/gftest/             headless render and six measurements
tools/sweep.py            no control is silently dead
tools/verify.sh           all of the above, in one go
```

### The gather, and why every weight in it is an area

Read this before touching a number in `main()` in `Shaders.cpp`. The whole thing
falls apart the moment one of them is "tuned".

- A sample stands for some **area** of source. For the pixel's own surface that
  is one pixel; for a tap drawn out of the gather region it is that region's
  area divided by however many taps came from it.
- A sample is **spread** over the area of its own circle of confusion.
- Its contribution is the first divided by the second. Nothing else is a free
  parameter — **and that is why the result does not change when Quality does.**

An earlier version weighted the loop taps without an area measure, which made a
sharp subject in front of a blurred background 77% itself at 96 taps and 96%
itself at 12. A quality control that changes the look is a bug.

**Three layers, ordered against the receiving pixel's own surface** — not
against the focal plane:

```
NEAR   in front of this surface. Composited OVER, opacity = how much of the
       gather region it covers.
SAME   this surface. Its accumulated weight IS its opacity, because every
       weight is an area over an area: a surface filling the neighbourhood sums
       to one and hides what is behind it; one that only clips the edge of it
       sums to less, and the background shows through the difference.
FAR    behind this surface, visible only through what SAME does not cover.
```

Splitting at the focal plane instead — which is the obvious arrangement, and
what most one-pass DoF shaders do — dissolves a sharp foreground into the
background it is supposed to be hiding, because its own contribution lands in
the same bucket as everything it occludes.

### The reach, and what it gives up

The gather only reaches as far as the largest circle of confusion that can
legitimately land on this pixel: this surface's own, or that of anything in
front of it (at most the near plane's).

```
R = min( max( |coc(centreD)|, |coc(1)| ), cocMax )
```

Sizing it by the largest circle *any* depth could produce spends most of the
taps on a region nothing can reach from — which is not a performance detail, it
is where the samples that make the bokeh smooth would have gone.

**What it gives up, honestly:** a surface far behind this one and blurred much
harder reaches this pixel only as far as this surface's own disc. Its outermost
halo is clipped. That is the price of one pass and no buffers.

### Stratified in two

Half the taps go inside a disc the size of *this pixel's* own blur, where nearly
all of the answer is; half over the rest of the reach, where foreground spill
comes from. Each carries its own stratum's area, so the estimator is unchanged
and only its variance moves. Without it, a shallow depth of field with the focus
near one end of the barrel is sampled by a handful of taps and draws a **spiral
instead of a disc** — which is what `gftest --bokeh` measured as a bokeh 30%
narrower in y than in x.

### The maths exists twice, on purpose

It has to run per-pixel on the GPU and it has to be readable and testable on the
CPU. Two copies of one formula drift apart — so `gftest --bokeh` measures the
disc the GPU actually drew against what `Lens.cpp` predicts, and that is why the
duplication is safe. Change one copy, change the other, then run it.

The OpenFX build is a third copy of the *pixel machinery* but **not** of the
model: it links `Lens.cpp`, `Rack.cpp`, `Rattle.cpp` and `Controls.cpp` straight
from source. Edit the shader's gather and edit `GafferOFX.cpp` too.

The rack now exists twice as well — stateful for FFGL, stateless for OpenFX —
and `gftest --focus` measures the two against each other over 5,700 frames for
exactly that reason.

## 4. Traps

### `half` is a GLSL reserved word

It cost the first shader compile of this project. So are `smooth`, `flat`,
`input`, `output`, `sample`, `filter`, `active`, `common` and a long tail of
others. The failure mode is nasty: the shader fails to compile at **runtime**,
`InitGL` returns `FF_FAIL`, and Resolume shows an effect that silently does
nothing with no message anywhere. That is what `source/Diag.cpp` is for, and it
earned its keep on day one.

### The iris has to be evaluated in the direction of the RECEIVER

The polygon that maps the sampling disc onto an n-gon is evaluated at
`ang + pi`, not at `ang`, and the **same value has to be used in the coverage
test**. A sample scatters into an iris-shaped patch, and this pixel sits at the
offset from the sample *to here*, which is the opposite direction.

Two ways to get this wrong, and neither is visible in the code:

- Use `ang` in both places, and an even blade count looks correct while an odd
  one comes out upside down.
- Place the samples on the polygon but test coverage against the circular
  `rad`, and every corner is placed and then immediately rejected — a hexagon
  setting that silently renders a **circle**. That is what `gftest --iris`
  caught: a six-fold signature of 0.0076 against a round-iris baseline of
  0.0079, i.e. nothing at all.

The blade count is also area-normalised (`irisK`), so choosing five blades
changes the *shape* of a highlight and not how much of the picture it covers.

### Highlight is a power MEAN, not a per-sample weighting

The obvious way to buy an out-of-focus highlight is to weight each sample by its
own brightness before averaging. That was in here first and it is a trap.

A weighted average normalises by the sum of the weights, so at a strong setting
the answer is simply whichever tap happened to be brightest. On high-contrast
material — which is exactly the material a highlight control is for — the output
stops being a disc and becomes a picture of the sampling pattern: hard combs and
blocks, a frame that gets *sharper* as the blur radius grows, and **no
improvement at all from more taps**, because it is variance in the estimator
rather than noise. 128 taps looked the same as 32.

The fix is to leave the area weights alone and transform the values instead:
raise, average, take the root. `Highlight` is that exponent, and it is **exactly
1** at the bottom of the control — both the shader and the OpenFX build branch
on that equality, so the mapping has to produce exactly 1 rather than nearly 1.

One consequence worth knowing: a colour that goes through the transform and back
can lose a last bit to `pow`. So the gather counts how many loop taps were
accepted, and **zero is handled separately** — nothing but this pixel's own
surface reached it, so the answer is that surface, untouched. That is not an
optimisation; it is what keeps the fully-focused null byte-exact at any
Highlight setting.

### The coverage ramp runs outward, not centred

`cov = clamp( (rad*shape - dist) / pixelH, 0, 1 )` — from `rad == dist` outward.
Centred on it (`... + 0.5`), a sample in perfect focus still lends a fraction of
itself to everything within half a pixel. That is defensible as resampling and
wrong as a lens: it means a frame with the focal plane exactly on the subject is
not quite the frame that went in. `gftest --null` measures it as bytes, and it
is the difference between **28,946 of them and none**.

It costs about half a pixel of measured radius, consistently, which is why
`--bokeh` reads ~0.5 px under prediction at every setting and the tolerance is
1.5 px.

### The Split seam must be clamped inside its own rect

Half a texel matters here in a way it does not elsewhere: a linear fetch at the
inside edge of the picture half takes half its weight from the **depth** half.
That is not a slightly wrong colour, it is an unrelated one. `fetchRect()`
clamps against the rectangle, never against the texture.

### The depth fetch is NOT routed through the edge mode

Deliberately, and for the same reason as the sibling plugin: off the frame the
depth extends from the edge whatever the picture is doing there, because
Transparent would read as alpha 0 — "infinitely far" — and put a ring of maximum
defocus just outside every frame.

### The rig integrator must be substepped, and the TIME clamped

Semi-implicit Euler is stable while `w*h < 2`. The frame can be 1/24 s and the
fastest axis runs at 1.73 × 24 Hz ≈ 41 Hz, which is well past that. `Rattle`
steps at a fixed 1/480 s and clamps the *elapsed time* rather than the step
count — capping the count instead keeps the substep proportional to the frame,
so an unclamped delta diverges anyway. `gftest --rattle` drives it with
one-second frames to prove it does not.

Each axis also has its **own** `w`. Sharing one was a real bug here: the
frequencies are declared apart precisely so the axes do not peak together, and
an oscillator integrated at a frequency other than the one it was kicked at
rings at the wrong pitch with no visible symptom.

### The rig is driven by the CHANGE in the envelope, not its level

A sustained bass note is a big envelope and a nearly constant one. Fed in as a
force it would lean the camera over and hold it there, which is not what
happens: the pressure a held note carries oscillates far above anything a rig
can follow. What shakes a rig at frame rate is the envelope's *arrival*. So the
drive is `max(0, d(env)/dt)`, gated at Threshold — a kick rings the rig and a
drone does not move it.

### Onset detection deliberately survives Drive at zero

Drive is how hard the music shakes the **camera**. The rack's audio cue is a
separate question, and "pull focus on the kick but keep the camera still" is a
real request rather than a contradiction. `Rattle::Update` therefore runs the
detector before it checks the drive, and `gftest --rattle` asserts both halves:
exactly zero displacement, and onsets still detected.

### Resolume sends SetTime in MILLISECONDS

The FFGL header never says. Measured live in Arena 7.27.1 at 20.0 per frame at
its 50 fps, and the SDK's own Particles sample divides by 1000 — while this
harness, and any host reading the header's silence as SI, sends seconds. A
plugin that assumes wrong runs a thousand times fast or freezes, **and no
offline harness can catch it**, because the harness is the thing sending
seconds.

`Clock` measures the host's clock against a real one and needs four frames to
agree before it settles. Until then it runs on the wall clock — wrong in origin,
right in rate, which is the safe way round.

### A ranged parameter cannot have a ranged default

`SetParamRange` exists and Resolume honours it, but
`CFFGLPluginManager::SetParamInfo` **clamps an `FF_TYPE_STANDARD` default into
0..1** before returning, and `SetParamRange` can only be called afterwards. There
is no `SetParamDefault`. So a parameter declared in hertz cannot declare a
default in hertz. **Every parameter here is a plain 0..1 float**, and the
conversions live in `Lens.cpp` and `Controls.cpp`. (SDK `b1afaf9`.)

### A display-only TEXT parameter needs `SetTextParameter` overridden

The SDK's `instantiateGL` pushes every parameter's default into a fresh instance
and destroys it on the first `FF_FAIL` — and the base
`CFFGLPlugin::SetTextParameter` is a stub returning `FF_FAIL`. Declaring the
About line without overriding the setter means **no real host can instantiate
the plugin at all**, and the harness cannot see it because it drives the class
directly and bypasses `plugMain`.

### The host owns the parameters, and a preset cannot forget it

Copied wholesale from the sibling plugin's issue #2, which reached a user there
and is fixed here before it could reach one. A preset is an **override**, not a
write: `effective()` returns the preset's value and both the render and
`GetFloatParameter` go through it. `hostValues[]` records what the host last
*sent*, which is not what the plugin is rendering with — an operator's edit
differs from the host's last word, and the host restating itself does not.

`gftest --presets` drives all three host behaviours (honours the value events,
ignores them, honours them but quantises to 1/1000) across every preset with no
GL involved.

### The plugin registers itself from a static constructor

`CFFGLPluginInfo` is a file-scope object in `Gaffer.cpp` that nothing references
by name. That is why `gaffer_core` is an **OBJECT** library and not a **STATIC**
one: in an archive the linker may drop the whole translation unit, and you get a
bundle that loads, exports `plugMain`, and reports that it contains no plugins.
Do not "tidy" it to STATIC. `verify.sh` greps the binary's strings for `GF01`,
which is present if and only if that translation unit survived.

### The macOS one that will get you

**`CMAKE_OSX_ARCHITECTURES` must be set before the first target is created.** Set
it later and CMake silently ignores it — an arm64-only binary that the build log
calls a success, and an Intel Resolume that quietly fails to load the plugin.
Always verify the artefact:

```bash
lipo -archs build/Gaffer.bundle/Contents/MacOS/Gaffer
```

### `vcpkg.json` is invisible from the CMakeLists

`find_package(GLEW REQUIRED)` is guarded by `if(NOT APPLE)`, and GLEW arrives
through the vcpkg manifest — which nothing in `CMakeLists.txt` mentions. Delete
it and every local build and every macOS CI job stays green while the Windows
job fails at *configure*.

### `cmake/InfoOFX.plist.in` is parameterised, and must stay that way

The version this was copied from in another repo had the previous plugin's name
hardcoded into `CFBundleExecutable`. That fails nothing — the bundle assembles,
the binary is universal, `nm` finds `OfxGetPlugin`, and a probe host renders a
correct frame — until `codesign` in the release job, after the tag, with a
message about a "subcomponent" that never mentions the plist. `verify.sh` runs
that exact step locally.

## 5. Testing

```bash
tools/verify.sh          # everything below, about 8 seconds
```

There is no unit-test rig for the picture and there cannot usefully be one. But
**half of this plugin is a number per frame**, and the other half is a
convolution whose kernel is literally visible.

### `--focus` and `--rattle` — no GPU at all

The properties are asserted directly, over thousands of frames:

- Off is **exactly** the Focus control, even when cued.
- Follow never moves faster than a full barrel in Speed seconds — measured worst
  frame 0.020833 against a limit of 0.020833 — and does reach its target.
- A Bar-synced sweep is periodic on the bar to **8.9e-16**, and reaches both
  marks.
- A pull lands exactly on a mark and alternates.
- Stutter is reproducible — a saved composition has to play back the same way
  twice.
- The stateless evaluator and the stateful one agree to **2.8e-17** over 5,700
  frames. That is the check that keeps the FFGL and OpenFX builds rendering the
  same rack.
- Drive at zero is an exact null, **and onsets are still detected**.
- 20,000 adversarial frames — a full-scale onset on alternate frames, straight
  into a 24 Hz resonance at the lightest damping — never exceed the travel
  limit. One-second frames do not diverge.
- One hit rings at 8.0 Hz against 8.0 declared, and decays.

### `--bokeh` — the impulse response of a lens IS the bokeh

Render a block of light through a *known, uniform* depth (Split H) and measure
the disc. One render answers three questions:

- the **radius**, against `Lens.cpp` — the only thing checking that the GLSL
  copy of the lens maths still agrees with the C++ one;
- the **energy**, against the same frame through a pinhole, because a lens
  neither creates nor destroys light and the `1/(pi r^2)` weighting is exactly
  that claim. Measured 1.0000 of the pinhole;
- and independently **in x and in y**, because a disc that is not round is an
  aspect-ratio bug that no amount of looking at a picture reliably finds.

48 combinations of focus × depth level × focal length pass; 12 are reported SKIP
because the predicted disc is too small to separate from the impulse's own width
or too big to stay in frame. A run of nothing but skips is a failure.

Two details that are not obvious and were both wrong first:

- **The measurement reads back at 32-bit float, not 8-bit.** A second moment
  lives in the dim outer ring of a disc, which is exactly what 8-bit
  quantisation throws away; the measured radius came out several percent short
  by a margin that changed with the shape of the impulse rather than with
  anything about the lens.
- **The impulse's own second moment is removed analytically**, by measuring the
  pinhole render too. A convolution adds variances, so the block's width comes
  out exactly rather than being tolerated — which is what allows a block big
  enough to see.

### `--iris` — n-fold symmetry, with the tap pattern subtracted

An angular Fourier spectrum of the bokeh, weighted by `r^2`. The outline is the
obvious approach and does not work: a gather draws a point of light as a few
dozen discrete taps, so the *edge* is a scatter of blobs and walking out to find
it measures the tap pattern.

The round iris is measured **first, as a baseline**, and subtracted as a vector.
The tap spiral is identical for every blade count, so what is left is exactly
what the iris did. Without the subtraction "8 blades" and "round" are
indistinguishable — the spiral's own 8-fold signature at this radius is 0.0733,
about as strong as an octagon's. With it: 5→0.1695, 6→0.1109, 8→0.0571,
9→0.0439, each at least twice its nearest unrelated harmonic.

### `--null` — two nulls, byte for byte

Aperture closed is the identity. So is a frame with the focal plane exactly on a
flat depth field — **with the whole gather running**, every tap rejected by the
coverage rule and only the pixel's own surface surviving. The second is the one
worth having: an error in the coverage rule that let one stray tap through would
be invisible in a picture and is one byte here. Both report 0 of 921,600.

### `--presets` — the parameter plumbing

Three host behaviours × every preset, no GL. See the trap above.

### A dead control is invisible to the compiler

```bash
python3 tools/sweep.py
```

All 30 parameters affect the output. Four things about this plugin will fool a
sweep, and all four cost a wrong answer before they were written down:

- **Half the controls are temporal.** A rack that sweeps, a rig that rings and
  an envelope that decays are all identical at t = 0. Every render walks 60
  frames of clock, transport and spectrum first.
- **The Rattle group is dead without audio.** `gftest --audio` injects a
  synthetic kick-and-hat spectrum through the same buffer parameter a host
  writes to. Without it the sweep reports nine dead controls, all false.
- **Rate is ignored while Sync is on a bar division** — correctly — and Speed
  needs a mode that has discrete moves. Both have CONTEXT entries.
- **The sampling instant matters.** At 120 bpm, frame 60 lands exactly on a
  quarter-note cue, where a rack taking 30 ms and one taking 5 s are both still
  on their starting mark. FRAMES is 67 for that reason, and Speed is sampled
  through a single hand cue rather than off the grid.

### The harness's own orientation trap

The test-picture builders return **bottom-up** buffers, ready for
`glTexImage2D`. `readBack()` returns **top-down**. Comparing one against the
other index by index silently compares row `y` with row `height-1-y` — which
does not look like a bug, because it produces a large, plausible and completely
*constant* error that does not change when the effect does.

### The OpenFX side

`ofxprobe` lives in the sibling `resolume-ofx-bridge` repo and is not wired into
`verify.sh`, because it may not be built:

```bash
../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.gaffer
```

Three things worth running there, all of which pass today:

- **The nulls**: `--set aperture=0 --set breathing=0`, and
  `--set aperture=0.6 --set breathing=0 --set depthGain=0.25 --set focus=0.5`,
  each report **0 bytes differ**. Note there is no `quality=0` caveat here —
  unlike the sibling plugin, this effect's nulls hold at every Quality setting,
  because the gather rejects rather than resamples.
- **Every preset, and its hand-set equivalent**: `--edit preset=N` (a real user
  edit, so the preset logic runs) is byte-identical to the same values set by
  hand, and no preset matches the null.
- All ten presets render something: 5,105 to 18,001 of 36,864 bytes differ.

## 6. What has never been checked

- **It has never been loaded into Resolume.** Not once. Everything here is the
  offline harness and `ofxprobe`. `cmake --install` puts the bundle where Arena
  looks.
- **It has never been loaded into Resolve** or any other OpenFX host. The OFX
  build has only met `ofxprobe`, so real texture sizes, tiling behaviour and
  premultiplication in a real host are unconfirmed — exactly what an offline
  harness cannot tell you, because it supplies its own images.
- **No audio has ever reached it.** The FFT reader has only ever seen `gftest`'s
  synthetic kick-and-hat. Whether Resolume's spectrum has the level and shape
  the onset detector's threshold assumes is the first thing to check in a host.
- **The transport has never been seen.** `SetBeatInfo`, the millisecond clock
  detection, and every bar-synced rack mode are driven by the harness's own
  synthetic transport.
- **No real depth pass has ever been through it.** The depth modes have only
  been fed a radial card, a luma checkerboard and a flat grey half-frame.
- **The Windows build has never been run**, or built — no release has been cut.
  A `workflow_dispatch` run of `release.yml` builds both platforms and publishes
  nothing, which is the cheap way to check the build before tagging.
- **Nothing has been timed at all.** Extreme is 128 taps, each with its own
  depth fetch, and each of those is a 5-tap when Smooth is up — 1,280 fetches
  per pixel in the worst case. Nobody has measured what that costs at 4K.
- Everything here comes from one M4 Max, never from CI — hosted macOS runners
  have no GPU, so `gftest`'s GL measurements cannot run there. The CI job
  compiles and checks the registration, and deliberately claims nothing else.

## 7. Conventions

- Public repo. "Commit" means commit **and** push.
- Standard AI disclaimer at the top of the README. Its last sentence is a
  factual claim about what has been verified, and it rots like any other; update
  it when section 6 changes.
- `source/StoatworksAbout.h` is normally generated by the backend's
  `sync-about.py` from the website's `projects.json`. This repo's copy is
  hand-written in the generated shape because there is no website entry yet, and
  `guide` and `page` are empty strings on purpose — the About block leaves a
  missing link out rather than showing a button that opens a 404.
- `ATTRIBUTIONS.md` is likewise generated from the backend; edit it there.

## Diagnostics

`source/Diag.{h,cpp}` is a small member of the fleet's `diag` family: a log file
only. No crash handler (a plugin has no business installing a process-wide signal
handler inside Resolume) and no bundle command (there is no UI to hang one off).

It covers the failure that actually happens — `InitGL` returning `FF_FAIL`
because the shader would not compile, which from the operator's side looks like
"the effect does nothing" with no message anywhere. The GL vendor/renderer/
version strings sit next to it because with one shader stage the driver is
nearly always the rest of the answer. One line is also written at frame 60 with
the host's raw clock value, the unit that was inferred from it, the frame time
and the transport — enough to settle "what did that host actually send" without
a log entry every frame for the rest of the show.

```
~/Library/Logs/gaffer/gaffer.<date>.log         # macOS
%LOCALAPPDATA%\gaffer\logs\                     # Windows
```

`GAFFER_LOG_DIR` overrides the location.
