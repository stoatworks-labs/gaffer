#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace gaffer
{
namespace controls
{
namespace
{
/// Geometric interpolation: `t` of the way from `lo` to `hi` in ratio rather
/// than in difference. What every control whose useful range spans more than
/// one order of magnitude wants.
double geometric( float value, double lo, double hi )
{
	const double t = std::clamp( double( value ), 0.0, 1.0 );
	return lo * std::pow( hi / lo, t );
}
} // namespace

RackMode Mode( float value )
{
	return RackMode( std::clamp( int( std::lround( value ) ), 0, 4 ) );
}

Sync SyncDivision( float value )
{
	return Sync( std::clamp( int( std::lround( value ) ), 0, 8 ) );
}

Band AudioBand( float value )
{
	return Band( std::clamp( int( std::lround( value ) ), 0, kBandCount - 1 ) );
}

double TravelSeconds( float value )
{
	return geometric( value, 0.03, 5.0 );
}

double RateHz( float value )
{
	return geometric( value, 0.05, 24.0 );
}

double Ease( float value )
{
	return std::clamp( double( value ), 0.0, 1.0 );
}

double AudioRelease( float value )
{
	return geometric( value, 0.03, 1.5 );
}

double Threshold( float value )
{
	return std::clamp( double( value ), 0.0, 1.0 );
}

double ResonanceHz( float value )
{
	//Capped at 24 Hz, and that cap is load-bearing rather than aesthetic: the
	//highest axis runs at 1.73x this, so the fastest oscillator in the model is
	//about 41 Hz, and the integrator's fixed substep of 1/480 s keeps w*h at
	//about 0.54 -- comfortably inside the stability bound of 2. Raise this and
	//check that sum again.
	return geometric( value, 2.0, 24.0 );
}

double Damping( float value )
{
	return geometric( value, 0.02, 1.0 );
}

double ShakeAmount( float value )
{
	const double t = std::clamp( double( value ), 0.0, 1.0 );

	//Squared, like the aperture: everything worth having is in the first
	//quarter, and beyond about 4% of frame height it stops being a camera being
	//rattled and starts being a camera being thrown.
	return 0.09 * t * t;
}

double RollAmount( float value )
{
	const double t = std::clamp( double( value ), 0.0, 1.0 );
	return 0.14 * t * t;//radians: about 8 degrees at the top
}

double DefocusAmount( float value )
{
	return 0.5 * std::clamp( double( value ), 0.0, 1.0 );
}

} // namespace controls
} // namespace gaffer
