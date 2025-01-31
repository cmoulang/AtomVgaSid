/*

6809 Dragon/Coco support by Phill Harvey-Smith
Godil 80 column and genlock support by David Banks

Copyright 2021-2025 Chris Moulang

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

#ifndef ATOMVGA_H_
#define ATOMVGA_H_

// Option for two levels of orange: normal for semi-graphics and bright for text
#ifndef INCLUDE_BRIGHT_ORANGE
#define INCLUDE_BRIGHT_ORANGE 0
#endif

#define RED_1 0x0400
#define RED_2 0x0800
#define RED 0x0C00

#define GREEN_1 0x1000
#define GREEN_2 0x2000
#define GREEN 0x3000

#define BLUE_1 0x4000
#define BLUE_2 0x8000
#define BLUE 0xC000

#define BLACK 0
#define YELLOW (RED | GREEN)
#define YELLOW_1 (RED_1 | GREEN_1)
#define WHITE (RED | GREEN | BLUE)
#define WHITE_1 (RED_1 | GREEN_1 | BLUE_1)
#define WHITE_2 (WHITE_1 << 1)
#define COLOUR YELLOW
#define CYAN (GREEN | BLUE)
#if INCLUDE_BRIGHT_ORANGE
#define ORANGE (RED | GREEN_1)
#define BRIGHT_ORANGE (RED | GREEN_2)
#else
#define ORANGE (RED | GREEN_2)
#endif
#define MAGENTA (RED | BLUE)

#define NO_COLOURS  (9+INCLUDE_BRIGHT_ORANGE)
uint16_t colour_palette_atom[NO_COLOURS] = {
    GREEN,
    YELLOW,
    BLUE,
    RED,
    WHITE,
    CYAN,
    MAGENTA,
    ORANGE,
    BLACK
#if INCLUDE_BRIGHT_ORANGE
    , BRIGHT_ORANGE
#endif
};

#define IDX_GREEN   0
#define IDX_YELLOW  1
#define IDX_BLUE    2
#define IDX_RED     3
#define IDX_WHITE   4
#define IDX_CYAN    5   
#define IDX_MAGENTA 6
#define IDX_ORANGE  7
#define IDX_BLACK   8

#define MAX_COLOUR (8+INCLUDE_BRIGHT_ORANGE)

#if INCLUDE_BRIGHT_ORANGE
#define IDX_BRIGHT_ORANGE 9
#endif


#define DEF_INK     GREEN 
#define DEF_PAPER   BLACK

#if INCLUDE_BRIGHT_ORANGE
#define DEF_INK_ALT BRIGHT_ORANGE
#else
#define DEF_INK_ALT ORANGE
#endif

uint16_t colour_palette_vga80[8] = {
    BLACK,
    BLUE,
    GREEN,
    CYAN,
    RED,
    MAGENTA,
    YELLOW,
    WHITE
};

uint16_t colour_palette_improved[4] = {
    BLACK,
    YELLOW,
    GREEN,
    MAGENTA,
};

uint16_t colour_palette_artifact1[4] = {
    BLACK,
    BLUE,
    ORANGE,
    WHITE,
};

uint16_t colour_palette_artifact2[4] = {
    BLACK,
    ORANGE,
    BLUE,
    WHITE,
};


// Masks to extract pixel colours from SG4 and SG6 bytes
#define SG4_COL_MASK    0x70
#define SG6_COL_MASK    0xC0

#define SG4_COL_SHIFT   4
#define SG6_COL_SHIFT   6

// Masks to extact bit paterns from SG4 and SG6
#define SG4_PAT_MASK    0x0F
#define SG6_PAT_MASK    0x3F

// Bytes / char array for text / semigraphics modes
// SG mode                     4  8  12 24  6
const uint sg_bytes_row[5]  = {1, 4, 6, 12, 1};

#define TEXT_INDEX  0
#define SG4_INDEX   0
#define SG8_INDEX   1
#define SG12_INDEX  2
#define SG24_INDEX  3
#define SG6_INDEX   4

uint16_t *colour_palette = colour_palette_atom;

//                             0  1a   1   2a    2   3a    3   4a    4
//                             0  1    2    3    4    5    6    7    8
const uint width_lookup[9] =  {32, 64, 128, 128, 128, 128, 128, 128, 256};
const uint height_lookup[9] = {16, 64,  64,  64,  96,  96, 192, 192, 192};

inline bool is_colour(uint mode);

uint get_width(uint mode)
{
    uint m = (mode + 1) / 2;
    return width_lookup[m];
};

uint get_height(uint mode)
{
    uint m = (mode + 1) / 2;
    return height_lookup[m];
};

uint bytes_per_row(uint mode)
{
    return is_colour(mode) ? get_width(mode) * 2 / 8 : get_width(mode) / 8;
};

#define COL80_OFF   0x00
#define COL80_ON    0x80
#define COL80_ATTR  0x08

#endif /* ATOMVGA_H_ */
