#include "Shaders.h"

namespace gaffer
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through. The usual FFGL vertex shader folds MaxUV in here, but
	//this effect has to do its geometry in picture space and scale only at the
	//fetch -- and in the Split modes there are two different rectangles to
	//scale into, which a single varying could not carry anyway.
	uv = vUV;
}
)";

const char* const kFragmentShader = R"(#version 410 core

uniform sampler2D InputTexture;

uniform vec2 MaxUV;       //the part of the input texture that is really picture
uniform vec2 HalfTexel;   //half an input texel, in picture space
uniform float Aspect;     //output picture width / height

//Where the picture and the depth map live inside the picture. xy is the
//offset, zw the scale. Identical, and (0,0,1,1), for every mode except the two
//Split ones -- which is the whole reason the geometry is done in output space
//and mapped in at the fetch.
uniform vec4 ColourRect;
uniform vec4 DepthRect;

uniform float DepthMode;   //0 radial, 1 luma, 2 alpha, 3 split H, 4 split V
uniform float DepthGain;   //signed scale on the field about the middle
uniform float Gamma;       //redistributes the raw field
uniform float DepthSmooth; //blur radius for a sampled field, picture space

uniform float Focus;       //the focal plane, in disparity: 1 is the near plane
uniform float CocGain;     //circle of confusion per unit of disparity, height units
uniform float CocMax;      //the largest it can be here, and so the gather radius

uniform float Blades;      //0 round, else the number of aperture blades
uniform float BladeRot;    //iris angle, radians
uniform float Highlight;   //power-mean exponent; exactly 1 is a plain average

uniform vec2 Shift;        //camera shake, height units
uniform float Roll;        //camera roll, radians
uniform float Scale;       //focus breathing

uniform float EdgeMode;    //0 transparent, 1 black, 2 clamp, 3 mirror, 4 wrap
uniform float Taps;        //how many samples the gather takes

in vec2 uv;
out vec4 fragColor;

const vec3 kLumaWeights = vec3( 0.2126, 0.7152, 0.0722 );
const float kPi = 3.141592653589793;

//The golden angle. Successive samples land as far from every previous one as
//an angle can, so a spiral of any length is evenly spread -- which is what
//lets the Quality control change the tap count without changing the shape of
//the bokeh, only its smoothness.
const float kGoldenAngle = 2.399963229728653;

//A sample is "in front of" the pixel being shaded, and so allowed to occlude
//it, once it is this much nearer in disparity. Not zero: a smooth depth
//gradient would otherwise put half of a neighbourhood in each bucket purely on
//rounding, and the composite would shimmer along every gradient in the frame.
const float kOcclusionMargin = 0.01;

//---------------------------------------------------------------------------
// Fetching.
//---------------------------------------------------------------------------
float mirrorCoord( float x )
{
	//GLSL mod() is x - y*floor(x/y), so this is already correct for negatives.
	float m = mod( x, 2.0 );
	return ( m > 1.0 ) ? ( 2.0 - m ) : m;
}

/// Sample inside one rectangle of the input, never nearer than half a texel to
/// that rectangle's own edge.
///
/// The half-texel margin is measured against the RECT and not against the
/// texture. In the Split modes the picture's inside edge is the depth map's
/// outside edge, and a linear fetch straddling it does not return a slightly
/// wrong colour -- it returns half a depth map.
vec4 fetchRect( vec2 p, vec4 rect )
{
	vec2 lo = rect.xy + HalfTexel;
	vec2 hi = rect.xy + rect.zw - HalfTexel;

	vec2 t = clamp( rect.xy + clamp( p, vec2( 0.0 ), vec2( 1.0 ) ) * rect.zw, lo, hi );

	return texture( InputTexture, t * MaxUV );
}

/// The picture, through the edge mode. Off the frame this is what the operator
/// asked for; a shake or a breath routinely reads from outside.
vec4 fetchPicture( vec2 p )
{
	bool outside = any( lessThan( p, vec2( 0.0 ) ) ) || any( greaterThan( p, vec2( 1.0 ) ) );

	if( EdgeMode < 0.5 )
	{
		if( outside )
			return vec4( 0.0 );//transparent, and already premultiplied
	}
	else if( EdgeMode < 1.5 )
	{
		if( outside )
			return vec4( 0.0, 0.0, 0.0, 1.0 );//opaque black
	}
	else if( EdgeMode < 2.5 )
	{
		p = clamp( p, vec2( 0.0 ), vec2( 1.0 ) );
	}
	else if( EdgeMode < 3.5 )
	{
		p = vec2( mirrorCoord( p.x ), mirrorCoord( p.y ) );
	}
	else
	{
		p = fract( p );
	}

	return fetchRect( p, ColourRect );
}

//---------------------------------------------------------------------------
// The depth field. Mirror of Lens.cpp -- run `gftest --probe` after touching
// either copy.
//---------------------------------------------------------------------------
float rhoHat( vec2 p )
{
	vec2 c = p - vec2( 0.5 );
	c.x *= Aspect;

	//Half the frame diagonal in height units, so the invented field is circular
	//in pixels and the frame corner lands at exactly 1.
	return length( c ) / ( 0.5 * sqrt( Aspect * Aspect + 1.0 ) );
}

float rawField( vec2 p )
{
	if( DepthMode < 0.5 )
		return 1.0 - clamp( rhoHat( p ), 0.0, 1.0 );

	//Deliberately NOT routed through the edge mode. Off the frame the depth
	//extends from the edge whatever the picture is doing there, because
	//Transparent would read as alpha 0 -- "infinitely far" -- and put a ring of
	//maximum defocus just outside every frame.
	vec4 texel = fetchRect( p, DepthRect );

	if( DepthMode < 1.5 )
		return dot( texel.rgb, kLumaWeights );//Luma

	if( DepthMode < 2.5 )
		return texel.a;//Alpha

	//Split H and Split V: the other half of the frame is a depth map, and a
	//depth map is a greyscale picture. Read it as luma so a map written to one
	//channel, or to all three, both work.
	return dot( texel.rgb, kLumaWeights );
}

float fieldAt( vec2 p )
{
	if( DepthSmooth <= 0.0 )
		return rawField( p );

	//Crude on purpose: this is softening a control field, not the picture. What
	//it repairs is the one artefact a depth-driven blur has -- a hard step in
	//the map puts a hard step in the blur, which the eye reads as a cut-out
	//rather than as a lens.
	vec2 r = vec2( DepthSmooth );
	float sum = 2.0 * rawField( p );
	sum += rawField( p + vec2( r.x, r.y ) );
	sum += rawField( p + vec2( r.x, -r.y ) );
	sum += rawField( p + vec2( -r.x, r.y ) );
	sum += rawField( p + vec2( -r.x, -r.y ) );
	return sum / 6.0;
}

/// The field turned into the disparity actually used. Scaled about the MIDDLE
/// of the range, not about the focal plane: scaling about anything that moves
/// slides the whole field off the end as soon as the gain goes negative, the
/// field clamps to a constant, and a constant field is a scene at one distance
/// -- so the control appears to do nothing over half its travel.
float disparityAt( vec2 p )
{
	float shaped = pow( clamp( fieldAt( p ), 0.0, 1.0 ), Gamma );
	return clamp( 0.5 + DepthGain * ( shaped - 0.5 ), 0.0, 1.0 );
}

/// The highlight transform, and its inverse.
///
/// A power mean: raise the samples, average, take the root. That is how an
/// out-of-focus highlight is bought, and the obvious alternative -- weighting
/// each sample by its own brightness -- is a trap that was in here first.
///
/// Weighting normalises by the sum of the weights, so at a strong setting the
/// answer is whichever tap happened to be brightest. On high-contrast material
/// that is not a smooth disc, it is a picture of the sampling pattern, and it
/// does not improve with more taps because it is variance in the ESTIMATOR
/// rather than noise. A power mean leaves the area weights alone and transforms
/// the values, so it stays exactly as smooth as the plain average.
///
/// At exactly 1 both of these are the identity, which is what makes Highlight a
/// control that can be turned off rather than merely turned down.
vec3 toHighlight( vec3 c )
{
	return Highlight == 1.0 ? c : pow( max( c, vec3( 0.0 ) ), vec3( Highlight ) );
}

vec3 fromHighlight( vec3 c )
{
	return Highlight == 1.0 ? c : pow( max( c, vec3( 0.0 ) ), vec3( 1.0 / Highlight ) );
}

//---------------------------------------------------------------------------
// The camera body.
//
// Shake, roll and focus breathing are one inverse transform applied to the
// point being shaded, which is what turns them into a read from somewhere else
// rather than a second pass over the picture.
//---------------------------------------------------------------------------
vec2 cameraPoint( vec2 p )
{
	vec2 q = p - vec2( 0.5 );

	//Into height units, so the shake is the same distance in pixels and the
	//roll is a rotation rather than a shear, on any aspect ratio.
	q.x *= Aspect;

	float c = cos( Roll );
	float s = sin( Roll );

	q = vec2( q.x * c - q.y * s, q.x * s + q.y * c ) * Scale + Shift;

	q.x /= Aspect;
	return q + vec2( 0.5 );
}

//---------------------------------------------------------------------------
// The gather.
//
// Read this before changing any weight in it. Every one of them is an area
// divided by an area, and the whole thing falls apart the moment one of them is
// "tuned".
//
//   * A sample stands for some AREA of source. For the pixel's own surface that
//     is one pixel; for a tap drawn out of the gather region it is that
//     region's area divided by however many taps were drawn from it.
//   * A sample is SPREAD over the area of its own circle of confusion.
//   * So its contribution here is the first divided by the second. Nothing else
//     is a free parameter, and that is why the result does not change when the
//     Quality control does.
//
// Three layers, ordered by depth against the pixel's own surface, because a
// surface is opaque and a gather that ignores that dissolves a sharp subject
// into whatever is behind it:
//
//   NEAR   in front of this surface. Composited OVER everything, with an
//          opacity equal to how much of the gather region it covers.
//   SAME   this surface itself. Its accumulated weight IS its opacity: a
//          surface filling the neighbourhood sums to one and hides what is
//          behind it, and one that only clips the edge of it sums to less.
//   FAR    behind this surface, and therefore visible only through whatever
//          the SAME layer does not cover.
//---------------------------------------------------------------------------
void main()
{
	//The output pixel's size, from the rasteriser rather than from a resolution
	//uniform, so it is right whatever the host is rendering at. It has to
	//happen in uniform control flow, so it happens first.
	vec2 pixel   = vec2( dFdx( uv.x ), dFdy( uv.y ) );
	float pixelH = max( abs( pixel.y ), 1e-6 );

	vec2 base = cameraPoint( uv );

	//One pixel's area written as a radius squared, which is the floor on how
	//small a circle of confusion is allowed to be. A sample in perfect focus is
	//not spread over zero area; it is spread over its own pixel.
	float minR2 = ( pixelH * pixelH ) / kPi;

	float centreD   = disparityAt( base );
	float centreCoc = abs( CocGain * ( centreD - Focus ) );

	//How far the gather has to reach, and no further. Only two things can land
	//on this pixel: this surface, spread over its own circle of confusion, and
	//anything IN FRONT of it -- whose circle of confusion is at most the near
	//plane's. Sizing the loop by the largest circle any depth could produce
	//instead would spend most of its taps on a region nothing can reach from,
	//which is not a performance detail: it is where the samples that make the
	//bokeh smooth would have gone.
	//
	//What this gives up is honest and worth stating: a surface far BEHIND this
	//one, blurred much harder than this one is, reaches this pixel only as far
	//as this surface's own disc. Its outermost halo is clipped. That is the
	//price of one pass and no buffers.
	float R = min( max( centreCoc, abs( CocGain * ( 1.0 - Focus ) ) ), CocMax );

	//The pinhole. Aperture at zero makes CocGain zero, which makes this the
	//identity -- exactly, byte for byte, which is what the null test measures.
	if( R <= pixelH * 0.5 )
	{
		vec4 sharp = fetchPicture( base );
		sharp.rgb  = clamp( sharp.rgb, vec3( 0.0 ), vec3( sharp.a ) );
		fragColor  = sharp;
		return;
	}

	int n      = int( Taps + 0.5 );
	int blades = int( Blades + 0.5 );
	float seg  = blades >= 3 ? ( 2.0 * kPi / float( blades ) ) : 0.0;

	//The inradius of a regular n-gon whose circumradius is 1, and then a scale
	//that gives it the same AREA as the unit disc. The area normalisation is
	//not physics, it is manners: changing the blade count should change the
	//SHAPE of an out-of-focus highlight, not how much of the picture it covers.
	//Without it, choosing five blades quietly sharpens the whole frame.
	float inrad = blades >= 3 ? cos( kPi / float( blades ) ) : 1.0;
	float irisK = blades >= 3
	                  ? sqrt( kPi / ( 0.5 * float( blades ) * sin( 2.0 * kPi / float( blades ) ) ) )
	                  : 1.0;

	//Stratified in two: half the taps inside a disc the size of THIS pixel's
	//own blur, where nearly all of the answer is, and half over the rest of the
	//reach, where the foreground spill comes from. Each carries its own
	//stratum's area, so the estimator is unchanged -- only its variance is. The
	//alternative, spreading every tap over the whole reach, leaves a shallow
	//depth of field being sampled by a handful of taps and drawing a spiral
	//instead of a disc.
	float inner  = clamp( centreCoc, sqrt( minR2 ), R );
	bool split   = inner < R * 0.995;
	//`half` would be the obvious name and it is a GLSL RESERVED WORD -- along
	//with `smooth`, `flat`, `input`, `output`, `sample`, `filter`, `active` and
	//a long tail of others. The failure is nasty: the shader fails to compile
	//at RUNTIME, InitGL returns FF_FAIL, and Resolume shows an effect that
	//silently does nothing. source/Diag.cpp exists for exactly that morning,
	//and this line is the reason it earned its keep on day one.
	int inTaps     = n / 2;
	float innerA = kPi * inner * inner;
	float outerA = kPi * ( R * R - inner * inner );

	vec4 nearSum  = vec4( 0.0 );
	float nearW   = 0.0;
	float nearCov = 0.0;

	vec4 farSum = vec4( 0.0 );
	float farW  = 0.0;

	float totalArea = 0.0;

	//The pixel's own surface goes in first and unconditionally: it is one
	//source pixel, spread over its own circle of confusion. When that circle is
	//a pixel wide this weight is exactly 1 and the SAME layer is fully opaque,
	//which is what keeps a sharp subject sharp in front of a blurred
	//background -- and what makes a fully focused frame come back byte for byte
	//identical.
	vec4 centre = fetchPicture( base );

	vec4 sameSum;
	float sameW;
	{
		sameW   = minR2 / max( centreCoc * centreCoc, minR2 );
		sameSum = vec4( toHighlight( centre.rgb ), centre.a ) * sameW;
	}

	//How many loop taps were accepted. Zero has to be handled separately, and
	//not as an optimisation: it is the case where nothing reached this pixel but
	//its own surface, and the answer is then that surface EXACTLY. Going through
	//the power mean and back would round-trip the colour through two pow() calls
	//and cost an occasional last bit -- which is the difference between a
	//fully focused frame being byte-identical to its input and merely looking
	//like it.
	int accepted = 0;

	for( int i = 0; i < n; ++i )
	{
		float fi  = float( i ) + 0.5;
		float ang = fi * kGoldenAngle + BladeRot;

		float shape = 1.0;
		if( blades >= 3 )
		{
			//An n-gon's boundary radius: the inradius over the cosine of the
			//angle off the nearest edge's normal. Mapping the sampling pattern
			//onto it is what makes an out-of-focus highlight a picture of the
			//iris. The area element scales with the square of it, so the
			//sample's own area does too, or the corners would be over-counted.
			//
			//Evaluated at ang + pi, not at ang, and that is the whole thing:
			//the sample SCATTERS into an iris-shaped patch, and this pixel sits
			//at the offset from the sample TO here, which is the opposite
			//direction. For an even blade count the two agree and nothing shows
			//it; for an odd one the pentagon comes out upside down. The same
			//value has to be used for the coverage test below or the corners
			//are placed and then immediately rejected -- which is a hexagon
			//setting that silently renders a circle.
			float a = mod( ang + kPi - BladeRot, seg ) - seg * 0.5;
			shape   = irisK * inrad / cos( a );
		}

		//sqrt of a uniform variate, so the samples are spread evenly by AREA.
		//Without it they crowd the middle and every bokeh has a bright core.
		float rr;
		float area;
		if( !split )
		{
			rr   = sqrt( fi / float( n ) ) * R;
			area = kPi * R * R / float( n );
		}
		else if( i < inTaps )
		{
			rr   = sqrt( ( float( i ) + 0.5 ) / float( inTaps ) ) * inner;
			area = innerA / float( inTaps );
		}
		else
		{
			float u = ( float( i - inTaps ) + 0.5 ) / float( n - inTaps );
			rr      = sqrt( inner * inner + u * ( R * R - inner * inner ) );
			area    = outerA / float( n - inTaps );
		}

		area *= shape * shape;
		totalArea += area;

		float dist = rr * shape;
		vec2 dir   = vec2( cos( ang ), sin( ang ) );
		vec2 sp    = base + vec2( dir.x / Aspect, dir.y ) * dist;

		float d   = disparityAt( sp );
		float rad = abs( CocGain * ( d - Focus ) );

		//The scattering rule, asked from the receiving end: does THIS sample's
		//own circle of confusion reach this far? The one-pixel ramp is an
		//antialias on the disc's edge, not a fudge factor.
		//
		//It ramps from rad == dist OUTWARD, deliberately, rather than being
		//centred on it. Centred, a sample in perfect focus still lends a
		//fraction of itself to everything within half a pixel -- which is
		//defensible as resampling and is wrong as a lens, because it means a
		//frame with the focal plane exactly on the subject is not quite the
		//frame that went in. `gftest --null` measures that as bytes, and it is
		//the difference between 29,000 of them and none.
		//`rad * shape`, not `rad`: the sample's blur patch is the iris, not a
		//circle, and its boundary in this direction is that far out.
		float cov = clamp( ( rad * shape - dist ) / pixelH, 0.0, 1.0 );
		if( cov <= 0.0 )
			continue;

		vec4 c = fetchPicture( sp );

		//Area represented, over area spread across.
		float w = cov * area / ( kPi * max( rad * rad, minR2 ) );

		c.rgb = toHighlight( c.rgb );
		++accepted;

		if( d > centreD + kOcclusionMargin )
		{
			nearSum += c * w;
			nearW += w;
			nearCov += cov * area;
		}
		else if( d < centreD - kOcclusionMargin )
		{
			farSum += c * w;
			farW += w;
		}
		else
		{
			sameSum += c * w;
			sameW += w;
		}
	}

	if( accepted == 0 )
	{
		//Nothing but this pixel's own surface reached it. See the note above:
		//the answer is that surface, untouched.
		centre.rgb = clamp( centre.rgb, vec3( 0.0 ), vec3( centre.a ) );
		fragColor  = centre;
		return;
	}

	vec4 same = sameW > 0.0 ? sameSum / sameW : vec4( 0.0 );

	//The accumulated weight of this surface IS how much of this pixel it
	//covers, because every weight in it is an area over an area. A surface
	//filling the neighbourhood sums to one and hides everything behind it; one
	//that only clips the edge of the neighbourhood sums to less, and the
	//background shows through the difference. That is the whole of the
	//occlusion model and it needed no extra term.
	vec4 result = farW > 0.0 ? mix( farSum / farW, same, clamp( sameW, 0.0, 1.0 ) ) : same;

	if( nearW > 0.0 && totalArea > 0.0 )
	{
		//How much of the reach is covered by material in front of this surface.
		//That fraction IS the opacity: the same number a real scatter would
		//arrive at, counted by area instead of by compositing.
		float alpha = clamp( nearCov / totalArea, 0.0, 1.0 );
		result      = mix( result, nearSum / nearW, alpha );
	}

	result.rgb = fromHighlight( result.rgb );

	//Premultiplied in, premultiplied out. Averaging premultiplied samples is
	//the correct filter -- it is unpremultiplied averaging that goes wrong at a
	//transparent edge -- so there is nothing to undo, only the invariant the
	//engine expects to hold. The power mean can push a channel a hair past the
	//alpha it came in under, which is what this also catches.
	result.rgb = clamp( result.rgb, vec3( 0.0 ), vec3( result.a ) );

	fragColor = result;
}
)";

} // namespace gaffer
