# gaffer — user guide

A simulated lens on a depth map, with the music holding it. This guide is about
using it. `README.md` is the reference for every control; `AGENTS.md` is about
how it works inside.

---

## Before anything else: give it some depth

gaffer blurs by distance, so it needs to know what is far away. It has five
answers and they are not interchangeable.

**If you have no depth information at all** — an ordinary clip, a camera feed, a
generator — use **Radial**. It invents a field: near on the optical axis, far at
the corners. Nothing is read out of the picture, so it cannot fail, and what you
get is the *look* of a shallow lens rather than a lens. On a centred subject
against a busy background it is convincing. On a flat graphic it is a vignette
that goes soft.

**If your clip already carries depth in a channel**, use **Luma** or **Alpha**.
Alpha is the good one: a rendered depth pass baked into the alpha channel is
real depth and gets a real result. Luma turns brightness into distance, which is
either exactly what you want (a greyscale depth clip) or an effect in its own
right (anything else) — bright things go one way, dark things the other, and
that is worth trying deliberately before you dismiss it.

**If you can author the clip**, use **Split H** or **Split V**. Render the
picture in the left half (or the top) and the depth map in the right half (or
the bottom), double-width or double-height. This is the only mode where the
picture and the depth are completely independent, so the depth map can be as
detailed as you like without touching a pixel of the image. Every depth-carrying
interchange format that is not a bespoke container does it this way, so a file
authored for something else usually drops straight in.

> **Split modes stretch the picture half over the whole frame.** Put the clip on
> a layer sized for the *picture*, not for the double-width file, and let
> Resolume fit it. The output comes back at the right aspect ratio.

### Getting the field right

Three controls shape whatever the depth source gave you, and they matter more
than they look:

- **Depth Gain** is signed. A quarter of the way up is *flat* — one distance,
  one uniform blur over the whole frame, which is a legitimate look and also the
  fastest way to check the lens is doing anything at all. Below that the field
  inverts, which is what you need when a depth map was authored with near =
  black.
- **Falloff** decides where most of the scene sits between the near and far
  ends. Turn it up if everything is bunched at one end of the barrel and the
  Focus control feels like it only works over a third of its travel.
- **Smooth** blurs the *depth map*, never the picture. A depth pass with hard
  edges makes blur with hard edges, which the eye reads as a cut-out rather than
  as a lens. This is the fix. It does nothing in Radial, correctly — a formula
  has no noise to smooth.

---

## Recipe: the bass rattles the camera

The one most people come for.

1. Route audio to the effect. In Resolume that is the **Audio** parameter's
   source picker — Local, Composition or External.
2. **Band → Low.** It is the default, and it is the point.
3. Turn **Drive** up. It starts at zero on purpose: everything else in the group
   is already set to something useful, so this one control is the whole gesture.
4. Watch the frame on a kick. If it does nothing, **Threshold** is too high. If
   it never stops moving, it is too low.

Then shape the rig, which is where the character is:

- **Resonance** is what the camera is standing on. Low (2–6 Hz) is a heavy
  tripod on a concrete floor: a slow, heavy lurch. High (15–24 Hz) is a light
  head on a hollow stage: a fast buzz. Nothing in between is wrong.
- **Damping** is how long it rings. At the bottom one kick keeps the frame
  moving for most of a bar — which sounds like a mistake and is, in fact,
  exactly what a badly clamped rig does. At the top each hit is a single lurch
  and it stops.
- **Shake** and **Roll** are how far it moves and how far it tips. Roll is worth
  more than you would think: a frame that only translates reads as a shaking
  *screen*, and a frame that tips reads as a shaking *camera*.
- **Defocus** knocks the focal plane on each hit. The lens elements are part of
  the rig too. This is what makes the picture go soft on the drop rather than
  just moving, and it works even on a flat depth field.

> **Release** is how fast the level falls after a hit. Short (down near the
> bottom) makes each kick a distinct event; long smears them into a single
> swell. If the rig feels like it is responding to the *tune* rather than to the
> *hits*, shorten it.

**Edges** decides what shows where the shake looks past the picture. Clamp is
the default and is invisible; Transparent is right if you are compositing over
something; Mirror is the film-ish one.

---

## Recipe: a rack focus on the bar

1. **Rack → Pull.**
2. Set **Focus** (mark A) and **Mark B** to the two planes you want. Watching
   the picture while you drag Focus with Rack on Off is the easy way to find
   them.
3. **Sync → Bar** (or 1/2, or whatever the track wants).
4. **Speed** is how long the move takes. Real racks are 0.3–1 second. Make sure
   it is comfortably shorter than the gap between cues or the rack never
   arrives.
5. **Ease** at 1 is a hand on a follow focus. At 0 it is a motor. Leave it near
   1 unless you want the mechanical version.

**Breathing** is worth turning up here. A real lens changes its field of view
slightly as it racks, and that tiny creep is a large part of why a rack focus
reads as a *lens* rather than as a blur crossfade. Push **Focal Length** up as
well and both the breathing and the depth-of-field collapse get more dramatic,
because on a real lens they are the same thing.

### Variations

- **Pull on the kick instead of the bar.** Set **Sync → Manual**. Now nothing
  fires the rack except the **Pull** button and the audio transient, so the
  focus racks when the drummer says so. Turn **Drive** down to zero if you want
  the focus moving but the camera still — the two are deliberately separate.
- **Follow.** Set **Rack → Follow** and animate **Focus** yourself, from a
  dashboard, a MIDI fader or a timeline. The lens lags behind your hand at the
  speed **Speed** allows, which is what makes an animated focus look pulled
  rather than keyframed.
- **Stutter.** A new random plane on every cue, the same sequence every time you
  play the composition back. Short Speed and 1/8 or 1/16 for a rhythm part.

---

## Recipe: polling through the focal planes

The fast end of **Sweep**, and it is a different effect from everything above.

1. **Rack → Sweep.**
2. **Focus → 1.0**, **Mark B → 0.0** — the whole barrel.
3. **Sync → Free**, **Rate** up high. Above about 8 Hz the focal plane is
   scanning through the entire depth of the picture faster than the eye can
   track, and every part of the frame takes its turn being the sharp one.
4. **Ease** *down*, near 0. At 1 the sweep dwells at each end; at 0 it moves
   through at a constant rate, which is what makes it read as a scan rather than
   as a wobble.
5. **Aperture** up. There is no point scanning a focal plane that barely blurs
   anything.

The **Focus Strobe** preset is exactly this. Try it against **Sync → 1/8** as
well: locked to the grid it stops being a texture and becomes a part.

---

## The iris, and why bokeh is a shape

Out-of-focus highlights take the shape of the aperture. That is not a stylistic
choice, it is what a lens does — and it is the difference between "blurry" and
"a lens".

- **Blades.** Round is a real answer (mirror lenses, some fast primes wide
  open), but 6 is the commonest iris there is and is the default. Five is the
  most obviously "photographic". Nine is very nearly round.
- **Rotation** turns the iris. On a still frame it does nothing you would
  notice; animate it slowly under a field of specular highlights and it is
  lovely.
- **Highlight** is the one that makes bokeh look like bokeh. The disc is
  averaged with a *power mean* — raise, average, take the root — so a small
  bright thing comes back as a distinct disc instead of a smear. It is the only
  control here that does not conserve light, and that is deliberate: a real
  highlight blooms because the source was brighter than the medium could hold,
  and an 8-bit clip has no such headroom. At zero you get an honest average and
  no balls, exactly.

**Bokeh Balls** is the preset for this: no motion at all, wide aperture, six
blades, Highlight up.

---

## Quality, and what it costs

**Quality** sets how many samples the gather takes — 16, 32, 64 or 128. It does
**not** change how big the blur is or what shape the bokeh is, only how smoothly
the disc is filled in.

Where it shows: a small, bright thing spread over a big disc. A specular
highlight at Fast with the aperture wide open draws the individual samples as a
spiral of dots rather than a filled circle. Ordinary footage hides this, because
neighbouring pixels' discs overlap.

So: **Fast for a busy clip, Best or Extreme for anything with hard specular
highlights and a wide aperture.** Nothing has been timed yet, and Extreme with
Smooth up is 128 samples each doing five depth reads — check the frame rate on
your own machine before committing to it in a show.

---

## Two things that are exactly nothing

Worth knowing, because they are the fastest way to prove the effect is doing
what you think:

- **Aperture at zero** is a pinhole, and the picture comes out untouched. Not
  approximately — byte for byte.
- **Depth Gain flat with Focus at 0.5** is a scene at one distance with the
  focal plane exactly on it. Also untouched, byte for byte, with the whole
  gather running.

If either of those changes the picture on your machine, something is wrong and
it is worth reporting.

---

## Troubleshooting

**Nothing happens at all.** Aperture is at zero, or the depth field is flat and
Focus is sitting on it. Turn Aperture up first.

**The whole frame is soft, evenly.** Your depth field has no variation — Depth
Gain near the flat setting, or a depth source with nothing in it (Alpha on a
clip with no alpha, Split H on a clip that is not side-by-side). Check by
switching to Radial: if Radial gives you a sharp middle and soft corners, the
lens is fine and the depth is the problem.

**The blur is inside out** — the background is sharp and the subject is soft.
Your depth map is authored the other way up. Push Depth Gain below its flat
point to invert it.

**Hard edges around objects, like a cut-out.** The depth map has hard edges.
Turn **Smooth** up.

**The camera shakes but never stops.** Damping is too low. Or Release is too
long, so the level never falls far enough between hits for the next one to
register as an arrival.

**The camera does not shake at all.** In order: is audio routed? Is Drive above
zero? Is Threshold too high? Is Band on something the track actually has content
in?

**The rack never reaches the second mark.** Speed is longer than the gap between
cues. Either shorten Speed or use a slower Sync division.

**Presets snap back to Custom.** That should not happen — it is a bug worth
reporting, with the host and version. The preset system here is built to survive
a host that pushes its own parameter values back at the plugin, which is what
causes that symptom elsewhere.

**The effect appears in the list but does nothing, and no error.** That is a
shader that would not compile. There is a log:

```
~/Library/Logs/gaffer/gaffer.<date>.log          # macOS
%LOCALAPPDATA%\gaffer\logs\                      # Windows
```

The GL vendor, renderer and version are logged next to the failure, which is
usually most of the answer.

---

## In Resolve, Nuke and the other OpenFX hosts

The same effect, with two differences forced by the host:

- **There is no transport**, so the bar divisions count against a **Tempo**
  control that only exists in that build.
- **There is no audio.** Rather than leave half the plugin dead, the rig is
  driven by a **Kick** control — a pulse on a chosen division of that same
  Tempo. It is a metronome rather than a microphone, but the rig, the resonance,
  the damping and the travel limit are the same code answering it.

Everything else — the lens, the depth sources, the five rack modes, the presets
— is the same, and a preset looks identical in both hosts because both read one
table.
