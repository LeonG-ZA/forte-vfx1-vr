/*
 * jsdos-vfx1-prof.h -- where does the emulator's time go?
 *
 * Wall-clock accumulators around the parts of DOSBox's main loop, read by the
 * page through the "vfx1-prof" backend event. Added by
 * integration/jsdos/apply.sh. Spans nest: VGA drawing, frame conversion and
 * posting happen inside PIC events, and sleeps can happen inside callbacks,
 * so the page reports each span as measured rather than adding them up.
 */
#ifndef JSDOS_VFX1_PROF_H
#define JSDOS_VFX1_PROF_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
	VFX1P_PIC,        /* PIC_RunQueue: scheduled events, including VGA drawing */
	VFX1P_CPU,        /* the CPU core running emulated instructions */
	VFX1P_CALLBACK,   /* DOSBox callbacks: DOS, BIOS, mouse and VFX1 services */
	VFX1P_TIMER,      /* TIMER_AddTick: 1 ms ticks, including sound mixing */
	VFX1P_SLEEP,      /* asyncify sleep: yielding to the browser */
	VFX1P_VGADRAW,    /* VGA_DrawPart and VGA_DrawSingleLine: scanlines to RGBA */
	VFX1P_GFXEND,     /* GFX_EndUpdate: changed lines to RGB and posting them */
	VFX1P_POST,       /* posting the frame message to the page */
	VFX1P_MIX,        /* MIXER_Mix */
	VFX1P_COUNT
};

extern double vfx1_prof_ms[VFX1P_COUNT];
extern unsigned vfx1_prof_calls[VFX1P_COUNT];
double vfx1_prof_now(void);

#ifdef __cplusplus
}
#endif

#define VFX1_PROF_BEGIN(v) double v = vfx1_prof_now()
#define VFX1_PROF_END(id, v) (vfx1_prof_ms[id] += vfx1_prof_now() - (v), vfx1_prof_calls[id]++)

#endif
