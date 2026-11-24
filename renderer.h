#ifndef _RENDERER_H_
#define _RENDERER_H_

#include <psp2/display.h>

void updateFramebuf(const SceDisplayFrameBuf *param);
void drawCharacter(int character, int x, int y);
void drawString(int x, int y, const char *str);
void drawStringF(int x, int y, const char *format, ...) __attribute__((format(printf, 3, 4)));
void setTextColor(uint32_t clr);

#endif
