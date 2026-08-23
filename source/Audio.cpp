#include "Audio.h"

#include <algorithm>
#include <cmath>

namespace gaffer
{
namespace
{
constexpr int kRange[ kBandCount ][ 2 ] = {
	{ 0, 63 },//Full Range
	{ 0, 7 }, //Low
	{ 8, 27 },//Mid
	{ 28, 63 }//High
};
} // namespace

void Audio::BandRange( Band band, int& first, int& last )
{
	const int which = std::clamp( int( band ), 0, kBandCount - 1 );

	first = kRange[ which ][ 0 ];
	last  = kRange[ which ][ 1 ];
}

void Audio::Reset()
{
	level.fill( 0.0f );
}

void Audio::Update( const float* bins, int count, double dt, double releaseSeconds )
{
	if( bins == nullptr || count <= 0 )
		return;

	//A clock that has not moved snaps rather than filtering, which is what the
	//first frame needs -- an exponential with dt of zero would hold the level
	//at whatever it was initialised to for as long as the host stayed paused.
	const float release = dt > 0.0
	                          ? 1.0f - std::exp( float( -dt / std::max( releaseSeconds, 1e-3 ) ) )
	                          : 1.0f;

	const int bins_ = std::min( count, kAudioBins );
	for( int i = 0; i < bins_; ++i )
	{
		const float raw = std::sqrt( std::max( 0.0f, bins[ i ] ) );

		if( raw >= level[ i ] )
			level[ i ] = raw;
		else
			level[ i ] += ( raw - level[ i ] ) * release;
	}
}

double Audio::Level( Band band ) const
{
	int first = 0;
	int last  = 0;
	BandRange( band, first, last );

	//The mean, not the peak. A peak follows whichever bin happens to be loudest
	//and jumps between them; what shakes a rig is the band's whole output at
	//once.
	double sum = 0.0;
	for( int i = first; i <= last; ++i )
		sum += level[ i ];

	const int counted = last - first + 1;
	return counted > 0 ? std::clamp( sum / double( counted ), 0.0, 1.0 ) : 0.0;
}

} // namespace gaffer
