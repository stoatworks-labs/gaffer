#pragma once

#include <chrono>

/**
    What time it is, and how long the last frame was.

    Both sound trivial. Neither is.

    ### What unit is the host's clock in?

    The FFGL header does not say. Resolume sends MILLISECONDS -- measured live
    in Arena 7.27.1 at 20.0 per frame at its 50 fps, and the SDK's own Particles
    sample divides by 1000 -- while the offline harness, and any host that reads
    the header's silence as "SI", sends seconds. A plugin that assumes wrong
    runs a thousand times fast or freezes solid, and **no offline harness can
    catch it**, because the harness is the thing sending seconds.

    So the unit is measured rather than assumed: the host's clock is compared
    against a real one, and the ratio is either about 1 or about 1000 with
    nothing plausible in between. Several frames have to agree before it is
    settled, so one odd frame at load cannot decide it. Until then everything
    runs on the wall clock -- wrong in origin, right in rate, which is the safe
    way round.

    This replaced an earlier version elsewhere in the fleet that guessed from
    the magnitude of a single frame delta. That one decided nothing between 0.5
    and 2.0, could lock to "seconds" off a burst of sub-millisecond frames while
    the host was still loading, and assumed seconds while undecided -- which is
    precisely the wrong answer for the one host everybody actually uses.

    ### How long was this frame?

    FFGL does not say that either, so it is the difference between two readings
    -- and that difference has to be clamped. An unclamped delta after a dropped
    frame, a window drag or an operator scrubbing the transport hands the rig
    model half a second in one step, and everything that integrates over it
    lurches. Clamping is one line and it is not optional.
*/
namespace gaffer
{

class Clock
{
public:
	/// Advance to this frame. `hostTime` is whatever the host last passed to
	/// SetTime; `hostTimeSeen` is false until it has passed anything at all,
	/// which is not the same as it having passed zero.
	void Update( double hostTime, bool hostTimeSeen );

	/// Seconds since the plugin started, normalised and monotonic.
	double Now() const { return now; }

	/// The frame just entered, in seconds, already clamped.
	double FrameSeconds() const { return frameSeconds; }

	/// 1.0 for a seconds host, 0.001 for a milliseconds host, 0.0 undecided.
	double ClockScale() const { return clockScale; }

	bool UnitDecided() const { return clockScale != 0.0; }

	/// Declare the unit instead of measuring it. The offline harness renders as
	/// fast as the GPU allows, so the measurement -- which compares host time
	/// against real elapsed time -- has nothing meaningful to compare.
	void SetScaleForTest( double scale ) { clockScale = scale; }

	void Reset();

	/// How many frames have to agree before the unit is settled.
	static constexpr int kVotesNeeded = 4;

private:
	/// Shorter than this is a duplicate call or a clock that has not moved;
	/// longer is a stall, a scrub or a dropped frame. Both are clamped rather
	/// than believed.
	static constexpr double kMinFrame = 1.0 / 240.0;
	static constexpr double kMaxFrame = 1.0 / 24.0;

	std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

	double clockScale   = 0.0;
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;

	double now          = 0.0;
	double lastNow      = -1.0;
	double frameSeconds = 1.0 / 60.0;
};

} // namespace gaffer
