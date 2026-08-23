#include "Clock.h"

#include <algorithm>

namespace gaffer
{

void Clock::Reset()
{
	lastNow      = -1.0;
	frameSeconds = 1.0 / 60.0;
}

void Clock::Update( double hostTime, bool hostTimeSeen )
{
	const double wallNow =
	    std::chrono::duration< double >( std::chrono::steady_clock::now() - startTime ).count();

	double normalised;

	if( !hostTimeSeen || hostTime < 0.0 )
	{
		//No host clock at all. The wall clock is already in seconds, so the
		//unit question does not arise and the scale must NOT be applied to it.
		normalised = wallNow;
	}
	else
	{
		if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
		{
			const double hostDelta = hostTime - lastRawTime;
			const double wallDelta = wallNow - lastWallTime;

			//A paused host, a looping clip or a stalled frame says nothing
			//about the unit, so it gets no vote either way.
			if( hostDelta > 0.0 && wallDelta >= 0.0005 )
			{
				const double ratio = hostDelta / wallDelta;

				if( ratio > 0.1 && ratio < 10.0 )
					++secondsVotes;
				else if( ratio > 100.0 && ratio < 10000.0 )
					++millisVotes;

				if( secondsVotes >= kVotesNeeded || millisVotes >= kVotesNeeded )
					clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
			}
		}

		lastRawTime = hostTime;

		//Until the unit is settled, run on the real clock rather than assume
		//one: wrong in origin but right in rate, where assuming seconds would
		//be a thousand times fast on the host most people are using.
		normalised = clockScale != 0.0 ? hostTime * clockScale : wallNow;
	}

	lastWallTime = wallNow;

	if( lastNow >= 0.0 )
	{
		//A host that loops a clip or scrubs backwards hands over a time that
		//goes DOWN. `now` follows it, because the cue grid is derived from the
		//host's own timeline and has to land where the timeline says -- but the
		//frame duration must not go negative, or every integrator in the plugin
		//runs backwards for one frame.
		const double delta = normalised > lastNow ? normalised - lastNow : 0.0;
		frameSeconds       = std::clamp( delta, kMinFrame, kMaxFrame );
	}

	lastNow = normalised;
	now     = normalised;
}

} // namespace gaffer
