#pragma once

#include <FFGLSDK.h>

#include "Audio.h"
#include "Clock.h"
#include "Presets.h"
#include "Rack.h"
#include "Rattle.h"
#include "StoatworksAboutParams.h"

/**
    gaffer -- a simulated lens on a depth map, with the music holding it.

    Two things a camera department does, and one machine underneath both.

    **The rattle.** Stand a camera in front of a stack and the low end arrives
    as pressure on a rig that has a mass and a stiffness of its own. It shakes
    at ITS frequency and keeps shaking after the hit, and the lens elements are
    part of that assembly -- so the frame moves AND the focus comes off. See
    Rattle.h, which is a damped mass on a spring per axis rather than a shake
    generator with an audio-shaped amplitude.

    **The rack.** A focus puller with two marks taped on the barrel, or a hand
    that never stops. Off, Follow, Pull, Sweep and Stutter are five things one
    person can do with one hand, and the fast end of Sweep is the frame polling
    through every plane in the picture several times a second. See Rack.h.

    They meet at one number. Both halves resolve on the CPU, every frame, into
    a focal plane and a camera transform; the shader is handed the answer and
    never learns that either module exists. That is deliberate: it is what makes
    the interesting half of this plugin testable without a GPU, and
    `gftest --focus` and `gftest --rattle` are where it is actually tested.

    **The depth is the honest part**, because a video clip has no depth. Five
    sources, and they are not five settings of one thing:

    - **Radial** invents a field. Nothing is read out of the picture, so it
      works on any footage and what it produces is the LOOK of a shallow lens
      rather than the lens.
    - **Luma** and **Alpha** read a field out of the clip's own channels.
    - **Split H** and **Split V** read it out of the other half of the frame,
      which is the only one of the five that can carry a real, independent
      depth pass -- a render, a LiDAR frame, an AI depth map -- at full
      resolution and unentangled from the picture.

    Everything downstream is common to all five, because they all end as a
    disparity in 0..1 and the lens does not care where it came from.

    See AGENTS.md for the traps.
*/
class Gaffer : public CFFGLPlugin
{
public:
	Gaffer();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;
	FFResult SetTime( double time ) override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetTextParameter( unsigned int index ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Let the offline harness declare the host's clock unit instead of leaving
	/// it to be measured. The harness renders as fast as the GPU allows, so the
	/// measurement -- host time against real time -- has nothing to measure.
	void SetClockScaleForTest( double scale );

	/// What advance() decided this frame, for the harness's trace.
	void FrameState( double& focus, double& shiftX, double& shiftY, double& roll, double& scale ) const;

private:
	/// The order the host shows them in: the lens, where the depth comes from,
	/// what the focus is doing, what the music is doing to the rig, and how the
	/// frame is fitted to the result.
	enum ParamID : FFUInt32
	{
		//Lens
		PT_FOCUS,
		PT_APERTURE,
		PT_FOCAL,
		PT_BLADES,
		PT_ROTATION,
		PT_HIGHLIGHT,
		PT_BREATHING,

		//Depth
		PT_DEPTH,
		PT_DEPTH_GAIN,
		PT_FALLOFF,
		PT_SMOOTH,

		//Rack
		PT_RACK,
		PT_MARK_B,
		PT_SPEED,
		PT_RATE,
		PT_SYNC,
		PT_EASE,
		PT_PULL,

		//Rattle
		PT_AUDIO_FFT,
		PT_DRIVE,
		PT_BAND,
		PT_THRESHOLD,
		PT_RELEASE,
		PT_SHAKE,
		PT_ROLL,
		PT_RESONANCE,
		PT_DAMPING,
		PT_DEFOCUS,

		//Output
		PT_EDGES,
		PT_QUALITY,

		//Preset. Declared after the real controls so their IDs -- which a saved
		//composition refers to -- do not shift under existing users. Anything
		//added later goes AFTER this, in a group of its own.
		PT_PRESET,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// The ParamID each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ gaffer::presets::kParamCount ] = {
		PT_FOCUS, PT_APERTURE, PT_FOCAL, PT_BLADES, PT_HIGHLIGHT, PT_BREATHING,
		PT_RACK, PT_MARK_B, PT_SPEED, PT_RATE, PT_SYNC, PT_EASE,
		PT_DRIVE, PT_BAND, PT_THRESHOLD, PT_RELEASE, PT_SHAKE, PT_ROLL,
		PT_RESONANCE, PT_DAMPING, PT_DEFOCUS
	};

	static_assert( sizeof( kPresetParamIDs ) / sizeof( kPresetParamIDs[ 0 ] )
	                   == gaffer::presets::kParamCount,
	               "the preset table and its FFGL binding have drifted apart" );

	void applyPreset( int presetIndex );
	float presetValue( int presetIndex, unsigned int id ) const;

	/// What the effect should actually render `id` with. A parameter a live
	/// preset covers takes the preset's value; everything else takes the
	/// host's. See hostValues for why a preset cannot simply overwrite params[]
	/// and expect it to stay overwritten.
	float effective( unsigned int id ) const;

	/// Run the clock, the spectrum, the rig and the focus puller for this
	/// frame, and leave `frameFocus`, `frameShift`, `frameRoll` and
	/// `frameScale` holding the answer. Everything in here is CPU-side and
	/// GL-free, which is what lets the harness drive it without a context.
	void advance();

	/// A continuous bar count recovered from the host's tempo and bar phase.
	/// The host sends a position WITHIN the current bar and never says which
	/// bar it is, so the whole number is reconciled from the clock -- exact
	/// while the clock estimate stays inside half a bar, and continuous across
	/// the bar line either way.
	double barCount() const;

	/// What the HOST last sent for each parameter, which is not the same thing
	/// as what the plugin is rendering with.
	///
	/// FFGL's host owns parameter state. It may push its own values back down
	/// at any time, and nothing obliges it to act on the value events a plugin
	/// raises when it changes one itself. A plugin that applies a preset by
	/// writing params[] and trusting the host to follow is relying on behaviour
	/// the specification does not promise -- and when the host instead restates
	/// the values it still believes in, an "a covered parameter changed, so the
	/// operator has taken over" rule fires on the host's own echo and the
	/// preset appears not to stick. This is the fleet's issue #2, fixed here
	/// before it could be reported.
	float hostValues[ PT_COUNT ];

	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;

	gaffer::Clock clock;
	gaffer::Audio audio;
	gaffer::Rattle rattle;
	gaffer::Rack rack;

	/// Set by the Pull button and consumed by the next frame. A button is an
	/// event, and an event that is not consumed exactly once is a button that
	/// either does nothing or fires forever.
	bool manualPull = false;

	bool hostTimeSeen = false;
	int rackModeSeen  = -1;

	//What advance() decided, and all the shader is told.
	double frameFocus = 0.5;
	double frameShift[ 2 ] = { 0.0, 0.0 };
	double frameRoll  = 0.0;
	double frameScale = 1.0;

	long long frames = 0;

	/// Zero-initialised: the constructor writes a default for every real
	/// control, but the About block's ids are never stored to -- pressing a
	/// button opens a browser and returns -- so without this GetFloatParameter
	/// hands the host whatever was on the stack for them.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
