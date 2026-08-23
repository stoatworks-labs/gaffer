#include "Gaffer.h"

#include "Controls.h"
#include "Diag.h"
#include "Lens.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace gaffer;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Gaffer >,                                  // Create method
	"GF01",                                                   // Plugin unique ID of maximum length 4.
	"gaffer",                                                 // Plugin name
	2,                                                        // API major version number
	1,                                                        // API minor version number
	0,                                                        // Plugin major version number
	1,                                                        // Plugin minor version number
	FF_EFFECT,                                                // Plugin type
	"Audio-reactive lens: bass rattle and rack focus on depth",// Plugin description
	"gaffer FFGL effect"                                      // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// Where the picture and the depth map sit inside the frame, as offset.xy and
/// scale.zw in picture space.
///
/// The Split modes are the only reason this exists, and they are the only
/// reason the whole shader does its geometry in output space: everywhere else
/// this is the identity and could have been folded into the vertex shader.
void rectsFor( DepthSource source, float colour[ 4 ], float depth[ 4 ] )
{
	colour[ 0 ] = 0.0f;
	colour[ 1 ] = 0.0f;
	colour[ 2 ] = 1.0f;
	colour[ 3 ] = 1.0f;

	depth[ 0 ] = colour[ 0 ];
	depth[ 1 ] = colour[ 1 ];
	depth[ 2 ] = colour[ 2 ];
	depth[ 3 ] = colour[ 3 ];

	if( source == DepthSource::SplitH )
	{
		//Side by side, picture on the left. The convention every SBS depth
		//clip already uses, so a file authored for anything else drops in.
		colour[ 2 ] = 0.5f;
		depth[ 0 ]  = 0.5f;
		depth[ 2 ]  = 0.5f;
	}
	else if( source == DepthSource::SplitV )
	{
		//Over and under, picture on TOP. FFGL hands over a bottom-up texture,
		//so the top half is the upper half of v -- getting this the wrong way
		//round swaps the picture for the depth map, which is unmistakable on
		//screen and completely invisible in the code.
		colour[ 1 ] = 0.5f;
		colour[ 3 ] = 0.5f;
		depth[ 3 ]  = 0.5f;
	}
}
} // namespace

Gaffer::Gaffer()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//Both halves of this plugin move on their own, so the host has to be asked
	//for a clock. Without this Resolume never calls SetTime and every timed
	//mode falls back to the wall clock -- which works, but is not the
	//composition's timeline and cannot be scrubbed.
	SetTimeSupported( true );

	for( float& value : params )
		value = 0.0f;

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// Set to a real lens rather than to nothing, but with BOTH moving halves
	// at rest: an effect that starts shaking the moment it is dropped on a
	// layer is an effect that gets taken off again. What it does out of the
	// box is put a shallow lens on the clip, which is legible on any footage
	// and is one control away from either of the two things it is for.
	//---------------------------------------------------------------------
	params[ PT_FOCUS ]      = 0.55f;//just in front of the middle distance
	params[ PT_APERTURE ]   = 0.35f;//shallow, not silly
	params[ PT_FOCAL ]      = 0.45f;
	params[ PT_BLADES ]     = 2.0f; //six: the commonest iris there is
	params[ PT_ROTATION ]   = 0.0f;
	params[ PT_HIGHLIGHT ]  = 0.30f;
	params[ PT_BREATHING ]  = 0.35f;

	params[ PT_DEPTH ]      = 0.0f; //Radial: invents its field, so it works on any clip
	params[ PT_DEPTH_GAIN ] = 0.5f; //1:1
	params[ PT_FALLOFF ]    = 0.5f; //gamma 1
	params[ PT_SMOOTH ]     = 0.25f;//no effect on Radial; a sane start for a map

	params[ PT_RACK ]       = 0.0f; //Off: the Focus control, directly
	params[ PT_MARK_B ]     = 0.0f; //the far end of the barrel
	params[ PT_SPEED ]      = 0.5f;
	params[ PT_RATE ]       = 0.5f;
	params[ PT_SYNC ]       = 4.0f; //Bar
	params[ PT_EASE ]       = 1.0f; //a hand, not a motor
	params[ PT_PULL ]       = 0.0f;

	params[ PT_DRIVE ]      = 0.0f; //silent until asked: see below
	params[ PT_BAND ]       = 1.0f; //Low. This plugin is about the bass.
	params[ PT_THRESHOLD ]  = 0.15f;
	params[ PT_RELEASE ]    = 0.35f;
	params[ PT_SHAKE ]      = 0.45f;
	params[ PT_ROLL ]       = 0.30f;
	params[ PT_RESONANCE ]  = 0.50f;
	params[ PT_DAMPING ]    = 0.45f;
	params[ PT_DEFOCUS ]    = 0.45f;

	params[ PT_EDGES ]      = 2.0f; //clamp: the picture continues rather than stopping
	params[ PT_QUALITY ]    = 1.0f; //good

	params[ PT_PRESET ]     = 0.0f; //Custom: the sliders are the truth

	// Drive defaults to zero on purpose, and it is the one default worth
	// arguing about. With no audio routed the FFT buffer is all zeros, so
	// nothing would move anyway -- but with audio routed and Drive up, dropping
	// this on a layer starts shaking the frame before the operator has seen
	// what it does to a still picture. Shake, Roll, Defocus and the rig itself
	// are all set to something useful, so turning the one control up is the
	// whole gesture.

	for( unsigned int i = 0; i < PT_COUNT; ++i )
		hostValues[ i ] = params[ i ];

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Every parameter is a plain 0..1 float even where it stands for hertz,
	// seconds or radians. SetParamRange exists, but SetParamInfo clamps an
	// FF_TYPE_STANDARD default into 0..1 *before* a range can be attached
	// (SDK b1afaf9), so a parameter declared in Hz cannot declare a default in
	// Hz. The conversions live in Lens.cpp and Controls.cpp.
	//---------------------------------------------------------------------
	SetParamInfof( PT_FOCUS, "Focus", FF_TYPE_STANDARD );
	SetParamInfof( PT_APERTURE, "Aperture", FF_TYPE_STANDARD );
	SetParamInfof( PT_FOCAL, "Focal Length", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_BLADES, "Blades", 6, params[ PT_BLADES ] );
	SetParamElementInfo( PT_BLADES, 0, "Round", 0.0f );
	SetParamElementInfo( PT_BLADES, 1, "5", 1.0f );
	SetParamElementInfo( PT_BLADES, 2, "6", 2.0f );
	SetParamElementInfo( PT_BLADES, 3, "7", 3.0f );
	SetParamElementInfo( PT_BLADES, 4, "8", 4.0f );
	SetParamElementInfo( PT_BLADES, 5, "9", 5.0f );

	SetParamInfof( PT_ROTATION, "Rotation", FF_TYPE_STANDARD );
	SetParamInfof( PT_HIGHLIGHT, "Highlight", FF_TYPE_STANDARD );
	SetParamInfof( PT_BREATHING, "Breathing", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_DEPTH, "Depth", 5, params[ PT_DEPTH ] );
	SetParamElementInfo( PT_DEPTH, 0, "Radial", 0.0f );
	SetParamElementInfo( PT_DEPTH, 1, "Luma", 1.0f );
	SetParamElementInfo( PT_DEPTH, 2, "Alpha", 2.0f );
	SetParamElementInfo( PT_DEPTH, 3, "Split H", 3.0f );
	SetParamElementInfo( PT_DEPTH, 4, "Split V", 4.0f );

	SetParamInfof( PT_DEPTH_GAIN, "Depth Gain", FF_TYPE_STANDARD );
	SetParamInfof( PT_FALLOFF, "Falloff", FF_TYPE_STANDARD );
	SetParamInfof( PT_SMOOTH, "Smooth", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_RACK, "Rack", 5, params[ PT_RACK ] );
	SetParamElementInfo( PT_RACK, 0, "Off", 0.0f );
	SetParamElementInfo( PT_RACK, 1, "Follow", 1.0f );
	SetParamElementInfo( PT_RACK, 2, "Pull", 2.0f );
	SetParamElementInfo( PT_RACK, 3, "Sweep", 3.0f );
	SetParamElementInfo( PT_RACK, 4, "Stutter", 4.0f );

	SetParamInfof( PT_MARK_B, "Mark B", FF_TYPE_STANDARD );
	SetParamInfof( PT_SPEED, "Speed", FF_TYPE_STANDARD );
	SetParamInfof( PT_RATE, "Rate", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_SYNC, "Sync", 9, params[ PT_SYNC ] );
	SetParamElementInfo( PT_SYNC, 0, "Manual", 0.0f );
	SetParamElementInfo( PT_SYNC, 1, "Free", 1.0f );
	SetParamElementInfo( PT_SYNC, 2, "4 Bars", 2.0f );
	SetParamElementInfo( PT_SYNC, 3, "2 Bars", 3.0f );
	SetParamElementInfo( PT_SYNC, 4, "Bar", 4.0f );
	SetParamElementInfo( PT_SYNC, 5, "1/2", 5.0f );
	SetParamElementInfo( PT_SYNC, 6, "1/4", 6.0f );
	SetParamElementInfo( PT_SYNC, 7, "1/8", 7.0f );
	SetParamElementInfo( PT_SYNC, 8, "1/16", 8.0f );

	SetParamInfof( PT_EASE, "Ease", FF_TYPE_STANDARD );
	SetParamInfo( PT_PULL, "Pull", FF_TYPE_EVENT, false );

	// Declared with a real element list so the host knows how many bins to
	// fill. Resolume draws this as its audio-source picker rather than as a
	// slider; a host that has no audio simply never writes to it, and every
	// bin stays zero.
	SetBufferParamInfo( PT_AUDIO_FFT, "Audio", kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < kAudioBins; ++i )
		SetParamElementInfo( PT_AUDIO_FFT, i, "", 0.0f );

	SetParamInfof( PT_DRIVE, "Drive", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_BAND, "Band", kBandCount, params[ PT_BAND ] );
	SetParamElementInfo( PT_BAND, 0, "Full Range", 0.0f );
	SetParamElementInfo( PT_BAND, 1, "Low", 1.0f );
	SetParamElementInfo( PT_BAND, 2, "Mid", 2.0f );
	SetParamElementInfo( PT_BAND, 3, "High", 3.0f );

	SetParamInfof( PT_THRESHOLD, "Threshold", FF_TYPE_STANDARD );
	SetParamInfof( PT_RELEASE, "Release", FF_TYPE_STANDARD );
	SetParamInfof( PT_SHAKE, "Shake", FF_TYPE_STANDARD );
	SetParamInfof( PT_ROLL, "Roll", FF_TYPE_STANDARD );
	SetParamInfof( PT_RESONANCE, "Resonance", FF_TYPE_STANDARD );
	SetParamInfof( PT_DAMPING, "Damping", FF_TYPE_STANDARD );
	SetParamInfof( PT_DEFOCUS, "Defocus", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_EDGES, "Edges", 5, params[ PT_EDGES ] );
	SetParamElementInfo( PT_EDGES, 0, "Transparent", 0.0f );
	SetParamElementInfo( PT_EDGES, 1, "Black", 1.0f );
	SetParamElementInfo( PT_EDGES, 2, "Clamp", 2.0f );
	SetParamElementInfo( PT_EDGES, 3, "Mirror", 3.0f );
	SetParamElementInfo( PT_EDGES, 4, "Wrap", 4.0f );

	SetOptionParamInfo( PT_QUALITY, "Quality", 4, params[ PT_QUALITY ] );
	SetParamElementInfo( PT_QUALITY, 0, "Fast", 0.0f );
	SetParamElementInfo( PT_QUALITY, 1, "Good", 1.0f );
	SetParamElementInfo( PT_QUALITY, 2, "Best", 2.0f );
	SetParamElementInfo( PT_QUALITY, 3, "Extreme", 3.0f );

	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, presets::kPresets[ i ].name, float( 1 + i ) );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}

	//Thirty controls is a very long way past the point where an ungrouped list
	//in somebody else's inspector stops being readable.
	for( FFUInt32 i = PT_FOCUS; i <= PT_BREATHING; ++i )
		SetParamGroup( i, "Lens" );
	for( FFUInt32 i = PT_DEPTH; i <= PT_SMOOTH; ++i )
		SetParamGroup( i, "Depth" );
	for( FFUInt32 i = PT_RACK; i <= PT_PULL; ++i )
		SetParamGroup( i, "Rack" );
	for( FFUInt32 i = PT_AUDIO_FFT; i <= PT_DEFOCUS; ++i )
		SetParamGroup( i, "Rattle" );
	for( FFUInt32 i = PT_EDGES; i <= PT_QUALITY; ++i )
		SetParamGroup( i, "Output" );
	SetParamGroup( PT_PRESET, "Preset" );

	FFGLLog::LogToHost( "Created gaffer effect" );

	diag::init();
}

void Gaffer::SetClockScaleForTest( double scale )
{
	clock.SetScaleForTest( scale );
}

void Gaffer::FrameState( double& focus, double& shiftX, double& shiftY, double& roll, double& scale ) const
{
	focus  = frameFocus;
	shiftX = frameShift[ 0 ];
	shiftY = frameShift[ 1 ];
	roll   = frameRoll;
	scale  = frameScale;
}

FFResult Gaffer::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

FFResult Gaffer::InitGL( const FFGLViewportStruct* vp )
{
	// The GL strings first, and unconditionally: when a shader will not compile
	// it is almost always the driver or the GL version, and knowing which
	// machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !shader.Compile( kVertexShader, kFragmentShader ) )
	{
		// Returning FF_FAIL here is invisible to the operator: the effect
		// simply does nothing in Resolume, with no message anywhere. This line
		// is the only record that it was the shader.
		diag::error( "shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "gaffer: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "gaffer: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );

	return CFFGLPlugin::InitGL( vp );
}

double Gaffer::barCount() const
{
	//The host always sends something -- Resolume calls SetBeatInfo
	//unconditionally and the SDK defaults to 120/0 -- but a host that never
	//does would leave bpm at zero and make barSeconds infinite, so it is
	//guarded.
	const double tempo      = bpm > 1.0f ? double( bpm ) : 120.0;
	const double barSeconds = 240.0 / tempo;//four beats to the bar
	const double within     = std::clamp( double( barPhase ), 0.0, 1.0 );

	//The host hands over a position WITHIN the current bar and never says which
	//bar it is. The clock estimates how many have passed, barPhase gives the
	//exact position inside this one, and the whole number reconciling them is
	//round(estimate - phase). Continuous across the bar line, and exact while
	//the clock estimate stays within half a bar of the truth.
	return within + std::round( clock.Now() / barSeconds - within );
}

void Gaffer::advance()
{
	++frames;

	clock.Update( hostTime, hostTimeSeen );

	const double now = clock.Now();
	const double dt  = clock.FrameSeconds();

	//--- the spectrum -----------------------------------------------------
	const ParamInfo* fft = FindParamInfo( PT_AUDIO_FFT );
	if( fft != nullptr && !fft->elements.empty() )
	{
		//elements[] is a vector of structs, so it cannot be handed to Audio as
		//a float array. Copying 64 floats a frame is not the thing to optimise.
		float bins[ kAudioBins ] = {};
		const int count = int( std::min< size_t >( fft->elements.size(), kAudioBins ) );
		for( int i = 0; i < count; ++i )
			bins[ i ] = fft->elements[ i ].value;

		audio.Update( bins, count, dt, controls::AudioRelease( effective( PT_RELEASE ) ) );
	}

	const double level = audio.Level( controls::AudioBand( effective( PT_BAND ) ) );

	//--- the rig ----------------------------------------------------------
	RattleSettings rig;
	rig.drive     = effective( PT_DRIVE );
	rig.threshold = controls::Threshold( effective( PT_THRESHOLD ) );
	rig.frequency = controls::ResonanceHz( effective( PT_RESONANCE ) );
	rig.damping   = controls::Damping( effective( PT_DAMPING ) );
	rattle.Configure( rig );
	rattle.Update( level, dt );

	//--- the focus puller -------------------------------------------------
	RackSettings puller;
	puller.mode          = controls::Mode( effective( PT_RACK ) );
	puller.markA         = effective( PT_FOCUS );
	puller.markB         = effective( PT_MARK_B );
	puller.travelSeconds = controls::TravelSeconds( effective( PT_SPEED ) );
	puller.rateHz        = controls::RateHz( effective( PT_RATE ) );
	puller.sync          = controls::SyncDivision( effective( PT_SYNC ) );
	puller.ease          = controls::Ease( effective( PT_EASE ) );

	//A mode change is a different person picking up the unit, not a
	//continuation of what the last one was doing. Without this, switching from
	//a half-finished Pull into Sweep would leave the sweep running from
	//wherever the pull had got to.
	const int mode = int( puller.mode );
	if( mode != rackModeSeen )
	{
		rack.Configure( puller );
		rack.Reset( puller.markA );
		rackModeSeen = mode;
		diag::info( "rack mode -> " + std::to_string( mode ) );
	}
	rack.Configure( puller );

	//The button and the music both cue the same puller. Consumed exactly once:
	//a press that is not cleared is a button that fires forever.
	const bool cue = manualPull || rattle.Fired();
	manualPull     = false;

	rack.Update( now, barCount(), dt, cue );

	//--- what the shader is told ------------------------------------------
	const double defocus = controls::DefocusAmount( effective( PT_DEFOCUS ) );
	frameFocus = std::clamp( rack.Focus() + rattle.Knock() * defocus, 0.0, 1.0 );

	const double shake = controls::ShakeAmount( effective( PT_SHAKE ) );
	frameShift[ 0 ]    = rattle.Pan() * shake;
	frameShift[ 1 ]    = rattle.Tilt() * shake;
	frameRoll          = rattle.Roll() * controls::RollAmount( effective( PT_ROLL ) );

	frameScale = breathScale( frameFocus,
	                          phiFromParam( effective( PT_FOCAL ) ),
	                          breathingFromParam( effective( PT_BREATHING ) ) );

	//One line, once, sixty frames in: enough to settle "what unit did that host
	//actually send" and "did the transport ever arrive" without writing a log
	//entry every frame for the rest of the show.
	if( frames == 60 )
	{
		diag::info( "clock raw=" + std::to_string( hostTime )
		            + " scale=" + std::to_string( clock.ClockScale() )
		            + " now=" + std::to_string( now )
		            + " frame=" + std::to_string( dt )
		            + " bpm=" + std::to_string( bpm )
		            + " barPhase=" + std::to_string( barPhase ) );
	}
}

FFResult Gaffer::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	advance();

	//FFGL requires the context to be left in a default state on return, so use
	//the scoped bindings for everything touched here.
	ScopedShaderBinding shaderBinding( shader.GetGLID() );
	ScopedSamplerActivation activateSampler( 0 );
	Scoped2DTextureBinding textureBinding( picture.Handle );

	shader.Set( "InputTexture", 0 );

	//The input texture can be larger than the picture inside it, and its
	//dimensions can change from frame to frame, so both of these are uniforms
	//rather than anything baked into the geometry.
	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
	shader.Set( "MaxUV", maxCoords.s, maxCoords.t );

	shader.Set( "HalfTexel",
	            0.5f / float( picture.Width ),
	            0.5f / float( picture.Height ) );

	const float aspect = float( picture.Width ) / float( picture.Height );
	shader.Set( "Aspect", aspect );

	const DepthSource source = depthSourceFromParam( params[ PT_DEPTH ] );

	float colourRect[ 4 ];
	float depthRect[ 4 ];
	rectsFor( source, colourRect, depthRect );
	shader.Set( "ColourRect", colourRect[ 0 ], colourRect[ 1 ], colourRect[ 2 ], colourRect[ 3 ] );
	shader.Set( "DepthRect", depthRect[ 0 ], depthRect[ 1 ], depthRect[ 2 ], depthRect[ 3 ] );

	shader.Set( "DepthMode", float( int( source ) ) );
	shader.Set( "DepthGain", float( depthGainFromParam( effective( PT_DEPTH_GAIN ) ) ) );
	shader.Set( "Gamma", float( gammaFromParam( effective( PT_FALLOFF ) ) ) );

	//Zero for the invented field, which is smooth by construction and has no
	//noise to remove -- and four extra fetches per tap is not a rounding error
	//at the top of the Quality range.
	shader.Set( "DepthSmooth",
	            depthSourceIsSampled( source )
	                ? float( smoothFromParam( effective( PT_SMOOTH ) ) )
	                : 0.0f );

	const double aperture = apertureFromParam( effective( PT_APERTURE ) );
	const double phi      = phiFromParam( effective( PT_FOCAL ) );

	shader.Set( "Focus", float( frameFocus ) );
	shader.Set( "CocGain", float( cocGain( frameFocus, aperture, phi ) ) );
	shader.Set( "CocMax", float( cocMax( frameFocus, aperture, phi ) ) );

	shader.Set( "Blades", float( bladeCount( bladesFromParam( effective( PT_BLADES ) ) ) ) );
	shader.Set( "BladeRot", float( rotationFromParam( params[ PT_ROTATION ] ) ) );
	shader.Set( "Highlight", float( highlightFromParam( effective( PT_HIGHLIGHT ) ) ) );

	shader.Set( "Shift", float( frameShift[ 0 ] ), float( frameShift[ 1 ] ) );
	shader.Set( "Roll", float( frameRoll ) );
	shader.Set( "Scale", float( frameScale ) );

	shader.Set( "EdgeMode", params[ PT_EDGES ] );
	shader.Set( "Taps", float( tapsFromParam( params[ PT_QUALITY ] ) ) );

	quad.Draw();

	return FF_SUCCESS;
}

FFResult Gaffer::DeInitGL()
{
	shader.FreeGLResources();
	quad.Release();

	return FF_SUCCESS;
}

FFResult Gaffer::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_PULL )
	{
		//A rising edge only. Resolume sends a 1 on press and a 0 on release,
		//and treating both as a cue would rack the focus twice for one press.
		if( value > 0.5f && params[ PT_PULL ] <= 0.5f )
			manualPull = true;

		params[ PT_PULL ]     = value;
		hostValues[ PT_PULL ] = value;
		return FF_SUCCESS;
	}

	if( index == PT_PRESET )
	{
		const int chosen = int( std::lround( value ) );
		if( chosen != int( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	//Deliberately not logged. A parameter change is not a diagnostic event: the
	//host already shows the value, and an operator animating a slider would put
	//a line in the log every frame. This log exists for the shader that will
	//not compile, and it is worth nothing if it is buried.
	const float lastFromHost = hostValues[ index ];
	hostValues[ index ]      = value;
	params[ index ]          = value;

	// A slider moved while a preset is active means the operator has taken
	// over, and the dropdown falls back to Custom. Two things that are NOT an
	// operator moving a slider arrive through this same call, and reading
	// either as an edit is what makes a preset look like it will not stick:
	//
	//   - a host that honours the value events raised by applyPreset reads the
	//     new values and hands them straight back;
	//   - a host that does not simply carries on pushing the values it still
	//     believes in, which are the ones from before the preset.
	//
	// So neither is judged by whether the value changed -- both changed
	// something -- but by what the value IS.
	const int active = int( std::lround( params[ PT_PRESET ] ) );
	if( active <= 0 )
		return FF_SUCCESS;

	const float covered = presetValue( active, index );
	if( covered < 0.0f )
		return FF_SUCCESS;//not a parameter this preset has an opinion about

	// A quantisation allowance rather than a float epsilon. A host that keeps
	// its parameters as anything shorter than a float -- or that round-trips
	// them through a UI, a MIDI value or a saved composition -- hands back a
	// number near ours rather than ours.
	constexpr float kSame = 1e-3f;

	if( std::fabs( value - covered ) <= kSame )
		return FF_SUCCESS;

	if( std::fabs( value - lastFromHost ) <= kSame )
		return FF_SUCCESS;

	diag::info( "preset dropped to Custom: parameter " + std::to_string( index )
	            + " moved to " + std::to_string( value )
	            + " (preset says " + std::to_string( covered )
	            + ", host last said " + std::to_string( lastFromHost ) + ")" );

	params[ PT_PRESET ] = 0.0f;
	RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );

	return FF_SUCCESS;
}

float Gaffer::presetValue( int presetIndex, unsigned int id ) const
{
	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return -1.0f;

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
		if( kPresetParamIDs[ j ] == id )
			return preset.v[ j ];

	return -1.0f;
}

float Gaffer::effective( unsigned int id ) const
{
	const float fromPreset = presetValue( int( std::lround( params[ PT_PRESET ] ) ), id );
	return fromPreset >= 0.0f ? fromPreset : params[ id ];
}

void Gaffer::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = float( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
	{
		diag::info( "preset: Custom" );
		return;//Custom: the sliders keep whatever they said
	}

	diag::info( std::string( "preset: " ) + presets::kPresets[ presetIndex - 1 ].name );

	// hostValues is deliberately NOT written here. It is the record of what the
	// host has said, and the host has not said anything -- if this wrote to it,
	// the host's next restatement of its own values would read as an operator
	// edit and drop the preset on the spot.

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float Gaffer::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	// The effective value, so a host that re-reads its sliders after a preset
	// -- on a value event, or when saving the composition -- gets the numbers
	// the effect is actually rendering with rather than the ones it happened to
	// send last.
	return effective( index );
}

FFResult Gaffer::SetTextParameter( unsigned int index, const char* )
{
	// The About text is generated on read and never stored -- but a set of it
	// must SUCCEED. The SDK's instantiateGL pushes every parameter's default
	// into a fresh instance and destroys it on the first FF_FAIL, and the base
	// SetTextParameter returns FF_FAIL -- so without this the plugin fails
	// FF_INSTANTIATE_GL in the host, with no message anywhere.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, nullptr );
}

char* Gaffer::GetTextParameter( unsigned int index )
{
	// The host is handed a bare pointer, so the string is kept as a member
	// rather than built on the stack here.
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}
