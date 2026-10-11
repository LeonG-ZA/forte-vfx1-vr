/*
 * jsdos-vfx1-prof.cpp -- storage and export for jsdos-vfx1-prof.h.
 */
#include "jsdos-vfx1-prof.h"

#ifdef EMSCRIPTEN
#include <emscripten.h>
#else
#include <chrono>
#define EMSCRIPTEN_KEEPALIVE
#endif

double vfx1_prof_ms[VFX1P_COUNT];
unsigned vfx1_prof_calls[VFX1P_COUNT];

double vfx1_prof_now(void) {
#ifdef EMSCRIPTEN
	return emscripten_get_now();
#else
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
#endif
}

/* Copies the accumulators into a static block and clears them: VFX1P_COUNT
 * doubles of milliseconds, then VFX1P_COUNT call counts as doubles. */
extern "C" double* EMSCRIPTEN_KEEPALIVE vfx1ProfTake(void) {
	static double out[2 * VFX1P_COUNT];
	for (int i = 0; i < VFX1P_COUNT; i++) {
		out[i] = vfx1_prof_ms[i];
		out[VFX1P_COUNT + i] = vfx1_prof_calls[i];
		vfx1_prof_ms[i] = 0;
		vfx1_prof_calls[i] = 0;
	}
	return out;
}
