#pragma once

/**
    The one shader pass.

    Every output pixel asks how far away the scene is, works out how big a
    circle of confusion the lens puts there, and gathers whatever is close
    enough to have landed on it. Nothing accumulates between frames and no pixel
    depends on any other, so there are no intermediate buffers at all -- which
    sidesteps both of the fleet's FFGL FBO bugs without having to think about
    them.

    Six things are worth knowing before editing:

    - **The lens maths is a mirror of Lens.cpp.** Two copies of one formula is a
      liability and `gftest --probe` is the answer to it: it measures what the
      GPU did against what the C++ predicts. Change one, change the other, run
      the probe.

    - **It gathers, but the rule it gathers by is a scattering rule.** A sample
      contributes to this pixel if that sample's OWN circle of confusion reaches
      this pixel -- which is what it means for a point to be smeared into a
      disc, asked from the other end. Weighting each contribution by 1/(pi r^2)
      is then not a fudge: it is the sample's energy divided over the area it
      was smeared across, and it is why a flat depth field makes this an exactly
      normalised disc convolution. `gftest --bokeh` measures that.

    - **Near and far are separated by the RECEIVING pixel's depth, not by the
      focal plane.** Anything nearer than the surface being shaded can occlude
      it, so those contributions are composited OVER the rest with an opacity
      equal to how much of the gather disc they cover. Split at the focal plane
      instead and a sharp foreground dissolves into the background behind it,
      because its own contribution lands in the same bucket as everything it is
      supposed to be hiding.

    - **All geometry is in OUTPUT picture space, 0..1, and both the rect mapping
      and MaxUV are applied at the last possible moment.** FFGL hands over a
      texture that may be larger than the picture in it, and in the Split modes
      the picture is half of what is left. This effect samples wherever it
      likes, so treating either boundary as the picture edge fetches something
      that is not the picture.

    - **The Split seam is clamped inside its own rect.** Half a texel matters
      here in a way it does not elsewhere: a linear fetch at the inside edge of
      the picture half takes half its weight from the DEPTH half, which is not a
      slightly wrong colour but a completely unrelated one.

    - **Uniform names have to match the C++ exactly.** A mismatch is not an
      error anywhere -- glGetUniformLocation returns -1 and glUniform on -1 is a
      documented no-op -- so the control is simply dead. `tools/sweep.py` is the
      only thing that catches it.
*/
namespace gaffer
{

/// Passes UV through untouched, in 0..1 picture space. Deliberately NOT
/// pre-multiplied by MaxUV the way a simple filter's vertex shader would.
extern const char* const kVertexShader;

extern const char* const kFragmentShader;

} // namespace gaffer
