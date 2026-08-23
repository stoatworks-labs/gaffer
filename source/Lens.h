#pragma once

/**
    The lens, in C++.

    A lens has exactly one plane in sharp focus. Everything else lands on the
    sensor as a disc rather than a point, and the diameter of that disc -- the
    circle of confusion -- is the entire subject of this file.

    Written out. A thin lens of focal length `f` focused at distance `z_f` puts
    its sensor at image distance `v(z_f) = f z_f / (z_f - f)`. An object at `z`
    would image at `v(z)`, so on the sensor it is a cone section of diameter

        c = A |v(z) - v(z_f)| / v(z)

    with `A` the aperture diameter. Substituting and cancelling -- the algebra
    is short and worth doing once -- every term in `z` collapses:

        c = A f z_f / (z_f - f) * | 1/z - 1/z_f |

    **The blur is linear in the difference of RECIPROCAL depths.** That is not
    an approximation made for the shader's benefit; it is what the thin-lens
    equation says. And the reciprocal of depth is disparity, which is what a
    depth map actually holds -- so the quantity this plugin has, and the
    quantity the physics wants, are the same quantity. With the field
    normalised into 0..1 (1 nearest, 0 infinitely far):

        coc(d) = K * ( d - focus )

    signed, because which side of the focal plane a surface sits on decides
    whether it occludes what is behind it.

    ### One number does two jobs

    The leading constant is worth staring at. Write phi = f / z_near, the focal
    length as a fraction of the near-plane distance, and the focus disparity as
    `focus`; then z_f = z_near / focus and

        K  = A * phi / ( 1 - phi * focus )     (up to a constant)
        v  =     f   / ( 1 - phi * focus )

    -- the same denominator. `v` is the image distance, and the image distance
    is what sets the frame's scale. So:

    - **Depth of field collapses as you focus closer.** K grows with `focus`.
    - **The frame grows as you focus closer.** That is focus breathing: rack a
      real lens and the field of view creeps.

    They are not two effects to be arranged next to each other. They are one
    fact about where the sensor is, and this file computes them from one
    expression. `Breathing` exists only because real lenses vary in how much of
    it they show -- an internal-focus cine lens hides nearly all of it -- so the
    control scales the physical amount rather than inventing a wobble.

    phi is bounded below 1 by construction (`kMaxPhi`), and `focus` is 0..1, so
    the denominator is unconditionally positive. **There is no divide-by-zero
    guard anywhere in this file and there must not be one** -- the pole is the
    lens being focused inside its own focal length, which the parameter mapping
    cannot reach. Same discipline as the sibling plugin's disparity form: remove
    the pole, do not guard it.

    ### What is NOT in here

    The gather itself -- which taps, what weights, how the near field is
    composited over the far -- lives in `Shaders.cpp`, because it is a
    resampling strategy rather than a fact about lenses. What this file owns is
    every number that strategy is handed.

    `Shaders.cpp` carries a GLSL mirror of the functions below and
    `gftest --probe` measures one against the other, which is the only thing
    that keeps two copies of one formula honest. Change one, change the other,
    then run the probe.
*/
namespace gaffer
{

/// Where the depth field comes from.
///
/// The first three are the sibling plugin's triad and behave identically here.
/// The two Split modes are new, and they are the only ones that can carry a
/// real, independent depth pass: the clip is authored double-width (or
/// double-height) with the picture in one half and the depth map in the other,
/// which is how every depth-carrying interchange format that is not a bespoke
/// container does it.
enum class DepthSource
{
	Radial = 0,//!< Invented: near on the optical axis, far at the corners.
	Luma   = 1,//!< The clip's own brightness read as a depth map.
	Alpha  = 2,//!< The clip's alpha read as a depth map.
	SplitH = 3,//!< Side by side: picture in the left half, depth in the right.
	SplitV = 4 //!< Over and under: picture on top, depth beneath.
};

/// Aperture blade count, which is what an out-of-focus highlight takes the
/// shape of. Round is a real answer and not "off" -- mirror lenses and some
/// fast primes are very nearly circular wide open.
enum class Blades
{
	Round = 0,
	Five  = 1,
	Six   = 2,
	Seven = 3,
	Eight = 4,
	Nine  = 5
};

/// Rec.709 luma weights, for reading a picture as a depth map.
inline constexpr double kLumaR = 0.2126;
inline constexpr double kLumaG = 0.7152;
inline constexpr double kLumaB = 0.0722;

/// The largest circle of confusion the plugin will produce, as a fraction of
/// the frame HEIGHT -- height rather than width, so the bokeh is round in
/// pixels on any aspect ratio.
///
/// A radius rather than a diameter: everything downstream wants the radius.
inline constexpr double kMaxCoc = 0.12;

/// The upper bound on phi = f / z_near. Below 1 by construction, which is what
/// keeps 1 - phi*focus positive for every reachable setting and is the reason
/// nothing here needs a guard.
inline constexpr double kMaxPhi = 0.8;

//---------------------------------------------------------------------------
// The depth field. Shared with the sibling plugin's treatment, because a
// normalised disparity field is a normalised disparity field.
//---------------------------------------------------------------------------

/// Half the frame diagonal in "height units" -- the space in which x has been
/// multiplied by the aspect ratio, so the invented radial field is circular in
/// pixels rather than circular in UV. Puts the frame corner at exactly 1.
double referenceRadius( double aspect );

/// The invented radial field before gamma: 1 on the optical axis falling to 0
/// at the frame corner. Evaluated in OUTPUT space, unlike the sampled fields.
double radialBase( double rhoHat );

/// The raw 0..1 field turned into the disparity actually used.
///
/// `gamma` redistributes it and `gain` is a signed scale **about the middle of
/// the range**: 1 leaves it alone, 0 collapses it to a constant, and -1 turns
/// the scene inside out. Scaling about the middle rather than about the focal
/// plane matters for the same reason it does in the sibling plugin -- scaling
/// about a moving reference slides the whole field off the end of the range as
/// soon as the gain goes negative and the reference is not centred, and a
/// constant field makes the control silently do nothing over half its travel.
///
/// A gain of 0 is a genuinely useful setting here rather than a degenerate one:
/// a flat field is a scene at one distance, so the lens applies one uniform
/// circle of confusion to the whole frame. That is the configuration
/// `gftest --bokeh` measures in, because it is the only one where the answer
/// is a single number.
double disparity( double base, double gamma, double gain );

//---------------------------------------------------------------------------
// The lens.
//---------------------------------------------------------------------------

/// The image distance, normalised so focusing at infinity gives exactly 1.
///
///     v(focus) = 1 / ( 1 - phi * focus )
///
/// Both the blur gain and the frame scale are built on this. Never below 1, and
/// never infinite: phi < 1 and focus <= 1.
double imageDistance( double focus, double phi );

/// The blur gain: circle-of-confusion radius per unit of disparity, in frame
/// height units. The shader needs this as a uniform, which is the only reason
/// it is public -- `coc()` below is the same thing with the subtraction done.
double cocGain( double focus, double aperture, double phi );

/// The circle-of-confusion radius, signed, in frame-height units.
///
/// Positive means the surface is NEARER than the focal plane. The sign is not
/// decoration -- the gather uses it to decide which contributions belong to the
/// near field, and the near field is the half that is allowed to spread over
/// its neighbours.
double coc( double d, double focus, double aperture, double phi );

/// The largest radius `coc()` can return at these settings, which is what the
/// gather uses as its sampling radius. Reached at d = 0 or d = 1, whichever is
/// further from the focal plane.
double cocMax( double focus, double aperture, double phi );

/// How much the frame scales when the lens racks to `focus`, with `breathing`
/// choosing how much of the physical amount to show. 1 is no change.
///
/// Normalised against focus at the middle of the barrel rather than against
/// infinity: an operator setting Breathing expects the frame to be roughly
/// where it was, growing one way and shrinking the other, not to leap.
double breathScale( double focus, double phi, double breathing );

/// The number of aperture blades, or 0 for a round iris.
int bladeCount( Blades blades );

//---------------------------------------------------------------------------
// Parameter mapping.
//
// Every parameter this plugin exposes is a plain 0..1 float even where it
// stands for a physical quantity. That is forced rather than chosen:
// CFFGLPluginManager::SetParamInfo clamps an FF_TYPE_STANDARD default into
// 0..1 *before* SetParamRange can widen the range (SDK b1afaf9), so a
// parameter declared in stops cannot state a default in stops. The conversions
// live here, where the harness can print both sides.
//---------------------------------------------------------------------------

/// Aperture slider -> the blur gain, 0..1. Squared, because the useful part of
/// the range is the bottom of it: half a slider of a linear aperture is already
/// an unrecognisable picture.
double apertureFromParam( float value );

/// Focal Length slider -> phi. 0 is a long lens a long way off, which neither
/// breathes nor changes its depth of field as it racks; the top of the range is
/// close-focus, where both are dramatic.
double phiFromParam( float value );

/// Highlight slider -> the exponent of the power mean the gather averages
/// with: raise the samples, average, take the root. Exactly 1 at zero, which
/// is a plain average and the only setting that conserves light.
///
/// A power mean and NOT a per-sample weighting. Weighting normalises by the
/// sum of the weights, so at a strong setting the answer is whichever tap
/// happened to be brightest -- on high-contrast material that draws the
/// sampling pattern rather than a disc, and it does not improve with more taps
/// because it is variance in the estimator rather than noise.
double highlightFromParam( float value );

/// Breathing slider -> how much of the physical breathing to show, 0..1.
double breathingFromParam( float value );

/// Rotation slider -> the iris angle in radians.
double rotationFromParam( float value );

/// Depth Gain slider -> the signed scale on the field. 0.5 is 1:1, the middle
/// of the bottom half is flat, and below that the field inverts -- which is
/// what a depth map authored the other way up needs.
double depthGainFromParam( float value );

/// Falloff slider -> the gamma on the raw field. 0.5 is 1.0, geometric either
/// side so the control is symmetric in the way a gamma actually behaves.
double gammaFromParam( float value );

/// Smooth slider -> the radius, in picture-space units, the depth map is
/// blurred over before use. Zero for the Radial field, which is smooth by
/// construction.
double smoothFromParam( float value );

/// Quality option -> how many taps the gather takes.
int tapsFromParam( float value );

/// Depth option -> which field.
DepthSource depthSourceFromParam( float value );

/// Blades option -> which iris.
Blades bladesFromParam( float value );

/// True for the sources that read the field out of the picture.
bool depthSourceIsSampled( DepthSource source );

/// True for the two sources that carry their depth in a separate half of the
/// frame, and so need the picture rescaled out of that half.
bool depthSourceIsSplit( DepthSource source );

} // namespace gaffer
