#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Fits the polynomial coefficients in firmware/src/dsp/fastmath.h (iteratively
# reweighted least squares toward minimax). Prints coefficients, highest power first.
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Fits the polynomial coefficients in firmware/src/dsp/fastmath.h (iteratively
# reweighted least squares toward minimax). Prints coefficients, highest power first.
import numpy as np
def fit(f, a, b, powers, rel=False, n=4000):
    x = np.cos(np.linspace(0, np.pi, n)) * (b-a)/2 + (a+b)/2
    y = f(x)
    A = np.stack([x**p for p in powers], 1)
    w = 1/np.abs(y) if rel else np.ones_like(y)
    # iterate reweighted LS toward minimax
    W = np.ones_like(y)
    for _ in range(60):
        c, *_ = np.linalg.lstsq(A*(w*W)[:,None], y*w*W, rcond=None)
        e = np.abs((A@c - y)*w)
        W = W * (1 + e/e.max())
    return c, e.max()
c,e = fit(np.exp2, -0.5, 0.5, range(7), rel=True); print("exp2", [f"{v:.9e}" for v in c[::-1]], e)
# log2 via atanh: log2(m) = sum c_k t^(2k+1), t in [-0.1716,0.1716]
f = lambda t: np.log2((1+t)/(1-t))
c,e = fit(f, -0.1716, 0.1716, [1,3,5,7,9]); print("log2", [f"{v:.9e}" for v in c[::-1]], e)
c,e = fit(np.sin, -np.pi, np.pi, [1,3,5,7,9,11]); print("sin11", [f"{v:.9e}" for v in c[::-1]], e)
c,e = fit(np.sin, -np.pi, np.pi, [1,3,5,7,9,11,13]); print("sin13", [f"{v:.9e}" for v in c[::-1]], e)
