/**
    gftest -- run gaffer offline, and measure what it did.

    Two halves of this plugin fail in two completely different ways, so there
    are two completely different kinds of check in here.

    **The half with no pixels.** Where the focal plane is, and how far the rig
    has been knocked, are numbers per frame produced by Rack.cpp and Rattle.cpp
    with no GL anywhere near them. `--focus` and `--rattle` drive those directly
    over thousands of frames and assert the properties they claim: that focus
    never leaves its marks, that a follow never exceeds its travel speed, that a
    synced sweep is exactly periodic on the bar, that a rig with no drive is
    exactly still, and that no input can make an oscillator run away. Those run
    without a GPU, which matters because a hosted CI runner does not have one.

    **The half that is a resampler.** A lens is a convolution, and the kernel of
    a convolution is what you get by putting a point of light through it -- so
    `--bokeh` renders an impulse and measures the disc that comes out. That one
    render answers three separate questions at once:

      * the RADIUS, against the circle of confusion Lens.cpp predicts, which is
        the only thing checking that the GLSL copy of the lens maths still
        agrees with the C++ one;
      * the ENERGY, against the same frame rendered through a pinhole, because a
        lens does not create or destroy light and the gather's 1/(pi r^2)
        weighting is exactly the claim that it does not;
      * and INDEPENDENTLY in x and y, because a disc that is not round is an
        aspect-ratio bug that no amount of looking at a picture reliably finds.

    `--iris` uses the same impulse to check the aperture shape: the bokeh from
    an n-bladed iris has n-fold rotational symmetry and does not have (n+1)-fold
    symmetry, which is a stronger statement than "it looked hexagonal".

    `--null` is the one that needs no interpretation at all. With the aperture
    closed, the breathing at zero and the rig at rest, this effect is the
    identity, and the output must be the input byte for byte.

    The depth used by --bokeh and --iris comes from the SPLIT H mode, and that
    is not incidental. It is the only mode in which the picture and the depth
    map are independent, so it is the only one where an impulse can be put
    through a known, uniform depth. Testing the lens through Luma would mean the
    impulse WAS the depth map.
*/

#include "Audio.h"
#include "Clock.h"
#include "Controls.h"
#include "Gaffer.h"
#include "Lens.h"
#include "Rack.h"
#include "Rattle.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace gaffer;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( (unsigned char)( value >> 24 ) );
	out.push_back( (unsigned char)( value >> 16 ) );
	out.push_back( (unsigned char)( value >> 8 ) );
	out.push_back( (unsigned char)( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, (uint32_t)data.size() );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, (uInt)( 4 + data.size() ) );
	putU32( out, (uint32_t)crc );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( (size_t)height * ( 1 + (size_t)width * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + (size_t)y * width * 4;
		raw.insert( raw.end(), row, row + (size_t)width * 4 );
	}

	uLongf compressedSize = compressBound( (uLong)raw.size() );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), (uLong)raw.size(), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, (uint32_t)width );
	putU32( ihdr, (uint32_t)height );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Test pictures.
//
// These builders return BOTTOM-UP buffers, ready for glTexImage2D, because
// that is what GL wants. readBack() returns top-down. Comparing one against the
// other index by index silently compares row y with row height-1-y, which does
// not look like a bug: it produces a large, plausible and completely CONSTANT
// error that does not change when the effect does.
//---------------------------------------------------------------------------
void setTexel( std::vector< unsigned char >& image, int width, int height, int x, int y,
               unsigned char r, unsigned char g, unsigned char b, unsigned char a )
{
	if( x < 0 || y < 0 || x >= width || y >= height )
		return;

	//y measured from the TOP here; the flip to GL's bottom-up order happens
	//once, at this single point, rather than somewhere in the middle of the
	//chain where it would be a permanent trap.
	const size_t index = ( (size_t)( height - 1 - y ) * width + x ) * 4;
	image[ index + 0 ] = r;
	image[ index + 1 ] = g;
	image[ index + 2 ] = b;
	image[ index + 3 ] = a;
}

/// Side by side: a block of light in the middle of the picture half, and a flat
/// level filling the depth half.
///
/// A BLOCK and not a single texel. A single texel of full white spread over a
/// disc of a hundred pixels' radius lands at a fraction of one 8-bit level and
/// reads back as zeros -- the measurement would be of the readback's rounding
/// rather than of the lens. The block's own second moment is removed
/// analytically by measuring the pinhole render as well, so nothing is lost by
/// making it big enough to see.
std::vector< unsigned char > buildSplitImpulse( int width, int height, double depthLevel, int blockPx )
{
	std::vector< unsigned char > image( (size_t)width * height * 4, 0 );

	const int half = width / 2;
	const unsigned char level = (unsigned char)std::lround( std::clamp( depthLevel, 0.0, 1.0 ) * 255.0 );

	//The depth half first, so nothing about the picture can be read as depth.
	for( int y = 0; y < height; ++y )
		for( int x = half; x < width; ++x )
			setTexel( image, width, height, x, y, level, level, level, 255 );

	//The picture half: black, with one block of light at its centre.
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < half; ++x )
			setTexel( image, width, height, x, y, 0, 0, 0, 255 );

	//Half as wide in TEXTURE pixels as it is tall, because the picture half is
	//stretched two to one on its way to the output -- so the block is square
	//where it is measured. An impulse that is square in the texture and oblong
	//on screen makes the x and y moments incomparable, and the roundness check
	//is then measuring the impulse rather than the bokeh.
	const int cx = half / 2;
	const int cy = height / 2;
	const int bw = std::max( 2, blockPx / 2 );
	for( int y = cy - blockPx / 2; y < cy + blockPx / 2; ++y )
		for( int x = cx - bw / 2; x < cx + bw / 2; ++x )
			setTexel( image, width, height, x, y, 255, 255, 255, 255 );

	return image;
}

/// A legible picture for --out: a chequer of coloured squares under a
/// horizontal depth ramp, so near and far are both visibly present.
std::vector< unsigned char > buildCard( int width, int height )
{
	std::vector< unsigned char > image( (size_t)width * height * 4, 0 );

	const int half = width / 2;
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < half; ++x )
		{
			const int cell = ( ( x / 24 ) + ( y / 24 ) ) & 1;
			const unsigned char base = cell ? 220 : 40;

			//A grid of bright dots on top, because a bokeh is only legible on
			//something small and bright.
			const bool dot = ( x % 48 < 4 ) && ( y % 48 < 4 );

			setTexel( image, width, height, x, y,
			          dot ? 255 : base,
			          dot ? 255 : (unsigned char)( base / 2 ),
			          dot ? 255 : (unsigned char)( 255 - base ),
			          255 );
		}

		for( int x = half; x < width; ++x )
		{
			//The depth half: near on the left of the picture, far on the right.
			const double t = double( x - half ) / double( std::max( width - half - 1, 1 ) );
			const unsigned char d = (unsigned char)std::lround( ( 1.0 - t ) * 255.0 );
			setTexel( image, width, height, x, y, d, d, d, 255 );
		}
	}

	return image;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without a
	//GPU, where it will at least prove the shader compiles.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
		kCGLPFAAccelerated,
		kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
		kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
		(CGLPixelFormatAttribute)0
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
		kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
		kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
		(CGLPixelFormatAttribute)0
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels, GLint internalFormat = GL_RGBA8 )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

bool runPass( Gaffer& plugin, GLuint source, GLuint targetFBO, int width, int height )
{
	FFGLTextureStruct inputStruct = {};
	inputStruct.Width = inputStruct.HardwareWidth = (FFUInt32)width;
	inputStruct.Height = inputStruct.HardwareHeight = (FFUInt32)height;
	inputStruct.Handle                              = source;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process = {};
	process.numInputTextures    = 1;
	process.inputTextures       = inputs;
	process.HostFBO             = targetFBO;

	glBindFramebuffer( GL_FRAMEBUFFER, targetFBO );
	glViewport( 0, 0, width, height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );
	return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
}

std::vector< unsigned char > readBack( GLuint fbo, int width, int height )
{
	std::vector< unsigned char > pixels( (size_t)width * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );

	//GL hands back bottom-up; everything above is written top-down.
	std::vector< unsigned char > flipped( pixels.size() );
	const size_t stride = (size_t)width * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + (size_t)y * stride,
		             pixels.data() + (size_t)( height - 1 - y ) * stride, stride );
	return flipped;
}

/// The same, in float, off a 32-bit float target.
///
/// The measurements below are second moments, and a second moment lives in the
/// dim outer ring of a disc -- which is exactly what 8-bit quantisation throws
/// away. Read the impulse back at 8 bits and the measured radius comes out
/// several percent short, by a margin that changes with the shape of the
/// impulse rather than with anything about the lens. That is not a tolerance to
/// widen; it is a measurement of the readback.
std::vector< float > readBackFloat( GLuint fbo, int width, int height )
{
	std::vector< float > pixels( (size_t)width * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );

	std::vector< float > flipped( pixels.size() );
	const size_t stride = (size_t)width * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + (size_t)y * stride,
		             pixels.data() + (size_t)( height - 1 - y ) * stride, stride * sizeof( float ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Measurement.
//---------------------------------------------------------------------------
struct Moments
{
	double energy = 0.0;///< sum of luma over the frame
	double cx     = 0.0;///< centroid, in pixels
	double cy     = 0.0;
	double xx     = 0.0;///< second moments about the centroid, in pixels squared
	double yy     = 0.0;
};

double lumaOf( const unsigned char* p )
{
	return ( kLumaR * p[ 0 ] + kLumaG * p[ 1 ] + kLumaB * p[ 2 ] ) / 255.0;
}

double lumaOf( const float* p )
{
	return kLumaR * p[ 0 ] + kLumaG * p[ 1 ] + kLumaB * p[ 2 ];
}

template< typename Pixel >
Moments momentsOf( const std::vector< Pixel >& rgba, int width, int height )
{
	Moments m;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const double w = lumaOf( &rgba[ ( (size_t)y * width + x ) * 4 ] );
			m.energy += w;
			m.cx += w * x;
			m.cy += w * y;
		}
	}

	if( m.energy <= 0.0 )
		return m;

	m.cx /= m.energy;
	m.cy /= m.energy;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const double w  = lumaOf( &rgba[ ( (size_t)y * width + x ) * 4 ] );
			const double dx = x - m.cx;
			const double dy = y - m.cy;
			m.xx += w * dx * dx;
			m.yy += w * dy * dy;
		}
	}

	m.xx /= m.energy;
	m.yy /= m.energy;
	return m;
}

//---------------------------------------------------------------------------
// Parameter access by display name.
//
// Names come from the plugin's own declaration rather than from a table here,
// so a parameter that is renamed or reordered cannot leave the harness quietly
// setting the wrong one.
//---------------------------------------------------------------------------
int indexOfParameter( Gaffer& plugin, const std::string& name )
{
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* declared = plugin.GetParamName( i );
		if( declared != nullptr && name == declared )
			return (int)i;
	}
	return -1;
}

bool setParameter( Gaffer& plugin, const std::string& name, float value )
{
	const int index = indexOfParameter( plugin, name );
	if( index < 0 )
		return false;
	plugin.SetFloatParameter( (unsigned int)index, value );
	return true;
}

float getParameter( Gaffer& plugin, const std::string& name )
{
	const int index = indexOfParameter( plugin, name );
	return index < 0 ? 0.0f : plugin.GetFloatParameter( (unsigned int)index );
}

/// A synthetic spectrum, because there is no audio here and there is none on a
/// hosted CI runner either.
///
/// Injected through the same public SetParamElementValue the host uses, into
/// the same FF_USAGE_FFT buffer parameter, so the plugin cannot tell the
/// difference -- which is the point. Without it every control in the Rattle
/// group reads as dead, and a sweep that reports twelve dead controls is a
/// sweep nobody looks at.
void injectSpectrum( Gaffer& plugin, double seconds )
{
	const int id = indexOfParameter( plugin, "Audio" );
	if( id < 0 )
		return;

	//A kick on every half second and a hat on every eighth of one: enough of a
	//pattern that the onset detector has edges to find, which is what the whole
	//Rattle group is downstream of.
	const double kick = std::exp( -std::fmod( seconds, 0.5 ) / 0.06 );
	const double hat  = std::exp( -std::fmod( seconds, 0.125 ) / 0.02 );

	for( int i = 0; i < kAudioBins; ++i )
	{
		const double f = double( i ) / double( kAudioBins - 1 );

		double v;
		if( i <= 7 )
			v = 0.9 * kick;
		else if( i <= 27 )
			v = 0.35 * ( 0.6 + 0.4 * std::sin( seconds * 2.1 + f * 6.0 ) );
		else
			v = 0.30 * hat;

		plugin.SetParamElementValue( (unsigned int)id, (unsigned int)i, float( v ) );
	}
}

//---------------------------------------------------------------------------
// --focus: the focus puller, against the properties it claims.
//
// No GL. This is the half of the plugin that is a number rather than a
// picture, and a number can simply be checked.
//---------------------------------------------------------------------------
struct Failures
{
	int count = 0;

	void check( bool ok, const char* what )
	{
		if( ok )
			return;
		std::printf( "  FAILED: %s\n", what );
		++count;
	}
};

int runFocusTest()
{
	Failures f;

	constexpr double kFps = 60.0;
	constexpr double kDt  = 1.0 / kFps;

	//120 bpm, four beats to the bar: two seconds, 120 frames, exactly.
	constexpr double kBarSeconds = 2.0;
	constexpr int kFramesPerBar  = 120;

	auto barsAt = []( double now ) { return now / kBarSeconds; };

	//--- Off is exactly the Focus control, forever ------------------------
	{
		RackSettings s;
		s.mode  = RackMode::Off;
		s.markA = 0.37;
		s.markB = 0.9;

		Rack rack;
		rack.Configure( s );

		bool exact = true;
		for( int i = 0; i < 600; ++i )
		{
			rack.Update( i * kDt, barsAt( i * kDt ), kDt, i % 37 == 0 );
			if( rack.Focus() != s.markA )
				exact = false;
		}
		f.check( exact, "Off is not exactly the Focus control (even a cue must not move it)" );
	}

	//--- Follow never exceeds its declared travel speed -------------------
	{
		RackSettings s;
		s.mode          = RackMode::Follow;
		s.markA         = 1.0;
		s.travelSeconds = 0.8;

		Rack rack;
		rack.Configure( s );
		rack.Reset( 0.0 );

		const double limit = kDt / s.travelSeconds;
		double previous    = 0.0;
		double worst       = 0.0;
		int settledAt      = -1;

		for( int i = 0; i < 600; ++i )
		{
			rack.Update( i * kDt, barsAt( i * kDt ), kDt, false );
			worst    = std::max( worst, std::fabs( rack.Focus() - previous ) );
			previous = rack.Focus();

			if( settledAt < 0 && std::fabs( rack.Focus() - s.markA ) < 1e-9 )
				settledAt = i;
		}

		std::printf( "  follow: fastest frame %.6f, limit %.6f, settled after %d frames\n",
		             worst, limit, settledAt );

		f.check( worst <= limit + 1e-12, "Follow moved faster than a full barrel in Speed seconds" );
		f.check( settledAt >= 0, "Follow never reached its target" );
		f.check( settledAt < 0 || settledAt * kDt >= s.travelSeconds * 0.5,
		         "Follow reached its target implausibly early -- the speed limit is not being applied" );
	}

	//--- A synced sweep is exactly periodic on the bar, and reaches both marks
	{
		RackSettings s;
		s.mode  = RackMode::Sweep;
		s.markA = 0.95;
		s.markB = 0.05;
		s.sync  = Sync::Bar;
		s.ease  = 0.0;//linear, so the endpoints are unmistakable

		Rack rack;
		rack.Configure( s );

		std::vector< double > trace;
		for( int i = 0; i < kFramesPerBar * 4; ++i )
		{
			rack.Update( i * kDt, barsAt( i * kDt ), kDt, false );
			trace.push_back( rack.Focus() );
		}

		double drift = 0.0;
		for( size_t i = 0; i + kFramesPerBar < trace.size(); ++i )
			drift = std::max( drift, std::fabs( trace[ i ] - trace[ i + kFramesPerBar ] ) );

		const double lowest  = *std::min_element( trace.begin(), trace.end() );
		const double highest = *std::max_element( trace.begin(), trace.end() );

		std::printf( "  sweep:  worst bar-to-bar drift %.3e, range %.4f..%.4f\n", drift, lowest, highest );

		f.check( drift < 1e-9, "a Bar-synced sweep is not exactly periodic on the bar" );
		f.check( highest > s.markA - 1e-6, "the sweep never reached mark A" );
		f.check( lowest < s.markB + 1e-6, "the sweep never reached mark B" );
		f.check( lowest >= std::min( s.markA, s.markB ) - 1e-9
		             && highest <= std::max( s.markA, s.markB ) + 1e-9,
		         "the sweep left the range between its marks" );
	}

	//--- A pull lands exactly on a mark, and alternates -------------------
	{
		RackSettings s;
		s.mode          = RackMode::Pull;
		s.markA         = 0.85;
		s.markB         = 0.15;
		s.sync          = Sync::Bar;
		s.travelSeconds = 0.5;

		Rack rack;
		rack.Configure( s );

		int landedOnA = 0;
		int landedOnB = 0;
		bool inRange  = true;

		for( int i = 0; i < kFramesPerBar * 8; ++i )
		{
			rack.Update( i * kDt, barsAt( i * kDt ), kDt, false );

			const double x = rack.Focus();
			if( x < std::min( s.markA, s.markB ) - 1e-9 || x > std::max( s.markA, s.markB ) + 1e-9 )
				inRange = false;

			//A bar is 2 s and a move is 0.5 s, so the frame just before each bar
			//line is always settled on a mark.
			if( i > kFramesPerBar && ( i + 1 ) % kFramesPerBar == 0 )
			{
				if( std::fabs( x - s.markA ) < 1e-9 )
					++landedOnA;
				else if( std::fabs( x - s.markB ) < 1e-9 )
					++landedOnB;
			}
		}

		std::printf( "  pull:   settled on A %d times, on B %d times\n", landedOnA, landedOnB );

		f.check( inRange, "a pull left the range between its marks" );
		f.check( landedOnA > 0 && landedOnB > 0, "a pull did not alternate between its two marks" );
		f.check( landedOnA + landedOnB == 7, "a pull was still moving when the next bar line arrived" );
	}

	//--- Stutter is reproducible, and stays between the marks -------------
	{
		RackSettings s;
		s.mode          = RackMode::Stutter;
		s.markA         = 0.9;
		s.markB         = 0.2;
		s.sync          = Sync::Eighth;
		s.travelSeconds = 0.1;

		std::vector< double > first;
		std::vector< double > second;
		for( int run = 0; run < 2; ++run )
		{
			Rack rack;
			rack.Configure( s );
			auto& into = run == 0 ? first : second;
			for( int i = 0; i < kFramesPerBar * 4; ++i )
			{
				rack.Update( i * kDt, barsAt( i * kDt ), kDt, false );
				into.push_back( rack.Focus() );
			}
		}

		bool identical = first == second;
		bool inRange   = true;
		double spread  = 0.0;
		for( double x : first )
		{
			if( x < s.markB - 1e-9 || x > s.markA + 1e-9 )
				inRange = false;
			spread = std::max( spread, std::fabs( x - first.front() ) );
		}

		std::printf( "  stutter: reproducible=%s, spread %.4f\n", identical ? "yes" : "NO", spread );

		f.check( identical, "Stutter is not reproducible -- a saved composition would not play back twice" );
		f.check( inRange, "Stutter left the range between its marks" );
		f.check( spread > 0.05, "Stutter never actually moved" );
	}

	//--- A hand cue advances the sequence without moving the grid ---------
	{
		RackSettings s;
		s.mode          = RackMode::Pull;
		s.markA         = 0.8;
		s.markB         = 0.2;
		s.sync          = Sync::Manual;
		s.travelSeconds = 0.2;

		Rack rack;
		rack.Configure( s );

		int moves = 0;
		double previous = -1.0;
		for( int i = 0; i < 600; ++i )
		{
			//A cue every second.
			const bool cue = ( i % 60 ) == 0 && i > 0;
			rack.Update( i * kDt, barsAt( i * kDt ), kDt, cue );

			if( previous >= 0.0 && std::fabs( rack.Focus() - previous ) > 1e-9 )
				++moves;
			previous = rack.Focus();
		}

		std::printf( "  manual: %d frames moved across 9 hand cues\n", moves );
		f.check( moves > 0, "a hand cue did nothing in Manual sync" );
	}

	if( f.count == 0 )
		std::printf( "  the focus puller holds every property it claims\n" );

	return f.count;
}

//---------------------------------------------------------------------------
// --rattle: the rig, against the properties it claims.
//
// The one that matters most is the last: no input at all may make an
// oscillator run away, because this number multiplies a blur radius and a
// frame offset, live, in front of people.
//---------------------------------------------------------------------------
int runRattleTest()
{
	Failures f;

	constexpr double kDt = 1.0 / 60.0;

	//--- Drive at zero is exactly still ----------------------------------
	{
		RattleSettings s;
		s.drive = 0.0;

		Rattle rig;
		rig.Configure( s );

		bool still  = true;
		int onsets  = 0;
		for( int i = 0; i < 2000; ++i )
		{
			const double env = 0.5 + 0.5 * std::sin( i * 0.7 );
			rig.Update( env, kDt );

			if( rig.Pan() != 0.0 || rig.Tilt() != 0.0 || rig.Roll() != 0.0 || rig.Knock() != 0.0 )
				still = false;
			if( rig.Fired() )
				++onsets;
		}

		std::printf( "  drive 0: still=%s, onsets still detected=%d\n", still ? "yes" : "NO", onsets );

		f.check( still, "Drive at zero is not an exact null" );
		f.check( onsets > 0,
		         "onset detection stopped with the drive down -- the rack's audio cue would die with it" );
	}

	//--- Nothing can make it run away ------------------------------------
	{
		RattleSettings s;
		s.drive     = 1.0;
		s.threshold = 0.0;
		s.frequency = 24.0;//the top of the control's range
		s.damping   = 0.02;//the bottom of it: the ringiest rig available

		Rattle rig;
		rig.Configure( s );

		double worst = 0.0;
		for( int i = 0; i < 20000; ++i )
		{
			//Adversarial: a full-scale onset on alternate frames, which is a
			//30 Hz square wave straight into a 24 Hz resonance.
			rig.Update( ( i & 1 ) ? 1.0 : 0.0, kDt );
			worst = std::max( { worst, std::fabs( rig.Pan() ), std::fabs( rig.Tilt() ),
			                    std::fabs( rig.Roll() ), std::fabs( rig.Knock() ) } );
		}

		std::printf( "  runaway: worst displacement over 20000 adversarial frames %.4f\n", worst );
		f.check( worst <= 1.0 + 1e-9, "the rig exceeded its own travel limit" );
		f.check( std::isfinite( worst ), "the rig diverged" );
	}

	//--- A long frame does not blow the integrator up ---------------------
	{
		RattleSettings s;
		s.drive     = 1.0;
		s.threshold = 0.0;
		s.frequency = 24.0;
		s.damping   = 0.02;

		Rattle rig;
		rig.Configure( s );

		double worst = 0.0;
		for( int i = 0; i < 2000; ++i )
		{
			//A second per frame: far past anything the clock would hand over,
			//which is exactly the point. Substepping has to hold anyway.
			rig.Update( ( i & 1 ) ? 1.0 : 0.0, 1.0 );
			worst = std::max( worst, std::fabs( rig.Pan() ) );
		}

		std::printf( "  long frames: worst %.4f\n", worst );
		f.check( std::isfinite( worst ) && worst <= 1.0 + 1e-9,
		         "an unclamped frame time made the integrator diverge" );
	}

	//--- One hit rings at the declared frequency and then settles ---------
	{
		RattleSettings s;
		s.drive     = 1.0;
		s.threshold = 0.05;
		s.frequency = 8.0;
		s.damping   = 0.05;

		Rattle rig;
		rig.Configure( s );

		//Silence, one full-scale onset, then silence.
		rig.Update( 0.0, kDt );
		rig.Update( 1.0, kDt );

		std::vector< double > trace;
		for( int i = 0; i < 240; ++i )
		{
			rig.Update( 0.0, kDt );
			trace.push_back( rig.Pan() );
		}

		//Zero crossings over the first second give the period directly. Pan is
		//axis 0, whose frequency multiplier is exactly 1.
		int crossings = 0;
		for( size_t i = 1; i < 60 && i < trace.size(); ++i )
			if( ( trace[ i - 1 ] < 0.0 ) != ( trace[ i ] < 0.0 ) )
				++crossings;

		const double measured = crossings / 2.0;//two crossings per cycle, over one second

		double peakEarly = 0.0;
		double peakLate  = 0.0;
		for( size_t i = 0; i < trace.size(); ++i )
			( i < 60 ? peakEarly : peakLate ) = std::max( i < 60 ? peakEarly : peakLate, std::fabs( trace[ i ] ) );

		std::printf( "  ring:   %.1f Hz measured against %.1f declared; peak %.4f then %.4f\n",
		             measured, s.frequency, peakEarly, peakLate );

		f.check( std::fabs( measured - s.frequency ) <= 1.0,
		         "the rig did not ring at the frequency it was told to" );
		f.check( peakEarly > 0.01, "one full-scale onset did not move the rig at all" );
		f.check( peakLate < peakEarly, "the ring did not decay" );
	}

	//--- Reproducible ----------------------------------------------------
	{
		RattleSettings s;
		s.drive     = 0.8;
		s.threshold = 0.1;
		s.frequency = 9.0;
		s.damping   = 0.2;

		std::vector< double > runs[ 2 ];
		for( int run = 0; run < 2; ++run )
		{
			Rattle rig;
			rig.Configure( s );
			for( int i = 0; i < 1200; ++i )
			{
				rig.Update( 0.5 + 0.5 * std::sin( i * 0.31 ), kDt );
				runs[ run ].push_back( rig.Pan() );
			}
		}

		f.check( runs[ 0 ] == runs[ 1 ], "the rig is not reproducible" );
	}

	if( f.count == 0 )
		std::printf( "  the rig holds every property it claims\n" );

	return f.count;
}

//---------------------------------------------------------------------------
// --presets: every factory preset survives every host behaviour.
//
// FFGL's host owns parameter state and may push it back down at any time, and
// nothing obliges it to act on the value events a plugin raises when it
// changes one itself. So there are three hosts to survive and the plugin cannot
// tell which it is talking to: one that honours the events and hands the new
// values straight back; one that ignores them and carries on restating what it
// still believes; and one that honours them but keeps its parameters shorter
// than a float, so what comes back is near the preset rather than equal to it.
//
// All three arrive as SetFloatParameter calls carrying a changed value, which
// is why "the value changed, so the operator has taken over" is the wrong test.
// It is the bug that reached a user in the sibling plugin; this is here so it
// cannot reach one from this plugin.
//
// No GL: this is the parameter plumbing, not the picture.
//---------------------------------------------------------------------------
int runPresetTest()
{
	using namespace gaffer::presets;

	// The display names of the parameters a preset covers, in presets::Param
	// order. Looked up through the plugin's own declaration rather than by
	// index, so a reordering cannot leave this quietly driving the wrong one.
	static const char* const kCoveredNames[ kParamCount ] = {
		"Focus", "Aperture", "Focal Length", "Blades", "Highlight", "Breathing",
		"Rack", "Mark B", "Speed", "Rate", "Sync", "Ease",
		"Drive", "Band", "Threshold", "Release", "Shake", "Roll",
		"Resonance", "Damping", "Defocus"
	};

	enum class Host
	{
		Honours,  //re-reads and hands the new values straight back
		Ignores,  //carries on restating the values it still believes in
		Quantises //honours the events, but through a 1/1000 UI
	};

	static const char* const kHostNames[] = { "honours", "ignores", "quantises" };

	int failures = 0;

	for( int hostIndex = 0; hostIndex < 3; ++hostIndex )
	{
		const Host host = Host( hostIndex );

		for( int p = 1; p <= kCount; ++p )
		{
			Gaffer plugin;

			const int presetId = indexOfParameter( plugin, "Preset" );
			if( presetId < 0 )
			{
				std::printf( "FAILED: no Preset parameter\n" );
				return 1;
			}

			//What the host believed before the preset was chosen: the defaults.
			float believed[ kParamCount ];
			for( int j = 0; j < kParamCount; ++j )
				believed[ j ] = getParameter( plugin, kCoveredNames[ j ] );

			plugin.SetFloatParameter( (unsigned int)presetId, float( p ) );

			//The host's next word, whatever kind of host it is.
			for( int j = 0; j < kParamCount; ++j )
			{
				const int id = indexOfParameter( plugin, kCoveredNames[ j ] );
				if( id < 0 )
				{
					std::printf( "FAILED: no parameter named '%s'\n", kCoveredNames[ j ] );
					return 1;
				}

				float sends = 0.0f;
				switch( host )
				{
					case Host::Honours: sends = plugin.GetFloatParameter( (unsigned int)id ); break;
					case Host::Ignores: sends = believed[ j ]; break;
					case Host::Quantises:
						sends = std::round( plugin.GetFloatParameter( (unsigned int)id ) * 1000.0f ) / 1000.0f;
						break;
				}

				plugin.SetFloatParameter( (unsigned int)id, sends );
			}

			const int stillActive = int( std::lround( plugin.GetFloatParameter( (unsigned int)presetId ) ) );
			if( stillActive != p )
			{
				std::printf( "FAILED: preset %d (%s) dropped to %d against a host that %s\n",
				             p, kPresets[ p - 1 ].name, stillActive, kHostNames[ hostIndex ] );
				++failures;
				continue;
			}

			//And it must still be RENDERING the preset's values, not merely
			//still showing its name in the dropdown.
			for( int j = 0; j < kParamCount; ++j )
			{
				const float rendering = getParameter( plugin, kCoveredNames[ j ] );
				if( std::fabs( rendering - kPresets[ p - 1 ].v[ j ] ) > 1e-6f )
				{
					std::printf( "FAILED: preset %d (%s) renders %s as %.4f, not %.4f, against a host that %s\n",
					             p, kPresets[ p - 1 ].name, kCoveredNames[ j ],
					             rendering, kPresets[ p - 1 ].v[ j ], kHostNames[ hostIndex ] );
					++failures;
					break;
				}
			}
		}
	}

	std::printf( "%d presets x 3 host behaviours: %s\n", kCount,
	             failures == 0 ? "all survive" : "FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// The GL-side measurements.
//---------------------------------------------------------------------------
struct Rig
{
	int width  = 0;
	int height = 0;
	GLuint source = 0;
	GLuint target = 0;
	GLuint fbo    = 0;

	/// A 32-bit float target for the measurements. Separate from the 8-bit one
	/// on purpose: the null test compares against an 8-bit input and has to be
	/// byte-exact, and the moment tests need the dim tail of a disc that 8 bits
	/// does not have.
	GLuint measureTarget = 0;
	GLuint measureFBO    = 0;
};

/// Put the plugin into the one configuration in which the lens's answer is a
/// single number: a uniform depth, no rack, no rattle, no breathing, and a
/// plain average rather than a highlight weighting.
void quietLens( Gaffer& plugin )
{
	setParameter( plugin, "Depth", 3.0f );      //Split H: picture and depth independent
	setParameter( plugin, "Depth Gain", 0.5f ); //1:1, so the depth half means what it says
	setParameter( plugin, "Falloff", 0.5f );    //gamma 1
	setParameter( plugin, "Smooth", 0.0f );
	setParameter( plugin, "Rack", 0.0f );       //Off
	setParameter( plugin, "Drive", 0.0f );      //the rig at rest
	setParameter( plugin, "Breathing", 0.0f );  //no scale, so the frame is where it was
	setParameter( plugin, "Highlight", 0.0f );  //the only setting that conserves light
	setParameter( plugin, "Blades", 0.0f );     //round
	setParameter( plugin, "Rotation", 0.0f );
	setParameter( plugin, "Edges", 2.0f );      //clamp
	setParameter( plugin, "Quality", 3.0f );    //extreme: the measurement is not about noise
}

int runBokehTest( Gaffer& plugin, const Rig& rig, double depthLevel, int blockPx, bool verbose )
{
	const float aperture = getParameter( plugin, "Aperture" );
	const float focusP   = getParameter( plugin, "Focus" );
	const float focalP   = getParameter( plugin, "Focal Length" );

	//--- what Lens.cpp says should happen ---------------------------------
	const double gain  = depthGainFromParam( getParameter( plugin, "Depth Gain" ) );
	const double gamma = gammaFromParam( getParameter( plugin, "Falloff" ) );
	const double d     = disparity( depthLevel, gamma, gain );
	const double phi   = phiFromParam( focalP );
	const double predictedHeights =
	    std::fabs( coc( d, focusP, apertureFromParam( aperture ), phi ) );
	const double predicted = predictedHeights * rig.height;

	//--- the pinhole, which is both the energy reference and the impulse's
	//    own second moment -------------------------------------------------
	setParameter( plugin, "Aperture", 0.0f );
	if( !runPass( plugin, rig.source, rig.measureFBO, rig.width, rig.height ) )
		return 1;
	const Moments pin = momentsOf( readBackFloat( rig.measureFBO, rig.width, rig.height ), rig.width, rig.height );

	//--- and the lens ------------------------------------------------------
	setParameter( plugin, "Aperture", aperture );
	if( !runPass( plugin, rig.source, rig.measureFBO, rig.width, rig.height ) )
		return 1;
	const Moments open = momentsOf( readBackFloat( rig.measureFBO, rig.width, rig.height ), rig.width, rig.height );

	if( pin.energy <= 0.0 )
	{
		std::printf( "  INCONCLUSIVE: the pinhole render has no light in it\n" );
		return 2;
	}

	//A convolution adds second moments, so the impulse's own width comes out
	//exactly rather than being tolerated. For a uniform disc of radius R the
	//variance along either axis is R^2/4, hence the factor of two.
	const double rx = 2.0 * std::sqrt( std::max( 0.0, open.xx - pin.xx ) );
	const double ry = 2.0 * std::sqrt( std::max( 0.0, open.yy - pin.yy ) );

	const double energyRatio = open.energy / pin.energy;

	if( verbose )
	{
		std::printf( "  moments: pin %.3f/%.3f  open %.3f/%.3f  (xx/yy, px^2)\n",
		             pin.xx, pin.yy, open.xx, open.yy );
		std::printf( "  depth %.3f -> disparity %.3f, focus %.3f, aperture %.3f, phi %.3f\n",
		             depthLevel, d, focusP, aperture, phi );
		std::printf( "  radius: predicted %.2f px, measured %.2f x %.2f px\n", predicted, rx, ry );
		std::printf( "  energy: %.4f of the pinhole\n", energyRatio );
	}

	Failures f;

	//The disc has to be big enough to be a disc and small enough to stay in
	//frame, or the numbers are about the frame edge rather than about the lens.
	if( predicted < 4.0 || predicted > rig.height * 0.35 )
	{
		std::printf( "  SKIP: predicted radius %.2f px is outside the measurable range\n", predicted );
		return 2;
	}

	const double tolerance = std::max( 1.5, predicted * 0.08 );
	f.check( std::fabs( rx - predicted ) <= tolerance, "the bokeh's width does not match the circle of confusion" );
	f.check( std::fabs( ry - predicted ) <= tolerance, "the bokeh's height does not match the circle of confusion" );
	f.check( std::fabs( rx - ry ) <= std::max( 1.5, predicted * 0.06 ),
	         "the bokeh is not round -- the aspect ratio is being applied wrongly" );
	f.check( std::fabs( energyRatio - 1.0 ) <= 0.06, "the lens created or destroyed light" );

	return f.count;
}

/// The bokeh's shape, which is what an aperture's blade count actually means.
///
/// Measured as an angular Fourier spectrum rather than as an outline. The
/// outline is the obvious approach and it does not work: a gather draws a point
/// of light as a few dozen individual taps, so the EDGE of a bokeh is a scatter
/// of blobs and walking out to find it measures the tap pattern. A Fourier
/// coefficient is an average over every pixel in the disc, and the tap pattern
/// -- a golden-angle spiral -- has no n-fold symmetry to contribute to it.
///
/// So: weight each pixel by its brightness and by r^2, which is where a shape's
/// angular information lives, and take the k-th harmonic. A round iris has
/// none of any order. An n-bladed one has a large one at k = n, and that is
/// exactly the statement "this bokeh is an n-gon".
struct Spectrum
{
	//Kept as real and imaginary parts rather than as magnitudes, because the
	//tap pattern is subtracted from the iris measurement below and a harmonic
	//is a vector. Subtracting magnitudes would leave whatever part of the two
	//happened to be out of phase, which reads as an octagon being less
	//eight-sided than a nonagon is nine-sided.
	double re[ 13 ] = {};
	double im[ 13 ] = {};

	double magnitude( int k ) const
	{
		return std::sqrt( re[ k ] * re[ k ] + im[ k ] * im[ k ] );
	}

	/// The magnitude of this spectrum's k-th harmonic once `baseline`'s has
	/// been taken out of it, as vectors.
	double excess( const Spectrum& baseline, int k ) const
	{
		const double dr = re[ k ] - baseline.re[ k ];
		const double di = im[ k ] - baseline.im[ k ];
		return std::sqrt( dr * dr + di * di );
	}

	int strongest = 0;
};

Spectrum measureIris( Gaffer& plugin, const Rig& rig, int blades )
{
	Spectrum out;

	//The Blades option is an index: 0 is Round, and 1..5 are 5..9 blades.
	setParameter( plugin, "Blades", float( blades == 0 ? 0 : blades - 4 ) );

	if( !runPass( plugin, rig.source, rig.measureFBO, rig.width, rig.height ) )
		return out;

	const std::vector< float > frame = readBackFloat( rig.measureFBO, rig.width, rig.height );
	const Moments m = momentsOf( frame, rig.width, rig.height );
	if( m.energy <= 0.0 )
		return out;

	double real[ 13 ] = {};
	double imag[ 13 ] = {};
	double total      = 0.0;

	for( int y = 0; y < rig.height; ++y )
	{
		for( int x = 0; x < rig.width; ++x )
		{
			const double v = lumaOf( &frame[ ( (size_t)y * rig.width + x ) * 4 ] );
			if( v <= 0.0 )
				continue;

			const double dx = x - m.cx;
			const double dy = y - m.cy;
			const double r2 = dx * dx + dy * dy;
			if( r2 <= 1.0 )
				continue;

			const double w     = v * r2;
			const double angle = std::atan2( dy, dx );

			total += w;
			for( int k = 1; k <= 12; ++k )
			{
				real[ k ] += w * std::cos( k * angle );
				imag[ k ] += w * std::sin( k * angle );
			}
		}
	}

	if( total <= 0.0 )
		return out;

	double best = 0.0;
	for( int k = 1; k <= 12; ++k )
	{
		out.re[ k ] = real[ k ] / total;
		out.im[ k ] = imag[ k ] / total;

		if( k >= 3 && out.magnitude( k ) > best )
		{
			best          = out.magnitude( k );
			out.strongest = k;
		}
	}

	return out;
}

int runIrisTest( Gaffer& plugin, const Rig& rig )
{
	Failures f;

	//The widest circle this plugin can make, because the shape of an iris is at
	//the rim and there has to be a rim to look at.
	setParameter( plugin, "Focus", 0.0f );
	setParameter( plugin, "Aperture", 1.0f );
	setParameter( plugin, "Focal Length", 0.0f );

	//The round iris first, as the BASELINE, and this is the part that makes the
	//measurement mean anything.
	//
	// A gather draws a point of light as a few dozen discrete taps, and the
	// r^2 weighting used here falls hardest on the rim -- which is the sparsest
	// part of that pattern. So the spectrum of a perfectly round bokeh is not
	// flat: the tap spiral leaves a signature of its own, and at this radius it
	// is worth as much as an eight-bladed iris is.
	//
	// The tap pattern is IDENTICAL for every blade count, so subtracting the
	// round measurement leaves exactly what the iris did and nothing else.
	// Without the subtraction, "8 blades" and "round" are indistinguishable and
	// the test would either pass everything or fail everything.
	const Spectrum round = measureIris( plugin, rig, 0 );

	std::printf( "  round:  tap-pattern baseline, strongest harmonic %d at %.4f\n",
	             round.strongest, round.magnitude( round.strongest ) );

	for( int blades : { 5, 6, 8, 9 } )
	{
		const Spectrum s = measureIris( plugin, rig, blades );

		const double signature = s.excess( round, blades );

		//The runner-up among harmonics that are NOT a multiple of the blade
		//count. A hexagon really is three-fold and two-fold symmetric as well,
		//so those are not evidence of anything having gone wrong.
		double rival = 0.0;
		int rivalAt  = 0;
		for( int k = 3; k <= 12; ++k )
		{
			if( k % blades == 0 )
				continue;
			const double excess = s.excess( round, k );
			if( excess > rival )
			{
				rival   = excess;
				rivalAt = k;
			}
		}

		std::printf( "  %d blades: %d-fold excess %.4f, largest unrelated %d-fold %.4f\n",
		             blades, blades, signature, rivalAt, rival );

		f.check( signature > 0.015, "an n-bladed iris left no n-fold signature in the bokeh" );
		f.check( signature > rival * 2.0,
		         "the bokeh's strongest symmetry is not the blade count" );
	}

	return f.count;
}

/// The two nulls, both of which have to be exact rather than close.
int runNullTest( Gaffer& plugin, const Rig& rig, const std::vector< unsigned char >& input )
{
	//The input builders return bottom-up buffers and readBack returns top-down,
	//so the comparison is against a flipped copy. Getting this wrong compares
	//row y with row height-1-y, which produces a large, plausible, CONSTANT
	//error that does not change when the effect does.
	std::vector< unsigned char > expected( input.size() );
	const size_t stride = (size_t)rig.width * 4;
	for( int y = 0; y < rig.height; ++y )
		std::memcpy( expected.data() + (size_t)y * stride,
		             input.data() + (size_t)( rig.height - 1 - y ) * stride, stride );

	auto differing = [ & ]( const std::vector< unsigned char >& got ) {
		size_t n = 0;
		for( size_t i = 0; i < got.size() && i < expected.size(); ++i )
			if( got[ i ] != expected[ i ] )
				++n;
		return n;
	};

	Failures f;

	setParameter( plugin, "Depth", 0.0f );     //Radial: no rect mapping at all
	setParameter( plugin, "Rack", 0.0f );
	setParameter( plugin, "Drive", 0.0f );
	setParameter( plugin, "Breathing", 0.0f );
	setParameter( plugin, "Edges", 2.0f );
	setParameter( plugin, "Quality", 3.0f );

	//--- the pinhole -------------------------------------------------------
	setParameter( plugin, "Aperture", 0.0f );
	if( !runPass( plugin, rig.source, rig.fbo, rig.width, rig.height ) )
		return 1;
	const size_t pinholeDiff = differing( readBack( rig.fbo, rig.width, rig.height ) );

	std::printf( "  pinhole: %zu of %zu bytes differ\n", pinholeDiff, expected.size() );
	f.check( pinholeDiff == 0, "the aperture-closed null is not exact" );

	//--- everything in focus ----------------------------------------------
	//
	// The stronger of the two, and the reason it is here: the gather actually
	// RUNS. The depth field is flattened, the focal plane is put on it, and
	// every tap's own circle of confusion is therefore zero -- so every tap is
	// rejected by the coverage rule and only the pixel's own surface survives.
	// An error in the coverage rule that let one stray tap through would be
	// invisible in a picture and is one byte here.
	setParameter( plugin, "Aperture", 0.6f );
	setParameter( plugin, "Depth Gain", 0.25f );//flat: the whole scene at one distance
	setParameter( plugin, "Focus", 0.5f );      //and the focal plane exactly on it
	if( !runPass( plugin, rig.source, rig.fbo, rig.width, rig.height ) )
		return 1;
	const size_t focusedDiff = differing( readBack( rig.fbo, rig.width, rig.height ) );

	std::printf( "  focused: %zu of %zu bytes differ, with the gather running\n",
	             focusedDiff, expected.size() );
	f.check( focusedDiff == 0, "a fully focused frame is not exactly the input" );

	return f.count;
}

void usage()
{
	std::printf(
		"gftest -- run gaffer offline, and measure what it did\n"
		"\n"
		"  Without a GPU:\n"
		"    --focus            the focus puller against the properties it claims\n"
		"    --rattle           the rig against the properties it claims\n"
		"    --presets          every factory preset survives every host behaviour\n"
		"\n"
		"  With one:\n"
		"    --bokeh            an impulse through the lens: radius, roundness, energy\n"
		"    --iris             the bokeh's rotational symmetry against the blade count\n"
		"    --null             aperture closed, and everything in focus, are the identity\n"
		"    --out PATH         render a picture (default /tmp/gaffer.png)\n"
		"\n"
		"  Shared:\n"
		"    --width N          output width (default 640)\n"
		"    --height N         output height (default 360)\n"
		"    --set \"Name=V\"     set a parameter by its display name, 0..1\n"
		"    --depth-level V    the level filling the depth half (default 0.75)\n"
		"    --block N          the impulse's size in texture pixels (default 20)\n"
		"    --impulse          render the impulse picture rather than the card\n"
		"    --frames N         advance the clock over N frames at 60 fps (default 1)\n"
		"    --audio            inject a synthetic kick-and-hat spectrum\n"
		"    --bpm B            the transport tempo to report (default 120)\n"
		"    --trace            print the focus and camera state of every frame\n"
		"    --list             print every parameter and its default, then exit\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outputPath = "/tmp/gaffer.png";
	int width              = 640;
	int height             = 360;
	double depthLevel      = 0.75;
	int blockPx            = 20;

	bool focusTest  = false;
	bool rattleTest = false;
	bool presetTest = false;
	bool bokehTest  = false;
	bool irisTest   = false;
	bool nullTest   = false;
	bool listOnly   = false;
	bool render     = false;
	bool impulse    = false;
	int frames      = 1;
	bool audio      = false;
	double bpm      = 120.0;
	bool trace      = false;

	std::vector< std::pair< std::string, float > > overrides;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		auto next             = [ & ]() -> std::string { return i + 1 < argc ? argv[ ++i ] : std::string(); };

		if( arg == "--out" )
		{
			outputPath = next();
			render     = true;
		}
		else if( arg == "--width" )
			width = std::atoi( next().c_str() );
		else if( arg == "--height" )
			height = std::atoi( next().c_str() );
		else if( arg == "--depth-level" )
			depthLevel = std::atof( next().c_str() );
		else if( arg == "--block" )
			blockPx = std::atoi( next().c_str() );
		else if( arg == "--focus" )
			focusTest = true;
		else if( arg == "--rattle" )
			rattleTest = true;
		else if( arg == "--presets" )
			presetTest = true;
		else if( arg == "--bokeh" )
			bokehTest = true;
		else if( arg == "--iris" )
			irisTest = true;
		else if( arg == "--null" )
			nullTest = true;
		else if( arg == "--frames" )
			frames = std::max( 1, std::atoi( next().c_str() ) );
		else if( arg == "--trace" )
			trace = true;
		else if( arg == "--audio" )
			audio = true;
		else if( arg == "--bpm" )
			bpm = std::atof( next().c_str() );
		else if( arg == "--impulse" )
			impulse = true;
		else if( arg == "--list" )
			listOnly = true;
		else if( arg == "--set" )
		{
			const std::string assignment = next();
			const size_t equals          = assignment.rfind( '=' );
			if( equals == std::string::npos )
			{
				std::fprintf( stderr, "gftest: --set wants Name=Value, got '%s'\n", assignment.c_str() );
				return 2;
			}
			overrides.emplace_back( assignment.substr( 0, equals ),
			                        std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
		}
		else if( arg == "--help" || arg == "-h" )
		{
			usage();
			return 0;
		}
		else
		{
			std::fprintf( stderr, "gftest: unknown argument '%s'\n", arg.c_str() );
			usage();
			return 2;
		}
	}

	// Before any GL, because none of these needs it -- and a self-test that
	// required a GPU would not run on a hosted CI box, which is where half of
	// this plugin most needs checking.
	if( focusTest || rattleTest || presetTest )
	{
		int failures = 0;
		if( focusTest )
			failures += runFocusTest();
		if( rattleTest )
			failures += runRattleTest();
		if( presetTest )
			failures += runPresetTest();
		return failures == 0 ? 0 : 1;
	}

	if( width <= 0 || height <= 0 )
	{
		std::fprintf( stderr, "gftest: width and height must both be positive\n" );
		return 2;
	}

	Gaffer plugin;

	//The harness renders as fast as the GPU allows, so the clock's
	//measurement -- host time against real elapsed time -- has nothing
	//meaningful to compare. Declare the unit instead of leaving it to be
	//inferred from a frame that took no time.
	plugin.SetClockScaleForTest( 1.0 );

	if( listOnly )
	{
		for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
			std::printf( "%2u  %-14s %.3f\n", i, plugin.GetParamName( i ), plugin.GetFloatParameter( i ) );
		return 0;
	}

	if( !bokehTest && !irisTest && !nullTest )
		render = true;

	//Defaults for the measurements, applied BEFORE the command line so a --set
	//can override any of them.
	if( bokehTest || irisTest )
		quietLens( plugin );

	for( const auto& override : overrides )
	{
		if( !setParameter( plugin, override.first, override.second ) )
		{
			std::fprintf( stderr, "gftest: no parameter named '%s' (try --list)\n", override.first.c_str() );
			return 2;
		}
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "gftest: could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	std::printf( "GL %s / %s\n", glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );

	FFGLViewportStruct viewport = { 0, 0, (FFUInt32)width, (FFUInt32)height };
	if( plugin.InitGL( &viewport ) != FF_SUCCESS )
	{
		std::fprintf( stderr, "gftest: InitGL failed -- see the diagnostics log\n" );
		return 1;
	}

	auto shutdown = [ & ]( int code ) -> int {
		plugin.DeInitGL();
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return code;
	};

	const std::vector< unsigned char > input =
	    ( bokehTest || irisTest || impulse ) ? buildSplitImpulse( width, height, depthLevel, blockPx )
	                              : buildCard( width, height );

	Rig rig;
	rig.width  = width;
	rig.height = height;
	rig.source = makeTexture( width, height, input.data() );
	rig.target = makeTexture( width, height, nullptr );
	rig.fbo    = makeFramebuffer( rig.target );

	rig.measureTarget = makeTexture( width, height, nullptr, GL_RGBA32F );
	rig.measureFBO    = makeFramebuffer( rig.measureTarget );

	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		std::fprintf( stderr, "gftest: incomplete framebuffer\n" );
		return shutdown( 1 );
	}

	if( nullTest )
		return shutdown( runNullTest( plugin, rig, input ) );

	if( bokehTest )
		return shutdown( runBokehTest( plugin, rig, depthLevel, blockPx, true ) );

	if( irisTest )
		return shutdown( runIrisTest( plugin, rig ) );

	if( render )
	{
		//Both halves of this plugin move, so a single frame at time zero cannot
		//tell a Sweep from a Stutter or a rig from a still one. --frames walks
		//the clock, the transport and the spectrum forward exactly as a host
		//would and keeps the last frame.
		const double dt = 1.0 / 60.0;
		for( int frame = 0; frame < frames; ++frame )
		{
			const double t = frame * dt;

			plugin.SetTime( t );

			if( bpm > 0.0 )
			{
				const double bars = t / ( 240.0 / bpm );
				plugin.SetBeatInfo( float( bpm ), float( bars - std::floor( bars ) ) );
			}

			if( audio )
				injectSpectrum( plugin, t );

			if( !runPass( plugin, rig.source, rig.fbo, width, height ) )
			{
				std::fprintf( stderr, "gftest: ProcessOpenGL failed\n" );
				return shutdown( 1 );
			}

			if( trace )
			{
				double focus, sx, sy, roll, scale;
				plugin.FrameState( focus, sx, sy, roll, scale );
				std::printf( "%4d t=%7.4f focus=%.4f shift=(%+.4f,%+.4f) roll=%+.4f scale=%.4f\n",
				             frame, t, focus, sx, sy, roll, scale );
			}
		}

		std::vector< unsigned char > frame = readBack( rig.fbo, width, height );

		//Composite on black so a transparent edge mode does not come out as an
		//invisible PNG.
		for( size_t i = 0; i + 3 < frame.size(); i += 4 )
			frame[ i + 3 ] = 255;

		if( !writePng( outputPath, width, height, frame ) )
		{
			std::fprintf( stderr, "gftest: could not write %s\n", outputPath.c_str() );
			return shutdown( 1 );
		}
		std::printf( "wrote %s\n", outputPath.c_str() );
	}

	return shutdown( 0 );
}
