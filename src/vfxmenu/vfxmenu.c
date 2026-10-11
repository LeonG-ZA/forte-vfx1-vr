/*
 * vfxmenu.c -- see include/vfx/vfxmenu.h.
 */
#include "vfx/vfxmenu.h"

#include <stdio.h>
#include <string.h>

extern const uint8_t vfxmenu_font16[256 * 16];

/* The classic DOS palette entries the screen uses. */
static const uint8_t BLUE[3] = { 0, 0, 170 };
static const uint8_t GREY[3] = { 170, 170, 170 };
static const uint8_t WHITE[3] = { 255, 255, 255 };
static const uint8_t YELLOW[3] = { 255, 255, 85 };
static const uint8_t CYAN[3] = { 0, 170, 170 };
static const uint8_t BLACK[3] = { 0, 0, 0 };
static const uint8_t LIGHTCYAN[3] = { 85, 255, 255 };

#define LIST_TOP 2
#define LIST_ROWS 20                /* rows 2..21 */
#define STATUS_ROW 23
#define FOOTER_ROW 24

static void copy_text(char *dst, size_t size, const char *src)
{
    snprintf(dst, size, "%s", src ? src : "");
}

void vfxmenu_begin(VfxMenu *m, const char *title, const char *footer)
{
    copy_text(m->title, sizeof m->title, title);
    copy_text(m->footer, sizeof m->footer, footer);
    m->status[0] = 0;
    m->count = 0;
    m->selected = 0;
    m->scroll = 0;
}

int vfxmenu_add(VfxMenu *m, VfxMenuKind kind, int id, const char *label, const char *value)
{
    VfxMenuItem *it;
    if (m->count >= VFXMENU_MAX_ITEMS) return -1;
    it = &m->items[m->count];
    it->kind = kind;
    it->id = id;
    copy_text(it->label, sizeof it->label, label);
    copy_text(it->value, sizeof it->value, value);
    return m->count++;
}

void vfxmenu_set_status(VfxMenu *m, const char *status)
{
    copy_text(m->status, sizeof m->status, status);
}

static int selectable(const VfxMenu *m, int i)
{
    return i >= 0 && i < m->count && m->items[i].kind != VFXMENU_HEADING;
}

static void keep_visible(VfxMenu *m)
{
    if (m->selected < m->scroll) m->scroll = m->selected;
    if (m->selected >= m->scroll + LIST_ROWS) m->scroll = m->selected - LIST_ROWS + 1;
    /* show a heading just above the first item */
    if (m->scroll > 0 && m->selected == m->scroll && m->items[m->scroll - 1].kind == VFXMENU_HEADING) m->scroll--;
    if (m->scroll < 0) m->scroll = 0;
}

void vfxmenu_select(VfxMenu *m, int index)
{
    int i;
    for (i = index < 0 ? 0 : index; i < m->count; i++)
        if (selectable(m, i)) { m->selected = i; keep_visible(m); return; }
    for (i = 0; i < m->count; i++)
        if (selectable(m, i)) { m->selected = i; keep_visible(m); return; }
    m->selected = 0;
}

static void move(VfxMenu *m, int dir)
{
    int i = m->selected;
    do {
        i += dir;
        if (i < 0 || i >= m->count) return;      /* stop at the ends */
    } while (!selectable(m, i));
    m->selected = i;
    keep_visible(m);
}

VfxMenuEvent vfxmenu_input(VfxMenu *m, const VfxMenuInput *in)
{
    VfxMenuEvent ev = { VFXMENU_EVENT_NONE, -1, 0 };
    if (!selectable(m, m->selected)) vfxmenu_select(m, m->selected);
    if (in->up) move(m, -1);
    if (in->down) move(m, 1);
    if (in->back) { ev.type = VFXMENU_EVENT_BACK; return ev; }
    if (!selectable(m, m->selected)) return ev;
    ev.item = m->selected;
    ev.id = m->items[m->selected].id;
    if (in->activate) ev.type = VFXMENU_EVENT_ACTIVATE;
    else if (in->left && m->items[m->selected].kind == VFXMENU_CHOICE) ev.type = VFXMENU_EVENT_LEFT;
    else if (in->right && m->items[m->selected].kind == VFXMENU_CHOICE) ev.type = VFXMENU_EVENT_RIGHT;
    else ev.item = -1;
    return ev;
}

void vfxmenu_draw_char(uint8_t *rgba, int width, int height, int col, int row,
                       unsigned char c, const uint8_t fg[3], const uint8_t bg[3])
{
    const uint8_t *glyph = vfxmenu_font16 + (size_t)c * 16u;
    int x, y;
    if (col < 0 || row < 0 || (col + 1) * 8 > width || (row + 1) * 16 > height) return;
    for (y = 0; y < 16; y++) {
        uint8_t *p = rgba + ((size_t)(row * 16 + y) * (size_t)width + (size_t)col * 8u) * 4u;
        for (x = 0; x < 8; x++, p += 4) {
            const uint8_t *c3 = (glyph[y] & (0x80 >> x)) ? fg : bg;
            p[0] = c3[0]; p[1] = c3[1]; p[2] = c3[2]; p[3] = 255;
        }
    }
}

static void text(uint8_t *rgba, int col, int row, const char *s, int max_cols,
                 const uint8_t fg[3], const uint8_t bg[3])
{
    int i;
    for (i = 0; s[i] && i < max_cols && col + i < VFXMENU_COLS; i++)
        vfxmenu_draw_char(rgba, VFXMENU_WIDTH, VFXMENU_HEIGHT, col + i, row, (unsigned char)s[i], fg, bg);
}

static void fill_row(uint8_t *rgba, int row, int from, int to, const uint8_t bg[3])
{
    int c;
    for (c = from; c < to; c++) vfxmenu_draw_char(rgba, VFXMENU_WIDTH, VFXMENU_HEIGHT, c, row, ' ', bg, bg);
}

void vfxmenu_draw(const VfxMenu *m, uint8_t *rgba)
{
    int row, c;
    char line[128];

    for (row = 0; row < VFXMENU_ROWS; row++) fill_row(rgba, row, 0, VFXMENU_COLS, BLUE);

    /* title bar */
    fill_row(rgba, 0, 0, VFXMENU_COLS, CYAN);
    snprintf(line, sizeof line, " %s", m->title);
    text(rgba, 0, 0, line, VFXMENU_COLS, BLACK, CYAN);

    /* items */
    for (row = 0; row < LIST_ROWS; row++) {
        int i = m->scroll + row;
        const VfxMenuItem *it;
        int sel, value_len, value_col;
        const uint8_t *fg, *bg;
        if (i >= m->count) break;
        it = &m->items[i];
        sel = i == m->selected;
        if (it->kind == VFXMENU_HEADING) {
            text(rgba, 2, LIST_TOP + row, it->label, VFXMENU_COLS - 4, YELLOW, BLUE);
            continue;
        }
        fg = sel ? BLACK : GREY;
        bg = sel ? GREY : BLUE;
        if (sel) fill_row(rgba, LIST_TOP + row, 3, VFXMENU_COLS - 3, GREY);
        text(rgba, 4, LIST_TOP + row, it->label, 44, sel ? BLACK : WHITE, bg);
        if (it->value[0]) {
            if (it->kind == VFXMENU_CHOICE) snprintf(line, sizeof line, "< %s >", it->value);
            else snprintf(line, sizeof line, "%s", it->value);
            value_len = (int)strlen(line);
            if (value_len > 28) value_len = 28;
            value_col = VFXMENU_COLS - 4 - value_len;
            text(rgba, value_col, LIST_TOP + row, line, value_len, fg, bg);
        }
    }

    /* scroll marks */
    if (m->scroll > 0) vfxmenu_draw_char(rgba, VFXMENU_WIDTH, VFXMENU_HEIGHT, VFXMENU_COLS - 2, LIST_TOP, 24, LIGHTCYAN, BLUE);
    if (m->scroll + LIST_ROWS < m->count)
        vfxmenu_draw_char(rgba, VFXMENU_WIDTH, VFXMENU_HEIGHT, VFXMENU_COLS - 2, LIST_TOP + LIST_ROWS - 1, 25, LIGHTCYAN, BLUE);

    /* status and footer */
    text(rgba, 2, STATUS_ROW, m->status, VFXMENU_COLS - 4, YELLOW, BLUE);
    fill_row(rgba, FOOTER_ROW, 0, VFXMENU_COLS, CYAN);
    for (c = 0; m->footer[c] && c < VFXMENU_COLS - 1; c++) line[c] = m->footer[c];
    line[c] = 0;
    text(rgba, 1, FOOTER_ROW, line, VFXMENU_COLS - 1, BLACK, CYAN);
}

int vfxmenu_repeat(VfxMenuRepeat *r, int held, double dt, double delay, double rate)
{
    if (!held) { r->held = 0; r->t = 0; return 0; }
    if (!r->held) { r->held = 1; r->t = -delay; return 1; }
    r->t += dt;
    if (r->t >= rate) { r->t -= rate; return 1; }
    return 0;
}
