/*
 * vfx-stereo.js -- recover the VFX1 headset's two eye images from a DOS frame.
 *
 * A VFX1 game draws into a 640x480 VGA frame that holds four 320x240 pages:
 * the left and right halves, each on the even and on the odd scanlines. The
 * VIP card showed one pair of pages per eye and the game flipped between the
 * even and odd pair through the card's page select register, so it could draw
 * the hidden pair without tearing. Descent's -vfx mode does exactly this
 * (main/game.c, game_render_frame_stereo_vfx, and main/vfx.c, vfx_set_page, in
 * Parallax's released source).
 *
 * Stock js-dos has no VIP card, so the page select writes go nowhere. The page
 * being drawn is found from the pixels instead: each delivered frame is
 * compared with the previous one, per scanline parity. A game draws one parity
 * at a time, sometimes across two deliveries, so a parity is shown only once
 * it has stopped changing. That costs one delivery of latency and never shows
 * a half-drawn page.
 *
 * Descent draws its HUD, crosshair included, into the left eye only. Its HUD
 * colours are pure greens, red and blue both zero, so mirrorHud copies those
 * pixels into the same place in the right eye. The HUD then sits at screen
 * depth in both eyes. A world pixel of exactly such a green would be copied
 * too; Descent's levels rarely use one.
 *
 * Plain script, no modules: it sets window.VfxStereo.
 *
 * Tests: web/test-stereo.html
 */
(function () {
  "use strict";

  // Samples every `step` pixels of each row; enough to see a page redraw.
  function changedPerParity(cur, prev, w, h, step) {
    const changed = [0, 0];
    for (let y = 0; y < h; y++) {
      const row = y * w * 3;
      for (let x = 0; x < w; x += step) {
        const i = row + x * 3;
        if (cur[i] !== prev[i] || cur[i + 1] !== prev[i + 1] || cur[i + 2] !== prev[i + 2]) changed[y & 1]++;
      }
    }
    return changed;
  }

  // Copies left-eye HUD pixels to the same place in the right eye. `eyes` is
  // w x h RGB with the left eye in the left half.
  function mirrorHud(eyes, w, h) {
    const half = w >> 1;
    let copied = 0;
    for (let y = 0; y < h; y++) {
      const row = y * w * 3;
      for (let x = 0; x < half; x++) {
        const i = row + x * 3;
        if (eyes[i] === 0 && eyes[i + 2] === 0 && eyes[i + 1] >= 64) {
          const j = i + half * 3;
          eyes[j] = 0; eyes[j + 1] = eyes[i + 1]; eyes[j + 2] = 0;
          copied++;
        }
      }
    }
    return copied;
  }

  function PageFlipExtractor(step) {
    this.step = step || 8;
    this.prev = null;
    this.pending = -1;        // parity being drawn, not yet shown
    this.shown = -1;          // parity currently shown
    this.eyes = null;         // w x h/2 RGB: left eye in the left half, right in the right
    this.flips = 0;
  }

  // Feed every delivered RGB frame. Returns true when a newly completed page
  // was copied into this.eyes.
  PageFlipExtractor.prototype.feed = function (rgb, w, h) {
    if (!this.prev || this.prev.length !== rgb.length) {
      this.prev = new Uint8Array(rgb.length);
      this.prev.set(rgb);
      this.pending = -1;
      this.shown = 0;
      return this.copy(rgb, w, h, 0);
    }
    const c = changedPerParity(rgb, this.prev, w, h, this.step);
    this.prev.set(rgb);
    const drawing = c[0] === 0 && c[1] === 0 ? -1 : (c[1] > c[0] ? 1 : 0);

    let done = -1;
    if (this.pending !== -1 && drawing !== this.pending) done = this.pending;
    if (drawing !== -1) this.pending = drawing;
    else if (done !== -1) this.pending = -1;
    if (done === -1) return false;

    // The finished page is untouched in this frame, so it can be copied from it.
    this.shown = done;
    this.flips++;
    return this.copy(rgb, w, h, done);
  };

  PageFlipExtractor.prototype.copy = function (rgb, w, h, parity) {
    const hh = h >> 1, rowBytes = w * 3;
    if (!this.eyes || this.eyes.length !== rowBytes * hh) this.eyes = new Uint8Array(rowBytes * hh);
    for (let y = 0; y < hh; y++) {
      const src = (2 * y + parity) * rowBytes;
      this.eyes.set(rgb.subarray(src, src + rowBytes), y * rowBytes);
    }
    return true;
  };

  window.VfxStereo = { changedPerParity, mirrorHud, PageFlipExtractor };
})();
