"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline where the
effect is genuinely doing something, and report any that made no difference.

    python3 tools/sweep.py

Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL. Exit code 1 means something is dead.

Four things about THIS plugin that will fool you, all of which cost a wrong
answer before they were written down:

  * **Half the controls are temporal and a single frame cannot see them.** A
    rack that sweeps, a rig that rings and an envelope that decays are all
    identical at t = 0. Every render here walks 60 frames of clock, transport
    and spectrum first, exactly as a host would, and keeps the last one.

  * **The Rattle group is dead without audio.** There is no sound here and none
    on a CI runner, so gftest injects a synthetic kick-and-hat spectrum through
    the same buffer parameter a host writes to. Without `--audio` this file
    would report nine dead controls, every one of them a false alarm.

  * **Rate is ignored while Sync is on a bar division**, which is correct and
    makes the control read dead against the default baseline. Its CONTEXT entry
    puts Sync back to Free. Speed is the same story the other way: it times a
    discrete move, so it needs Pull rather than Sweep.

  * **The FFT buffer parameter has no meaningful float value.** Sweeping it
    reports a false dead, so it is skipped by name.
"""
import subprocess, zlib, struct, sys, tempfile

SC = tempfile.mkdtemp(prefix="gfsweep")

WIDTH, HEIGHT = 640, 360

# Not a round number, and not a multiple of the cue grid, on purpose: at 120 bpm
# frame 60 lands exactly ON a quarter-note cue, where a rack that takes 30 ms and
# a rack that takes 5 s are both still at their starting mark. Speed reported as
# dead, and it was the sampling instant rather than the control.
FRAMES = 67

# A baseline where both halves of the effect are moving, so nothing reads dead
# merely because the thing it modifies is switched off.
BASE = {
    "Focus": 0.55,
    "Aperture": 0.50,     # NOT 0 -- a pinhole is the identity
    "Focal Length": 0.45,
    "Blades": 2,          # six, so Rotation has corners to turn
    "Rotation": 0.0,
    "Highlight": 0.30,
    "Breathing": 0.35,

    "Depth": 3,           # Split H: the card carries a real depth ramp in its right half
    "Depth Gain": 0.50,   # NOT the flat setting, which is a scene at one distance
    "Falloff": 0.50,
    "Smooth": 0.25,

    "Rack": 3,            # Sweep, so the focus is somewhere other than its mark
    "Mark B": 0.05,
    "Speed": 0.50,
    "Rate": 0.50,
    "Sync": 6,            # 1/4
    "Ease": 1.0,
    "Pull": 0.0,

    "Drive": 0.80,        # NOT 0 -- the rig at rest is the identity
    "Band": 1,            # Low
    "Threshold": 0.15,
    "Release": 0.35,
    "Shake": 0.60,
    "Roll": 0.50,
    "Resonance": 0.50,
    "Damping": 0.45,
    "Defocus": 0.50,

    "Edges": 2,           # clamp, so edge pixels carry picture rather than nothing
    "Quality": 1,
}

# Continuous controls whose two ends are the SAME value. Rotation is a full turn,
# so 0 and 1 are the same iris angle -- sweeping it end to end measures nothing.
RANGE = {
    "Rotation": (0.0, 0.5),
}

# Options are discrete; sweep them across their real element range.
DISCRETE = {
    "Blades": (0, 5),
    "Depth": (0, 4),
    "Rack": (0, 4),
    "Sync": (0, 8),
    "Band": (0, 3),
    "Edges": (0, 4),
    "Quality": (0, 3),
    "Preset": (0, 10),
    "Pull": (0, 1),
}

# The buffer parameter the host fills with FFT bins. Its float value means
# nothing, so sweeping it is a guaranteed false alarm.
SKIP = {"Audio"}

# A few controls need a baseline of their own to be visible at all.
CONTEXT = {
    # Rate only does anything when the cue grid is NOT counted in bars.
    "Rate": {"Sync": 1},
    # Speed times a discrete move, which Sweep does not have -- and it has to be
    # sampled DURING one. On a cue grid the fast rack has landed and is sitting
    # on the same mark the slow one started from, so the two agree to three
    # decimal places at almost every instant. One hand cue at frame zero and no
    # grid at all puts the whole move under the sampling frame.
    "Speed": {"Rack": 2, "Sync": 0, "Pull": 1.0, "Focus": 0.95, "Mark B": 0.0,
              "Aperture": 0.6},
    # The button cues a rack, and only a mode with cues can answer it.
    "Pull": {"Rack": 2, "Sync": 0, "Speed": 0.3},
    # Breathing scales the frame by how far the focus is from the middle of the
    # barrel, so it does nothing at all with the focus in the middle.
    "Breathing": {"Rack": 0, "Focus": 1.0},
    # Smoothing a ramp barely changes it. The Luma field off this card is a
    # checkerboard, which is all step and is what Smooth is for.
    "Smooth": {"Depth": 1},
    # An edge mode only decides what happens off the frame, and only a hard
    # shake reads off the frame.
    "Edges": {"Shake": 1.0, "Roll": 1.0, "Drive": 1.0},
    # Tap count shows on a big disc, not a small one.
    "Quality": {"Aperture": 0.9},
    # The iris shape lives at the rim of a big out-of-focus highlight, and a
    # frame that is also being shaken has nothing still enough to compare.
    "Blades": {"Aperture": 0.9, "Rack": 0, "Drive": 0.0},
    "Rotation": {"Aperture": 0.9, "Rack": 0, "Drive": 0.0, "Blades": 1},
    # A knock on the lens is only worth measuring while the lens is actually
    # knocked. A slow, lightly damped rig is still near its first peak an eighth
    # of a second after the kick, which is where this frame lands.
    "Defocus": {"Rack": 0, "Drive": 1.0, "Resonance": 0.0, "Damping": 0.2,
                "Threshold": 0.05, "Aperture": 0.45},
}


def render(path, overrides):
    args = ["./build/gftest", "--out", path,
            "--width", str(WIDTH), "--height", str(HEIGHT),
            "--frames", str(FRAMES), "--audio"]
    merged = dict(BASE)
    merged.update(overrides)
    for k, v in merged.items():
        args += ["--set", f"{k}={v}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", r.stdout, r.stderr)
        sys.exit(1)
    return open(path, "rb").read()


def pixels(png):
    i = 8
    idat = b""
    w = h = 0
    while i < len(png):
        ln = struct.unpack(">I", png[i:i + 4])[0]
        t = png[i + 4:i + 8]
        d = png[i + 8:i + 8 + ln]
        if t == b"IHDR":
            w, h = struct.unpack(">II", d[:8])
        if t == b"IDAT":
            idat += d
        i += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4
    return b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(h))


def diff(a, b):
    pa, pb = pixels(a), pixels(b)
    n = len(pa)
    changed = 0
    total = 0
    for i in range(0, n, 4):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        if d > 2:
            changed += 1
        total += d
    return changed / (n / 4) * 100, total / (n / 4)


names = subprocess.run(["./build/gftest", "--list"], capture_output=True, text=True).stdout
params = [" ".join(l.split()[1:-1]) for l in names.strip().splitlines()]

# The About block is a text field and browser buttons, declared last. They never
# touch a pixel, so sweeping them only buries a real dead control.
if "About" in params:
    params = params[:params.index("About")]
params = [p for p in params if p not in SKIP]

print(f"{'parameter':<16} {'pixels changed':>15} {'mean delta':>11}   verdict")
dead = []
for p in params:
    lo, hi = DISCRETE.get(p, RANGE.get(p, (0.0, 1.0)))
    context = CONTEXT.get(p, {})
    a = render(f"{SC}/a.png", {**context, p: lo})
    b = render(f"{SC}/b.png", {**context, p: hi})
    pct, mean = diff(a, b)
    ok = pct > 0.5
    if not ok:
        dead.append(p)
    print(f"{p:<16} {pct:14.2f}% {mean:11.3f}   {'ok' if ok else '*** NO EFFECT ***'}")

print()
if dead:
    print("DEAD CONTROLS:", ", ".join(dead))
    sys.exit(1)
print(f"all {len(params)} parameters affect the output")
