#pragma once

/**
    The focus puller.

    Where `Lens.h` says what a focal plane DOES, this says where the focal plane
    IS -- as a function of time, the host's transport, and whatever the operator
    or the music just asked for. It is the second half of the plugin and the
    half with no pixels in it at all: everything here is one number per frame,
    which is why it is also the half that can actually be tested.

    The model is a person with their hand on a follow focus, and every mode is
    something that person can do:

      Off       the barrel is where it was left. The Focus control, directly.
      Follow    the Focus control is where you are POINTING, and the hand takes
                time to get there. Animate Focus in the host and the lens lags
                the way a real one does, because a puller cannot teleport and
                neither can this.
      Pull      two marks taped on the barrel, and a rack from one to the other
                on a cue. The classic shot: A to B, hold, B to A.
      Sweep     the hand never stops. A continuous run between the marks, which
                at a slow rate is a searching focus and at a fast one is the
                frame strobing through every plane in the picture.
      Stutter   a new plane on every cue, chosen from a fixed sequence. Focus
                as a rhythm part rather than as a move.

    ### Cues

    Pull and Stutter need something to fire them, and Sweep needs a period. Both
    come from the same place: a grid, plus anything that has happened by hand.

    The grid is derived, never accumulated. Given a continuous bar count from
    the host and a division, the cue index is `floor(bars/period)` and the
    position within the current cue is the fraction -- so a rack is exactly on
    the bar line, it stays exactly on it after an hour, and scrubbing the
    transport lands where the timeline says rather than where an integrator
    happens to have got to. The only state is a count of the cues fired by hand,
    which is added to the index. **A hand cue moves the sequence on; it does not
    move the grid.**

    ### The one invariant worth protecting

    Focus never leaves the range the marks describe, and in Follow it never
    moves faster than the declared travel time. Both are checked directly by
    `gftest --focus`, and both matter for a reason that is not tidiness: this
    number drives a blur radius, and a focus that overshoots its marks is a
    frame that goes further out of focus than any control asked for. On a show,
    in front of people.
*/
namespace gaffer
{

enum class RackMode
{
	Off     = 0,
	Follow  = 1,
	Pull    = 2,
	Sweep   = 3,
	Stutter = 4
};

/// What the cue grid is counted in.
///
/// Manual is first because it is the one an operator reaches for when they want
/// the rack to answer to something other than the clock -- a button, or the
/// transient detector. In Sweep, which has no cues to withhold, Manual runs
/// free at Rate.
enum class Sync
{
	Manual    = 0,
	Free      = 1,//!< Rate, in Hz, off the plugin's own clock.
	FourBars  = 2,
	TwoBars   = 3,
	Bar       = 4,
	Half      = 5,
	Quarter   = 6,
	Eighth    = 7,
	Sixteenth = 8
};

struct RackSettings
{
	RackMode mode = RackMode::Off;

	/// The two marks on the barrel, as disparity: 1 is the near plane.
	/// `markA` is the plugin's Focus control, so Off and Follow use it alone.
	double markA = 0.5;
	double markB = 0.0;

	/// How long a full-barrel move takes, in seconds. The travel-time limit in
	/// Follow and the duration of a rack in Pull and Stutter.
	double travelSeconds = 0.6;

	/// Free-run cue rate, in Hz. Ignored unless `sync` is Manual or Free.
	double rateHz = 1.0;

	Sync sync = Sync::Free;

	/// 0 is a linear move -- a motor. 1 is fully eased at both ends, which is
	/// what a hand does. Real pulls are nearer 1 than 0.
	double ease = 1.0;
};

/**
	The state a focus puller has: where the barrel is, and how many cues have
	been called by hand.
*/
class Rack
{
public:
	void Configure( const RackSettings& settings );

	/// Advance one frame.
	///
	/// `now` is seconds on the plugin's normalised clock, `bars` the continuous
	/// bar count recovered from the host transport, `dt` the frame's duration
	/// already clamped, and `fired` true when a button press or an audio
	/// transient has called a cue this frame.
	void Update( double now, double bars, double dt, bool fired );

	/// Where the focal plane is, as disparity in 0..1.
	double Focus() const
	{
		return focus;
	}

	/// True while a Pull or Stutter move is in progress, which is the only
	/// thing about this that is worth logging.
	bool Moving() const
	{
		return moving;
	}

	/// Put the barrel somewhere and forget everything else. Used on the first
	/// frame, and whenever the mode changes -- a mode change is a different
	/// person picking up the unit, not a continuation.
	void Reset( double atFocus );

	/// The cue period in bars for a synced division, or 0 for the two that are
	/// not counted in bars.
	static double BarsPerCue( Sync sync );

	/// The eased position of a move that is `t` of the way through, 0..1.
	/// Public because both this and the offline harness need it, and because it
	/// is the one piece of shaping here that is a matter of taste rather than
	/// of mechanism.
	static double Shape( double t, double ease );

private:
	/// Cue index and position within the cue, from the grid plus hand cues.
	void cuePosition( double now, double bars, long long& index, double& within ) const;

	/// The plane Stutter jumps to on cue `index`. A hash rather than a random
	/// number generator: the sequence has to be the same on every machine and
	/// on every replay of the same timeline, or a saved composition does not
	/// play back the same way twice.
	double stutterTarget( long long index ) const;

	RackSettings config;

	double focus     = 0.5;
	double moveFrom  = 0.5;
	double moveTo    = 0.5;
	double moveStart = 0.0;
	bool moving      = false;

	long long handCues = 0;
	long long lastCue  = 0;
	bool haveLastCue   = false;
	bool started       = false;
};

} // namespace gaffer
