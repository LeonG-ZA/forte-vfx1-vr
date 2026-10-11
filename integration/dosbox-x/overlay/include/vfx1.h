/*
 *  VFX1 head tracking for DOSBox-X -- INT 33h mouse path.
 *
 *  Part of the DOSBox VFX1 bridge project. Licensed GPL v2 or later, matching
 *  DOSBox-X, because this file is compiled into it.
 */
#ifndef DOSBOX_VFX1_H
#define DOSBOX_VFX1_H

extern "C" {
#include "vfx/vfxcore_api.h"
}

/*
 * A pose source answers "where is the head now". Three function pointers, so
 * that adding one is a new translation unit rather than an edit to the module
 * that consumes them.
 *
 * This is where the layering rule lands on the emulator side: vfx1.h
 * and vfx1_mouse.cpp must never name an XR API, a graphics API or a transport,
 * and tools/check_layering.sh enforces that. Each vfx1_source_*.cpp may name
 * exactly one, which is what makes it a source rather than part of the core.
 */
struct VfxPoseSource {
    const char *name;              /* the [vfx1] posesource value selecting it */
    bool (*open)(void);
    void (*close)(void);
    bool (*poll)(VfxPose *out);    /* false when no pose is available */
};

/* Each lives in its own translation unit. */
const VfxPoseSource *VFX1_Source_None(void);
const VfxPoseSource *VFX1_Source_Script(void);
const VfxPoseSource *VFX1_Source_Net(void);

/* Called once from MOUSE_Init(), after INT 33h emulation exists. Reads the
 * [vfx1] config section and, if enabled, starts feeding head-tracking motion
 * into the mouse interface. */
void VFX1_Init(void);

/* Make wherever the user is looking now the new forward. Bound to a mapper
 * key, and safe to call before VFX1_Init(). */
void VFX1_Mouse_Recenter(void);

/* True when head tracking is enabled and a pose source is open. Widening
 * DOSBox-X's two cursor-lock gates keys off this, so it must stay cheap. */
bool VFX1_Mouse_Active(void);

#endif /* DOSBOX_VFX1_H */
