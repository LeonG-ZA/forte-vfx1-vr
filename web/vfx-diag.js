/*
 * vfx-diag.js -- measure the head-to-game-camera delay from the game's pixels.
 *
 * Reprojection needs to know which head orientation each game frame was
 * rendered for. That cannot be read from the game, but it can be observed: when
 * the ship yaws, the 3D view shifts sideways. Comparing each frame with the one
 * before gives the camera's yaw step in pixels; comparing those steps with the
 * recorded head yaw at different look-back delays finds the delay that explains
 * them best. What that best fit fails to explain, frame by frame, is the jitter
 * reprojection would show.
 *
 * Plain script; sets window.VfxDiag. Tests: web/test-diag.html
 */
(function () {
  "use strict";

  const DEG = 180 / Math.PI;

  // Yaw of a quaternion in degrees, same convention as src/bridge/vfxmath.c.
  function yawDeg(q) {
    return Math.atan2(2 * (q.x * q.z + q.w * q.y), 1 - 2 * (q.x * q.x + q.y * q.y)) * DEG;
  }

  function wrap180(a) {
    while (a > 180) a -= 360;
    while (a <= -180) a += 360;
    return a;
  }

  // ------------------------------------------------------------ head log

  function HeadLog(capacity) {
    this.cap = capacity || 20000;
    this.t = new Float64Array(this.cap);
    this.yaw = new Float64Array(this.cap);
    this.n = 0;
  }

  HeadLog.prototype.push = function (timeMs, q) {
    if (this.n === this.cap) {                     // keep the newest half
      const half = this.cap >> 1;
      this.t.copyWithin(0, half); this.yaw.copyWithin(0, half);
      this.n -= half;
    }
    this.t[this.n] = timeMs;
    this.yaw[this.n] = yawDeg(q);
    this.n++;
  };

  // Unwrapped-safe interpolation of yaw at timeMs; null outside the log.
  HeadLog.prototype.at = function (timeMs) {
    const n = this.n;
    if (n < 2 || timeMs < this.t[0] || timeMs > this.t[n - 1]) return null;
    let lo = 0, hi = n - 1;
    while (hi - lo > 1) {
      const mid = (lo + hi) >> 1;
      if (this.t[mid] <= timeMs) lo = mid; else hi = mid;
    }
    const span = this.t[hi] - this.t[lo];
    const f = span > 0 ? (timeMs - this.t[lo]) / span : 0;
    return this.yaw[lo] + f * wrap180(this.yaw[hi] - this.yaw[lo]);
  };

  // ------------------------------------------------------------ frame shift

  /*
   * Horizontal shift of frame content between two RGB frames, in pixels.
   * Positive means the content moved right. Uses a band of the image that
   * avoids Descent's static cockpit overlay, which would otherwise pull the
   * answer towards zero.
   */
  function FrameShift(opts) {
    opts = opts || {};
    this.maxShift = opts.maxShift || 24;
    this.band = opts.band || { x0: 0.25, x1: 0.75, y0: 0.15, y1: 0.55 };
    this.prev = null;
    this.prevW = 0; this.prevH = 0;
  }

  FrameShift.prototype.luma = function (rgb, w, h) {
    const b = this.band, S = this.maxShift;
    const y0 = Math.floor(b.y0 * h), y1 = Math.floor(b.y1 * h);
    const rows = [];
    for (let y = y0; y < y1; y += 2) {
      const row = new Float32Array(w);
      for (let x = 0, i = y * w * 3; x < w; x++, i += 3) row[x] = 0.299 * rgb[i] + 0.587 * rgb[i + 1] + 0.114 * rgb[i + 2];
      rows.push(row);
    }
    return { rows, x0: Math.max(S, Math.floor(b.x0 * w)), x1: Math.min(w - S, Math.floor(b.x1 * w)) };
  };

  // Returns { shift, quality } or null for the first frame or a size change.
  FrameShift.prototype.feed = function (rgb, w, h) {
    const cur = this.luma(rgb, w, h);
    const prev = this.prev;
    const sameSize = prev && this.prevW === w && this.prevH === h;
    this.prev = cur; this.prevW = w; this.prevH = h;
    if (!sameSize) return null;

    const S = this.maxShift, costs = new Float64Array(2 * S + 1);
    for (let s = -S; s <= S; s++) {
      let sad = 0;
      for (let r = 0; r < cur.rows.length; r++) {
        const a = prev.rows[r], c = cur.rows[r];
        for (let x = cur.x0; x < cur.x1; x++) sad += Math.abs(c[x + s] - a[x]);
      }
      costs[s + S] = sad;
    }
    let best = 0;
    for (let i = 1; i < costs.length; i++) if (costs[i] < costs[best]) best = i;
    let shift = best - S;
    if (best > 0 && best < costs.length - 1) {      // sub-pixel parabola fit
      const l = costs[best - 1], m = costs[best], r = costs[best + 1];
      const den = l - 2 * m + r;
      if (den > 0) shift += 0.5 * (l - r) / den;
    }
    let mean = 0;
    for (const c of costs) mean += c;
    mean /= costs.length;
    const quality = mean > 0 ? 1 - costs[best] / mean : 0;    // 0 flat, near 1 sharp
    return { shift, quality };
  };

  // ------------------------------------------------------------ analysis

  /*
   * frames: [{ t, shift }] where shift is the content step between this frame
   * and the previous one.
   *
   * Two separate questions, answered two ways:
   *
   *  1. What is the delay? For each candidate look-back L, compare the camera's
   *     yaw change over a SPAN of frames (the summed shifts) with the head's yaw
   *     change over the same span, looked up L earlier. Using a span rather than
   *     one step matters: arrival-time noise is the same size either way, but
   *     the signal grows with the span, so the delay estimate is not dragged by
   *     that noise. A one-step fit measured 75 ms on simulated data whose true
   *     mean delay was 62.4 ms.
   *
   *  2. How big is the jitter? At that delay, the per-frame residual: how far
   *     each single frame's step disagrees with the head's step. That is the
   *     jump a viewer sees when a frame arrives, so it stays per-frame.
   */
  function analyze(frames, headLog, opts) {
    opts = opts || {};
    const maxL = opts.maxL || 250, stepL = opts.stepL || 5;
    const span = opts.span || 6;
    const minMotionDeg = opts.minMotionDeg === undefined ? 0.2 : opts.minMotionDeg;

    // Cumulative camera track in pixels.
    const cum = new Float64Array(frames.length);
    for (let k = 1; k < frames.length; k++) cum[k] = cum[k - 1] + frames[k].shift;

    function fit(L, lag) {
      let sxy = 0, sxx = 0, syy = 0, n = 0;
      const pts = [];
      for (let k = lag; k < frames.length; k++) {
        let usable = true;                                     // skip spans with unreliable steps
        for (let j = k - lag + 1; j <= k; j++) if (frames[j].ok === false) { usable = false; break; }
        if (!usable) continue;
        const a = headLog.at(frames[k - lag].t - L), b = headLog.at(frames[k].t - L);
        if (a === null || b === null) continue;
        const d = wrap180(b - a), s = cum[k] - cum[k - lag];
        if (Math.abs(d) < minMotionDeg * lag) continue;       // only moving spans inform
        pts.push([d, s]);
        sxy += d * s; sxx += d * d; syy += s * s; n++;
      }
      if (n < 20 || sxx === 0 || syy === 0) return null;
      return { gain: sxy / sxx, corr: sxy / Math.sqrt(sxx * syy), pts, n };
    }

    const results = [];
    for (let L = 0; L <= maxL; L += stepL) {
      const f = fit(L, span);
      if (!f) continue;
      let rss = 0;
      for (const [d, s] of f.pts) rss += (s - f.gain * d) * (s - f.gain * d);
      results.push({ L, gain: f.gain, corr: f.corr, spanRmsPx: Math.sqrt(rss / f.n), n: f.n });
    }
    if (!results.length) return null;
    let best = results[0];
    for (const r of results) if (r.spanRmsPx < best.spanRmsPx) best = r;

    // Per-frame jitter at a given delay, using the span fit's gain.
    function jitterAt(L, gain) {
      const one = fit(L, 1);
      if (!one) return null;
      let rss = 0;
      for (const [d, s] of one.pts) rss += (s - gain * d) * (s - gain * d);
      const rmsPx = Math.sqrt(rss / one.n);
      return { rmsPx, rmsDeg: Math.abs(gain) > 1e-6 ? rmsPx / Math.abs(gain) : null, n: one.n };
    }

    // Placement error over a span at a given look-back, in degrees, using the
    // best fit's gain. This is the error a wrong look-back causes: the display
    // is put where the head was L ago while the content shows where the camera
    // actually was, so the world drifts against the headset over several frames.
    function spanErrorAt(L) {
      const q = Math.round(L / stepL) * stepL;
      const f = fit(q, span);
      if (!f) return null;
      let rss = 0;
      for (const [d, s] of f.pts) rss += (s - best.gain * d) * (s - best.gain * d);
      return Math.sqrt(rss / f.n) / Math.abs(best.gain);
    }

    const bestJitter = jitterAt(best.L, best.gain);
    return {
      best: { L: best.L, gain: best.gain, corr: best.corr, n: best.n,
              rmsPx: bestJitter ? bestJitter.rmsPx : null, rmsDeg: bestJitter ? bestJitter.rmsDeg : null,
              spanErrorDeg: spanErrorAt(best.L) },
      jitterAt: (L) => jitterAt(Math.round(L / stepL) * stepL, best.gain),
      spanErrorAt,
      all: results,
    };
  }

  function intervalStats(frames) {
    const d = [];
    for (let k = 1; k < frames.length; k++) d.push(frames[k].t - frames[k - 1].t);
    if (d.length < 2) return null;
    const mean = d.reduce((a, b) => a + b, 0) / d.length;
    const sd = Math.sqrt(d.reduce((a, b) => a + (b - mean) * (b - mean), 0) / d.length);
    const s = d.slice().sort((a, b) => a - b);
    return { mean, sd, p10: s[Math.floor(s.length * 0.1)], p90: s[Math.floor(s.length * 0.9)] };
  }

  /*
   * Step response: the head yaws by `sizeDeg` at each time in `stepTimes`, then
   * holds. From the frames' cumulative content shift, measure how the game's
   * camera follows: when it starts (10%), is halfway (50%), and nearly there
   * (90%), plus overshoot. This separates a pure delay from a camera that eases
   * towards the target, which a sweep cannot do.
   */
  function stepResponse(frames, stepTimes, sizeDeg, opts) {
    opts = opts || {};
    const pre = opts.preMs || 200, settleFrom = opts.settleFromMs || 900, settleTo = opts.settleToMs || 1300;
    const cum = [];
    let c = 0;
    for (let k = 0; k < frames.length; k++) {
      if (k > 0) c += frames[k].ok === false ? 0 : frames[k].shift;
      cum.push({ t: frames[k].t, c });
    }
    const meanOver = (a, b) => {
      let s = 0, n = 0;
      for (const p of cum) if (p.t >= a && p.t < b) { s += p.c; n++; }
      return n ? s / n : null;
    };
    const steps = [];
    for (let i = 0; i < stepTimes.length; i++) {
      const ts = stepTimes[i], dir = i % 2 === 0 ? 1 : -1;
      const c0 = meanOver(ts - pre, ts), c1 = meanOver(ts + settleFrom, ts + settleTo);
      if (c0 === null || c1 === null) continue;
      const D = c1 - c0;
      if (Math.abs(D) < 1e-6) continue;
      const cross = (frac) => {
        for (const p of cum) if (p.t >= ts && (p.c - c0) / D >= frac) return p.t - ts;
        return null;
      };
      let peak = 0;
      for (const p of cum) if (p.t >= ts && p.t < ts + settleTo) peak = Math.max(peak, (p.c - c0) / D);
      steps.push({ t10: cross(0.1), t50: cross(0.5), t90: cross(0.9), overshoot: peak - 1,
                   pxPerDeg: D / (dir * sizeDeg) });
    }
    if (!steps.length) return null;
    const med = (k) => {
      const xs = steps.map((s) => s[k]).filter((x) => x !== null).sort((a, b) => a - b);
      return xs.length ? xs[xs.length >> 1] : null;
    };
    return { steps: steps.length, t10: med("t10"), t50: med("t50"), t90: med("t90"),
             overshoot: med("overshoot"), pxPerDeg: med("pxPerDeg") };
  }

  window.VfxDiag = { yawDeg, wrap180, HeadLog, FrameShift, analyze, intervalStats, stepResponse };
})();
