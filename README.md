# gaffer

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The lens is verified
> numerically by an offline harness that drives the real plugin class in a
> headless GL context: a point of light is put through it and the disc that
> comes out is measured against an independent C++ implementation of the circle
> of confusion across 48 parameter combinations, in x and y separately, with the
> light it neither created nor destroyed measured at 1.0000 of the pinhole. The
> focus puller and the rig are asserted directly over tens of thousands of
> frames, and both nulls are exact to the byte. It has **never been loaded into
> Resolume, or into any OpenFX host**, no audio has ever reached it, and no
> Windows build has been run (see [Status](#status)). Check it in your own rig
> before trusting it in a show.

A simulated lens on a depth map, with the music holding it — an
[FFGL](https://github.com/resolume/ffgl) effect for [Resolume](https://resolume.com)
Arena and Avenue, and an [OpenFX](https://openeffects.org) effect for DaVinci
Resolve, Nuke, Natron and Vegas.

<!-- downloads:start -->

## Download

**[v0.1.2](https://github.com/stoatworks-labs/gaffer/releases/tag/v0.1.2)** — prebuilt for macOS, Windows and Linux. Pick your platform:

<details>
<summary><b>macOS</b> — Universal (Apple Silicon + Intel)</summary>

| Build | Download | Size |
| --- | --- | --- |
| Universal (Apple Silicon + Intel) · .dmg disk image | [`gaffer-0.1.2-macos-universal.dmg`](https://github.com/stoatworks-labs/gaffer/releases/download/v0.1.2/gaffer-0.1.2-macos-universal.dmg) | 228 KB |
| Universal (Apple Silicon + Intel) · .zip archive | [`gaffer-macos-universal.zip`](https://github.com/stoatworks-labs/gaffer/releases/latest/download/gaffer-macos-universal.zip) | 189 KB |
| Universal (Apple Silicon + Intel) · .zip archive (OpenFX — Resolve, Vegas, Nuke) | [`gaffer-ofx-macos-universal.zip`](https://github.com/stoatworks-labs/gaffer/releases/latest/download/gaffer-ofx-macos-universal.zip) | 268 KB |

</details>

<details>
<summary><b>Windows</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .exe installer | [`gaffer-0.1.2-windows-x86_64-setup.exe`](https://github.com/stoatworks-labs/gaffer/releases/download/v0.1.2/gaffer-0.1.2-windows-x86_64-setup.exe) | 226 KB |
| x64 · .zip archive | [`gaffer-windows-x86_64.zip`](https://github.com/stoatworks-labs/gaffer/releases/latest/download/gaffer-windows-x86_64.zip) | 119 KB |
| x64 · .zip archive (OpenFX — Resolve, Vegas, Nuke) | [`gaffer-ofx-windows-x86_64.zip`](https://github.com/stoatworks-labs/gaffer/releases/latest/download/gaffer-ofx-windows-x86_64.zip) | 79 KB |

</details>

<details>
<summary><b>Linux</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .zip archive (OpenFX — Resolve, Vegas, Nuke) | [`gaffer-ofx-linux-x86_64.zip`](https://github.com/stoatworks-labs/gaffer/releases/latest/download/gaffer-ofx-linux-x86_64.zip) | 738 KB |

</details>

All builds, checksums and release notes: [github.com/stoatworks-labs/gaffer/releases](https://github.com/stoatworks-labs/gaffer/releases).

macOS builds are signed and notarised and open normally. The Windows builds are unsigned, so SmartScreen warns once.

<!-- downloads:end -->

## What it does

One plane of the picture is sharp and everything else is a disc, sized by how
far it is from that plane. Two things then move it.

**The rattle.** A camera in front of a loud PA does not shake like the music, it
shakes like a camera: the pressure hits an assembly with a mass and a stiffness
of its own, and it answers at *its* frequency and keeps answering after the hit
has gone. That is what the rig here is — four damped oscillators, one per axis,
driven by the arrival of the bass rather than by its level, so a kick rings it
and a drone does not move it. The lens elements are part of the same assembly,
so the frame moves and the focus comes off together.

**The rack.** A focus puller with two marks taped on the barrel:

- **Follow** — the Focus control is where you are *pointing*, and the hand takes
  time to get there. Animate Focus in the host and the lens lags, because a
  puller cannot teleport and neither can this.
- **Pull** — a rack from one mark to the other on a cue: the bar line, a button,
  or the kick drum.
- **Sweep** — the hand never stops. Slow, it is a searching focus; fast, it is
  the frame polling through every plane in the picture several times a second.
- **Stutter** — a new plane on every cue. Focus as a rhythm part.

The depth comes from one of five places, and they are not five settings of one
thing. **Radial** invents a field and works on any clip at all. **Luma** and
**Alpha** read one out of the picture's own channels. **Split H** and **Split V**
read it out of the other half of a double-width or double-height frame — the
only one of the five that can carry a real, independent depth pass at full
resolution.

## Install

**macOS.** Copy `Gaffer.bundle` into

```
~/Documents/Resolume Arena/Extra Effects/
```

(or `Extra Effects` beside `Resolume Avenue`). Restart Resolume; the effect
appears under Effects as **gaffer**.

macOS artefacts are Developer ID-signed and notarised, so they open with no
warning; the Windows ones are unsigned and SmartScreen will object once. See
[docs/UNSIGNED.md](docs/UNSIGNED.md).

**Windows.** Copy `Gaffer.dll` into

```
%USERPROFILE%\Documents\Resolume Arena\Extra Effects\
```

**OpenFX** (Resolve, Nuke, Natron, Vegas). Copy `Gaffer.ofx.bundle` into
`/Library/OFX/Plugins` (macOS) or `C:\Program Files\Common Files\OFX\Plugins`
(Windows). It appears under **Stoatworks**.

## Controls

### Lens

| Control | What it does |
| --- | --- |
| **Focus** | Which depth is sharp. 1 is the nearest surface, 0 the furthest. With the Rack running, this is mark A. |
| **Aperture** | How shallow the depth of field is. Zero is a pinhole, and a pinhole is the null. |
| **Focal Length** | How much the lens cares where it is focused. At zero it is a long lens a long way off and neither the depth of field nor the framing changes as it racks; at the top it is close-focus, where both do — dramatically. |
| **Blades** | The shape of the iris, which is the shape an out-of-focus highlight takes. Round, or 5 to 9. |
| **Rotation** | Which way the iris points. Nothing to do on a round one. |
| **Highlight** | How much an out-of-focus highlight blooms. The gather averages with a power mean rather than a plain one, so a bright thing comes back as a distinct disc instead of a smear. Zero is exactly a plain average, which is the only setting that conserves light; the reason the default is not zero is that film has headroom an 8-bit clip does not. |
| **Breathing** | How much the frame creeps as the focus racks. Real lenses vary from almost none to a lot; the physical amount is at 1. |

### Depth

| Control | What it does |
| --- | --- |
| **Depth** | Radial, Luma, Alpha, Split H or Split V. See above. |
| **Depth Gain** | How much depth the scene has, signed. A quarter of the way up is flat — one distance, one uniform blur — and below that the field inverts, which is what a depth map authored the other way up needs. |
| **Falloff** | Gamma on the depth field: where between the near and far ends most of the scene sits. |
| **Smooth** | Blurs the depth field, not the picture. Nothing in Radial, which is already smooth; on a real depth map it is what stops a hard depth step reading as a cut-out. |

### Rack

| Control | What it does |
| --- | --- |
| **Rack** | Off, Follow, Pull, Sweep or Stutter. |
| **Mark B** | The other mark on the barrel. Focus is mark A. |
| **Speed** | How long a full-barrel move takes: 30 ms to 5 seconds. |
| **Rate** | The cue rate when Sync is not counting bars. Reaches 24 a second, which is what Sweep's fast end is for. |
| **Sync** | What the cues are counted in: Manual, Free, 4 Bars down to 1/16. Manual takes no cues at all in Pull and Stutter — only the button and the music — and runs free at Rate in Sweep. |
| **Ease** | 0 is a motor, 1 is a hand. Real pulls are nearer 1. |
| **Pull** | Fires a cue now. |

### Rattle

| Control | What it does |
| --- | --- |
| **Audio** | The spectrum. Resolume draws this as its audio-source picker. |
| **Drive** | How hard the music shakes the rig. **Zero by default**, and zero is an exact null: the camera does not move at all. |
| **Band** | Which part of the spectrum: Full Range, Low, Mid or High. Low is the default, because this is a plugin about bass. |
| **Threshold** | How big a hit has to be to count. At the top, nothing does. |
| **Release** | How long a hit takes to die away. |
| **Shake** | How far the frame moves at full deflection. |
| **Roll** | How far it rotates: about eight degrees at the top. |
| **Resonance** | The rig's own frequency, 2 to 24 Hz. A heavy tripod on a solid floor is low; a light head on a hollow stage is high. |
| **Damping** | How quickly it settles. At the bottom, one hit rings for whole bars. |
| **Defocus** | How far a hit knocks the focal plane. The lens elements are part of the rig too. |

### Output

| Control | What it does |
| --- | --- |
| **Edges** | What to show where a shake or a breath looks past the picture. |
| **Quality** | How many samples the gather takes: 16, 32, 64 or 128. It does not change the size of the blur or the shape of the bokeh, only how smoothly it is filled — and a small bright thing spread over a big disc is where the difference shows. |

### Presets

Ten, in one dropdown: three rigs (**Kick Rattle**, **Subwoofer**, **Cheap
Tripod**), five racks (**Rack A to B**, **Follow Focus**, **Focus Sweep**,
**Focus Strobe**, **Focus Stutter**), one that ties them together
(**Pull On Kick** — the rack fires on the bass, the camera shakes with it), and
one that is only about the iris (**Bokeh Balls**).

Presets deliberately do not touch the Depth group. Whether a clip carries a
usable depth map, in which channel and which way up, is a fact about the footage
rather than a look.

## Two things worth knowing

**Drive is zero out of the box.** With no audio routed nothing would move
anyway, but with audio routed and Drive up, dropping this on a layer starts
shaking the frame before you have seen what it does to a still picture. Shake,
Roll, Defocus and the rig itself are all set to something useful — turning up
the one control is the whole gesture.

**A pinhole is exact.** Aperture at zero returns the picture unchanged, byte for
byte — and so does a frame with the focal plane exactly on a flat depth field,
with the whole gather running. Both are measured, not asserted.

## Status

Verified offline, and only offline:

| Check | Result |
| --- | --- |
| Bokeh radius vs `Lens.cpp`, 48 combinations | within 1.5 px, consistently ~0.5 px under (the coverage ramp) |
| Bokeh roundness, x against y | within 0.25 px |
| Light conserved through the lens | 1.0000 of the pinhole |
| Iris symmetry, 5/6/8/9 blades | n-fold signature 0.17 / 0.11 / 0.057 / 0.044, each ≥2× its nearest unrelated harmonic |
| Aperture-closed null | 0 of 921,600 bytes differ |
| Fully-focused null, gather running | 0 of 921,600 bytes differ |
| Follow never exceeds its travel speed | worst frame 0.020833 against a limit of 0.020833 |
| Bar-synced sweep, periodicity | 8.9e-16 |
| Stateless vs stateful rack (OpenFX vs FFGL) | 2.8e-17 over 5,700 frames |
| Rig bounded under 20,000 adversarial frames | never exceeds its travel limit |
| Rig rings at the declared frequency | 8.0 Hz measured against 8.0 declared |
| Drive at zero | exactly still, onsets still detected |
| Every preset × three host behaviours | all survive |
| Dead controls | none of 30 |
| OpenFX under `ofxprobe` | loads, renders, both nulls exact, every preset byte-identical to its hand-set equivalent |

What that does **not** cover:

- **It has never been loaded into Resolume.** Not once.
- **It has never been loaded into Resolve**, or any other OpenFX host. The OFX
  build has only met `ofxprobe`.
- **No audio has ever reached it.** The FFT reader has only seen the harness's
  synthetic kick-and-hat. Whether Resolume's spectrum has the level the onset
  threshold assumes is the first thing to check.
- **The transport has never been seen** — every bar-synced mode is driven by a
  synthetic one.
- **No real depth pass has been through it.** Only a radial card, a luma
  checkerboard and a flat grey half-frame.
- **The Windows build has never been run**, and no release has been cut.
- **Nothing has been timed.** Extreme is 128 taps, each with its own depth
  fetch, and each of those is a 5-tap when Smooth is up.

## Building

```bash
git clone --recursive https://github.com/stoatworks-labs/gaffer
cd gaffer
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build        # into Resolume's Extra Effects
```

macOS builds are universal (arm64 + x86_64) by default; add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster development build. Windows needs
GLEW, which arrives through `vcpkg.json`.

Everything checkable without a host, in about eight seconds:

```bash
tools/verify.sh
```

`AGENTS.md` explains the model and the traps. `CLAUDE.md` is the short command
reference.

## Licence

MIT — see [LICENSE](LICENSE). Third-party components in
[ATTRIBUTIONS.md](ATTRIBUTIONS.md).
