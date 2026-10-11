/*
 * vfx-reproject.js -- orientation reprojection for the WebXR adapter.
 *
 * A DOS game on the Quest delivers about 20 frames a second; the headset
 * refreshes at 72 or more. Between game frames, the last frame must stay
 * where it was rendered in the world while the head keeps moving, or the
 * image drags with the head.
 *
 * libvfxbridge does this per pixel in src/bridge/reproject.c: rotate each view
 * ray by conj(renderPose) * currentPose and resample. On a GPU with a headset
 * compositor there is a cheaper equivalent with identical results for pure
 * rotation: place each game frame's virtual display along the head orientation
 * it was rendered for, and keep it there. The headset's own per-eye projection
 * then does the re-warp, with correct stereo, for free.
 *
 * The hard part is not the geometry but knowing the render orientation. A pose
 * becomes mouse counts, crosses into the js-dos worker, is read by the game at
 * the start of a frame, rendered, and posted back. So the render orientation is
 * looked up from a short history of head poses, about one game frame before the
 * frame arrived. That is the same estimate libvfxbridge's estimate_render_pose
 * makes when a core does not report its render pose.
 *
 * Plain script, no modules: it sets window.VfxReproject, and has no globals of
 * its own that could collide with js-dos's.
 *
 * Tests: web/test-reproject.html
 */
(function () {
  "use strict";

  // ------------------------------------------------------------ quaternions

  function normalize(q) {
    const n = Math.hypot(q.w, q.x, q.y, q.z) || 1;
    return { w: q.w / n, x: q.x / n, y: q.y / n, z: q.z / n };
  }

  function conjugate(q) {
    return { w: q.w, x: -q.x, y: -q.y, z: -q.z };
  }

  function multiply(a, b) {
    return {
      w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
      x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
  }

  function slerp(a, b, t) {
    let d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0) { b = { w: -b.w, x: -b.x, y: -b.y, z: -b.z }; d = -d; }
    if (d > 0.9995) {
      return normalize({ w: a.w + t * (b.w - a.w), x: a.x + t * (b.x - a.x),
                         y: a.y + t * (b.y - a.y), z: a.z + t * (b.z - a.z) });
    }
    const th = Math.acos(d), s = Math.sin(th);
    const wa = Math.sin((1 - t) * th) / s, wb = Math.sin(t * th) / s;
    return normalize({ w: wa * a.w + wb * b.w, x: wa * a.x + wb * b.x,
                       y: wa * a.y + wb * b.y, z: wa * a.z + wb * b.z });
  }

  // Angle between two orientations, in degrees.
  function angleDeg(a, b) {
    const d = Math.abs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);
    return 2 * Math.acos(Math.min(1, d)) * 180 / Math.PI;
  }

  // ------------------------------------------------------------ VFX1 angles

  // Head orientation as the VFX1 head tracker reports it (Forte SDK README):
  // positive yaw turns left, positive pitch looks up, positive roll tilts the
  // head right, applied yaw then pitch then roll about the head's own axes.
  // WebXR's head frame is +X right, +Y up, -Z forward, so yaw is about +Y,
  // pitch about +X and roll about -Z: q = Ry(yaw) Rx(pitch) Rz(-roll).
  function senseAngles(q) {
    const { w, x, y, z } = normalize(q);
    // Rotation matrix entries needed for the Y-X-Z decomposition.
    const m02 = 2 * (x * z + w * y), m22 = 1 - 2 * (x * x + y * y);
    const m12 = 2 * (y * z - w * x);
    const m10 = 2 * (x * y + w * z), m11 = 1 - 2 * (x * x + z * z);
    const pitch = Math.asin(Math.max(-1, Math.min(1, -m12)));
    const yaw = Math.atan2(m02, m22);
    const roll = -Math.atan2(m10, m11);
    return { yaw, pitch, roll };
  }

  // Tilt as a two-axis inclinometer like the CyberPuck's measures it: pitch is
  // how far the forward axis (-Z) rises above level, roll how far the right
  // axis (+X) drops below it, each -90..90 degrees and independent of yaw.
  // Unlike senseAngles, nothing jumps by 180 degrees when the device points
  // straight up or down, which a hand-held controller easily does.
  function tiltAngles(q) {
    const { w, x, y, z } = normalize(q);
    const forwardY = 2 * (w * x - y * z);          // world Y of the local -Z axis
    const rightY = 2 * (x * y + w * z);            // world Y of the local +X axis
    const clamp = (v) => Math.max(-1, Math.min(1, v));
    return { pitch: Math.asin(clamp(forwardY)), roll: -Math.asin(clamp(rightY)) };
  }

  // Radians to VFX1 units: 65536 to a turn, wrapped into a signed 16-bit value.
  function senseUnits(rad) {
    let v = Math.round(rad / (2 * Math.PI) * 65536) % 65536;
    if (v < 0) v += 65536;
    return v >= 32768 ? v - 65536 : v;
  }

  // ------------------------------------------------------------ matrices

  // Rigid transform (rotation q, translation p) as a column-major 4x4, the
  // layout XRRigidTransform.matrix uses.
  function rigidMatrix(q, p) {
    const { w, x, y, z } = normalize(q);
    const xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    return new Float32Array([
      1 - 2 * (yy + zz), 2 * (xy + wz),     2 * (xz - wy),     0,
      2 * (xy - wz),     1 - 2 * (xx + zz), 2 * (yz + wx),     0,
      2 * (xz + wy),     2 * (yz - wx),     1 - 2 * (xx + yy), 0,
      p.x,               p.y,               p.z,               1,
    ]);
  }

  function mul4(a, b) {
    const o = new Float32Array(16);
    for (let c = 0; c < 4; c++)
      for (let r = 0; r < 4; r++) {
        let s = 0;
        for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
        o[c * 4 + r] = s;
      }
    return o;
  }

  /*
   * Model matrix for the virtual display: a quad of the given half extents,
   * `dist` metres in front of `position`, facing along `orientation`.
   * Head-locked uses the current head orientation; reprojected uses the
   * orientation the displayed game frame was rendered for.
   */
  function displayMatrix(orientation, position, dist, halfW, halfH) {
    const local = new Float32Array([halfW, 0, 0, 0, 0, halfH, 0, 0, 0, 0, 1, 0, 0, 0, -dist, 1]);
    return mul4(rigidMatrix(orientation, position), local);
  }

  // ------------------------------------------------------------ pose history

  function PoseHistory(capacity) {
    this.cap = capacity || 128;
    this.t = new Float64Array(this.cap);
    this.q = new Array(this.cap);
    this.head = 0;
    this.count = 0;
  }

  PoseHistory.prototype.push = function (timeMs, q) {
    this.t[this.head] = timeMs;
    this.q[this.head] = normalize(q);
    this.head = (this.head + 1) % this.cap;
    if (this.count < this.cap) this.count++;
  };

  // Orientation at timeMs, interpolated; clamped to the recorded window.
  PoseHistory.prototype.at = function (timeMs) {
    if (!this.count) return null;
    let newer = null, older = null;
    for (let i = 0; i < this.count; i++) {
      const idx = (this.head - 1 - i + this.cap) % this.cap;
      if (this.t[idx] <= timeMs) { older = idx; break; }
      newer = idx;
    }
    if (older === null) return this.q[newer];
    if (newer === null || this.t[newer] <= this.t[older]) return this.q[older];
    const f = (timeMs - this.t[older]) / (this.t[newer] - this.t[older]);
    return slerp(this.q[older], this.q[newer], Math.max(0, Math.min(1, f)));
  };

  // ------------------------------------------------------------ latency

  /*
   * How far back to look up the render pose. Auto: one smoothed game-frame
   * interval plus a fixed allowance for the worker round trip. A fixed value
   * in milliseconds overrides it, for tuning by eye in the headset.
   */
  function LatencyEstimator(fixedMs, transportMs) {
    this.fixed = fixedMs;
    this.transport = transportMs === undefined ? 8 : transportMs;
    this.interval = 50;
    this.last = null;
  }

  LatencyEstimator.prototype.frameArrived = function (timeMs) {
    if (this.last !== null) {
      const dt = timeMs - this.last;
      if (dt > 5 && dt < 500) this.interval += 0.1 * (dt - this.interval);
    }
    this.last = timeMs;
  };

  LatencyEstimator.prototype.ms = function () {
    if (this.fixed !== null && this.fixed !== undefined) return this.fixed;
    return Math.max(15, Math.min(150, this.interval + this.transport));
  };

  // ------------------------------------------------------------ camera model

  /*
   * A model of where a mouse-path game's camera actually points, for games
   * whose camera does not snap to the mouse. Measured for Descent with
   * web/vfx-diag.js stepResponse: the camera starts moving about 85 ms after
   * the head, then eases in like a first-order lag with a time constant around
   * 130 ms. A single look-back delay cannot represent that easing, which is why
   * look-back reprojection jittered in Descent.
   *
   * The model's output at time t is the orientation the frame arriving at t
   * shows, so it is used directly as that frame's render pose.
   */
  function CameraModel(delayMs, tauMs, capacity) {
    this.delay = delayMs;
    this.tau = tauMs;
    this.cam = null;
    this.lastT = null;
    this.history = new PoseHistory(capacity || 256);
  }

  CameraModel.prototype.update = function (nowMs, headHistory) {
    const target = headHistory.at(nowMs - this.delay);
    if (!target) return;
    if (this.cam === null) {
      this.cam = target;
    } else {
      const dt = Math.max(0, nowMs - this.lastT);
      const alpha = this.tau > 0 ? 1 - Math.exp(-dt / this.tau) : 1;
      this.cam = slerp(this.cam, target, alpha);
    }
    this.lastT = nowMs;
    this.history.push(nowMs, this.cam);
  };

  CameraModel.prototype.at = function (timeMs) {
    return this.history.at(timeMs);
  };

  window.VfxReproject = {
    normalize, conjugate, multiply, slerp, angleDeg, senseAngles, senseUnits, tiltAngles,
    rigidMatrix, mul4, displayMatrix,
    PoseHistory, LatencyEstimator, CameraModel,
  };
})();
