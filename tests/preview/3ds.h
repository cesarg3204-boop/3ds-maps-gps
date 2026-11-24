/* Minimal stand-in for libctru so the UI code can be drawn on a PC (preview only). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32; typedef int16_t s16;
typedef enum { GFX_TOP, GFX_BOTTOM } gfxScreen_t;
typedef enum { GFX_LEFT } gfx3dSide_t;
typedef struct { u8 *gfx; u16 asciiOffset, numChars; } ConsoleFont;
typedef struct { ConsoleFont font; } PrintConsole;
PrintConsole *consoleGetDefault(void);
u8 *gfxGetFramebuffer(gfxScreen_t s, gfx3dSide_t side, u16 *w, u16 *h);
u64 osGetTime(void);
typedef struct { u16 px, py; } touchPosition;
typedef struct { s16 dx, dy; } circlePosition;
enum { KEY_A=1, KEY_B=2, KEY_SELECT=4, KEY_START=8, KEY_DRIGHT=16, KEY_DLEFT=32, KEY_DUP=64, KEY_DDOWN=128,
  KEY_R=256, KEY_L=512, KEY_X=1024, KEY_Y=2048, KEY_ZL=1<<14, KEY_ZR=1<<15, KEY_TOUCH=1<<20,
  KEY_UP=KEY_DUP, KEY_DOWN=KEY_DDOWN };
typedef struct { int dummy; } SwkbdState;
typedef enum { SWKBD_BUTTON_LEFT, SWKBD_BUTTON_MIDDLE, SWKBD_BUTTON_RIGHT, SWKBD_BUTTON_NONE } SwkbdButton;
enum { SWKBD_TYPE_WESTERN, SWKBD_NOTEMPTY_NOTBLANK };
static inline void swkbdInit(SwkbdState *s, int t, int b, int m) {(void)s;(void)t;(void)b;(void)m;}
static inline void swkbdSetHintText(SwkbdState *s, const char *t) {(void)s;(void)t;}
static inline void swkbdSetButton(SwkbdState *s, int b, const char *t, bool x) {(void)s;(void)b;(void)t;(void)x;}
static inline void swkbdSetValidation(SwkbdState *s, int a, int b, int c) {(void)s;(void)a;(void)b;(void)c;}
static inline void swkbdSetInitialText(SwkbdState *s, const char *t) {(void)s;(void)t;}
static inline SwkbdButton swkbdInputText(SwkbdState *s, char *b, int n) {(void)s;(void)b;(void)n;return SWKBD_BUTTON_LEFT;}
static inline void hidScanInput(void) {}
static inline u32 hidKeysDown(void) {return 0;}
static inline u32 hidKeysHeld(void) {return 0;}
static inline void hidTouchRead(touchPosition *t) {t->px=t->py=0;}
static inline void hidCircleRead(circlePosition *c) {c->dx=c->dy=0;}
static inline bool aptMainLoop(void) {return false;}
static inline void gfxInitDefault(void) {}
static inline void gfxSet3D(bool b) {(void)b;}
static inline void gfxFlushBuffers(void) {}
static inline void gfxSwapBuffers(void) {}
static inline void gspWaitForVBlank(void) {}
static inline void gfxExit(void) {}
static inline int romfsInit(void) {return 0;}
static inline int romfsExit(void) {return 0;}
static inline int socInit(u32 *b, u32 n) {(void)b;(void)n;return 0;}
static inline int socExit(void) {return 0;}
static inline void osSetSpeedupEnable(bool b) {(void)b;}
