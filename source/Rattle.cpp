#include "Rattle.h"

#include <algorithm>
#include <cmath>

namespace gaffer
{
namespace
{
constexpr double kTwoPi = 6.283185307179586;

/// Each axis's natural frequency, as a multiple of the Resonance control.
///
/// Deliberately incommensurate. A rig is stiffer fore-and-aft than it is in
/// pan, and stiffer again in roll; if these matched, all four axes would peak
/// on the same hit and the camera would run back and forth along one line
/// instead of shaking. The lens is the highest -- it is the lightest thing in
/// the assembly and the most tightly held.
constexpr double kAxisFrequency[ 4 ] = { 1.00, 1.31, 0.79, 1.73 };

/// How hard a hit pushes each axis, and which way. Fixed numbers rather than a
/// random spread: the same music has to shake the same way on every machine and
/// on every replay.
constexpr double kAxisGain[ 4 ] = { 1.00, -0.72, 0.55, 0.90 };

/// The bottom of the damping range. Zero damping is an oscillator that never
/// settles, and one hit would leave the frame moving for the rest of the show.
constexpr double kMinDamping = 0.02;
} // namespace

void Rattle::Configure( const RattleSettings& settings )
{
	config = settings;
}

void Rattle::Reset()
{
	for( Axis& axis : state )
	{
		axis.x = 0.0;
		axis.v = 0.0;
	}

	lastEnv = -1.0;
	onset   = 0.0;
	fired   = false;
}

void Rattle::Update( double env, double dt )
{
	fired = false;
	onset = 0.0;

	const double drive = std::clamp( config.drive, 0.0, 1.0 );

	const double level = std::clamp( env, 0.0, 1.0 );
	const double step  = std::max( dt, 0.0 );

	//The force is the envelope's rise, not its level. See the header: a held
	//note does not push a tripod over.
	if( lastEnv >= 0.0 && step > 0.0 )
	{
		const double rise = ( level - lastEnv ) / step;
		if( rise > 0.0 )
		{
			//The threshold is on the RISE per second, scaled so the control
			//reads on the same 0..1 scale as the envelope: a full-scale onset
			//arriving inside one 60 fps frame is 1.0.
			const double normalised = std::clamp( rise / 60.0, 0.0, 1.0 );
			const double gate       = std::clamp( config.threshold, 0.0, 1.0 );

			if( normalised > gate )
			{
				//Measured from the threshold rather than from zero, so raising
				//the gate does not also quietly turn the drive down.
				onset = ( normalised - gate ) / std::max( 1.0 - gate, 1e-3 );
				fired = true;
			}
		}
	}
	lastEnv = level;

	if( drive <= 0.0 )
	{
		//The null, and it has to be exact rather than nearly exact: with the
		//drive at zero this plugin's geometry must be the identity, and the
		//harness measures that as a byte comparison. Held at rest rather than
		//merely scaled by zero, so turning the drive back up starts from still
		//instead of releasing a bar's worth of stored energy at once.
		//
		//The onset detector above still ran, deliberately. Drive is how hard
		//the music shakes the CAMERA; the rack's audio cue is a separate
		//question, and "pull focus on the kick, but keep the camera still" is a
		//real request rather than a contradiction.
		for( Axis& axis : state )
		{
			axis.x = 0.0;
			axis.v = 0.0;
		}
		return;
	}

	const double w    = kTwoPi * std::max( config.frequency, 0.1 );
	const double zeta = std::clamp( config.damping, kMinDamping, 1.0 );

	//A hit is an impulse: a change in velocity, delivered once, not a force
	//held for a frame. Delivering it as a velocity step rather than smeared
	//across the substeps keeps the response independent of the frame rate,
	//which is what stops the same music shaking harder on a slower machine.
	if( fired )
	{
		for( int i = 0; i < 4; ++i )
			state[ i ].v += onset * drive * kAxisGain[ i ] * w * kAxisFrequency[ i ] * 0.5;
	}

	//Substepped, because semi-implicit Euler is only stable while w*h < 2 and
	//the frame time can be 1/24 s at the top of the frequency range. Stepping
	//at the frame rate would make a high Resonance setting diverge instead of
	//ring -- on a slow machine, silently, mid-show.
	//Clamp the TIME, not the step count. Capping the count instead would keep
	//the substep length proportional to the frame -- so an unclamped frame
	//delta would push w*h past the stability bound and diverge, which is the
	//exact failure the substepping is here to prevent.
	const double bounded = std::min( step, kMaxSubSteps * kSubStep );
	const int steps      = int( std::ceil( bounded / kSubStep ) );
	const double h       = steps > 0 ? bounded / double( steps ) : 0.0;

	for( int s = 0; s < steps; ++s )
	{
		for( int i = 0; i < 4; ++i )
		{
			Axis& axis = state[ i ];

			//Each axis on its own spring. Sharing one w here was the bug this
			//comment exists to stop coming back: the frequencies are declared
			//apart precisely so the axes do not peak together, and an
			//oscillator integrated at a frequency other than the one it was
			//kicked at rings at the wrong pitch with no visible symptom.
			const double wi = w * kAxisFrequency[ i ];
			const double a  = -( wi * wi ) * axis.x - 2.0 * zeta * wi * axis.v;

			axis.v += a * h;
			axis.x += axis.v * h;

			//The mount's travel limit. A real head bottoms out and stops; so
			//does this, and the velocity goes with it rather than being stored
			//up to fling the frame back. Without this an envelope that happens
			//to arrive on the resonance every cycle would pump the amplitude
			//up without bound, and this number multiplies a blur radius.
			if( axis.x > 1.0 )
			{
				axis.x = 1.0;
				axis.v = std::min( axis.v, 0.0 );
			}
			else if( axis.x < -1.0 )
			{
				axis.x = -1.0;
				axis.v = std::max( axis.v, 0.0 );
			}
		}
	}
}

} // namespace gaffer
