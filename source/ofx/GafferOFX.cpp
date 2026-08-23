/// The OpenFX build of gaffer, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts.
///
/// Same effect as the FFGL build, and the model has one home: this file LINKS
/// Lens.cpp, Rack.cpp, Rattle.cpp and Controls.cpp rather than copying them.
/// What is mirrored here is the per-pixel machinery of Shaders.cpp -- the
/// camera transform, the depth field, the stratified gather, the coverage rule
/// and the three-layer composite -- because the GPU did that per fragment and
/// here it runs on the CPU. Edit the fragment shader's pixel machinery and edit
/// this too. The lens maths itself is edited once, in Lens.cpp.
///
/// ---------------------------------------------------------------- Two hosts
///
/// Two things FFGL supplies that OpenFX does not, and they are handled
/// differently on purpose:
///
/// **There is no transport.** FFGL hands over a tempo and a position within the
/// bar; OFX hands over a frame number and a frame rate. So the bar count is
/// computed from a Tempo control that exists only in this build, and the rack's
/// cue grid runs off that. Everything else about the rack is identical, and
/// `gftest --focus` measures the two evaluators against each other precisely
/// because they are now two.
///
/// **There is no audio.** No FFT buffer, no spectrum, nothing. Leaving the
/// whole Rattle group inert would be honest and would also make half the plugin
/// missing in Resolve, so this build drives the rig from a Kick control -- a
/// pulse on a chosen division of the same Tempo. It is a metronome rather than
/// a microphone, and the rig itself, the resonance, the damping and the travel
/// limit are the same code answering it.
///
/// **A host renders frames in any order.** So nothing here may accumulate
/// between renders. Off, Pull, Sweep and Stutter are derived from the grid and
/// answered exactly by Rack::EvaluateStateless. Follow and the rig are
/// genuinely stateful, so both are simulated forward over a bounded look-back
/// window ending at the frame being rendered -- deterministic, scrub-safe, and
/// costing a few hundred arithmetic steps per frame rather than per pixel.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Controls.h"
#include "../Lens.h"
#include "../Presets.h"
#include "../Rack.h"
#include "../Rattle.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.gaffer";
constexpr const char* kPluginName       = "gaffer";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"A simulated lens on a depth map, with the music holding it.\n\n"
	"One focal plane is sharp and everything else is a disc, sized by how far "
	"it is from that plane. The depth can be invented (Radial, which works on "
	"any clip), read out of the picture's own luma or alpha, or carried in the "
	"other half of a side-by-side or over-under frame -- which is the only one "
	"of the five that can hold a real depth pass at full resolution.\n\n"
	"The focus does not have to sit still. Follow lags the Focus control the "
	"way a hand on a follow focus does; Pull racks between two marks on a cue; "
	"Sweep runs between them continuously, and fast enough it polls through "
	"every plane in the picture several times a second; Stutter picks a new "
	"plane on every cue.\n\n"
	"The rig shakes too. In this host there is no audio, so the Kick control "
	"drives it off the Tempo instead: a camera in front of a stack is a mass "
	"on a spring and answers at its own frequency rather than the drummer's.\n\n"
	"Aperture at zero is a pinhole, and a pinhole is the null.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamPreset    = "preset";
constexpr const char* kParamFocus     = "focus";
constexpr const char* kParamAperture  = "aperture";
constexpr const char* kParamFocal     = "focalLength";
constexpr const char* kParamBlades    = "blades";
constexpr const char* kParamRotation  = "rotation";
constexpr const char* kParamHighlight = "highlight";
constexpr const char* kParamBreathing = "breathing";
constexpr const char* kParamDepth     = "depth";
constexpr const char* kParamDepthGain = "depthGain";
constexpr const char* kParamFalloff   = "falloff";
constexpr const char* kParamSmooth    = "smooth";
constexpr const char* kParamRack      = "rack";
constexpr const char* kParamMarkB     = "markB";
constexpr const char* kParamSpeed     = "speed";
constexpr const char* kParamRate      = "rate";
constexpr const char* kParamSync      = "sync";
constexpr const char* kParamEase      = "ease";
constexpr const char* kParamTempo     = "tempo";
constexpr const char* kParamKick      = "kick";
constexpr const char* kParamDrive     = "drive";
constexpr const char* kParamThreshold = "threshold";
constexpr const char* kParamRelease   = "release";
constexpr const char* kParamShake     = "shake";
constexpr const char* kParamRoll      = "roll";
constexpr const char* kParamResonance = "resonance";
constexpr const char* kParamDamping   = "damping";
constexpr const char* kParamDefocus   = "defocus";
constexpr const char* kParamEdges     = "edges";
constexpr const char* kParamQuality   = "quality";

enum class EdgeMode
{
	Transparent = 0,
	Black       = 1,
	Clamp       = 2,
	Mirror      = 3,
	Wrap        = 4
};

/// How often the Kick control pulses the rig, as a fraction of a bar. Index 0
/// is Off, which is what a clip with no rhythm in it wants.
constexpr double kKickBars[] = { 0.0, 1.0, 0.5, 0.25, 0.125 };

/// How far back the two stateful parts are simulated from. Long enough that a
/// Follow at the slowest Speed has settled and a rig at the lightest Damping
/// has rung out, and short enough that the cost is invisible.
constexpr double kLookBackSeconds = 4.0;

/// Everything the render needs, in the physical units Lens.h works in. Filled
/// once per frame from the 0..1 parameters through the same conversions the
/// FFGL build uses.
struct LensSettings
{
	double focus     = 0.5;
	double cocGain   = 0.0;
	double cocMax    = 0.0;

	double depthGain = 1.0;
	double gamma     = 1.0;
	double smooth    = 0.0;
	gaffer::DepthSource depth = gaffer::DepthSource::Radial;

	double colourRect[ 4 ] = { 0.0, 0.0, 1.0, 1.0 };
	double depthRect[ 4 ]  = { 0.0, 0.0, 1.0, 1.0 };

	int blades       = 0;
	double bladeRot  = 0.0;
	/// The power-mean exponent, exactly 1 for a plain average. See Lens.h.
	double highlight = 1.0;

	double shiftX = 0.0;
	double shiftY = 0.0;
	double roll   = 0.0;
	double scale  = 1.0;

	EdgeMode edges = EdgeMode::Clamp;
	int taps       = 32;
};

/// GLSL mod(): x - y*floor(x/y), correct for negatives, which std::fmod is not.
inline double glslMod( double x, double y )
{
	return x - y * std::floor( x / y );
}

inline double mirrorCoord( double x )
{
	const double m = glslMod( x, 2.0 );
	return ( m > 1.0 ) ? ( 2.0 - m ) : m;
}

constexpr double kPi          = 3.141592653589793;
constexpr double kGoldenAngle = 2.399963229728653;
constexpr double kOcclusionMargin = 0.01;

class GafferProcessorBase : public OFX::ImageProcessor
{
public:
	explicit GafferProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setup( OFX::Image* src, const LensSettings& settings, bool premultipliedValue )
	{
		srcImg        = src;
		lens          = settings;
		premultiplied = premultipliedValue;

		const OfxRectI b = src->getBounds();
		srcW             = b.x2 - b.x1;
		srcH             = b.y2 - b.y1;

		const double par = src->getPixelAspectRatio() > 0.0 ? src->getPixelAspectRatio() : 1.0;
		aspect           = double( srcW ) * par / double( srcH );
	}

protected:
	OFX::Image* srcImg = nullptr;
	LensSettings lens;
	bool premultiplied = false;
	int srcW           = 0;
	int srcH           = 0;
	double aspect      = 1.0;
};

template< class PIX, int nComponents, int maxValue >
class GafferProcessor : public GafferProcessorBase
{
public:
	explicit GafferProcessor( OFX::ImageEffect& effect ) :
		GafferProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const OfxRectI dstBounds = _dstImg->getBounds();
		const int dstW           = dstBounds.x2 - dstBounds.x1;
		const int dstH           = dstBounds.y2 - dstBounds.y1;
		const double invW        = 1.0 / double( dstW );
		const double invH        = 1.0 / double( dstH );

		//One output pixel, in frame-height units. The shader takes this from
		//dFdy of the varying; here it is simply the reciprocal of the height.
		const double pixelH = std::max( invH, 1e-6 );
		const double minR2  = ( pixelH * pixelH ) / kPi;

		const int n = std::max( lens.taps, 1 );

		const double seg   = lens.blades >= 3 ? ( 2.0 * kPi / double( lens.blades ) ) : 0.0;
		const double inrad = lens.blades >= 3 ? std::cos( kPi / double( lens.blades ) ) : 1.0;
		const double irisK =
		    lens.blades >= 3
		        ? std::sqrt( kPi / ( 0.5 * double( lens.blades ) * std::sin( 2.0 * kPi / double( lens.blades ) ) ) )
		        : 1.0;

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix = static_cast< PIX* >( _dstImg->getPixelAddress( window.x1, y ) );

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				const double ux = ( x - dstBounds.x1 + 0.5 ) * invW;
				const double uy = ( y - dstBounds.y1 + 0.5 ) * invH;

				double baseX, baseY;
				cameraPoint( ux, uy, baseX, baseY );

				double out[ 4 ];
				gather( baseX, baseY, pixelH, minR2, n, seg, inrad, irisK, out );

				double r = out[ 0 ];
				double g = out[ 1 ];
				double b = out[ 2 ];
				double a = out[ 3 ];

				// Samples were averaged premultiplied, which is the correct
				// filter at a transparent edge. Premultiplied output just keeps
				// the invariant rgb <= a; straight output unpremultiplies.
				if( premultiplied || nComponents == 3 )
				{
					r = std::min( r, a );
					g = std::min( g, a );
					b = std::min( b, a );
				}
				else if( a > 0.0 )
				{
					r /= a;
					g /= a;
					b /= a;
				}

				dstPix[ 0 ] = quantise( r );
				dstPix[ 1 ] = quantise( g );
				dstPix[ 2 ] = quantise( b );
				if( nComponents == 4 )
					dstPix[ 3 ] = quantise( a );
			}
		}
	}

private:
	/// Mirror of cameraPoint() in Shaders.cpp: shake, roll and focus breathing
	/// as one inverse transform of the point being shaded.
	void cameraPoint( double px, double py, double& outX, double& outY ) const
	{
		double qx = ( px - 0.5 ) * aspect;
		double qy = py - 0.5;

		const double c = std::cos( lens.roll );
		const double s = std::sin( lens.roll );

		const double rx = ( qx * c - qy * s ) * lens.scale + lens.shiftX;
		const double ry = ( qx * s + qy * c ) * lens.scale + lens.shiftY;

		outX = rx / aspect + 0.5;
		outY = ry + 0.5;
	}

	/// Mirror of main() in Shaders.cpp. Read the comments there before changing
	/// a weight here: every one of them is an area divided by an area.
	void gather( double baseX, double baseY, double pixelH, double minR2, int n,
	             double seg, double inrad, double irisK, double out[ 4 ] ) const
	{
		const double centreD   = disparityAt( baseX, baseY );
		const double centreCoc = std::fabs( lens.cocGain * ( centreD - lens.focus ) );

		const double reach = std::min(
		    std::max( centreCoc, std::fabs( lens.cocGain * ( 1.0 - lens.focus ) ) ), lens.cocMax );

		if( reach <= pixelH * 0.5 )
		{
			fetchPicture( baseX, baseY, out );
			return;
		}

		const double innerR = std::clamp( centreCoc, std::sqrt( minR2 ), reach );
		const bool split    = innerR < reach * 0.995;
		const int inTaps    = n / 2;
		const double innerA = kPi * innerR * innerR;
		const double outerA = kPi * ( reach * reach - innerR * innerR );

		double nearSum[ 4 ] = { 0.0, 0.0, 0.0, 0.0 };
		double nearW        = 0.0;
		double nearCov      = 0.0;

		double farSum[ 4 ] = { 0.0, 0.0, 0.0, 0.0 };
		double farW        = 0.0;

		double totalArea = 0.0;

		double centre[ 4 ];
		fetchPicture( baseX, baseY, centre );

		double sameSum[ 4 ];
		double sameW;
		{
			double c[ 4 ] = { centre[ 0 ], centre[ 1 ], centre[ 2 ], centre[ 3 ] };
			toHighlight( c );

			sameW = minR2 / std::max( centreCoc * centreCoc, minR2 );
			for( int k = 0; k < 4; ++k )
				sameSum[ k ] = c[ k ] * sameW;
		}

		//Zero has to be handled separately, and not as an optimisation: it is
		//the case where nothing reached this pixel but its own surface, and the
		//answer is then that surface EXACTLY. Round-tripping it through the
		//power mean would cost an occasional last bit, which is the difference
		//between a fully focused frame being byte-identical to its input and
		//merely looking like it.
		int accepted = 0;

		for( int i = 0; i < n; ++i )
		{
			const double fi  = double( i ) + 0.5;
			const double ang = fi * kGoldenAngle + lens.bladeRot;

			double shape = 1.0;
			if( lens.blades >= 3 )
			{
				//At ang + pi, not ang: the sample scatters into an iris-shaped
				//patch and this pixel sits at the offset from the sample TO
				//here. Even blade counts hide the difference; odd ones come out
				//upside down. See the note in Shaders.cpp.
				const double a = glslMod( ang + kPi - lens.bladeRot, seg ) - seg * 0.5;
				shape          = irisK * inrad / std::cos( a );
			}

			double rr;
			double area;
			if( !split )
			{
				rr   = std::sqrt( fi / double( n ) ) * reach;
				area = kPi * reach * reach / double( n );
			}
			else if( i < inTaps )
			{
				rr   = std::sqrt( ( double( i ) + 0.5 ) / double( inTaps ) ) * innerR;
				area = innerA / double( inTaps );
			}
			else
			{
				const double u = ( double( i - inTaps ) + 0.5 ) / double( n - inTaps );
				rr             = std::sqrt( innerR * innerR + u * ( reach * reach - innerR * innerR ) );
				area           = outerA / double( n - inTaps );
			}

			area *= shape * shape;
			totalArea += area;

			const double dist = rr * shape;
			const double dirX = std::cos( ang );
			const double dirY = std::sin( ang );

			const double sx = baseX + ( dirX / aspect ) * dist;
			const double sy = baseY + dirY * dist;

			const double d   = disparityAt( sx, sy );
			const double rad = std::fabs( lens.cocGain * ( d - lens.focus ) );

			const double cov = std::clamp( ( rad * shape - dist ) / pixelH, 0.0, 1.0 );
			if( cov <= 0.0 )
				continue;

			double c[ 4 ];
			fetchPicture( sx, sy, c );

			const double w = cov * area / ( kPi * std::max( rad * rad, minR2 ) );

			toHighlight( c );
			++accepted;

			if( d > centreD + kOcclusionMargin )
			{
				for( int k = 0; k < 4; ++k )
					nearSum[ k ] += c[ k ] * w;
				nearW += w;
				nearCov += cov * area;
			}
			else if( d < centreD - kOcclusionMargin )
			{
				for( int k = 0; k < 4; ++k )
					farSum[ k ] += c[ k ] * w;
				farW += w;
			}
			else
			{
				for( int k = 0; k < 4; ++k )
					sameSum[ k ] += c[ k ] * w;
				sameW += w;
			}
		}

		if( accepted == 0 )
		{
			for( int k = 0; k < 4; ++k )
				out[ k ] = centre[ k ];
			return;
		}

		double same[ 4 ] = { 0.0, 0.0, 0.0, 0.0 };
		if( sameW > 0.0 )
			for( int k = 0; k < 4; ++k )
				same[ k ] = sameSum[ k ] / sameW;

		const double opacity = std::clamp( sameW, 0.0, 1.0 );
		for( int k = 0; k < 4; ++k )
		{
			const double behind = farW > 0.0 ? farSum[ k ] / farW : same[ k ];
			out[ k ]            = behind + ( same[ k ] - behind ) * opacity;
		}

		if( nearW > 0.0 && totalArea > 0.0 )
		{
			const double alpha = std::clamp( nearCov / totalArea, 0.0, 1.0 );
			for( int k = 0; k < 4; ++k )
				out[ k ] += ( nearSum[ k ] / nearW - out[ k ] ) * alpha;
		}

		fromHighlight( out );
	}

	/// The highlight transform and its inverse: a power mean, applied to the
	/// colour and never to the alpha. Mirror of toHighlight/fromHighlight in
	/// Shaders.cpp -- and, like them, exactly the identity at 1.
	void toHighlight( double c[ 4 ] ) const
	{
		if( lens.highlight == 1.0 )
			return;
		for( int k = 0; k < 3; ++k )
			c[ k ] = std::pow( std::max( c[ k ], 0.0 ), lens.highlight );
	}

	void fromHighlight( double c[ 4 ] ) const
	{
		if( lens.highlight == 1.0 )
			return;
		for( int k = 0; k < 3; ++k )
			c[ k ] = std::pow( std::max( c[ k ], 0.0 ), 1.0 / lens.highlight );
	}

	static double lumaOf( const double c[ 4 ] )
	{
		return c[ 0 ] * gaffer::kLumaR + c[ 1 ] * gaffer::kLumaG + c[ 2 ] * gaffer::kLumaB;
	}

	double rhoHat( double px, double py ) const
	{
		const double cx = ( px - 0.5 ) * aspect;
		const double cy = py - 0.5;
		return std::sqrt( cx * cx + cy * cy ) / ( 0.5 * std::sqrt( aspect * aspect + 1.0 ) );
	}

	double rawField( double px, double py ) const
	{
		if( lens.depth == gaffer::DepthSource::Radial )
			return 1.0 - std::clamp( rhoHat( px, py ), 0.0, 1.0 );

		//Deliberately NOT routed through the edge mode, exactly as in the
		//shader: off the frame the depth extends from the edge whatever the
		//picture is doing there.
		double texel[ 4 ];
		fetchRect( px, py, lens.depthRect, texel );

		if( lens.depth == gaffer::DepthSource::Alpha )
			return texel[ 3 ];

		return lumaOf( texel );
	}

	double fieldAt( double px, double py ) const
	{
		if( lens.smooth <= 0.0 )
			return rawField( px, py );

		const double r = lens.smooth;
		double sum     = 2.0 * rawField( px, py );
		sum += rawField( px + r, py + r );
		sum += rawField( px + r, py - r );
		sum += rawField( px - r, py + r );
		sum += rawField( px - r, py - r );
		return sum / 6.0;
	}

	double disparityAt( double px, double py ) const
	{
		const double shaped = std::pow( std::clamp( fieldAt( px, py ), 0.0, 1.0 ), lens.gamma );
		return std::clamp( 0.5 + lens.depthGain * ( shaped - 0.5 ), 0.0, 1.0 );
	}

	/// The picture, through the edge mode.
	void fetchPicture( double px, double py, double out[ 4 ] ) const
	{
		const bool outside = px < 0.0 || py < 0.0 || px > 1.0 || py > 1.0;

		switch( lens.edges )
		{
		case EdgeMode::Transparent:
			if( outside )
			{
				out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0;
				return;
			}
			break;
		case EdgeMode::Black:
			if( outside )
			{
				out[ 0 ] = out[ 1 ] = out[ 2 ] = 0.0;
				out[ 3 ]                       = 1.0;
				return;
			}
			break;
		case EdgeMode::Clamp:
			px = std::clamp( px, 0.0, 1.0 );
			py = std::clamp( py, 0.0, 1.0 );
			break;
		case EdgeMode::Mirror:
			px = mirrorCoord( px );
			py = mirrorCoord( py );
			break;
		case EdgeMode::Wrap:
			px = glslMod( px, 1.0 );
			py = glslMod( py, 1.0 );
			break;
		}

		fetchRect( px, py, lens.colourRect, out );
	}

	/// A bilinear tap inside one rectangle of the source, kept half a texel
	/// inside THAT rectangle rather than inside the image. In the Split modes
	/// the picture's inside edge is the depth map's outside edge, and a tap
	/// straddling it does not return a slightly wrong colour: it returns half a
	/// depth map.
	void fetchRect( double px, double py, const double rect[ 4 ], double out[ 4 ] ) const
	{
		const double u = rect[ 0 ] + std::clamp( px, 0.0, 1.0 ) * rect[ 2 ];
		const double v = rect[ 1 ] + std::clamp( py, 0.0, 1.0 ) * rect[ 3 ];

		const double loX = rect[ 0 ] * srcW + 0.5;
		const double hiX = ( rect[ 0 ] + rect[ 2 ] ) * srcW - 0.5;
		const double loY = rect[ 1 ] * srcH + 0.5;
		const double hiY = ( rect[ 1 ] + rect[ 3 ] ) * srcH - 0.5;

		double fx = std::clamp( u * srcW, loX, std::max( loX, hiX ) ) - 0.5;
		double fy = std::clamp( v * srcH, loY, std::max( loY, hiY ) ) - 0.5;

		fx = std::clamp( fx, 0.0, double( srcW - 1 ) );
		fy = std::clamp( fy, 0.0, double( srcH - 1 ) );

		const int x0    = int( fx );
		const int y0    = int( fy );
		const int x1    = std::min( x0 + 1, srcW - 1 );
		const int y1    = std::min( y0 + 1, srcH - 1 );
		const double tx = fx - x0;
		const double ty = fy - y0;

		double p00[ 4 ], p10[ 4 ], p01[ 4 ], p11[ 4 ];
		texel( x0, y0, p00 );
		texel( x1, y0, p10 );
		texel( x0, y1, p01 );
		texel( x1, y1, p11 );

		for( int c = 0; c < 4; ++c )
		{
			const double top    = p00[ c ] + ( p10[ c ] - p00[ c ] ) * tx;
			const double bottom = p01[ c ] + ( p11[ c ] - p01[ c ] ) * tx;
			out[ c ]            = top + ( bottom - top ) * ty;
		}
	}

	/// One texel, premultiplied RGBA in 0..1. Straight-alpha input is
	/// premultiplied here so the averaging above filters correctly -- and so a
	/// luma depth reading matches the FFGL build, where the host always hands
	/// over premultiplied pixels.
	void texel( int x, int y, double out[ 4 ] ) const
	{
		const OfxRectI b  = srcImg->getBounds();
		const PIX* srcPix = static_cast< const PIX* >( srcImg->getPixelAddress( b.x1 + x, b.y1 + y ) );
		if( !srcPix )
		{
			out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0;
			return;
		}

		out[ 0 ] = srcPix[ 0 ] / double( maxValue );
		out[ 1 ] = srcPix[ 1 ] / double( maxValue );
		out[ 2 ] = srcPix[ 2 ] / double( maxValue );
		out[ 3 ] = nComponents == 4 ? srcPix[ 3 ] / double( maxValue ) : 1.0;

		if( !premultiplied && nComponents == 4 )
		{
			out[ 0 ] *= out[ 3 ];
			out[ 1 ] *= out[ 3 ];
			out[ 2 ] *= out[ 3 ];
		}
	}

	static PIX quantise( double v )
	{
		if( maxValue == 1 )
			return PIX( v );

		v = std::clamp( v, 0.0, 1.0 );
		return PIX( v * maxValue + 0.5 );
	}
};

/// The synthetic drive, standing in for a spectrum this host does not have.
///
/// A decaying pulse on the chosen division of the Tempo. Analytic rather than
/// integrated, so the rig's response does not depend on how finely the
/// look-back happens to be stepped -- the onset detector reads the envelope's
/// true derivative either way.
double kickEnvelope( double seconds, double barSeconds, int kick, double releaseSeconds )
{
	if( kick <= 0 || kick >= int( sizeof( kKickBars ) / sizeof( kKickBars[ 0 ] ) ) )
		return 0.0;

	const double period = std::max( kKickBars[ kick ] * barSeconds, 1e-3 );
	const double since  = seconds - period * std::floor( seconds / period );

	return std::exp( -since / std::max( releaseSeconds, 1e-3 ) );
}

class GafferPlugin : public OFX::ImageEffect
{
public:
	explicit GafferPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		preset    = fetchChoiceParam( kParamPreset );
		focus     = fetchDoubleParam( kParamFocus );
		aperture  = fetchDoubleParam( kParamAperture );
		focal     = fetchDoubleParam( kParamFocal );
		blades    = fetchChoiceParam( kParamBlades );
		rotation  = fetchDoubleParam( kParamRotation );
		highlight = fetchDoubleParam( kParamHighlight );
		breathing = fetchDoubleParam( kParamBreathing );
		depth     = fetchChoiceParam( kParamDepth );
		depthGain = fetchDoubleParam( kParamDepthGain );
		falloff   = fetchDoubleParam( kParamFalloff );
		smooth    = fetchDoubleParam( kParamSmooth );
		rack      = fetchChoiceParam( kParamRack );
		markB     = fetchDoubleParam( kParamMarkB );
		speed     = fetchDoubleParam( kParamSpeed );
		rate      = fetchDoubleParam( kParamRate );
		sync      = fetchChoiceParam( kParamSync );
		ease      = fetchDoubleParam( kParamEase );
		tempo     = fetchDoubleParam( kParamTempo );
		kick      = fetchChoiceParam( kParamKick );
		drive     = fetchDoubleParam( kParamDrive );
		threshold = fetchDoubleParam( kParamThreshold );
		release   = fetchDoubleParam( kParamRelease );
		shake     = fetchDoubleParam( kParamShake );
		rollAmt   = fetchDoubleParam( kParamRoll );
		resonance = fetchDoubleParam( kParamResonance );
		damping   = fetchDoubleParam( kParamDamping );
		defocus   = fetchDoubleParam( kParamDefocus );
		edges     = fetchChoiceParam( kParamEdges );
		quality   = fetchChoiceParam( kParamQuality );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src( srcClip->fetchImage( args.time ) );

		const LensSettings settings = settingsAtTime( args.time );
		const bool premultiplied    = srcClip->getPreMultiplication() == OFX::eImagePreMultiplied;

		const OFX::BitDepthEnum bitDepth    = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		switch( bitDepth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? run< GafferProcessor< unsigned char, 4, 255 > >( args, dst.get(), src.get(), settings, premultiplied )
				: run< GafferProcessor< unsigned char, 3, 255 > >( args, dst.get(), src.get(), settings, premultiplied );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? run< GafferProcessor< unsigned short, 4, 65535 > >( args, dst.get(), src.get(), settings, premultiplied )
				: run< GafferProcessor< unsigned short, 3, 65535 > >( args, dst.get(), src.get(), settings, premultiplied );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? run< GafferProcessor< float, 4, 1 > >( args, dst.get(), src.get(), settings, premultiplied )
				: run< GafferProcessor< float, 3, 1 > >( args, dst.get(), src.get(), settings, premultiplied );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;

		using namespace gaffer::presets;

		if( paramName == kParamPreset )
		{
			int chosen = 0;
			preset->getValue( chosen );
			if( chosen <= 0 || chosen > kCount || applyingPreset )
				return;

			// The copy IS the preset -- same table as the FFGL build, same 0..1
			// space. One edit block so undo takes the whole preset back at once.
			const Preset& p = kPresets[ chosen - 1 ];
			applyingPreset  = true;
			beginEditBlock( "Preset" );
			for( int i = 0; i < kParamCount; ++i )
				setIfChanged( i, p.v[ i ] );
			endEditBlock();
			applyingPreset = false;
			return;
		}

		// Editing a covered control while a preset is active hands control back
		// to the sliders. Judged by value, not by the change reason: hosts are
		// not consistent about reasons, but "still equal to the preset" is
		// unambiguous and also absorbs the host echoing our own setValues.
		if( applyingPreset || args.reason == OFX::eChangeTime )
			return;

		int active = 0;
		preset->getValue( active );
		if( active <= 0 || active > kCount )
			return;

		const Preset& p = kPresets[ active - 1 ];
		for( int i = 0; i < kParamCount; ++i )
		{
			if( nameOf( i ) != paramName || !differs( i, p.v[ i ] ) )
				continue;

			applyingPreset = true;
			preset->setValue( 0 );
			applyingPreset = false;
			return;
		}
	}

	bool isIdentity( const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip, double& identityTime ) override
	{
		// A pinhole with the camera bolted down. Deliberately conservative:
		// breathing is only inert at the middle of the barrel, and the rig is
		// only inert with nothing driving it, so both are required to be off
		// outright rather than reasoned about.
		int kickValue = 0;
		kick->getValueAtTime( args.time, kickValue );

		const bool pinhole = aperture->getValueAtTime( args.time ) <= 0.0;
		const bool still   = breathing->getValueAtTime( args.time ) <= 0.0
		                   && ( kickValue == 0 || drive->getValueAtTime( args.time ) <= 0.0 );

		if( pinhole && still )
		{
			identityClip = srcClip;
			identityTime = args.time;
			return true;
		}
		return false;
	}

private:
	//-----------------------------------------------------------------------
	// The preset table is plain floats and its Param order is shared with the
	// FFGL build. These bind that order to this host's handles.
	//
	// kBand has no counterpart here: it chooses a slice of a spectrum, and this
	// host has no spectrum. It is skipped rather than mapped onto something
	// else, so a preset that leans on it simply does not set it.
	//-----------------------------------------------------------------------
	std::string nameOf( int param ) const
	{
		using namespace gaffer::presets;
		switch( param )
		{
		case kFocus: return kParamFocus;
		case kAperture: return kParamAperture;
		case kFocalLength: return kParamFocal;
		case kBlades: return kParamBlades;
		case kHighlight: return kParamHighlight;
		case kBreathing: return kParamBreathing;
		case kRack: return kParamRack;
		case kMarkB: return kParamMarkB;
		case kSpeed: return kParamSpeed;
		case kRate: return kParamRate;
		case kSync: return kParamSync;
		case kEase: return kParamEase;
		case kDrive: return kParamDrive;
		case kThreshold: return kParamThreshold;
		case kRelease: return kParamRelease;
		case kShake: return kParamShake;
		case kRoll: return kParamRoll;
		case kResonance: return kParamResonance;
		case kDamping: return kParamDamping;
		case kDefocus: return kParamDefocus;
		case kBand:
		default: return {};
		}
	}

	OFX::DoubleParam* doubleFor( int param ) const
	{
		using namespace gaffer::presets;
		switch( param )
		{
		case kFocus: return focus;
		case kAperture: return aperture;
		case kFocalLength: return focal;
		case kHighlight: return highlight;
		case kBreathing: return breathing;
		case kMarkB: return markB;
		case kSpeed: return speed;
		case kRate: return rate;
		case kEase: return ease;
		case kDrive: return drive;
		case kThreshold: return threshold;
		case kRelease: return release;
		case kShake: return shake;
		case kRoll: return rollAmt;
		case kResonance: return resonance;
		case kDamping: return damping;
		case kDefocus: return defocus;
		default: return nullptr;
		}
	}

	OFX::ChoiceParam* choiceFor( int param ) const
	{
		using namespace gaffer::presets;
		switch( param )
		{
		case kBlades: return blades;
		case kRack: return rack;
		case kSync: return sync;
		default: return nullptr;
		}
	}

	bool differs( int param, float v ) const
	{
		if( OFX::DoubleParam* p = doubleFor( param ) )
		{
			double current = 0.0;
			p->getValue( current );
			return std::fabs( current - double( v ) ) > 1e-4;
		}
		if( OFX::ChoiceParam* p = choiceFor( param ) )
		{
			int current = 0;
			p->getValue( current );
			return current != int( std::lround( v ) );
		}
		return false;
	}

	void setIfChanged( int param, float v )
	{
		if( !differs( param, v ) )
			return;

		if( OFX::DoubleParam* p = doubleFor( param ) )
			p->setValue( double( v ) );
		else if( OFX::ChoiceParam* p = choiceFor( param ) )
			p->setValue( int( std::lround( v ) ) );
	}

	//-----------------------------------------------------------------------
	LensSettings settingsAtTime( double t ) const
	{
		const double fps        = dstClip->getFrameRate() > 0.0 ? dstClip->getFrameRate() : 25.0;
		const double seconds    = t / fps;
		const double bpm        = std::clamp( tempo->getValueAtTime( t ), 20.0, 300.0 );
		const double barSeconds = 240.0 / bpm;//four beats to the bar
		const double bars       = seconds / barSeconds;

		int rackValue = 0, syncValue = 4, kickValue = 0;
		int depthValue = 0, bladesValue = 0, edgesValue = 2, qualityValue = 1;
		rack->getValueAtTime( t, rackValue );
		sync->getValueAtTime( t, syncValue );
		kick->getValueAtTime( t, kickValue );
		depth->getValueAtTime( t, depthValue );
		blades->getValueAtTime( t, bladesValue );
		edges->getValueAtTime( t, edgesValue );
		quality->getValueAtTime( t, qualityValue );

		gaffer::RackSettings rs;
		rs.mode          = gaffer::controls::Mode( float( rackValue ) );
		rs.markA         = focus->getValueAtTime( t );
		rs.markB         = markB->getValueAtTime( t );
		rs.travelSeconds = gaffer::controls::TravelSeconds( float( speed->getValueAtTime( t ) ) );
		rs.rateHz        = gaffer::controls::RateHz( float( rate->getValueAtTime( t ) ) );
		rs.sync          = gaffer::controls::SyncDivision( float( syncValue ) );
		rs.ease          = gaffer::controls::Ease( float( ease->getValueAtTime( t ) ) );

		const double releaseSeconds = gaffer::controls::AudioRelease( float( release->getValueAtTime( t ) ) );

		//--- the rig, simulated over a bounded look-back --------------------
		//
		// A damped oscillator is history, and this host will ask for frame 700
		// before frame 3. So it is run forward from a fixed distance back to
		// exactly here, every frame: deterministic, identical on a scrub and on
		// a playthrough, and a few hundred multiplies rather than a few hundred
		// per pixel.
		gaffer::RattleSettings rig;
		rig.drive     = drive->getValueAtTime( t );
		rig.threshold = gaffer::controls::Threshold( float( threshold->getValueAtTime( t ) ) );
		rig.frequency = gaffer::controls::ResonanceHz( float( resonance->getValueAtTime( t ) ) );
		rig.damping   = gaffer::controls::Damping( float( damping->getValueAtTime( t ) ) );

		gaffer::Rattle rattle;
		rattle.Configure( rig );

		{
			const double step  = 1.0 / 240.0;
			const double start = std::max( 0.0, seconds - kLookBackSeconds );
			const int steps    = int( ( seconds - start ) / step );

			for( int i = 0; i <= steps; ++i )
			{
				const double at = start + i * step;
				rattle.Update( kickEnvelope( at, barSeconds, kickValue, releaseSeconds ), step );
			}
		}

		//--- the focus puller -----------------------------------------------
		double focalPlane;
		if( rs.mode == gaffer::RackMode::Follow )
		{
			// The one mode that cannot be evaluated at an instant: it is
			// defined by where the Focus control has BEEN. Stepped at the frame
			// rate rather than finer, because the Focus animation has no
			// resolution beyond a frame anyway and every step here is a host
			// call.
			gaffer::Rack follower;
			const double step   = 1.0 / fps;
			const double startT = std::max( 0.0, t - kLookBackSeconds * fps );

			gaffer::RackSettings walk = rs;
			walk.markA                = focus->getValueAtTime( startT );

			follower.Configure( walk );
			follower.Reset( walk.markA );

			for( double at = startT; at <= t + 1e-9; at += 1.0 )
			{
				walk.markA = focus->getValueAtTime( at );
				follower.Configure( walk );
				follower.Update( at / fps, ( at / fps ) / barSeconds, step, false );
			}

			focalPlane = follower.Focus();
		}
		else
		{
			focalPlane = gaffer::Rack::EvaluateStateless( rs, seconds, bars, barSeconds );
		}

		const double knock = gaffer::controls::DefocusAmount( float( defocus->getValueAtTime( t ) ) );
		focalPlane         = std::clamp( focalPlane + rattle.Knock() * knock, 0.0, 1.0 );

		//--- what the render is told ----------------------------------------
		LensSettings s;

		const double ap  = gaffer::apertureFromParam( float( aperture->getValueAtTime( t ) ) );
		const double phi = gaffer::phiFromParam( float( focal->getValueAtTime( t ) ) );

		s.focus   = focalPlane;
		s.cocGain = gaffer::cocGain( focalPlane, ap, phi );
		s.cocMax  = gaffer::cocMax( focalPlane, ap, phi );

		s.depth     = gaffer::depthSourceFromParam( float( depthValue ) );
		s.depthGain = gaffer::depthGainFromParam( float( depthGain->getValueAtTime( t ) ) );
		s.gamma     = gaffer::gammaFromParam( float( falloff->getValueAtTime( t ) ) );
		s.smooth    = gaffer::depthSourceIsSampled( s.depth )
		                ? gaffer::smoothFromParam( float( smooth->getValueAtTime( t ) ) )
		                : 0.0;

		rectsFor( s.depth, s.colourRect, s.depthRect );

		s.blades    = gaffer::bladeCount( gaffer::bladesFromParam( float( bladesValue ) ) );
		s.bladeRot  = gaffer::rotationFromParam( float( rotation->getValueAtTime( t ) ) );
		s.highlight = gaffer::highlightFromParam( float( highlight->getValueAtTime( t ) ) );

		const double shakeAmt = gaffer::controls::ShakeAmount( float( shake->getValueAtTime( t ) ) );
		s.shiftX = rattle.Pan() * shakeAmt;
		s.shiftY = rattle.Tilt() * shakeAmt;
		s.roll   = rattle.Roll() * gaffer::controls::RollAmount( float( rollAmt->getValueAtTime( t ) ) );
		s.scale  = gaffer::breathScale( focalPlane, phi,
		                                gaffer::breathingFromParam( float( breathing->getValueAtTime( t ) ) ) );

		s.edges = EdgeMode( edgesValue );
		s.taps  = gaffer::tapsFromParam( float( qualityValue ) );

		return s;
	}

	/// Where the picture and the depth map sit inside the frame. The same table
	/// as the FFGL build's, and the same reason it exists: the Split modes.
	static void rectsFor( gaffer::DepthSource source, double colour[ 4 ], double depth[ 4 ] )
	{
		colour[ 0 ] = 0.0;
		colour[ 1 ] = 0.0;
		colour[ 2 ] = 1.0;
		colour[ 3 ] = 1.0;
		std::memcpy( depth, colour, sizeof( double ) * 4 );

		if( source == gaffer::DepthSource::SplitH )
		{
			colour[ 2 ] = 0.5;
			depth[ 0 ]  = 0.5;
			depth[ 2 ]  = 0.5;
		}
		else if( source == gaffer::DepthSource::SplitV )
		{
			//Picture on TOP. OFX rows run upward from the bottom of the bounds,
			//the same way FFGL's texture does, so the upper half is the upper
			//half of v in both builds -- get this the wrong way round and the
			//picture and the depth map swap places.
			colour[ 1 ] = 0.5;
			colour[ 3 ] = 0.5;
			depth[ 3 ]  = 0.5;
		}
	}

	template< class Processor >
	void run( const OFX::RenderArguments& args, OFX::Image* dst, OFX::Image* src,
	          const LensSettings& settings, bool premultiplied )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setup( src, settings, premultiplied );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::ChoiceParam* preset    = nullptr;
	OFX::DoubleParam* focus     = nullptr;
	OFX::DoubleParam* aperture  = nullptr;
	OFX::DoubleParam* focal     = nullptr;
	OFX::ChoiceParam* blades    = nullptr;
	OFX::DoubleParam* rotation  = nullptr;
	OFX::DoubleParam* highlight = nullptr;
	OFX::DoubleParam* breathing = nullptr;
	OFX::ChoiceParam* depth     = nullptr;
	OFX::DoubleParam* depthGain = nullptr;
	OFX::DoubleParam* falloff   = nullptr;
	OFX::DoubleParam* smooth    = nullptr;
	OFX::ChoiceParam* rack      = nullptr;
	OFX::DoubleParam* markB     = nullptr;
	OFX::DoubleParam* speed     = nullptr;
	OFX::DoubleParam* rate      = nullptr;
	OFX::ChoiceParam* sync      = nullptr;
	OFX::DoubleParam* ease      = nullptr;
	OFX::DoubleParam* tempo     = nullptr;
	OFX::ChoiceParam* kick      = nullptr;
	OFX::DoubleParam* drive     = nullptr;
	OFX::DoubleParam* threshold = nullptr;
	OFX::DoubleParam* release   = nullptr;
	OFX::DoubleParam* shake     = nullptr;
	OFX::DoubleParam* rollAmt   = nullptr;
	OFX::DoubleParam* resonance = nullptr;
	OFX::DoubleParam* damping   = nullptr;
	OFX::DoubleParam* defocus   = nullptr;
	OFX::ChoiceParam* edges     = nullptr;
	OFX::ChoiceParam* quality   = nullptr;

	/// True while our own setValues are in flight, so the resulting
	/// changedParam callbacks are not mistaken for the operator editing.
	bool applyingPreset = false;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                          const char* name, const char* label, const char* hint, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

OFX::ChoiceParamDescriptor* defineChoice( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                          const char* name, const char* label, const char* hint,
                                          std::initializer_list< const char* > options, int def )
{
	OFX::ChoiceParamDescriptor* p = desc.defineChoiceParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	for( const char* option : options )
		p->appendOption( option );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

} // namespace

mDeclarePluginFactory( GafferPluginFactory, {}, {} );

void GafferPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// This gathers from a disc around every pixel whose size it does not know
	// until it has read the depth, and in the Split modes it reads from the far
	// side of the frame, so it cannot render from tiles. Frames are still
	// independent of each other and of render order -- which is not free here
	// and is why nothing in this build accumulates between renders.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void GafferPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// Same parameters, same 0..1 ranges, same defaults as the FFGL build, so
	// the two inspectors read identically and one set of docs covers both.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Factory lenses and rigs. Picking one sets the controls it covers; "
	                      "editing any of them afterwards falls back to Custom." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < gaffer::presets::kCount; ++i )
		presetParam->appendOption( gaffer::presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	//--- Lens ---------------------------------------------------------------
	OFX::GroupParamDescriptor* lens = desc.defineGroupParam( "Lens" );
	lens->setLabels( "Lens", "Lens", "Lens" );

	defineSlider( desc, page, kParamFocus, "Focus",
	              "Which depth is sharp. 1 is the nearest surface, 0 the furthest. "
	              "With the Rack running this is mark A.",
	              0.55 )
		->setParent( *lens );
	defineSlider( desc, page, kParamAperture, "Aperture",
	              "How shallow the depth of field is. Zero is a pinhole, and a "
	              "pinhole is the null: the picture comes out untouched.",
	              0.35 )
		->setParent( *lens );
	defineSlider( desc, page, kParamFocal, "Focal Length",
	              "How much the lens cares where it is focused. At zero it is a long "
	              "lens a long way off: the depth of field and the framing do not "
	              "change as it racks. At the top it is close-focus, where both do.",
	              0.45 )
		->setParent( *lens );

	defineChoice( desc, page, kParamBlades, "Blades",
	              "The shape of the iris, which is the shape an out-of-focus highlight takes.",
	              { "Round", "5", "6", "7", "8", "9" }, 2 )
		->setParent( *lens );

	defineSlider( desc, page, kParamRotation, "Rotation",
	              "Which way the iris is pointing. Nothing to do on a round one.", 0.0 )
		->setParent( *lens );
	defineSlider( desc, page, kParamHighlight, "Highlight",
	              "How much an out-of-focus highlight blooms. Zero is a plain average, "
	              "which is the only setting that conserves light.",
	              0.30 )
		->setParent( *lens );
	defineSlider( desc, page, kParamBreathing, "Breathing",
	              "How much the frame creeps as the focus racks. Real lenses vary from "
	              "almost none to a lot; the physical amount is at 1.",
	              0.35 )
		->setParent( *lens );

	//--- Depth --------------------------------------------------------------
	OFX::GroupParamDescriptor* depthGroup = desc.defineGroupParam( "Depth" );
	depthGroup->setLabels( "Depth", "Depth", "Depth" );

	defineChoice( desc, page, kParamDepth, "Depth",
	              "Where the depth comes from. Radial invents it and works on any clip. "
	              "Luma and Alpha read it out of the picture's own channels. Split H and "
	              "Split V read it out of the other half of a double-width or "
	              "double-height frame, which is the only one of the five that can carry "
	              "a real depth pass.",
	              { "Radial", "Luma", "Alpha", "Split H", "Split V" }, 0 )
		->setParent( *depthGroup );

	defineSlider( desc, page, kParamDepthGain, "Depth Gain",
	              "How much depth the scene has, signed. A quarter of the way up is flat "
	              "-- one distance, one uniform blur -- and below that the field inverts, "
	              "which is what a depth map authored the other way up needs.",
	              0.50 )
		->setParent( *depthGroup );
	defineSlider( desc, page, kParamFalloff, "Falloff",
	              "Gamma on the depth field: where between the near and far ends most of "
	              "the scene sits. 0.5 is linear.",
	              0.50 )
		->setParent( *depthGroup );
	defineSlider( desc, page, kParamSmooth, "Smooth",
	              "Blurs the depth field, not the picture. Nothing in Radial, which is "
	              "already smooth; on a real depth map it is what stops a hard depth step "
	              "reading as a cut-out.",
	              0.25 )
		->setParent( *depthGroup );

	//--- Rack ---------------------------------------------------------------
	OFX::GroupParamDescriptor* rackGroup = desc.defineGroupParam( "Rack" );
	rackGroup->setLabels( "Rack", "Rack", "Rack" );

	defineChoice( desc, page, kParamRack, "Rack",
	              "What the focus is doing. Off leaves it on the Focus control. Follow "
	              "lags it the way a hand on a follow focus does. Pull racks between the "
	              "two marks on a cue. Sweep runs between them continuously. Stutter "
	              "picks a new plane on every cue.",
	              { "Off", "Follow", "Pull", "Sweep", "Stutter" }, 0 )
		->setParent( *rackGroup );

	defineSlider( desc, page, kParamMarkB, "Mark B",
	              "The other mark on the barrel. Focus is mark A.", 0.0 )
		->setParent( *rackGroup );
	defineSlider( desc, page, kParamSpeed, "Speed",
	              "How long a full-barrel move takes: 30 ms at the bottom, 5 seconds at "
	              "the top.",
	              0.50 )
		->setParent( *rackGroup );
	defineSlider( desc, page, kParamRate, "Rate",
	              "The cue rate when Sync is not counting bars, from a cue every twenty "
	              "seconds to twenty-four a second.",
	              0.50 )
		->setParent( *rackGroup );

	defineChoice( desc, page, kParamSync, "Sync",
	              "What the cues are counted in. Manual takes none at all in Pull and "
	              "Stutter, and runs free at Rate in Sweep.",
	              { "Manual", "Free", "4 Bars", "2 Bars", "Bar", "1/2", "1/4", "1/8", "1/16" }, 4 )
		->setParent( *rackGroup );

	defineSlider( desc, page, kParamEase, "Ease",
	              "0 is a motor and 1 is a hand. Real pulls are nearer 1.", 1.0 )
		->setParent( *rackGroup );

	// This host has no transport, so the tempo the bar divisions count against
	// has to be stated rather than received. The FFGL build has no equivalent
	// because Resolume sends one.
	OFX::DoubleParamDescriptor* tempoParam = desc.defineDoubleParam( kParamTempo );
	tempoParam->setLabels( "Tempo", "Tempo", "Tempo" );
	tempoParam->setHint( "Beats per minute for the bar divisions. This host has no "
	                     "transport to ask, so it is set here." );
	tempoParam->setRange( 20.0, 300.0 );
	tempoParam->setDisplayRange( 40.0, 200.0 );
	tempoParam->setDefault( 120.0 );
	tempoParam->setParent( *rackGroup );
	page->addChild( *tempoParam );

	//--- Rattle -------------------------------------------------------------
	OFX::GroupParamDescriptor* rattleGroup = desc.defineGroupParam( "Rattle" );
	rattleGroup->setLabels( "Rattle", "Rattle", "Rattle" );

	defineChoice( desc, page, kParamKick, "Kick",
	              "What hits the rig. There is no audio in this host, so it is a pulse on "
	              "a division of the Tempo rather than a microphone.",
	              { "Off", "Bar", "1/2", "1/4", "1/8" }, 0 )
		->setParent( *rattleGroup );

	defineSlider( desc, page, kParamDrive, "Drive",
	              "How hard each hit shakes the rig. Zero is an exact null: the camera "
	              "does not move at all.",
	              0.0 )
		->setParent( *rattleGroup );
	defineSlider( desc, page, kParamThreshold, "Threshold",
	              "How big a hit has to be to count. At the top, nothing does.", 0.15 )
		->setParent( *rattleGroup );
	defineSlider( desc, page, kParamRelease, "Release",
	              "How long a hit takes to die away.", 0.35 )
		->setParent( *rattleGroup );
	defineSlider( desc, page, kParamShake, "Shake",
	              "How far the frame moves at full deflection, as a fraction of its height.",
	              0.45 )
		->setParent( *rattleGroup );
	defineSlider( desc, page, kParamRoll, "Roll",
	              "How far the frame rotates at full deflection: about eight degrees at the top.",
	              0.30 )
		->setParent( *rattleGroup );
	defineSlider( desc, page, kParamResonance, "Resonance",
	              "The rig's own frequency, 2 to 24 Hz. A heavy tripod on a solid floor is "
	              "low; a light head on a hollow stage is high.",
	              0.50 )
		->setParent( *rattleGroup );
	defineSlider( desc, page, kParamDamping, "Damping",
	              "How quickly it settles. At the bottom one hit rings for whole bars.", 0.45 )
		->setParent( *rattleGroup );
	defineSlider( desc, page, kParamDefocus, "Defocus",
	              "How far a hit knocks the focal plane. The lens elements are part of the "
	              "rig too, so a loud enough bass takes the focus off the subject.",
	              0.45 )
		->setParent( *rattleGroup );

	//--- Output -------------------------------------------------------------
	OFX::GroupParamDescriptor* output = desc.defineGroupParam( "Output" );
	output->setLabels( "Output", "Output", "Output" );

	defineChoice( desc, page, kParamEdges, "Edges",
	              "What to show where a shake or a breath looks past the picture.",
	              { "Transparent", "Black", "Clamp", "Mirror", "Wrap" }, 2 )
		->setParent( *output );

	defineChoice( desc, page, kParamQuality, "Quality",
	              "How many samples the gather takes: 16, 32, 64 or 128. It does not change "
	              "the size of the blur, only how smoothly it is filled -- and a small "
	              "bright thing spread over a big disc is where the difference shows.",
	              { "Fast", "Good", "Best", "Extreme" }, 1 )
		->setParent( *output );

	// The Stoatworks About block: a read-only credit line and one push button
	// per link, in a group that starts folded. Last, so it sits under the
	// effect's own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* GafferPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new GafferPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static GafferPluginFactory* factory =
		new GafferPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
