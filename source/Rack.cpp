#include "Rack.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gaffer
{
namespace
{
/// A triangle over one period: 0 at the start, 1 halfway, 0 at the end. Both
/// marks are reached exactly, once each, and the run is continuous across the
/// period boundary -- a sawtooth would put a jump cut in the focus every cycle,
/// which reads as a glitch rather than as a sweep.
double triangle( double phase )
{
	const double f = phase - std::floor( phase );
	return 1.0 - std::fabs( 2.0 * f - 1.0 );
}

/// splitmix64, for the Stutter sequence. A hash and not a generator: the same
/// cue index has to give the same plane on every machine, on every replay, and
/// after the composition has been saved and reopened.
double hashUnit( long long index )
{
	uint64_t x = static_cast< uint64_t >( index ) + 0x9E3779B97F4A7C15ull;
	x          = ( x ^ ( x >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
	x          = ( x ^ ( x >> 27 ) ) * 0x94D049BB133111EBull;
	x          = x ^ ( x >> 31 );

	return double( x >> 11 ) / double( 1ull << 53 );
}
} // namespace

double Rack::BarsPerCue( Sync sync )
{
	switch( sync )
	{
		case Sync::FourBars: return 4.0;
		case Sync::TwoBars: return 2.0;
		case Sync::Bar: return 1.0;
		case Sync::Half: return 0.5;
		case Sync::Quarter: return 0.25;
		case Sync::Eighth: return 0.125;
		case Sync::Sixteenth: return 0.0625;
		case Sync::Manual:
		case Sync::Free:
		default: return 0.0;
	}
}

double Rack::Shape( double t, double ease )
{
	const double u = std::clamp( t, 0.0, 1.0 );

	//Smootherstep: zero velocity AND zero acceleration at both ends. A hand on
	//a follow focus does not start or stop instantly, and the ordinary
	//smoothstep still shows a kick at the ends when the move is fast.
	const double s = u * u * u * ( u * ( u * 6.0 - 15.0 ) + 10.0 );

	return u + std::clamp( ease, 0.0, 1.0 ) * ( s - u );
}

void Rack::Configure( const RackSettings& settings )
{
	config = settings;
}

void Rack::Reset( double atFocus )
{
	focus       = std::clamp( atFocus, 0.0, 1.0 );
	moveFrom    = focus;
	moveTo      = focus;
	moveStart   = 0.0;
	moving      = false;
	handCues    = 0;
	lastCue     = 0;
	haveLastCue = false;
	started     = true;
}

double Rack::StutterTarget( double markA, double markB, long long index )
{
	return markA + ( markB - markA ) * hashUnit( index );
}

double Rack::EvaluateStateless( const RackSettings& s, double seconds, double bars, double barSeconds )
{
	const double a = std::clamp( s.markA, 0.0, 1.0 );
	const double b = std::clamp( s.markB, 0.0, 1.0 );

	if( s.mode == RackMode::Off || s.mode == RackMode::Follow )
		return a;

	const double period = BarsPerCue( s.sync );
	const double rate   = std::max( s.rateHz, 1e-4 );

	//How far through the sequence we are, and how long one cue lasts. Manual
	//has no grid to derive anything from, so it runs free at Rate -- which is
	//also what Sweep does with Manual selected, and is why the two share this.
	const double position    = period > 0.0 ? bars / period : seconds * rate;
	const double cueSeconds  = period > 0.0 ? period * std::max( barSeconds, 1e-6 ) : 1.0 / rate;

	if( s.mode == RackMode::Sweep )
	{
		const double f  = position - std::floor( position );
		const double tri = 1.0 - std::fabs( 2.0 * f - 1.0 );
		return a + ( b - a ) * Shape( tri, s.ease );
	}

	const double whole    = std::floor( position );
	const long long index = static_cast< long long >( whole );
	const double elapsed  = ( position - whole ) * cueSeconds;
	const double t        = elapsed / std::max( s.travelSeconds, 1e-4 );

	double from;
	double to;
	if( s.mode == RackMode::Stutter )
	{
		from = StutterTarget( a, b, index - 1 );
		to   = StutterTarget( a, b, index );
	}
	else
	{
		from = ( index & 1 ) ? a : b;
		to   = ( index & 1 ) ? b : a;
	}

	return std::clamp( from + ( to - from ) * Shape( t, s.ease ), 0.0, 1.0 );
}

void Rack::cuePosition( double now, double bars, long long& index, double& within ) const
{
	const double period = BarsPerCue( config.sync );

	double position;
	if( config.sync == Sync::Manual )
	{
		//No grid at all: every cue arrives by hand, so the index never advances
		//on its own and there is no phase to report.
		index  = 0;
		within = 0.0;
		return;
	}

	if( period > 0.0 )
		position = bars / period;
	else
		position = now * std::max( config.rateHz, 1e-4 );

	//floor(), not a truncation: bars can be negative when a host reports a
	//position before its own zero, and truncation would make the cue at -0.5
	//and the cue at +0.5 the same cue.
	const double whole = std::floor( position );

	index  = static_cast< long long >( whole );
	within = position - whole;
}

void Rack::Update( double now, double bars, double dt, bool fired )
{
	if( !started )
		Reset( config.markA );

	const double a = std::clamp( config.markA, 0.0, 1.0 );
	const double b = std::clamp( config.markB, 0.0, 1.0 );

	long long gridIndex = 0;
	double within       = 0.0;
	cuePosition( now, bars, gridIndex, within );

	switch( config.mode )
	{
		case RackMode::Off:
		{
			//No dynamics whatsoever. The null for the whole rack half, and it
			//has to be exact: Off must render identically to the plugin having
			//no rack in it, or the harness cannot use it as a baseline.
			focus  = a;
			moving = false;
			return;
		}

		case RackMode::Follow:
		{
			//An exponential approach, then a hard cap on how far the hand can
			//travel in one frame. The exponential alone would move a long way
			//in the first frame of a big jump, which is exactly the thing a
			//follow focus cannot do.
			const double travel  = std::max( config.travelSeconds, 1e-4 );
			const double maxStep = std::max( dt, 0.0 ) / travel;
			const double diff    = a - focus;

			if( std::fabs( diff ) <= maxStep )
			{
				//Within one frame's travel: land on it exactly rather than
				//approaching it forever. Still inside the speed limit, which is
				//why the test for it is the distance and not the step.
				focus = a;
			}
			else
			{
				const double tau  = travel * 0.35;
				double step       = diff * ( 1.0 - std::exp( -std::max( dt, 0.0 ) / tau ) );
				step              = std::clamp( step, -maxStep, maxStep );
				focus             = std::clamp( focus + step, 0.0, 1.0 );
			}

			moving = std::fabs( a - focus ) > 1e-9;
			return;
		}

		case RackMode::Sweep:
		{
			//Stateless: the position comes from the transport, so a scrub lands
			//where the timeline says and an hour of playback has not drifted.
			double phase;
			if( config.sync == Sync::Manual || config.sync == Sync::Free )
				phase = now * std::max( config.rateHz, 1e-4 );
			else
				phase = bars / BarsPerCue( config.sync );

			focus  = a + ( b - a ) * Shape( triangle( phase ), config.ease );
			moving = true;
			return;
		}

		case RackMode::Pull:
		case RackMode::Stutter:
		default:
			break;
	}

	//--- Pull and Stutter: discrete moves, fired by the grid or by hand. ---

	if( !haveLastCue )
	{
		lastCue     = gridIndex;
		haveLastCue = true;
	}

	bool cue = false;
	if( gridIndex != lastCue )
	{
		lastCue = gridIndex;
		cue     = true;
	}
	if( fired )
	{
		//A hand cue advances the sequence without touching the grid, so a
		//button press during a synced rack swaps the marks over and everything
		//afterwards stays on the bar line.
		++handCues;
		cue = true;
	}

	if( cue )
	{
		const long long index = gridIndex + handCues;

		moveFrom  = focus;
		moveTo    = config.mode == RackMode::Stutter
		              ? StutterTarget( config.markA, config.markB, index )
		              : ( ( index & 1 ) ? b : a );
		moveStart = now;
		moving    = true;
	}

	if( !moving )
		return;

	const double travel = std::max( config.travelSeconds, 1e-4 );
	const double t      = ( now - moveStart ) / travel;

	//t < 0 is the host having scrubbed backwards past the start of the move.
	//Landing on the mark is the only sensible answer -- the alternative is a
	//move that sits at its start point until something else cues it.
	if( t >= 1.0 || t < 0.0 )
	{
		focus  = std::clamp( moveTo, 0.0, 1.0 );
		moving = false;
		return;
	}

	focus = std::clamp( moveFrom + ( moveTo - moveFrom ) * Shape( t, config.ease ), 0.0, 1.0 );
}

} // namespace gaffer
