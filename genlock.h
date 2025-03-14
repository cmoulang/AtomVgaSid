/*

genlock implentation

Copyright 2021-2025 David Banks

This file is part of AtomVgaSid

AtomVgaSid is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

AtomVgaSid is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
AtomVgaSid. If not, see <https://www.gnu.org/licenses/>.

*/


#ifndef INCLUDE_GENLOCK_H
#define INCLUDE_GENLOCK_H

// With an Atom line of 63.695us (Nomimal Atom)
// 251.000000 (refdiv = 2; fbdiv = 251, vco = 1506 pd1 = 6, pd2 = 1, clkdiv = 0x4ff (1279), ppm =   3.44
#define SYS_FREQ 251000

// If REFDIV is other than 1, then specify other parameters for pll_init()
#define REFDIV 2
#define VCO 1506000000
#define PD1 6
#define PD2 1

#define SCANVIDEO_PIO         pio0
#define SCANVIDEO_SCANLINE_SM 0
#define SCANVIDEO_TIMING_SM   3

#define GENLOCK_PIO           pio0
#define GENLOCK_SM            1

#ifndef USE_SCANVIDEO_PRIVATE
#define USE_SCANVIDEO_PRIVATE 1
#endif

typedef enum {
    GENLOCK_OFF,
    GENLOCK_VARY_CLK,
#if USE_SCANVIDEO_PRIVATE
    GENLOCK_VARY_HTOTAL,
    GENLOCK_VARY_VTOTAL_ALT1,
    GENLOCK_VARY_VTOTAL_ALT2,
    GENLOCK_VARY_VTOTAL_ALT3,
#endif
    GENLOCK_NUM_MODES
} genlock_mode_t;

typedef struct genlock {
    // Mode number
    int mode;
    // Target
    int vsync_target;
    // PD controller parameters
    int nominal;
    int min;
    int max;
    double coeff_a;
    double coeff_b;
    // Functions
    void (*init)(struct genlock *);
    void (*post_init)(struct genlock *);
    void (*process_line)(struct genlock *, int line);
    void (*update)(struct genlock *, int value);
    void (*destroy)(struct genlock *);
    // Variables
    double current;
} genlock_t;

extern scanvideo_mode_t custom_mode;

/// @brief Load PIO state machine to track the offset between VGA VS and Atom FS
void genlock_initialize_pio();

void genlock_initialize();

void genlock_debug(int on);

genlock_t *genlock_factory(genlock_mode_t mode);

#endif
