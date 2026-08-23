/**
 * gaffer — browser demo.
 *
 * `VERTEX` and `FRAGMENT` are `kVertexShader` and `kFragmentShader` from
 * `source/Shaders.cpp`, copied across unedited by `demo/tools/sync_shaders.py`.
 * `demo/tools/check_shaders.py` compares them character for character and is
 * run by `tools/verify.sh`, because nothing else can: plugin.js cannot include
 * a C++ file.
 *
 * The conversions below are ports of `source/Lens.cpp`, `source/Controls.cpp`,
 * `source/Rack.cpp` and `source/Rattle.cpp` — the CPU half of the plugin, which
 * is what turns the host's 0..1 into the uniforms the shader wants. They are
 * ported rather than re-derived for the same reason the plugin keeps them in
 * one file each: the maths already exists twice on purpose (C++ because it has
 * to be readable and testable, GLSL because it has to run per pixel), and
 * `gftest --bokeh` is what keeps those two honest. A third, invented copy here
 * would have nothing checking it at all.
 *
 * **This page shows the OpenFX parameter set, not the FFGL one**, and that is
 * not a shortcut. A browser has no audio routed to it and no transport, exactly
 * as Resolve has none — so it gets the same two controls that build has,
 * `Tempo` and `Kick`, and the rig is driven by a pulse on a division of the
 * tempo rather than by a spectrum. Every other control is the same in both.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

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
`;

const FRAGMENT = `#version 410 core

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
// The depth field. Mirror of Lens.cpp -- run \`gftest --probe\` after touching
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
	//\`half\` would be the obvious name and it is a GLSL RESERVED WORD -- along
	//with \`smooth\`, \`flat\`, \`input\`, \`output\`, \`sample\`, \`filter\`, \`active\` and
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
		//frame that went in. \`gftest --null\` measures that as bytes, and it is
		//the difference between 29,000 of them and none.
		//\`rad * shape\`, not \`rad\`: the sample's blur patch is the iris, not a
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
`;

//---------------------------------------------------------------------------
// Ports of source/Lens.cpp
//---------------------------------------------------------------------------

const clamp01 = (v) => Math.min(1, Math.max(0, v));

/// Geometric interpolation: `t` of the way from lo to hi in RATIO rather than
/// in difference. What every control whose useful range spans more than one
/// order of magnitude wants.
const geometric = (v, lo, hi) => lo * Math.pow(hi / lo, clamp01(v));

/// The largest circle of confusion the plugin will produce, as a fraction of
/// the frame HEIGHT. Height, so the bokeh is round in pixels on any aspect.
const kMaxCoc = 0.12;

/// The upper bound on phi = f / z_near. Below 1 by construction, which is what
/// keeps 1 - phi*focus positive at every reachable setting and is why nothing
/// here needs a divide-by-zero guard.
const kMaxPhi = 0.8;

/// Squared. A linear aperture spends three quarters of its travel between
/// "unrecognisable" and "slightly more unrecognisable".
const apertureFromParam = (v) => clamp01(v) * clamp01(v);

const phiFromParam = (v) => kMaxPhi * clamp01(v);

/// The exponent of the power mean the gather averages with. **Exactly 1** at
/// zero, and the shader branches on that equality, so it has to be exact.
const highlightFromParam = (v) => 1 + 2 * clamp01(v);

const breathingFromParam = clamp01;
const rotationFromParam = (v) => 2 * Math.PI * clamp01(v);

/// -1 at the bottom, flat at a quarter, +1 in the middle, +3 at the top.
/// Asymmetric on purpose: inverting a field is a yes/no about how a depth map
/// was authored, so half the travel spent on -1..0 would be half a control
/// wasted.
const depthGainFromParam = (v) => {
  const t = clamp01(v);
  return t <= 0.5 ? -1 + 4 * t : 1 + 4 * (t - 0.5);
};

/// Geometric about 1: equal travel either side gives reciprocal gammas.
const gammaFromParam = (v) => Math.pow(4, 2 * clamp01(v) - 1);

/// Up to 2% of the frame. Beyond that the depth map stops describing the
/// picture it came from.
const smoothFromParam = (v) => 0.02 * clamp01(v);

const tapsFromParam = (v) => [16, 32, 64, 128][Math.min(3, Math.max(0, Math.round(v)))];

const bladeCount = (v) => [0, 5, 6, 7, 8, 9][Math.min(5, Math.max(0, Math.round(v)))];

/// The image distance, normalised so focusing at infinity gives exactly 1.
/// Both the blur gain and the frame scale are built on this — which is the one
/// idea in the whole lens: depth of field collapsing as you focus close and the
/// frame creeping as you rack are the same fact about where the sensor is.
const imageDistance = (focus, phi) => 1 / (1 - phi * clamp01(focus));

/// The blur gain, normalised by the image distance at the near end of the
/// barrel. What survives that normalisation is the part that is real: focused
/// at the near plane the depth of field is 1/(1-phi) times shallower than
/// focused at infinity, which is exactly what a lens does.
const cocGain = (focus, aperture, phi) =>
  (kMaxCoc * clamp01(aperture) * imageDistance(focus, phi)) / imageDistance(1, phi);

const cocMax = (focus, aperture, phi) => {
  const f = clamp01(focus);
  return cocGain(f, aperture, phi) * Math.max(f, 1 - f);
};

/// Against the MIDDLE of the barrel, not against infinity: an operator turning
/// Breathing up expects the framing to stay roughly where it was and creep
/// either way, not to jump the moment the control leaves zero.
const breathScale = (focus, phi, breathing) =>
  1 + clamp01(breathing) * (imageDistance(focus, phi) / imageDistance(0.5, phi) - 1);

//---------------------------------------------------------------------------
// Ports of source/Controls.cpp
//---------------------------------------------------------------------------

const travelSeconds = (v) => geometric(v, 0.03, 5.0);
const rateHz = (v) => geometric(v, 0.05, 24.0);
const audioRelease = (v) => geometric(v, 0.03, 1.5);
const resonanceHz = (v) => geometric(v, 2.0, 24.0);
const dampingFromParam = (v) => geometric(v, 0.02, 1.0);
const shakeAmount = (v) => 0.09 * clamp01(v) * clamp01(v);
const rollAmount = (v) => 0.14 * clamp01(v) * clamp01(v);
const defocusAmount = (v) => 0.5 * clamp01(v);

//---------------------------------------------------------------------------
// Port of source/Rack.cpp
//---------------------------------------------------------------------------

/// Bars per cue for each Sync division. Manual and Free are not counted in
/// bars and return 0.
const BARS_PER_CUE = [0, 0, 4, 2, 1, 0.5, 0.25, 0.125, 0.0625];

/// Smootherstep: zero velocity AND zero acceleration at both ends. A hand on a
/// follow focus does not start or stop instantly, and the ordinary smoothstep
/// still shows a kick at the ends when the move is fast.
function shape(t, ease) {
  const u = clamp01(t);
  const s = u * u * u * (u * (u * 6 - 15) + 10);
  return u + clamp01(ease) * (s - u);
}

/// splitmix64 in doubles, for the Stutter sequence — a hash and not a
/// generator, so the same cue index gives the same plane on every machine.
function hashUnit(index) {
  let x = BigInt(Math.trunc(index)) + 0x9e3779b97f4a7c15n;
  const M = (1n << 64n) - 1n;
  x = ((x ^ (x >> 30n)) * 0xbf58476d1ce4e5b9n) & M;
  x = ((x ^ (x >> 27n)) * 0x94d049bb133111ebn) & M;
  x = (x ^ (x >> 31n)) & M;
  return Number(x >> 11n) / Number(1n << 53n);
}

const stutterTarget = (a, b, index) => a + (b - a) * hashUnit(index);

/// Port of Rack::EvaluateStateless. Off, Pull, Sweep and Stutter are all
/// derived from the grid rather than accumulated, so all four can be answered
/// exactly at any instant — which is why the cue index is a floor() of the bar
/// count and not a counter. Follow is the exception and is integrated below.
function rackStateless(s, seconds, bars, barSeconds) {
  const a = clamp01(s.markA);
  const b = clamp01(s.markB);

  if (s.mode === 0 || s.mode === 1) return a;

  const period = BARS_PER_CUE[s.sync] || 0;
  const rate = Math.max(s.rateHz, 1e-4);

  const position = period > 0 ? bars / period : seconds * rate;
  const cueSeconds = period > 0 ? period * Math.max(barSeconds, 1e-6) : 1 / rate;

  if (s.mode === 3) {
    const f = position - Math.floor(position);
    return a + (b - a) * shape(1 - Math.abs(2 * f - 1), s.ease);
  }

  const whole = Math.floor(position);
  const elapsed = (position - whole) * cueSeconds;
  const t = elapsed / Math.max(s.travelSeconds, 1e-4);

  const odd = ((whole % 2) + 2) % 2 === 1;
  const from = s.mode === 4 ? stutterTarget(a, b, whole - 1) : odd ? a : b;
  const to = s.mode === 4 ? stutterTarget(a, b, whole) : odd ? b : a;

  return clamp01(from + (to - from) * shape(t, s.ease));
}

/// The one mode that cannot be evaluated at an instant: Follow is defined by
/// where the Focus control has BEEN. An exponential approach, then a hard cap
/// on how far the hand can travel in one frame — the exponential alone would
/// move a long way in the first frame of a big jump, which is exactly what a
/// follow focus cannot do.
function followStep(focus, target, dt, travel) {
  const maxStep = Math.max(dt, 0) / Math.max(travel, 1e-4);
  const diff = clamp01(target) - focus;

  if (Math.abs(diff) <= maxStep) return clamp01(target);

  const tau = Math.max(travel, 1e-4) * 0.35;
  const step = Math.max(-maxStep, Math.min(maxStep, diff * (1 - Math.exp(-Math.max(dt, 0) / tau))));
  return clamp01(focus + step);
}

//---------------------------------------------------------------------------
// Port of source/Rattle.cpp
//---------------------------------------------------------------------------

const TWO_PI = 2 * Math.PI;

/// Deliberately incommensurate. A rig is stiffer fore-and-aft than in pan, and
/// stiffer again in roll; if these matched, all four axes would peak on the
/// same hit and the camera would run back and forth along one line instead of
/// shaking. The lens is highest — the lightest thing in the assembly.
const AXIS_FREQ = [1.0, 1.31, 0.79, 1.73];
const AXIS_GAIN = [1.0, -0.72, 0.55, 0.9];

const SUB_STEP = 1 / 480;
const MAX_SUB_STEPS = 32;

class Rattle {
  constructor() {
    this.x = [0, 0, 0, 0];
    this.v = [0, 0, 0, 0];
    this.lastEnv = -1;
  }

  reset() {
    this.x = [0, 0, 0, 0];
    this.v = [0, 0, 0, 0];
    this.lastEnv = -1;
  }

  update(env, dt, cfg) {
    const level = clamp01(env);
    const step = Math.max(dt, 0);

    let onset = 0;

    // The force is the envelope's RISE, not its level. A held note does not
    // push a tripod over: the pressure it carries oscillates far above anything
    // a rig can follow, and what shakes a rig at frame rate is the arrival.
    if (this.lastEnv >= 0 && step > 0) {
      const rise = (level - this.lastEnv) / step;
      if (rise > 0) {
        const normalised = clamp01(rise / 60);
        const gate = clamp01(cfg.threshold);
        if (normalised > gate) onset = (normalised - gate) / Math.max(1 - gate, 1e-3);
      }
    }
    this.lastEnv = level;

    const drive = clamp01(cfg.drive);
    if (drive <= 0) {
      // The null, and it has to be exact: with the drive at zero the geometry
      // is the identity. Held at rest rather than scaled by zero, so turning it
      // back up starts from still.
      this.x = [0, 0, 0, 0];
      this.v = [0, 0, 0, 0];
      return;
    }

    const w = TWO_PI * Math.max(cfg.frequency, 0.1);
    const zeta = Math.min(1, Math.max(0.02, cfg.damping));

    // A hit is an impulse: a change in velocity delivered once, not a force
    // held for a frame. That is what stops the same music shaking harder on a
    // slower machine.
    if (onset > 0) {
      for (let i = 0; i < 4; i += 1) {
        this.v[i] += onset * drive * AXIS_GAIN[i] * w * AXIS_FREQ[i] * 0.5;
      }
    }

    // Substepped, because semi-implicit Euler is only stable while w*h < 2 and
    // a browser tab that has been in the background hands over a large delta.
    // The TIME is clamped, not the step count: capping the count keeps the
    // substep proportional to the frame, so a long delta diverges anyway.
    const bounded = Math.min(step, MAX_SUB_STEPS * SUB_STEP);
    const steps = Math.ceil(bounded / SUB_STEP);
    const h = steps > 0 ? bounded / steps : 0;

    for (let s = 0; s < steps; s += 1) {
      for (let i = 0; i < 4; i += 1) {
        const wi = w * AXIS_FREQ[i];
        const a = -(wi * wi) * this.x[i] - 2 * zeta * wi * this.v[i];

        this.v[i] += a * h;
        this.x[i] += this.v[i] * h;

        // The mount's travel limit. A real head bottoms out and stops, and so
        // does this — without it an envelope arriving on the resonance every
        // cycle would pump the amplitude up without bound, and this number
        // multiplies a blur radius.
        if (this.x[i] > 1) {
          this.x[i] = 1;
          this.v[i] = Math.min(this.v[i], 0);
        } else if (this.x[i] < -1) {
          this.x[i] = -1;
          this.v[i] = Math.max(this.v[i], 0);
        }
      }
    }
  }
}

/// The synthetic drive, standing in for a spectrum a browser does not have.
/// The same one the OpenFX build uses: a decaying pulse on a division of the
/// tempo, analytic rather than integrated so the rig's response does not depend
/// on how finely the frame happens to be stepped.
const KICK_BARS = [0, 1, 0.5, 0.25, 0.125];

function kickEnvelope(seconds, barSeconds, kick, release) {
  if (kick <= 0 || kick >= KICK_BARS.length) return 0;
  const period = Math.max(KICK_BARS[kick] * barSeconds, 1e-3);
  return Math.exp(-(seconds - period * Math.floor(seconds / period)) / Math.max(release, 1e-3));
}

//---------------------------------------------------------------------------
// Where the picture and the depth map sit inside the frame. Identity for every
// mode except the two Split ones, which is the whole reason the shader does its
// geometry in output space.
//---------------------------------------------------------------------------
function rectsFor(depthMode) {
  if (depthMode === 3) return { colour: [0, 0, 0.5, 1], depth: [0.5, 0, 0.5, 1] };
  if (depthMode === 4) return { colour: [0, 0.5, 1, 0.5], depth: [0, 0, 1, 0.5] };
  return { colour: [0, 0, 1, 1], depth: [0, 0, 1, 1] };
}

//---------------------------------------------------------------------------
// The renderer: one pass, exactly as ProcessOpenGL does it.
//---------------------------------------------------------------------------

function createRenderer(gl, quad) {
  const shader = new Program(gl, VERTEX, FRAGMENT, 'gaffer');

  const rig = new Rattle();
  let previousTime = -1;
  let followFocus = 0.55;

  return {
    render({ input, params, time }) {
      // The kit accumulates `time` from frame deltas and can step or reset it,
      // so the delta is taken here rather than trusted from the wall clock. A
      // reset runs backwards, which would run every integrator in reverse.
      let dt = previousTime < 0 ? 1 / 60 : time - previousTime;
      if (dt <= 0 || dt > 0.5) {
        rig.reset();
        followFocus = params.get('focus');
        dt = 1 / 60;
      }
      previousTime = time;

      const bpm = Math.min(300, Math.max(20, params.get('tempo')));
      const barSeconds = 240 / bpm; // four beats to the bar
      const bars = time / barSeconds;

      const rack = {
        mode: Math.round(params.get('rack')),
        markA: params.get('focus'),
        markB: params.get('markB'),
        travelSeconds: travelSeconds(params.get('speed')),
        rateHz: rateHz(params.get('rate')),
        sync: Math.round(params.get('sync')),
        ease: clamp01(params.get('ease')),
      };

      rig.update(
        kickEnvelope(time, barSeconds, Math.round(params.get('kick')), audioRelease(params.get('release'))),
        dt,
        {
          drive: params.get('drive'),
          threshold: clamp01(params.get('threshold')),
          frequency: resonanceHz(params.get('resonance')),
          damping: dampingFromParam(params.get('damping')),
        },
      );

      let focus;
      if (rack.mode === 1) {
        followFocus = followStep(followFocus, rack.markA, dt, rack.travelSeconds);
        focus = followFocus;
      } else {
        focus = rackStateless(rack, time, bars, barSeconds);
        followFocus = focus;
      }

      focus = clamp01(focus + rig.x[3] * defocusAmount(params.get('defocus')));

      const aperture = apertureFromParam(params.get('aperture'));
      const phi = phiFromParam(params.get('focalLength'));
      const depthMode = Math.round(params.get('depth'));
      const rects = rectsFor(depthMode);

      shader.use();
      bindTexture(gl, 0, input.texture);
      shader.setSampler('InputTexture', 0);

      shader.set('MaxUV', 1, 1);
      shader.set('HalfTexel', 0.5 / input.width, 0.5 / input.height);
      shader.set('Aspect', input.width / input.height);

      shader.set('ColourRect', ...rects.colour);
      shader.set('DepthRect', ...rects.depth);

      shader.set('DepthMode', depthMode);
      shader.set('DepthGain', depthGainFromParam(params.get('depthGain')));
      shader.set('Gamma', gammaFromParam(params.get('falloff')));
      // Zero for the invented field, which is smooth by construction and has no
      // noise to remove — and four extra fetches per tap is not a rounding
      // error at the top of the Quality range.
      shader.set('DepthSmooth', depthMode === 0 ? 0 : smoothFromParam(params.get('smooth')));

      shader.set('Focus', focus);
      shader.set('CocGain', cocGain(focus, aperture, phi));
      shader.set('CocMax', cocMax(focus, aperture, phi));

      shader.set('Blades', bladeCount(params.get('blades')));
      shader.set('BladeRot', rotationFromParam(params.get('rotation')));
      shader.set('Highlight', highlightFromParam(params.get('highlight')));

      const shake = shakeAmount(params.get('shake'));
      shader.set('Shift', rig.x[0] * shake, rig.x[1] * shake);
      shader.set('Roll', rig.x[2] * rollAmount(params.get('roll')));
      shader.set('Scale', breathScale(focus, phi, breathingFromParam(params.get('breathing'))));

      shader.set('EdgeMode', Math.round(params.get('edges')));
      shader.set('Taps', tapsFromParam(params.get('quality')));

      quad.draw();
    },
  };
}

//---------------------------------------------------------------------------

const RACK_MODES = ['Off', 'Follow', 'Pull', 'Sweep', 'Stutter'];
const SYNC_NAMES = ['Manual', 'Free', '4 Bars', '2 Bars', 'Bar', '1/2', '1/4', '1/8', '1/16'];

function focusDisplay(v) {
  if (v >= 0.999) return 'the near plane';
  if (v <= 0.001) return 'infinity';
  return `d ${v.toFixed(2)}`;
}

function depthGainDisplay(v) {
  const g = depthGainFromParam(v);
  if (Math.abs(g) < 1e-6) return 'flat — one distance';
  return g > 0 ? `${g.toFixed(2)}×` : `${g.toFixed(2)}× inverted`;
}

mountDemo({
  name: 'gaffer',
  pluginId: 'GF01',
  tagline:
    'A simulated lens on a depth map, with the music holding it. One plane is sharp and everything else is a disc sized by how far it is from that plane — and two things move it: a rig that rattles when the bass arrives, because a camera in front of a PA is a mass on a spring and answers at its own frequency, and a focus puller with two marks on the barrel.',
  repo: 'https://github.com/stoatworks-labs/gaffer',
  // No `page`: there is no project page on the website yet, and the kit leaves
  // a missing link out rather than rendering one that 404s. Same reasoning as
  // the empty `guide` and `page` in source/StoatworksAbout.h.

  showBackdrop: true,

  params: [
    {
      id: 'focus', name: 'Focus', type: 'standard', default: 0.55, group: 'Lens',
      display: focusDisplay,
      hint: 'Which depth is sharp: 1 is the nearest surface, 0 the furthest. With the Rack running this is mark A.',
    },
    {
      id: 'aperture', name: 'Aperture', type: 'standard', default: 0.35, group: 'Lens',
      display: (v) => (v <= 0 ? 'pinhole — the null' : `${(apertureFromParam(v) * 100).toFixed(0)}%`),
      hint: 'How shallow the depth of field is. Zero is a pinhole and the picture comes back untouched — byte for byte, in the plugin.',
    },
    {
      id: 'focalLength', name: 'Focal Length', type: 'standard', default: 0.45, group: 'Lens',
      display: (v) => `ϕ ${phiFromParam(v).toFixed(2)}`,
      hint: 'How much the lens cares where it is focused. At zero it is a long lens a long way off and neither the depth of field nor the framing changes as it racks; at the top both do, because they are the same fact about where the sensor is.',
    },
    {
      id: 'blades', name: 'Blades', type: 'option', default: 2, group: 'Lens',
      elements: ['Round', '5', '6', '7', '8', '9'],
      hint: 'The shape of the iris, which is the shape an out-of-focus highlight takes. Area-normalised, so changing it changes the shape and not how much of the picture the disc covers.',
    },
    {
      id: 'rotation', name: 'Rotation', type: 'standard', default: 0.0, group: 'Lens',
      display: (v) => `${((rotationFromParam(v) * 180) / Math.PI).toFixed(0)}°`,
      hint: 'Which way the iris points. Nothing to do on a round one.',
    },
    {
      id: 'highlight', name: 'Highlight', type: 'standard', default: 0.3, group: 'Lens',
      display: (v) => (v <= 0 ? 'plain average' : `^${highlightFromParam(v).toFixed(2)}`),
      hint: 'How much an out-of-focus highlight blooms. The disc is averaged with a power mean rather than a plain one, so a bright thing comes back as a distinct disc instead of a smear. Zero is exactly a plain average, and the only setting that conserves light.',
    },
    {
      id: 'breathing', name: 'Breathing', type: 'standard', default: 0.35, group: 'Lens',
      display: (v) => `${(v * 100).toFixed(0)}%`,
      hint: 'How much the frame creeps as the focus racks. Real lenses vary from almost none to a lot; the physical amount is at 1.',
    },

    {
      id: 'depth', name: 'Depth', type: 'option', default: 0, group: 'Depth',
      elements: ['Radial', 'Luma', 'Alpha', 'Split H', 'Split V'],
      hint: 'Where the depth comes from. Radial invents a field and works on any clip. Luma and Alpha read one out of the picture. Split H and Split V read it out of the other half of a double-width or double-height frame — none of the clips on this page is one, so those two have nothing real to read here.',
    },
    {
      id: 'depthGain', name: 'Depth Gain', type: 'standard', default: 0.5, group: 'Depth',
      display: depthGainDisplay,
      hint: 'How much depth the scene has, signed. A quarter of the way up is flat — one distance, one uniform blur — and below that the field inverts, which is what a depth map authored the other way up needs.',
    },
    {
      id: 'falloff', name: 'Falloff', type: 'standard', default: 0.5, group: 'Depth',
      display: (v) => `γ ${gammaFromParam(v).toFixed(2)}`,
      hint: 'Gamma on the depth field: where between the near and far ends most of the scene sits.',
    },
    {
      id: 'smooth', name: 'Smooth', type: 'standard', default: 0.25, group: 'Depth',
      display: (v) => `${(smoothFromParam(v) * 100).toFixed(2)}%`,
      hint: 'Blurs the depth field, not the picture. Nothing in Radial, which is already smooth; on a real depth map it is what stops a hard depth step reading as a cut-out.',
    },

    {
      id: 'rack', name: 'Rack', type: 'option', default: 0, group: 'Rack',
      elements: RACK_MODES,
      hint: 'What the focus is doing. Off leaves it on the Focus control. Follow lags it the way a hand on a follow focus does. Pull racks between the two marks on a cue. Sweep runs between them continuously. Stutter picks a new plane on every cue.',
    },
    {
      id: 'markB', name: 'Mark B', type: 'standard', default: 0.0, group: 'Rack',
      display: focusDisplay,
      hint: 'The other mark on the barrel. Focus is mark A.',
    },
    {
      id: 'speed', name: 'Speed', type: 'standard', default: 0.5, group: 'Rack',
      display: (v) => `${travelSeconds(v).toFixed(2)} s`,
      hint: 'How long a full-barrel move takes. Real racks are a third of a second to a second.',
    },
    {
      id: 'rate', name: 'Rate', type: 'standard', default: 0.5, group: 'Rack',
      display: (v) => `${rateHz(v).toFixed(2)} Hz`,
      hint: 'The cue rate when Sync is not counting bars. The fast end of this is what Sweep is for: the focal plane scanning the whole picture several times a second.',
    },
    {
      id: 'sync', name: 'Sync', type: 'option', default: 4, group: 'Rack',
      elements: SYNC_NAMES,
      hint: 'What the cues are counted in. Manual takes no cues at all in Pull and Stutter — in the plugin that is the button and the kick drum — and runs free at Rate in Sweep.',
    },
    {
      id: 'ease', name: 'Ease', type: 'standard', default: 1.0, group: 'Rack',
      display: (v) => (v <= 0 ? 'linear — a motor' : `${(v * 100).toFixed(0)}% — a hand`),
      hint: '0 is a motor and 1 is a hand. Real pulls are nearer 1. On a Sweep it decides whether the focus dwells at each mark or runs straight through.',
    },

    {
      id: 'kick', name: 'Kick', type: 'option', default: 0, group: 'Rattle',
      elements: ['Off', 'Bar', '1/2', '1/4', '1/8'],
      hint: 'What hits the rig. A browser has no audio routed to it, so this is a pulse on a division of the Tempo — exactly what the OpenFX build does, and for the same reason.',
    },
    {
      id: 'tempo', name: 'Tempo', type: 'standard', default: 120, min: 40, max: 200, group: 'Rattle',
      display: (v) => `${v.toFixed(0)} bpm`,
      hint: 'Beats per minute, for the Kick and for every bar-counted Sync division. In Resolume this comes from the host and there is no control.',
    },
    {
      id: 'drive', name: 'Drive', type: 'standard', default: 0.0, group: 'Rattle',
      display: (v) => (v <= 0 ? 'still — the null' : `${(v * 100).toFixed(0)}%`),
      hint: 'How hard each hit shakes the rig. Zero is an exact null: the camera does not move at all. Turn Kick on as well or there is nothing to answer.',
    },
    {
      id: 'threshold', name: 'Threshold', type: 'standard', default: 0.15, group: 'Rattle',
      hint: 'How big a hit has to be to count. The rig is driven by the ARRIVAL of the bass rather than by its level, and this is the gate on that.',
    },
    {
      id: 'release', name: 'Release', type: 'standard', default: 0.35, group: 'Rattle',
      display: (v) => `${audioRelease(v).toFixed(2)} s`,
      hint: 'How long a hit takes to die away.',
    },
    {
      id: 'shake', name: 'Shake', type: 'standard', default: 0.45, group: 'Rattle',
      display: (v) => `${(shakeAmount(v) * 100).toFixed(1)}% of height`,
      hint: 'How far the frame moves at full deflection.',
    },
    {
      id: 'roll', name: 'Roll', type: 'standard', default: 0.3, group: 'Rattle',
      display: (v) => `${((rollAmount(v) * 180) / Math.PI).toFixed(1)}°`,
      hint: 'How far the frame rotates at full deflection. Worth more than it sounds: a frame that only translates reads as a shaking screen, and one that tips reads as a shaking camera.',
    },
    {
      id: 'resonance', name: 'Resonance', type: 'standard', default: 0.5, group: 'Rattle',
      display: (v) => `${resonanceHz(v).toFixed(1)} Hz`,
      hint: 'The rig’s own frequency. A heavy tripod on a solid floor is low; a light head on a hollow stage is high. It answers at THIS frequency, not at the music’s.',
    },
    {
      id: 'damping', name: 'Damping', type: 'standard', default: 0.45, group: 'Rattle',
      display: (v) => `ζ ${dampingFromParam(v).toFixed(3)}`,
      hint: 'How quickly it settles. At the bottom one hit rings for whole bars.',
    },
    {
      id: 'defocus', name: 'Defocus', type: 'standard', default: 0.45, group: 'Rattle',
      display: (v) => `±${(defocusAmount(v) * 100).toFixed(0)}% of barrel`,
      hint: 'How far a hit knocks the focal plane. The lens elements are part of the rig too, so this works even on a scene at one distance.',
    },

    {
      id: 'edges', name: 'Edges', type: 'option', default: 2, group: 'Output',
      elements: ['Transparent', 'Black', 'Clamp', 'Mirror', 'Wrap'],
      hint: 'What to show where a shake or a breath looks past the picture.',
    },
    {
      id: 'quality', name: 'Quality', type: 'option', default: 1, group: 'Output',
      elements: ['Fast', 'Good', 'Best', 'Extreme'],
      display: (v) => `${tapsFromParam(v)} taps`,
      hint: 'How many samples the gather takes. It does not change the size of the blur or the shape of the bokeh, only how smoothly the disc is filled — and a small bright thing spread over a big disc is where the difference shows.',
    },
  ],

  sources: ['scene', 'grid', 'spot', 'detail', 'alpha', 'bars'],

  // The factory presets, from source/Presets.h, translated into this build's
  // parameter set — Band has no counterpart without audio, so the three that
  // set it simply do not. Plus two that exist to make a claim checkable rather
  // than to look like anything.
  presets: {
    'Kick Rattle': { focus: 0.55, aperture: 0.35, focalLength: 0.4, blades: 2, highlight: 0.3, breathing: 0.35, rack: 0, drive: 0.7, kick: 3, threshold: 0.12, release: 0.35, shake: 0.45, roll: 0.3, resonance: 0.55, damping: 0.45, defocus: 0.45 },
    'Subwoofer': { focus: 0.55, aperture: 0.45, focalLength: 0.55, blades: 2, highlight: 0.4, breathing: 0.5, rack: 0, drive: 0.9, kick: 2, threshold: 0.08, release: 0.55, shake: 0.75, roll: 0.45, resonance: 0.2, damping: 0.3, defocus: 0.7 },
    'Cheap Tripod': { focus: 0.55, aperture: 0.3, focalLength: 0.35, blades: 1, highlight: 0.25, breathing: 0.3, rack: 0, drive: 0.65, kick: 4, threshold: 0.15, release: 0.2, shake: 0.28, roll: 0.55, resonance: 0.85, damping: 0.12, defocus: 0.25 },

    'Rack A to B': { focus: 0.85, aperture: 0.45, focalLength: 0.5, blades: 2, highlight: 0.35, breathing: 0.55, rack: 2, markB: 0.15, speed: 0.55, sync: 4, ease: 1, drive: 0, kick: 0 },
    'Follow Focus': { focus: 0.6, aperture: 0.4, focalLength: 0.45, blades: 2, highlight: 0.3, breathing: 0.65, rack: 1, speed: 0.72, sync: 0, ease: 1, drive: 0, kick: 0 },
    'Focus Sweep': { focus: 0.95, aperture: 0.5, focalLength: 0.45, blades: 2, highlight: 0.35, breathing: 0.45, rack: 3, markB: 0.05, speed: 0.5, sync: 6, ease: 1, drive: 0, kick: 0 },
    'Focus Strobe': { focus: 1.0, aperture: 0.55, focalLength: 0.6, blades: 3, highlight: 0.55, breathing: 0.35, rack: 3, markB: 0.0, speed: 0.2, rate: 0.9, sync: 1, ease: 0.3, drive: 0, kick: 0 },
    'Focus Stutter': { focus: 0.9, aperture: 0.5, focalLength: 0.5, blades: 2, highlight: 0.45, breathing: 0.4, rack: 4, markB: 0.05, speed: 0.28, sync: 7, ease: 0.85, drive: 0, kick: 0 },
    'Pull On Kick': { focus: 0.9, aperture: 0.5, focalLength: 0.5, blades: 2, highlight: 0.4, breathing: 0.5, rack: 2, markB: 0.1, speed: 0.4, sync: 6, ease: 1, drive: 0.6, kick: 3, threshold: 0.18, release: 0.3, shake: 0.35, roll: 0.25, resonance: 0.5, damping: 0.4, defocus: 0.35 },
    'Bokeh Balls': { focus: 0.35, aperture: 0.75, focalLength: 0.55, blades: 2, highlight: 0.85, breathing: 0.0, rack: 0, drive: 0, kick: 0, shake: 0, roll: 0, defocus: 0 },

    'Null: a pinhole': { aperture: 0, breathing: 0, rack: 0, drive: 0, kick: 0 },
    'Null: everything in focus': { aperture: 0.6, depthGain: 0.25, focus: 0.5, breathing: 0, rack: 0, drive: 0, kick: 0, quality: 3 },
  },

  differences: [
    'Two of the plugin’s claims are checkable here rather than taken on trust. "Null: a pinhole" closes the aperture, and "Null: everything in focus" flattens the depth field and puts the focal plane exactly on it — with the whole gather still running, every tap rejected by the coverage rule. In the plugin both come back byte for byte identical to the input, at every Quality setting.',
    'This page carries the OPENFX parameter set, not the FFGL one. A browser has no audio routed to it and no transport, exactly as Resolve has none — so the rig is driven by a Kick pulse on a division of a Tempo control rather than by a spectrum, and there is no Band. In Resolume the spectrum is real, and the Rattle group answers the music instead of a metronome.',
    'Split H and Split V have nothing to read here. They are the modes that carry a real depth pass in the other half of a double-width or double-height frame, and none of the clips on this page is one. Radial invents a field and Luma and Alpha read the generated clip’s own channels — which is exactly what the plugin does with an ordinary clip, and worth knowing that the plugin has never been given a rendered depth map either.',
    'The plugin’s numerical proof — its bokeh measured against an independent C++ circle of confusion over 48 combinations in x and y separately, light conserved at 1.0000 of the pinhole, the focus puller and the rig asserted over tens of thousands of frames, and both nulls exact to the byte — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});
