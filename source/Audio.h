#pragma once

#include <array>

/**
    The spectrum the host delivers, folded down to one number.

    FFGL gives a plugin 64 FFT bins through a buffer parameter, once per frame.
    That is a modulation source and not a signal source -- 60 values a second,
    not 48,000 -- and everything downstream of it here treats it that way.

    Two decisions, both of which have a visible wrong answer:

    **sqrt the magnitudes.** Bin magnitudes bunch hard against zero. Used raw,
    the picture answers the kick drum and nothing else, and every control looks
    dead on anything but the loudest transient.

    **Fast up, slow down.** A level that arrives a frame late reads as broken.
    One that takes ~150 ms to die away reads as intended. Symmetric smoothing
    trades the transient for lag, which is worse than either.

    Kept free of any FFGL type so the offline harness can drive it directly with
    a synthetic spectrum -- which it has to, because there is no audio on a
    hosted CI runner and no host in the harness.
*/
namespace gaffer
{

/// What the host sends. Fixed by FFGL, not by us.
inline constexpr int kAudioBins = 64;

/// Which slice of the spectrum drives the rig.
///
/// The boundaries are a three-way cabinet's crossover points rather than equal
/// thirds: a woofer is finished by a few hundred hertz and a tweeter has not
/// started until several kHz, so equal thirds would leave the bass band with
/// almost no content -- and bass is the whole point of this plugin.
enum class Band
{
	FullRange = 0,
	Low       = 1,
	Mid       = 2,
	High      = 3
};

inline constexpr int kBandCount = 4;

/**
	The per-bin envelope follower, and the band fold.
*/
class Audio
{
public:
	/// Advance one frame. `bins` is the host's buffer (or the harness's),
	/// `count` how many of them there are, `dt` the frame in seconds, and
	/// `releaseSeconds` the fall time.
	void Update( const float* bins, int count, double dt, double releaseSeconds );

	/// The chosen band's level, 0..1.
	double Level( Band band ) const;

	/// Everything to silence. Called when the drive is off so that turning it
	/// back on does not start from a stale spectrum.
	void Reset();

	/// First and last bin of each band, over the 64 the host sends.
	static void BandRange( Band band, int& first, int& last );

private:
	std::array< float, kAudioBins > level{};
};

} // namespace gaffer
