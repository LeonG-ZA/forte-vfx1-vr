/* test_vfxmenu.c -- the shared DOS-style menu: navigation, events, drawing. */
#include "vfxtest.h"
#include "vfx/vfxmenu.h"

#include <string.h>

static VfxMenu menu;
static uint8_t canvas[VFXMENU_WIDTH * VFXMENU_HEIGHT * 4];

static VfxMenuInput none(void) { VfxMenuInput in; memset(&in, 0, sizeof in); return in; }

static void test_navigation(void)
{
    VfxMenuInput in = none();
    VfxMenuEvent ev;
    vfxmenu_begin(&menu, "VFX1 Quest", "A select  B back");
    vfxmenu_add(&menu, VFXMENU_HEADING, 0, "Games", NULL);
    vfxmenu_add(&menu, VFXMENU_ACTION, 10, "descent-vfx1.zip", NULL);
    vfxmenu_add(&menu, VFXMENU_ACTION, 11, "quake106.zip", NULL);
    vfxmenu_add(&menu, VFXMENU_HEADING, 0, "Settings", NULL);
    vfxmenu_add(&menu, VFXMENU_CHOICE, 20, "Puck mouse", "off");
    vfxmenu_select(&menu, 0);
    CHECK(menu.selected == 1);                      /* headings are skipped */

    in.down = 1;
    vfxmenu_input(&menu, &in);
    CHECK(menu.selected == 2);
    ev = vfxmenu_input(&menu, &in);
    CHECK(menu.selected == 4 && ev.type == VFXMENU_EVENT_NONE);    /* over the heading */
    vfxmenu_input(&menu, &in);
    CHECK(menu.selected == 4);                      /* stops at the end */

    in = none(); in.right = 1;
    ev = vfxmenu_input(&menu, &in);
    CHECK(ev.type == VFXMENU_EVENT_RIGHT && ev.id == 20 && ev.item == 4);
    in = none(); in.left = 1;
    ev = vfxmenu_input(&menu, &in);
    CHECK(ev.type == VFXMENU_EVENT_LEFT);

    in = none(); in.up = 1;
    vfxmenu_input(&menu, &in);
    in = none(); in.right = 1;
    ev = vfxmenu_input(&menu, &in);
    CHECK(ev.type == VFXMENU_EVENT_NONE);           /* actions ignore left and right */
    in = none(); in.activate = 1;
    ev = vfxmenu_input(&menu, &in);
    CHECK(ev.type == VFXMENU_EVENT_ACTIVATE && ev.id == 11);
    in = none(); in.back = 1;
    ev = vfxmenu_input(&menu, &in);
    CHECK(ev.type == VFXMENU_EVENT_BACK);
}

static void test_scrolling(void)
{
    VfxMenuInput in = none();
    int i;
    char name[32];
    vfxmenu_begin(&menu, "Games", "");
    for (i = 0; i < 50; i++) {
        snprintf(name, sizeof name, "game%02d.zip", i);
        vfxmenu_add(&menu, VFXMENU_ACTION, i, name, NULL);
    }
    vfxmenu_select(&menu, 0);
    in.down = 1;
    for (i = 0; i < 30; i++) vfxmenu_input(&menu, &in);
    CHECK(menu.selected == 30);
    CHECK(menu.scroll <= 30 && menu.scroll + 20 > 30);
    for (i = 0; i < 100; i++) vfxmenu_input(&menu, &in);
    CHECK(menu.selected == 49 && menu.scroll == 30);
}

static int pixel_is(int x, int y, int r, int g, int b)
{
    const uint8_t *p = canvas + (y * VFXMENU_WIDTH + x) * 4;
    return p[0] == r && p[1] == g && p[2] == b && p[3] == 255;
}

static void test_drawing(void)
{
    int x, y, lit = 0;
    vfxmenu_begin(&menu, "VFX1 Quest", "footer");
    vfxmenu_add(&menu, VFXMENU_ACTION, 1, "Resume", NULL);
    vfxmenu_select(&menu, 0);
    memset(canvas, 0x55, sizeof canvas);
    vfxmenu_draw(&menu, canvas);
    CHECK(pixel_is(VFXMENU_WIDTH - 1, 10 * 16, 0, 0, 170));      /* blue background */
    CHECK(pixel_is(0, 0, 0, 170, 170));                           /* cyan title bar */
    /* the title's "V" at column 1 has black pixels in its cell */
    for (y = 0; y < 16; y++)
        for (x = 8; x < 16; x++)
            if (pixel_is(x, y, 0, 0, 0)) lit++;
    CHECK(lit > 8);
    /* the selected row is grey behind its text */
    CHECK(pixel_is(3 * 8, 2 * 16, 170, 170, 170));
}

static void test_repeat(void)
{
    VfxMenuRepeat r = { 0, 0 };
    int i, fires = 0;
    CHECK(vfxmenu_repeat(&r, 1, 0.014, 0.4, 0.1) == 1);          /* on press */
    for (i = 0; i < 71; i++) fires += vfxmenu_repeat(&r, 1, 1.0 / 72, 0.4, 0.1);
    CHECK(fires >= 5 && fires <= 7);                              /* about a second held */
    CHECK(vfxmenu_repeat(&r, 0, 0.014, 0.4, 0.1) == 0);
    CHECK(vfxmenu_repeat(&r, 1, 0.014, 0.4, 0.1) == 1);
}

int main(void)
{
    test_navigation();
    test_scrolling();
    test_drawing();
    test_repeat();
    TEST_MAIN_END();
}
