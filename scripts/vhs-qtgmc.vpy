"""Bob a USB-Live2 NTSC capture and optionally double both raster dimensions."""

import vapoursynth as vs
from havsfunc import QTGMC

# These defaults also support running vspipe directly with only a source path.
scale = globals().get("scale", "2")
preset = globals().get("preset", "Slower")
core = vs.core
clip = core.ffms2.Source(source=source, cache=False)
if (clip.width, clip.height) != (720, 480):
    raise ValueError("Expected a 720x480 USB-Live2 NTSC recording.")

# Assign the NTSC cadence without dropping or repeating frames.
input_frames = clip.num_frames
clip = core.std.AssumeFPS(clip, fpsnum=30000, fpsden=1001)
clip = QTGMC(clip, TFF=True, FPSDivisor=1, Preset=preset)
if clip.num_frames != 2 * input_frames or (clip.fps_num, clip.fps_den) != (60000, 1001):
    raise ValueError("QTGMC must produce one 59.94p frame per source field.")

if scale == "2":
    # Transposing subsampled chroma would swap the sampling axes. Expand it before NNEDI3.
    clip = core.resize.Spline36(clip, format=vs.YUV444P16)
    nnedi3 = core.znedi3.nnedi3 if hasattr(core, "znedi3") else core.nnedi3.nnedi3
    clip = nnedi3(clip, field=1, dh=True, nsize=0)
    clip = nnedi3(clip.std.Transpose(), field=1, dh=True, nsize=0).std.Transpose()
    # Correct NNEDI3's half-pixel shift while keeping the doubled raster.
    clip = core.resize.Spline36(clip, format=vs.YUV422P10, src_left=-0.5, src_top=-0.5)
elif scale == "1":
    clip = core.resize.Spline36(clip, format=vs.YUV422P10)
else:
    raise ValueError("Expected scale=1 or scale=2.")
clip.set_output()
