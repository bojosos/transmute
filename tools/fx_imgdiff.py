"""imgdiff.py a.png b.png: pixel statistics between two images."""
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert("RGBA")).astype(np.int32)
b = np.asarray(Image.open(sys.argv[2]).convert("RGBA")).astype(np.int32)
if a.shape != b.shape:
    print("shape mismatch", a.shape, b.shape)
    sys.exit(2)
d = np.abs(a - b)
n = d.size
print("channels differing: %d / %d (%.3f%%)  max %d  mean %.4f  >2: %d  >8: %d" % (
    (d > 0).sum(), n, 100.0 * (d > 0).sum() / n, d.max(), d.mean(), (d > 2).sum(), (d > 8).sum()))
px = (d.max(axis=2) > 0)
ys, xs = np.nonzero(px)
if len(ys):
    print("differing pixels: %d, bbox x %d..%d y %d..%d" % (len(ys), xs.min(), xs.max(), ys.min(), ys.max()))
