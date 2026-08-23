#pragma once

/**
    The rig.

    A camera in front of a loud PA does not move like the music. It moves like
    a camera: the pressure wave hits a body bolted to a head bolted to a set of
    legs, and that assembly has a mass, a stiffness and a damping of its own. It
    answers at ITS frequency, not the drummer's, and it keeps answering after
    the hit has gone.

    So this is not a shake generator with an audio-shaped amplitude. It is a
    damped mass on a spring, per axis, and the audio is the force on it:

        x'' + 2*zeta*w*x' + w^2*x = F(t)      w = 2*pi*frequency

    Everything a real rattle does falls out of that rather than being arranged.
    One kick rings and settles. Kicks at the rig's own frequency build up, and
    kicks off it do not. Turning the damping down turns a knock into a wobble
    that outlives the bar. None of that is a special case in here.

    ### The force is the CHANGE, not the level

    The plugin has an envelope, not a pressure. They are not the same thing and
    the difference decides how this behaves:

    A sustained bass note is a big envelope and a nearly constant one. Feeding
    that in as a force would lean the camera over and hold it there for as long
    as the note lasts, which is not what happens -- a held note does not push a
    tripod in one direction, because the pressure it carries is oscillating far
    above anything a rig can follow. What actually shakes the rig at frame rate
    is the envelope's ARRIVAL: the leading edge of the kick.

    So the drive is `max(0, d(env)/dt)`, gated at Threshold. A kick rings the
    rig; a drone does not move it. That also makes `Fired()` meaningful, and it
    is what the rack's audio cue is wired to -- the same onset that shakes the
    camera is the one that pulls focus.

    ### Four axes, three frequencies apart

    Pan, tilt, roll and one for the lens itself. Their natural frequencies are
    deliberately not equal: a rig is stiffer in some directions than others, and
    if they matched, every axis would peak together and the camera would rattle
    back and forth along one diagonal line like a slider rather than shaking.

    ### Two things that are not physics but are not negotiable

    **The integrator is substepped at a fixed rate.** Semi-implicit Euler is
    stable while `w*h < 2`; the plugin's frame time can reach 1/24 s and the
    frequency can reach the top of its range, which is well past that. Stepping
    the oscillator at the frame rate would make a high Resonance setting explode
    instead of ringing -- silently, and only on a slow machine. It steps at
    kSubStep and takes as many steps as the frame needs.

    **The travel is clamped.** A mount has a limit and hits it; so does this,
    and the velocity is dropped when it does. That is what makes the output
    bounded for every input, including an envelope crafted to hit the resonance
    every cycle -- which matters because this number ends up multiplying a blur
    radius and a frame offset, live, in front of people.
*/
namespace gaffer
{

struct RattleSettings
{
	/// How hard the audio hits the rig, 0..1. Zero is an exact null: every
	/// output is 0.0 for every input, forever.
	double drive = 0.0;

	/// Onsets below this are not hits. 0..1, on the same scale as the envelope.
	double threshold = 0.1;

	/// The rig's fundamental, in Hz. Roughly: a heavy tripod is 4-8, a
	/// lightweight sticks-and-fluid-head 10-18, a handheld operator lower and
	/// much more damped.
	double frequency = 9.0;

	/// The damping ratio, 0..1. Below about 0.1 it rings for whole bars; at 1
	/// it is critically damped and each hit is a single lurch.
	double damping = 0.25;
};

/**
	Four damped oscillators and the onset detector that drives them.
*/
class Rattle
{
public:
	void Configure( const RattleSettings& settings );

	/// Advance one frame. `env` is the chosen band's smoothed level, 0..1;
	/// `dt` the frame's duration in seconds, already clamped.
	void Update( double env, double dt );

	/// Normalised displacements, each in -1..1. Multiply by whatever the Shake,
	/// Roll and Defocus controls are worth.
	double Pan() const { return state[ 0 ].x; }
	double Tilt() const { return state[ 1 ].x; }
	double Roll() const { return state[ 2 ].x; }
	double Knock() const { return state[ 3 ].x; }

	/// True on a frame where an onset cleared the threshold. The rack's audio
	/// cue: the hit that shakes the camera is the hit that pulls focus.
	bool Fired() const { return fired; }

	/// The onset that was accepted this frame, 0..1, for diagnostics.
	double Onset() const { return onset; }

	/// Everything to rest. Called when the drive is off, so switching it back
	/// on does not release a bar's worth of stored energy at once.
	void Reset();

	/// The integrator's fixed step. Public because the harness asserts against
	/// it: the stability bound this satisfies is the reason it exists.
	static constexpr double kSubStep = 1.0 / 480.0;

	/// The most substeps a single frame may take. The clamped frame time
	/// already bounds this; the constant is here so a caller that forgets to
	/// clamp cannot spend a second inside one Update.
	static constexpr int kMaxSubSteps = 32;

private:
	struct Axis
	{
		double x = 0.0;///< displacement, clamped to -1..1
		double v = 0.0;///< velocity
	};

	RattleSettings config;

	Axis state[ 4 ];
	double lastEnv = -1.0;
	double onset   = 0.0;
	bool fired     = false;
};

} // namespace gaffer
