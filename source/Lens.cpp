#include "Lens.h"

#include <algorithm>
#include <cmath>

namespace gaffer
{

double referenceRadius( double aspect )
{
	return 0.5 * std::sqrt( aspect * aspect + 1.0 );
}

double radialBase( double rhoHat )
{
	return 1.0 - std::clamp( rhoHat, 0.0, 1.0 );
}

double disparity( double base, double gamma, double gain )
{
	const double shaped = std::pow( std::clamp( base, 0.0, 1.0 ), gamma );

	//About the middle of the range. See the note in Lens.h: about anything that
	//moves and the field slides off the end as soon as the gain goes negative.
	return std::clamp( 0.5 + gain * ( shaped - 0.5 ), 0.0, 1.0 );
}

double imageDistance( double focus, double phi )
{
	//phi < 1 and focus <= 1, so this denominator cannot reach zero. Nothing to
	//guard, by construction rather than by luck.
	return 1.0 / ( 1.0 - phi * std::clamp( focus, 0.0, 1.0 ) );
}

/// The blur gain, in frame-height units per unit of disparity.
///
/// Normalised by the image distance at the near end of the barrel rather than
/// left in physical units, for two reasons. The absolute constant contains the
/// aperture diameter and the near-plane distance, neither of which is knowable
/// from a video clip -- so it was always going to be a made-up number. And the
/// normalisation bounds the sampling radius at kMaxCoc for every setting, which
/// is what lets the gather size its loop from the parameters alone.
///
/// What survives the normalisation is the part that is real: the RATIO. Focused
/// at the near plane the depth of field is 1/(1-phi) times shallower than
/// focused at infinity, and that is exactly what a lens does.
double cocGain( double focus, double aperture, double phi )
{
	const double vHere = imageDistance( focus, phi );
	const double vNear = imageDistance( 1.0, phi );

	return kMaxCoc * std::clamp( aperture, 0.0, 1.0 ) * vHere / vNear;
}

double coc( double d, double focus, double aperture, double phi )
{
	return cocGain( focus, aperture, phi ) * ( std::clamp( d, 0.0, 1.0 ) - std::clamp( focus, 0.0, 1.0 ) );
}

double cocMax( double focus, double aperture, double phi )
{
	const double f = std::clamp( focus, 0.0, 1.0 );

	//The field spans 0..1, so the furthest any surface can be from the focal
	//plane is whichever end of that span is further away.
	return cocGain( f, aperture, phi ) * std::max( f, 1.0 - f );
}

double breathScale( double focus, double phi, double breathing )
{
	//Against the middle of the barrel, not against infinity. An operator who
	//turns Breathing up expects the framing to stay roughly where it was and
	//creep either way as the focus racks; normalising at one end would instead
	//jump the frame the moment the control left zero.
	const double here   = imageDistance( focus, phi );
	const double middle = imageDistance( 0.5, phi );

	return 1.0 + std::clamp( breathing, 0.0, 1.0 ) * ( here / middle - 1.0 );
}

int bladeCount( Blades blades )
{
	switch( blades )
	{
		case Blades::Five: return 5;
		case Blades::Six: return 6;
		case Blades::Seven: return 7;
		case Blades::Eight: return 8;
		case Blades::Nine: return 9;
		case Blades::Round:
		default: return 0;
	}
}

//---------------------------------------------------------------------------
// Parameter mapping.
//---------------------------------------------------------------------------

double apertureFromParam( float value )
{
	const double t = std::clamp( double( value ), 0.0, 1.0 );

	//Squared. A linear aperture spends the top three quarters of its travel
	//between "unrecognisable" and "slightly more unrecognisable", and the whole
	//interesting range -- the point where a face separates from its background
	//-- is squashed into the first inch of the slider.
	return t * t;
}

double phiFromParam( float value )
{
	return kMaxPhi * std::clamp( double( value ), 0.0, 1.0 );
}

double highlightFromParam( float value )
{
	//The exponent of a power mean: raise, average, take the root. **Exactly 1
	//at zero**, which is a plain average and the only setting that conserves
	//light -- and both the shader and the OpenFX build branch on that equality,
	//so it has to be exact rather than nearly so.
	//
	//3 at the top. Beyond about 3 the mean is following the brightest thing in
	//the disc closely enough that a highlight stops being a disc and starts
	//being a hard-edged blob.
	return 1.0 + 2.0 * std::clamp( double( value ), 0.0, 1.0 );
}

double breathingFromParam( float value )
{
	return std::clamp( double( value ), 0.0, 1.0 );
}

double rotationFromParam( float value )
{
	//A full turn, though an n-gon repeats every 2pi/n -- the extra travel costs
	//nothing and saves an operator working out which fraction of the slider
	//their particular blade count needs.
	return 6.283185307179586 * std::clamp( double( value ), 0.0, 1.0 );
}

double depthGainFromParam( float value )
{
	const double t = std::clamp( double( value ), 0.0, 1.0 );

	//-1 at the bottom, 0 (flat) at a quarter, +1 at the middle, +3 at the top.
	//Asymmetric on purpose: inverting a field is a yes/no about how the map was
	//authored, so half the travel spent on -1..0 would be half a control
	//wasted, while the >1 end is where a shallow depth pass gets pushed into a
	//usable spread.
	if( t <= 0.5 )
		return -1.0 + 4.0 * t;

	return 1.0 + 4.0 * ( t - 0.5 );
}

double gammaFromParam( float value )
{
	//Geometric about 1: 0.5 is gamma 1, and equal travel either side gives
	//reciprocal gammas.
	const double t = std::clamp( double( value ), 0.0, 1.0 );
	return std::pow( 4.0, 2.0 * t - 1.0 );
}

double smoothFromParam( float value )
{
	//Up to 2% of the frame. Beyond that the depth map stops describing the
	//picture it came from and the blur lands in the wrong places.
	return 0.02 * std::clamp( double( value ), 0.0, 1.0 );
}

int tapsFromParam( float value )
{
	//A point of light at the top of the aperture range is spread over a disc a
	//tenth of the frame high, which is thousands of pixels -- and no tap count
	//that runs in real time fills that from a single source pixel. A specular
	//highlight will show the sampling pattern at Fast, and that is what Best
	//and Extreme are for. Real footage hides it: neighbouring pixels' discs
	//overlap, so the pattern only shows where a bright thing is small.
	switch( int( std::lround( value ) ) )
	{
		case 0: return 16; //Fast
		case 2: return 64; //Best
		case 3: return 128;//Extreme
		case 1:
		default: return 32;//Good
	}
}

DepthSource depthSourceFromParam( float value )
{
	const int index = std::clamp( int( std::lround( value ) ), 0, 4 );
	return DepthSource( index );
}

Blades bladesFromParam( float value )
{
	const int index = std::clamp( int( std::lround( value ) ), 0, 5 );
	return Blades( index );
}

bool depthSourceIsSampled( DepthSource source )
{
	return source != DepthSource::Radial;
}

bool depthSourceIsSplit( DepthSource source )
{
	return source == DepthSource::SplitH || source == DepthSource::SplitV;
}

} // namespace gaffer
