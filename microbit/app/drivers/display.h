/*
 * display.h - micro:bit v2 5x5 LED matrix driver.
 *
 * The matrix is a true 5x5 grid: each row is an anode (driven high) and
 * each column a cathode (driven low to light). Only one row is lit at a
 * time, so a background task scans all five rows fast enough for
 * persistence of vision. display_init() starts that task.
 *
 * Framebuffer coordinates: x = column (0..4, left->right),
 *                          y = row    (0..4, top->bottom).
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <tk/tkernel.h>

/* Configure the matrix GPIO and start the refresh task. */
void display_init(void);
/* Application callers may keep visual refresh below a mesh owner.  Returns
 * nonzero only after the refresh task has been created and started. */
int display_init_with_priority(UINT priority);

void display_clear(void);
void display_fill(void);                       /* all 25 LEDs on */
void display_set_pixel(INT x, INT y, BOOL on);

/* Show one immutable 5x5 role glyph. Valid digits are 1 through 6. */
void display_show_digit(UINT digit);

/* Synchronously scan a role glyph, then turn the matrix fully off.  This
 * intentionally creates no display task and is used before routed radio
 * initialization by benchmark firmware. */
void display_show_benchmark_role(UINT role, UINT scan_ms);

/* Replace the whole framebuffer. rows[y] is a 5-bit mask; bit x lights
   the pixel at column x. */
void display_set_rows(const UB rows[5]);

#endif /* DISPLAY_H */
