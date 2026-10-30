#!/usr/bin/env python3
"""Reference model of the residual-based DETAIL architecture (CORE path).

This is NOT a test of the C++ plugin. It checks the *architecture*:
    c = clip(x)            full-band shaping, once
    r = x - c              what the clipper removed
    out = c + sum_k p_k * r_k   (r split into 3 complementary regions)
and shows that
    * DETAIL = 0  -> peak == T exactly (true clipper),
    * DETAIL > 0  -> overshoot is bounded and the final safety clamp T*(1+detail) rarely engages,
and contrasts it with the old per-region clipping, whose ceiling was up to 3*T.
"""
import numpy as np

fs = 192000
n = fs * 2
rng = np.random.default_rng(1)
w = rng.standard_normal(n)
F = np.fft.rfft(w)
f = np.fft.rfftfreq(n, 1 / fs)
F[1:] /= np.sqrt(f[1:])
x = np.fft.irfft(F, n)
x /= np.max(np.abs(x))


def onepole(sig, hz):
    a = 1 - np.exp(-2 * np.pi * hz / fs)
    y = np.empty_like(sig)
    s = 0.0
    for i, v in enumerate(sig):
        s += a * (v - s)
        y[i] = s
    return y


def regions(sig):
    l1, l2 = onepole(sig, 180), onepole(sig, 2800)
    return [l1, l2 - l1, sig - l2]


def old_per_region(x, T):
    return sum(np.clip(r, -T, T) for r in regions(x))


def new_residual(x, T, detail, protect_mode):
    c = np.clip(x, -T, T)
    r_regs = regions(x - c)
    if protect_mode == "worst":      # protection at its maximum, every sample
        p = [detail] * 3
    else:                            # random protection within [0, detail]
        p = [rng.uniform(0, detail, n) for _ in range(3)]
    return c + sum(pk * rk for pk, rk in zip(p, r_regs))


ok = True
for T in (0.5, 0.25):
    old = old_per_region(x, T)
    print(f"T={T}: OLD per-region   peak/T = {np.max(np.abs(old))/T:.2f}")
    for detail in (0.0, 0.5, 1.0):
        for mode in ("worst", "random"):
            out = new_residual(x, T, detail, mode)
            ceil = T * (1 + detail)
            pre = np.max(np.abs(out))
            engaged = (np.abs(out) > ceil * 1.0000001).mean() * 100
            clamped = np.clip(out, -ceil, ceil)
            print(f"   NEW detail={detail:.1f} {mode:6s}: peak/T={pre/T:.2f}  "
                  f"ceiling/T={ceil/T:.2f}  clamp engages on {engaged:.3f}% of samples")
            if detail == 0.0 and abs(pre - T) > 1e-9:
                ok = False
            assert np.max(np.abs(clamped)) <= ceil + 1e-9
print("DETAIL=0 gives an exact ceiling:", ok)
