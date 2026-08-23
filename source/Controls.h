#pragma once

#include "Audio.h"
#include "Rack.h"

/**
    Every 0..1 host parameter that is not part of the lens maths, turned into
    the quantity it stands for.

    Why they are all 0..1 in the first place: SetParamRange exists and Resolume
    honours it, but CFFGLPluginManager::SetParamInfo clamps an FF_TYPE_STANDARD
    default into 0..1 *before* a range can be attached (SDK b1afaf9), and there
    is no SetParamDefault. So a parameter declared in Hz cannot state a default
    in Hz. Keeping the host side unitless sidesteps it entirely, and the
    conversions live here where the harness can print both numbers.

    The lens's own mappings are in Lens.cpp instead, because the shader mirrors
    those and `gftest --probe` measures the mirror. Nothing in this file has a
    GLSL counterpart: it all resolves on the CPU before a uniform is set.
*/
namespace gaffer
{
namespace controls
{

RackMode Mode( float value );
Sync SyncDivision( float value );
Band AudioBand( float value );

/// Speed slider -> seconds for a full-barrel move. Geometric, because the
/// difference between 0.1 s and 0.2 s is a different shot and the difference
/// between 3 s and 3.1 s is nothing.
double TravelSeconds( float value );

/// Rate slider -> cue rate in Hz, for the un-synced modes. Reaches well past
/// anything musical at the top: the fast end of this control is the point of
/// the Sweep mode, where focus strobes through every plane in the frame.
double RateHz( float value );

/// Ease slider -> 0..1, passed straight through. The one control here that
/// needs no conversion, kept in this file so nothing has to remember which
/// ones do.
double Ease( float value );

/// Release slider -> the envelope's fall time in seconds.
double AudioRelease( float value );

/// Threshold slider -> 0..1, on the same scale as the onset detector.
double Threshold( float value );

/// Resonance slider -> the rig's fundamental in Hz. The bottom is a heavy
/// tripod on a solid floor; the top is a light head on a hollow stage.
double ResonanceHz( float value );

/// Damping slider -> the damping ratio. Geometric at the bottom, where the
/// interesting range is: the difference between 0.03 and 0.06 is a wobble that
/// lasts a bar or half of one.
double Damping( float value );

/// Shake slider -> the frame offset at full deflection, as a fraction of the
/// frame HEIGHT. Height so the shake is the same distance in pixels whatever
/// the aspect ratio.
double ShakeAmount( float value );

/// Roll slider -> the frame rotation at full deflection, in radians.
double RollAmount( float value );

/// Defocus slider -> how far a full-deflection knock moves the focal plane, in
/// disparity. Half the barrel at the top of the range, which is enough to take
/// a sharp frame to fully soft and back on one kick.
double DefocusAmount( float value );

} // namespace controls
} // namespace gaffer
