/*
 * vfxmenu.h -- a DOS-style text menu drawn into a pixel buffer.
 *
 * The in-headset launcher's menu (docs/ARCHITECTURE.md): a blue 80x25 text
 * screen in the VGA 8x16 font, with a title, a scrolling list of items and a
 * footer. Items are actions, or choices whose value steps left and right.
 * The host builds pages from its own state, feeds navigation input, acts on
 * the events that come back, and draws the buffer onto a quad in its world.
 *
 * Nothing here knows about OpenXR, WebXR, libretro or files, so the native app,
 * the WebXR page and a future virtual room can all use it. Plain C99.
 */
#ifndef VFX_VFXMENU_H
#define VFX_VFXMENU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VFXMENU_COLS 80
#define VFXMENU_ROWS 25
#define VFXMENU_WIDTH (VFXMENU_COLS * 8)       /* 640 */
#define VFXMENU_HEIGHT (VFXMENU_ROWS * 16)     /* 400 */
#define VFXMENU_MAX_ITEMS 512
#define VFXMENU_LABEL 64
#define VFXMENU_VALUE 40

typedef enum VfxMenuKind {
    VFXMENU_ACTION,         /* activate */
    VFXMENU_CHOICE,         /* value steps left and right; activate steps right */
    VFXMENU_HEADING         /* not selectable */
} VfxMenuKind;

typedef struct VfxMenuItem {
    VfxMenuKind kind;
    int id;                 /* the host's meaning */
    char label[VFXMENU_LABEL];
    char value[VFXMENU_VALUE];
} VfxMenuItem;

typedef struct VfxMenu {
    char title[VFXMENU_COLS + 1];
    char footer[VFXMENU_COLS + 1];
    char status[VFXMENU_COLS + 1];      /* one line above the footer */
    VfxMenuItem items[VFXMENU_MAX_ITEMS];
    int count, selected, scroll;
} VfxMenu;

typedef struct VfxMenuInput {
    int up, down, left, right, activate, back;   /* 1 on the frame it happens */
} VfxMenuInput;

typedef enum VfxMenuEventType {
    VFXMENU_EVENT_NONE,
    VFXMENU_EVENT_ACTIVATE,     /* item: an action or choice was activated */
    VFXMENU_EVENT_LEFT,         /* item: step a choice down */
    VFXMENU_EVENT_RIGHT,        /* item: step a choice up */
    VFXMENU_EVENT_BACK
} VfxMenuEventType;

typedef struct VfxMenuEvent {
    VfxMenuEventType type;
    int item;                   /* index into items, -1 for none */
    int id;
} VfxMenuEvent;

/* Start a page: clears the items, keeps nothing but what the host sets. */
void vfxmenu_begin(VfxMenu *m, const char *title, const char *footer);
/* Returns the new item's index, or -1 when full. */
int vfxmenu_add(VfxMenu *m, VfxMenuKind kind, int id, const char *label, const char *value);
void vfxmenu_set_status(VfxMenu *m, const char *status);
/* Select the first selectable item at or after index. */
void vfxmenu_select(VfxMenu *m, int index);

VfxMenuEvent vfxmenu_input(VfxMenu *m, const VfxMenuInput *in);

/* Draw the whole screen into RGBA, VFXMENU_WIDTH x VFXMENU_HEIGHT. */
void vfxmenu_draw(const VfxMenu *m, uint8_t *rgba);

/* Draw one character cell, for hosts that want text elsewhere. */
void vfxmenu_draw_char(uint8_t *rgba, int width, int height, int col, int row,
                       unsigned char c, const uint8_t fg[3], const uint8_t bg[3]);

/* Repeats a held direction: 1 on the press, then every `rate` seconds after
 * `delay`. Feed it the held state and the frame time. */
typedef struct VfxMenuRepeat { int held; double t; } VfxMenuRepeat;
int vfxmenu_repeat(VfxMenuRepeat *r, int held, double dt, double delay, double rate);

#ifdef __cplusplus
}
#endif

#endif /* VFX_VFXMENU_H */
