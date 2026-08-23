#pragma once

/**
    Factory presets: lenses and rigs somebody has actually stood behind.

    Both hosts already let a user save their own, so what belongs in the plugin
    is the curated set. There are two halves to this effect and they are usually
    wanted one at a time -- a rattle for the drop, a rack for the moment before
    it -- so most entries here commit to one and leave the other at rest.

    The values live in the same 0..1 parameter space both builds expose, so ONE
    table drives the FFGL and OpenFX plugins and a preset looks identical in
    Resolume and in Resolve. Plain data, no logic: the machinery for applying a
    preset lives with each host's glue.

    Element 0 of the host-facing dropdown is "Custom" and is not in this table.
    It is not a preset; it means the sliders are the truth.

    Four things a preset deliberately does not touch:

    - **Depth, Depth Gain, Falloff and Smooth.** Whether a clip carries a usable
      depth map, in which channel, which way up, and how noisy it is, are facts
      about the FOOTAGE. A preset that switched a plain clip to Split H would
      throw half the picture away.
    - **Rotation**, which is where the operator wants the iris pointing.
    - **Edges and Quality**, which are calls about the frame and the GPU.
    - **Anything in the OpenFX build that FFGL supplies from the host.** The
      audio fields are inert there, so a preset that leans on Drive reads as a
      still lens in Resolve. That is stated on each one that does.
*/

namespace gaffer
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds this
/// order to its ParamIDs and the OFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift apart in
/// silence.
enum Param
{
	kFocus,
	kAperture,
	kFocalLength,
	kBlades,
	kHighlight,
	kBreathing,

	kRack,
	kMarkB,
	kSpeed,
	kRate,
	kSync,
	kEase,

	kDrive,
	kBand,
	kThreshold,
	kRelease,
	kShake,
	kRoll,
	kResonance,
	kDamping,
	kDefocus,

	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// All values are host-parameter values, 0..1. Option parameters hold the
// element index as a float: Blades 0=Round 1..5=5..9 blades; Rack 0=Off
// 1=Follow 2=Pull 3=Sweep 4=Stutter; Sync 0=Manual 1=Free 2=4 Bars 3=2 Bars
// 4=Bar 5=1/2 6=1/4 7=1/8 8=1/16; Band 0=Full 1=Low 2=Mid 3=High.
// See Lens.cpp and Controls.cpp for what each slider position means.
inline constexpr Preset kPresets[] = {
	//                    Focus Apert Focal Blade Highl Breat | Rack  MarkB Speed Rate  Sync  Ease  | Drive Band  Thres Relse Shake Roll  Reson Damp  Defoc
	{ "Kick Rattle",   { 0.55f,0.35f,0.40f, 2.0f,0.30f,0.35f,   0.0f,0.00f,0.45f,0.50f, 1.0f,1.00f,   0.70f, 1.0f,0.12f,0.35f,0.45f,0.30f,0.55f,0.45f,0.45f } },
	{ "Subwoofer",     { 0.55f,0.45f,0.55f, 2.0f,0.40f,0.50f,   0.0f,0.00f,0.45f,0.50f, 1.0f,1.00f,   0.90f, 1.0f,0.08f,0.55f,0.75f,0.45f,0.20f,0.30f,0.70f } },
	{ "Cheap Tripod",  { 0.55f,0.30f,0.35f, 1.0f,0.25f,0.30f,   0.0f,0.00f,0.45f,0.50f, 1.0f,1.00f,   0.65f, 1.0f,0.15f,0.20f,0.28f,0.55f,0.85f,0.12f,0.25f } },

	{ "Rack A to B",   { 0.85f,0.45f,0.50f, 2.0f,0.35f,0.55f,   2.0f,0.15f,0.55f,0.50f, 4.0f,1.00f,   0.00f, 1.0f,0.15f,0.35f,0.30f,0.20f,0.50f,0.40f,0.30f } },
	{ "Follow Focus",  { 0.60f,0.40f,0.45f, 2.0f,0.30f,0.65f,   1.0f,0.00f,0.72f,0.50f, 0.0f,1.00f,   0.00f, 1.0f,0.15f,0.35f,0.30f,0.20f,0.50f,0.40f,0.30f } },
	{ "Focus Sweep",   { 0.95f,0.50f,0.45f, 2.0f,0.35f,0.45f,   3.0f,0.05f,0.50f,0.50f, 6.0f,1.00f,   0.00f, 1.0f,0.15f,0.35f,0.30f,0.20f,0.50f,0.40f,0.30f } },

	// The one the "poll through every plane in the picture" idea is actually
	// for: a full-barrel sweep fast enough that no plane is ever settled on.
	{ "Focus Strobe",  { 1.00f,0.55f,0.60f, 3.0f,0.55f,0.35f,   3.0f,0.00f,0.20f,0.90f, 1.0f,0.30f,   0.00f, 1.0f,0.15f,0.20f,0.20f,0.15f,0.60f,0.35f,0.20f } },
	{ "Focus Stutter", { 0.90f,0.50f,0.50f, 2.0f,0.45f,0.40f,   4.0f,0.05f,0.28f,0.50f, 7.0f,0.85f,   0.00f, 1.0f,0.15f,0.25f,0.25f,0.20f,0.55f,0.35f,0.25f } },

	// Needs audio. In an OpenFX host there is none, so this reads as a still
	// lens rather than as a broken one.
	{ "Pull On Kick",  { 0.90f,0.50f,0.50f, 2.0f,0.40f,0.50f,   2.0f,0.10f,0.40f,0.50f, 0.0f,1.00f,   0.60f, 1.0f,0.18f,0.30f,0.35f,0.25f,0.50f,0.40f,0.35f } },

	// No motion at all: this one is about what the iris does to a highlight.
	{ "Bokeh Balls",   { 0.35f,0.75f,0.55f, 2.0f,0.85f,0.00f,   0.0f,0.00f,0.45f,0.50f, 1.0f,1.00f,   0.00f, 1.0f,0.15f,0.35f,0.00f,0.00f,0.50f,0.40f,0.00f } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace gaffer
