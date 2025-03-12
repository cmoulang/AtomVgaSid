/*

Main program

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
#include "atom_if.h"
#include "atom_sid.h"
#include "atom_term.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "pico.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"
#include "pico/sync.h"
#include "hardware/irq.h"
#include "hardware/vreg.h"
#include "hardware/pll.h"
#include "hardware/clocks.h"
#include "atomvga.h"
#include "fonts.h"
#include "platform.h"
#if (PLATFORM == PLATFORM_DRAGON)
#include "eeprom.h"
#endif

#define YARRB_REG0 0xBFFE
#define YARRB_4MHZ 0x20
#define VIA_DA 0xB801

// PIA and frambuffer address moved into platform.h -- PHS

#ifdef GENLOCK

// GENLOCK code included

#include "genlock.h"

#define vga_mode custom_mode

static genlock_mode_t genlock_setting = GENLOCK_OFF;

#else

// GENLOCK code not included, default to original clock
#define vga_mode vga_mode_640x480_60
#define SYS_FREQ 250000

#endif

static uint32_t vga80_lut[128 * 4];

volatile uint8_t fontno = DEFAULT_FONT;
volatile uint8_t max_lower = LOWER_END;

volatile uint16_t ink = DEF_INK;
volatile uint16_t ink_alt = DEF_INK_ALT;
volatile uint16_t paper = DEF_PAPER;

volatile uint8_t autoload = 0;

volatile uint8_t artifact = 0;

volatile bool reset_flag = false;

void reset_vga80();

void core1_func();

static semaphore_t video_initted;

bool updated;

#define CSI "\x1b["

void load_ee(void);

// Returns video mode as far as VDG is concerned, with the bits :
//  b3  b2  b1  b0
//  GM2 GM1 GM0 A/G
int get_mode()
{
#if (PLATFORM == PLATFORM_ATOM)
    return (eb_get(PIA_ADDR) & 0xf0) >> 4;
#elif (PLATFORM == PLATFORM_DRAGON)
    return ((eb_get(PIA_ADDR) & 0x80) >> 7) | ((eb_get(PIA_ADDR) & 0x70) >> 3);
#endif
}

#if (PLATFORM == PLATFORM_ATOM)
 
struct alt_colour_q_elem {
    absolute_time_t timeout;
    bool value;
};

typedef struct alt_colour_q_elem alt_colour_q_elem_t;

queue_t _ac_q;
volatile int _ac_offset;

/// @brief initialis the alt colour variables
static void alt_colour_init()
{
    queue_init(&_ac_q, sizeof (alt_colour_q_elem_t), 10);
    _ac_offset = 31500; // Not quite two frames delay
}

int read_vsync_offset();

/// @brief update the alt colour flag but delayed until the correct line in the next frame
/// @param data 
static inline void alt_colour_update(uint8_t data)
{
    static bool next_value=false;
    bool x = (data & 0x8);
    if (x != next_value)
    {
        alt_colour_q_elem_t e;
        next_value = x;
        int offset = read_vsync_offset();
        e.timeout =  make_timeout_time_us(_ac_offset - offset);
        e.value = x;
        queue_try_add(&_ac_q, &e);
    }
}

static inline bool alt_colour()
{
    static bool current_value = false;
    alt_colour_q_elem_t e;
    if (queue_try_peek(&_ac_q, &e))
    {
        if (get_absolute_time() > e.timeout)
        {
            current_value = e.value;
            queue_remove_blocking(&_ac_q, &e);
        }
    }
    return current_value;
}
#elif (PLATFORM == PLATFORM_DRAGON)
static inline bool alt_colour()
{
    return (eb_get(PIA_ADDR) & 0x08);
}
#endif


inline bool is_artifact(uint mode)
{
    return ((0 != artifact) && (0x0F == mode)) ? true : false;
}

// Treat artifacted pmode 4 as pmode 3 with a different palette
inline bool is_colour(uint mode)
{
    return !(mode & 0b10);
};

const uint debug_text_len = 32;
char debug_text[33];
bool debug = false;

void set_debug_text(char *text)
{
    strncpy(debug_text, text, debug_text_len);

    for (unsigned int i = strnlen(text, debug_text_len); i < debug_text_len; i++)
    {
        debug_text[i] = ' ';
    }

    for (unsigned int i = 0; i < debug_text_len; i++)
    {
        unsigned char c = debug_text[i];

#if (PLATFORM == PLATFORM_ATOM)
        c = c + 0x20;
        if (c < 0x80)
        {
            c = c ^ 0x60;
        }
#else
        if ((c >= 0x20) && (c <= 0x3F))
        {
            c = c + 0x40;
        }
        else if ((c >= 0x60) && (c <= 0x7F))
        {
            c = c - 0x60;
        }
#endif
        debug_text[i] = c;
    }
}

#define DEBUG_BUF_SIZE 80

void update_debug_text()
{
    if (debug)
    {
        char buffer[DEBUG_BUF_SIZE];
        uint mode = get_mode();

        uint bytes = bytes_per_row(mode) * get_height(mode);

        uint m = (mode + 1) / 2;
#if (PLATFORM == PLATFORM_ATOM)
        snprintf(buffer, DEBUG_BUF_SIZE, "mode %x/%d %dx%d %s %d",
                 mode,
                 m,
                 get_width(mode),
                 get_height(mode),
                 is_colour(mode) ? "col" : "b&w",
                 bytes);
#elif (PLATFORM == PLATFORM_DRAGON)
        snprintf(buffer, DEBUG_BUF_SIZE, "mode %x/%d %dx%d %s %d %04X %02X",
                 mode,
                 m,
                 get_width(mode),
                 get_height(mode),
                 is_colour(mode) ? "col" : "b&w",
                 bytes,
                 SAMBits,
                 artifact);
#endif
        set_debug_text(buffer);
    }
    else
    {
        set_debug_text("");
    }
}

#if (PLATFORM == PLATFORM_ATOM)

#define _command_buffer_length 30
char _command_buffer[_command_buffer_length];

bool is_command(char *cmd,
                char **params)
{
    eb_get_chars(_command_buffer, _command_buffer_length, CMD_BASE);
    char *p = (char *)_command_buffer;
    *params = (char *)NULL;

    while (*cmd != 0)
    {
        if (*cmd++ != *p++)
        {
            return false;
        }
    }

    if ((ATOM_EOL == *p) || (SPACE == *p))
    {
        *params = p;
        return true;
    }
    return false;
}

bool uint8_param(char *params,
                 int *output,
                 int min,
                 int max)
{
    int try;
    // DMB: Avoid sscanf, it bloats the uf2 size by 100KB
    // if (sscanf(params, "%d", &try))
    char *end;
    try = strtol(params, &end, 10);
    if (end > params)
    {
        if ((try >= min) && (try <= max))
        {
            *output = try;
            return true;
        }
    }
    return false;
}

#endif

void switch_font(uint8_t new_font)
{
    uint8_t font_range;

    // make sure new fontno is valid.....
    fontno = (new_font < FONT_COUNT) ? new_font : DEFAULT_FONT;

    // Calculate range of available lower case symbols
    font_range = fonts[fontno].last_upper - fonts[fontno].first_upper;

    max_lower = (font_range < LOWER_RANGE) ? LOWER_START + font_range : LOWER_END;
}

void switch_colour(uint8_t newcolour,
                   volatile uint16_t *tochange)
{
    if (newcolour < NO_COLOURS)
    {
        *tochange = colour_palette_atom[newcolour];
    }
}

volatile bool support_lower = false;

void print_str(int line_num, char *str)
{
    printf("%s\n", str);
    set_debug_text(str);
    eb_set_chars(GetVidMemBase() + 0x020 * line_num, debug_text, 32);
}

void set_sys_clock_pll_refdiv(uint refdiv, uint32_t vco_freq, uint post_div1, uint post_div2)
{
    if (!running_on_fpga())
    {
        clock_configure(clk_sys,
                        CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                        CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                        48 * MHZ,
                        48 * MHZ);

        pll_init(pll_sys, refdiv, vco_freq, post_div1, post_div2);
        uint32_t freq = vco_freq / (post_div1 * post_div2);

        // Configure clocks
        // CLK_REF = XOSC (12MHz) / 1 = 12MHz
        clock_configure(clk_ref,
                        CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC,
                        0, // No aux mux
                        12 * MHZ,
                        12 * MHZ);

        // CLK SYS = PLL SYS (125MHz) / 1 = 125MHz
        clock_configure(clk_sys,
                        CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                        CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                        freq, freq);

        clock_configure(clk_peri,
                        0, // Only AUX mux on ADC
                        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                        48 * MHZ,
                        48 * MHZ);
    }
}

void __no_inline_not_in_flash_func(event_handler)()
{
    static absolute_time_t reset_timeout;
    dma_hw->ints1 = 1u << eb_get_event_chan();
    int address = eb_get_event();
    while (address > 0)
    {
        as_count++;
        if (address >= eb_pico_addr(SID_BASE_ADDR) && address < eb_pico_addr(SID_BASE_ADDR + SID_WRITEABLE))
        {
            as_sid_write(address);
        }
        else if (address == eb_pico_addr(YARRB_REG0))
        {
            uint8_t data = *(uint8_t*)address;
            if ((data & YARRB_4MHZ) && (watchdog_hw->scratch[0] != EB_65C02_MAGIC_NUMBER))
            {
                // Stop the 6502 interface, set the magic number and reboot in 65C02 mode
                eb_shutdown();
                watchdog_hw->scratch[0] = EB_65C02_MAGIC_NUMBER;
                watchdog_enable(1, false);
            }
        }
        else if (address == eb_pico_addr(RESET_VEC)) {
            reset_timeout = make_timeout_time_us(2);
        }
        else if (address == eb_pico_addr(RESET_VEC+1)) {
            if (get_absolute_time() < reset_timeout)
            {
                reset_flag = true;
            }
        }
#if (PLATFORM == PLATFORM_ATOM)
        else if (address == eb_pico_addr(PIA_ADDR + 2))
        {
            alt_colour_update(*(uint8_t*)address);
        }
#endif
        else if (address == eb_pico_addr(VIA_DA))
        {
            uint8_t data = *(uint8_t*)address;
            at_putchar(data);
        }
        address = eb_get_event();
    }
}



int atomvga_main(void)
{
    alt_colour_init();
    stdio_uart_init();
    printf("Atom VGA built " __DATE__ " " __TIME__ "\r\n");
    stdio_uart_deinit();

    // Set overclock and over voltage if necessary
    uint sys_freq = SYS_FREQ;
    if (sys_freq > 250000)
    {
        vreg_set_voltage(VREG_VOLTAGE_1_25);
    }

#ifdef REFDIV
    set_sys_clock_pll_refdiv(REFDIV, VCO, PD1, PD2);
#else
    set_sys_clock_khz(sys_freq, true);
#endif

    stdio_uart_init();
    uint sys_freq_hz=clock_get_hz(clk_sys);
    printf("System clock = %d\n", sys_freq_hz);


    switch_font(DEFAULT_FONT);

#if (PLATFORM == PLATFORM_DRAGON)
    init_ee();
    read_ee(EE_ADDRESS, EE_AUTOLOAD, (uint8_t *)&autoload);
    if (AUTO_ON == autoload)
    {
        load_ee();
    }
#endif

    eb_memset(0, 0, 0x10000);
    eb_memset(GetVidMemBase(), VDG_SPACE, 0x200);

    char mess[32];

    // Display message and build date/time
    print_str(4, DEBUG_MESS);
    print_str(5, __DATE__ " " __TIME__);
    snprintf(mess, 32, "BASE=%04X, PIA=%04X", GetVidMemBase(), PIA_ADDR);
    print_str(6, mess);
    print_str(7, "SID VERSION");

    if (watchdog_hw->scratch[0] == EB_65C02_MAGIC_NUMBER)
    {
        print_str(8, "R65C02/4MHZ MODE");
    }

    // set read and write permissions
    eb_set_perm(EB_ADDRESS_LOW, EB_PERM_NONE, EB_ADDRESS_HIGH - EB_ADDRESS_LOW);
    eb_set_perm(FB_ADDR, EB_PERM_WRITE_ONLY, VID_MEM_SIZE);
    eb_set_perm(COL80_BASE, EB_PERM_READ_WRITE, 16);
    eb_set_perm_byte(PIA_ADDR, EB_PERM_WRITE_ONLY);
    eb_set_perm_byte(PIA_ADDR + 2, EB_PERM_WRITE_ONLY);
    eb_set_perm_byte(YARRB_REG0, EB_PERM_WRITE_ONLY);
    eb_set_perm(0xF000, EB_PERM_WRITE_ONLY, 0x20);
    eb_set_perm_byte(VIA_DA, EB_PERM_WRITE_ONLY);
    eb_set_perm_byte(RESET_VEC, EB_PERM_READ_SNOOP);
    eb_set_perm_byte(RESET_VEC+1, EB_PERM_READ_SNOOP);
    print_perm_range();

    // create a semaphore to be posted when video init is complete
    sem_init(&video_initted, 0, 1);
    stdio_uart_deinit();
    // launch all the video on core 1, so it isn't affected by USB handling on core 0
    multicore_launch_core1(core1_func);

    // wait for initialization of video to be complete
    sem_acquire_blocking(&video_initted);
    stdio_uart_init();
    puts("VIDEO INITIALISED");

    eb_init(pio1);
    as_init();
    at_init();
    eb_set_exclusive_handler(event_handler);

    as_run();

    for (;;) {};

    for (;;)
    {
        sleep_ms(10);
        at_poll();
    };
}

#if (PLATFORM == PLATFORM_ATOM)
void check_command()
{
    char *params = (char *)NULL;
    int temp;

    if (is_command("RAM", &params))
    {
        eb_set_perm(0xA00, EB_PERM_READ_WRITE, 0x100);
        ClearCommand();
    }
    else if (is_command("ROM", &params))
    {
        eb_set_perm(0xA00, EB_PERM_READ_ONLY, 0x100);
        eb_set_string(0xA00, "ROM DEMO - #A00-#AFF IS NOW     READ ONLY MEMORY CONTROLLED BY  THE PICO\r");
        ClearCommand();
    }
    else if (is_command("FRAME", &params))
    {
        temp = 0;
        if (uint8_param(params, &temp, 0, 100000))
        {
            _ac_offset = temp;
        }
        ClearCommand();
    }
    else if (is_command("STOP", &params))
    {
        // Stop the atom interface
        eb_shutdown();
    }
    // else if (is_command("DEBUG", &params))
    // {
    //     debug = true;
    //     ClearCommand();
    // }
    // else if (is_command("NODEBUG", &params))
    // {
    //     debug = false;
    //     ClearCommand();
    // }
    else if (is_command("LOWER", &params))
    {
        support_lower = true;
        ClearCommand();
    }
    else if (is_command("NOLOWER", &params))
    {
        support_lower = false;
        ClearCommand();
    }
    else if (is_command("CHARSET", &params))
    {
        temp = fontno;
        if (uint8_param(params, &temp, 0, FONT_COUNT - 1))
        {
            switch_font(temp);
        }
        ClearCommand();
    }
    else if (is_command("FG", &params))
    {
        if (uint8_param(params, &temp, 0, NO_COLOURS - 1))
        {
            switch_colour(temp, &ink);
        }
        ClearCommand();
    }
    else if (is_command("FGA", &params))
    {
        if (uint8_param(params, &temp, 0, NO_COLOURS - 1))
        {
            switch_colour(temp, &ink_alt);
        }
        ClearCommand();
    }
    else if (is_command("BG", &params))
    {
        if (uint8_param(params, &temp, 0, NO_COLOURS - 1))
        {
            switch_colour(temp, &paper);
        }
        ClearCommand();
    }
    else if (is_command("ARTI", &params))
    {
        if (uint8_param(params, &temp, 0, 2))
        {
            artifact = temp;
        }
        ClearCommand();
    }
    else if (is_command("80COL", &params))
    {
        eb_set(COL80_BASE, COL80_ON);
        ClearCommand();
    }
#ifdef GENLOCK
    else if (is_command("GENLOCK", &params))
    {
        if (uint8_param(params, &temp, 0, GENLOCK_NUM_MODES - 1))
        {
            genlock_setting = (genlock_mode_t)temp;
        }
        ClearCommand();
    }
    else if (is_command("GENDEBUG", &params))
    {
        if (uint8_param(params, &temp, 0, 1))
        {
            genlock_debug(temp);
        }
        ClearCommand();
    }
#endif
}
#elif (PLATFORM == PLATFORM_DRAGON)

void save_ee(void)
{
    if (0 <= write_ee(EE_ADDRESS, EE_AUTOLOAD, autoload))
    {
        write_ee(EE_ADDRESS, EE_FONTNO, fontno);
        write_ee_bytes(EE_ADDRESS, EE_INK, (uint8_t *)&ink, sizeof(ink));
        write_ee_bytes(EE_ADDRESS, EE_PAPER, (uint8_t *)&paper, sizeof(paper));
        write_ee_bytes(EE_ADDRESS, EE_INK_ALT, (uint8_t *)&ink_alt, sizeof(ink_alt));
        write_ee(EE_ADDRESS, EE_ISLOWER, support_lower);

        printf("fontno=%02X, ink=%04X, paper=%04X, alt_ink=%04X, lower=%d\n", fontno, ink, paper, ink_alt, support_lower);
    }
}

void load_ee(void)
{
    uint8_t tempb;

    if (0 <= read_ee(EE_ADDRESS, EE_AUTOLOAD, (uint8_t *)&autoload))
    {
        read_ee(EE_ADDRESS, EE_FONTNO, &tempb);
        switch_font(tempb);

        read_ee_bytes(EE_ADDRESS, EE_INK, (uint8_t *)&ink, sizeof(ink));
        read_ee_bytes(EE_ADDRESS, EE_PAPER, (uint8_t *)&paper, sizeof(paper));
        read_ee_bytes(EE_ADDRESS, EE_INK_ALT, (uint8_t *)&ink_alt, sizeof(ink_alt));
        read_ee(EE_ADDRESS, EE_ISLOWER, (uint8_t *)&support_lower);

        printf("fontno=%02X, ink=%04X, paper=%04X, alt_ink=%04X, lower=%d\n", fontno, ink, paper, ink_alt, support_lower);
    }
}

void set_auto(uint8_t state)
{
    write_ee(EE_ADDRESS, EE_AUTOLOAD, state);
}

void check_command()
{
    static uint8_t oldcommand = 0;
    uint8_t command = eb_get(DRAGON_CMD_ADDR);

    if (command != oldcommand)
    {
        switch (command)
        {
        case DRAGON_CMD_DEBUG:
            debug = true;
            break;
        case DRAGON_CMD_NODEBUG:
            debug = false;
            break;
        case DRAGON_CMD_LOWER:
            support_lower = true;
            break;
        case DRAGON_CMD_NOLOWER:
            support_lower = false;
            break;
        case DRAGON_CMD_ARTIOFF:
            artifact = 0;
            break;
        case DRAGON_CMD_ARTI1:
            artifact = 1;
            break;
        case DRAGON_CMD_ARTI2:
            artifact = 2;
            break;
        case DRAGON_CMD_SAVEEE:
            save_ee();
            break;
        case DRAGON_CMD_LOADEE:
            load_ee();
            break;
        case DRAGON_CMD_AUTOOFF:
            set_auto(AUTO_OFF);
            break;
        case DRAGON_CMD_AUTOON:
            set_auto(AUTO_ON);
            break;
        }

        oldcommand = command;
    }
}
#endif

void check_reset(void)
{
    if (reset_flag)
    {
        // back to 32 column mode
        reset_vga80();

        // reset colours if ink and paper are the same
        if (ink == paper)
        {
            ink = DEF_INK;
            paper = DEF_PAPER;
            ink_alt = DEF_INK_ALT;
        }

        as_reset();

        reset_flag = false;
    }
}

const uint chars_per_row = 32;

const uint vga_width = 640;
const uint vga_height = 480;

const uint max_width = 512;
const uint max_height = 384;

const uint vertical_offset = (vga_height - max_height) / 2;
const uint horizontal_offset = (vga_width - max_width) / 2;

const uint debug_start = max_height + vertical_offset;

uint16_t *add_border(uint16_t *p, uint16_t border_colour, uint16_t len)
{
    *p++ = COMPOSABLE_COLOR_RUN;
    *p++ = border_colour;
    *p++ = len - 3;
    return p;
}

// Process Text mode, Semigraphics modes.
//
// 6847 control Atom    Dragon
// INV          D7      D6
// !A/G         PA4     PB7
// !A/S         D6      D7
// CSS          PC3     PB3
// !INT/EXT     D6      PB4
// GM0          PA5     PB4
// GM1          PA6     PB5
// GM2          PA7     PB6
//
// Due to the interaction between the SAM and the VDG on the Dragon, there are 3
// extra semigraphics modes, SG8, SG12 and SG24, that split the character space up
// into 8, 12 and 24 pixels. These pixels are still half a character width but are
// 3, 2 and 1 scanlines high respectively.
// This happens by programming the VDG in text mode and the SAM in graphics mode.
// The memory used for these modes is incresed so that each vertical part of the
// character space is 32 bytes apart in memory.
// For example in the SG8 mode, the top 2 pixels are encoded in the first byte
// the next 2 in the second byte and so on.
// In this way the bytes per character line are 128, 192, and 384.
//
// Mode     Bytes/line  Bytes/chars colours resolution  memory
// SG4      32          1           8       64x32       512
// SG6      32          1           4       64x48       512
// SG8      128         4           8       64x64       2048
// SG12     192         6           8       64x96       3072
// SG24     384         12          8       64x192      6144
//
// In SG8, SG12, SG24, the byte format is the same as the byte format for SG4
// However if ralative_line_no is < 6 pixels 2 and 3 are plotted. If it is > 6
// pixels 0 and 1 are plotted.
//

// Changed parameter memory to be called vdu_base to avoid clash with global memory -- PHS
uint16_t *do_text(scanvideo_scanline_buffer_t *buffer, uint relative_line_num, size_t vdu_base, uint16_t *p, bool is_debug)
{
    // Screen is 16 rows x 32 columns
    // Each char is 12 x 8 pixels
    // Note we divide ralative_line_number by 2 as we are double scanning each 6847 line to
    // 2 VGA lines.
    uint row = (relative_line_num / 2) / 12;              // char row
    uint sub_row = (relative_line_num / 2) % 12;          // scanline within current char row
    uint sgidx = is_debug ? TEXT_INDEX : GetSAMSG();      // index into semigraphics table
    uint rows_per_char = 12 / sg_bytes_row[sgidx];        // bytes per character space vertically
    uint8_t *fontdata = fonts[fontno].fontdata + sub_row; // Local fontdata pointer

    if (row < 16)
    {
        // Calc start address for this row
        uint vdu_address = ((chars_per_row * sg_bytes_row[sgidx]) * row) + (chars_per_row * (sub_row / rows_per_char));

        for (int col = 0; col < 32; col++)
        {
            // Get character data from RAM and extract inv,ag,int/ext
            uint ch = eb_get(vdu_base + vdu_address + col);
            bool inv = (ch & INV_MASK) ? true : false;
            bool as = (ch & AS_MASK) ? true : false;
            bool intext = GetIntExt(ch);

            uint16_t fg_colour;
            uint16_t bg_colour = paper;

            // Deal with text mode first as we can decide this purely on the setting of the
            // alpha/semi bit.
            if (!as)
            {
                uint8_t b = fontdata[(ch & 0x3f) * 12];

                fg_colour = alt_colour() ? ink_alt : ink;

                if (support_lower && ch >= LOWER_START && ch <= max_lower)
                {
                    b = fontdata[((ch & 0x3f) + 64) * 12];

                    if (LOWER_INVERT)
                    {
                        bg_colour = fg_colour;
                        fg_colour = paper;
                    }
                }
                else if (inv)
                {
                    bg_colour = fg_colour;
                    fg_colour = paper;
                }

                if (b == 0)
                {
                    *p++ = COMPOSABLE_COLOR_RUN;
                    *p++ = bg_colour;
                    *p++ = 16 - 3;
                }
                else
                {
                    // The internal character generator is only 6 bits wide, however external
                    // character ROMS are 8 bits wide so we must handle them here
                    uint16_t c = (b & 0x80) ? fg_colour : bg_colour;
                    *p++ = COMPOSABLE_RAW_RUN;
                    *p++ = c;
                    *p++ = 16 - 3;
                    *p++ = c;
                    for (uint8_t mask = 0x40; mask > 0; mask = mask >> 1)
                    {
                        c = (b & mask) ? fg_colour : bg_colour;
                        *p++ = c;
                        *p++ = c;
                    }
                }
            }
            else // Semigraphics
            {
                uint colour_index;

                if (as && intext)
                {
                    sgidx = SG6_INDEX; // SG6
                }

                colour_index = (SG6_INDEX == sgidx) ? (ch & SG6_COL_MASK) >> SG6_COL_SHIFT : (ch & SG4_COL_MASK) >> SG4_COL_SHIFT;

                if (alt_colour() && (SG6_INDEX == sgidx))
                {
                    colour_index += 4;
                }

                fg_colour = colour_palette_atom[colour_index];

                uint pix_row = (SG6_INDEX == sgidx) ? 2 - (sub_row / 4) : 1 - (sub_row / 6);

                uint16_t pix0 = ((ch >> (pix_row * 2)) & 0x1) ? fg_colour : bg_colour;
                uint16_t pix1 = ((ch >> (pix_row * 2)) & 0x2) ? fg_colour : bg_colour;
                *p++ = COMPOSABLE_COLOR_RUN;
                *p++ = pix1;
                *p++ = 8 - 3;
                *p++ = COMPOSABLE_COLOR_RUN;
                *p++ = pix0;
                *p++ = 8 - 3;
            }
        }
    }
    return p;
}

void draw_color_bar(scanvideo_scanline_buffer_t *buffer)
{
    const uint mode = get_mode();
    const uint line_num = scanvideo_scanline_number(buffer->scanline_id);
    uint16_t *p = (uint16_t *)buffer->data;
    int relative_line_num = line_num - vertical_offset;
    uint16_t *art_palette = (1 == artifact) ? colour_palette_artifact1 : colour_palette_artifact2;

    if (line_num == 0)
    {
        check_command();
        //update_debug_text();
        check_reset();
    }

    uint16_t *palette = colour_palette;
    if (alt_colour())
    {
        palette += 4;
    }

    // Graphics modes have a coloured border, text modes have a black border
    uint16_t border_colour = (mode & 1) ? palette[0] : 0;
    //uint debug_end = debug ? debug_start + 24 : debug_start;
    uint debug_end = debug_start;

    if (relative_line_num < 0 || line_num >= debug_end)
    {
        // Add top/bottom borders
        p = add_border(p, border_colour, vga_width);
    }
    else
    {

        // Add left border
        p = add_border(p, border_colour, horizontal_offset - 1);

        if (line_num >= debug_start && line_num < debug_end) // Debug in 'text' mode
        {
            // p = do_text(buffer, line_num - debug_start, debug_text, p, true);
        }
        else if (!(mode & 1)) // Alphanumeric or Semigraphics
        {
            if (relative_line_num >= 0 && relative_line_num < (16 * 24))
            {
                p = do_text(buffer, relative_line_num, GetVidMemBase(), p, false);
            }
        }
        else // Grapics modes
        {
            const int height = get_height(mode);
            relative_line_num = (relative_line_num / 2) * height / 192;
            if (relative_line_num >= 0 && relative_line_num < height)
            {
                uint vdu_address = GetVidMemBase() + bytes_per_row(mode) * relative_line_num;
                size_t bp = vdu_address;

                *p++ = COMPOSABLE_RAW_RUN;
                *p++ = border_colour;
                *p++ = 512 + 2 - 3;

                // Add a second extra pixel, so the two-pixel alignment is the same between text and graphics modes
                // (this gives RGBtoHDMI a better chance of sampling the middle of the pixel-pair in both modes
                *p++ = border_colour;

                const uint pixel_count = get_width(mode);

                if (is_colour(mode))
                {
                    uint32_t word = 0;
                    for (uint pixel = 0; pixel < pixel_count; pixel++)
                    {
                        if ((pixel % 16) == 0)
                        {
                            word = eb_get32(bp);
                            bp += 4;
                        }
                        uint x = (word >> 30) & 0b11;
                        uint16_t colour = palette[x];
                        if (pixel_count == 128)
                        {
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                        }
                        else if (pixel_count == 64)
                        {
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                        }
                        word = word << 2;
                    }
                }
                else
                {
                    uint16_t fg = palette[0];
                    for (uint i = 0; i < pixel_count / 32; i++)
                    {
                        const uint32_t b = eb_get32(bp);
                        bp += 4;
                        if (pixel_count == 256)
                        {
                            if (0 == artifact)
                            {
                                for (uint32_t mask = 0x80000000; mask > 0;)
                                {
                                    uint16_t colour = (b & mask) ? fg : 0;
                                    *p++ = colour;
                                    *p++ = colour;
                                    mask = mask >> 1;

                                    colour = (b & mask) ? fg : 0;
                                    *p++ = colour;
                                    *p++ = colour;
                                    mask = mask >> 1;

                                    colour = (b & mask) ? fg : 0;
                                    *p++ = colour;
                                    *p++ = colour;
                                    mask = mask >> 1;

                                    colour = (b & mask) ? fg : 0;
                                    *p++ = colour;
                                    *p++ = colour;
                                    mask = mask >> 1;
                                }
                            }
                            else
                            {
                                uint32_t word = b;
                                for (uint apixel = 0; apixel < 16; apixel++)
                                {
                                    uint acol = (word >> 30) & 0b11;
                                    uint16_t colour = art_palette[acol];

                                    *p++ = colour;
                                    *p++ = colour;
                                    *p++ = colour;
                                    *p++ = colour;

                                    word = word << 2;
                                }
                            }
                        }

                        else if (pixel_count == 128)
                        {
                            for (uint32_t mask = 0x80000000; mask > 0; mask = mask >> 1)
                            {
                                uint16_t colour = (b & mask) ? palette[0] : 0;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                            }
                        }
                        else if (pixel_count == 64)
                        {
                            for (uint32_t mask = 0x80000000; mask > 0; mask = mask >> 1)
                            {
                                uint16_t colour = (b & mask) ? palette[0] : 0;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                            }
                        }
                    }
                }
            }
        }

        // Add right border
        p = add_border(p, border_colour, horizontal_offset);

    }

    // black pixel to end line
    *p++ = COMPOSABLE_RAW_1P;
    *p++ = 0;
    // end of line with alignment padding
    if (!(3u & (uintptr_t)p))
    {
        *p++ = COMPOSABLE_EOL_SKIP_ALIGN;
    }
    else
    {
        *p++ = COMPOSABLE_EOL_ALIGN;
    }
    *p++ = 0;

    buffer->data_used = ((uint32_t *)p) - buffer->data;
    assert(buffer->data_used < buffer->data_max);
}

void reset_vga80()
{
    eb_set(COL80_BASE, COL80_OFF);
    eb_set(COL80_FG, 0xB2);
    eb_set(COL80_BG, 0x00);
    eb_set(COL80_STAT, 0x12);
}

void initialize_vga80()
{
    // Reset the VGA80 hardware
    reset_vga80();
    // Use an LUT to allow two pixels to be calculated at once, taking account of the attribute byte for colours
    //
    // Bit  8  7  6  5  4  3  2  1   0
    //      --bgc--  x  --fgc--  p1 p0
    //
    for (int i = 0; i < 128 * 4; i++)
    {
        vga80_lut[i] = ((i & 1) ? colour_palette_vga80[(i >> 2) & 7] : colour_palette_vga80[(i >> 6) & 7]) << 16;
        vga80_lut[i] |= ((i & 2) ? colour_palette_vga80[(i >> 2) & 7] : colour_palette_vga80[(i >> 6) & 7]);
    }
}

uint16_t *do_text_vga80(scanvideo_scanline_buffer_t *buffer, uint relative_line_num, uint16_t *p)
{
    // Screen is 80 columns by 40 rows
    // Each char is 12 x 8 pixels
    uint row = relative_line_num / 12;
    uint sub_row = relative_line_num % 12;

    uint8_t *fd = fonts[FONT_CGA_THIN].fontdata + sub_row;

    if (row < 40)
    {
        // Compute the start address of the current row in the Atom framebuffer
        uint char_addr = GetVidMemBase() + 80 * row;

        // Read the VGA80 control registers
        uint vga80_ctrl1 = eb_get(COL80_FG);
        uint vga80_ctrl2 = eb_get(COL80_BG);

        *p++ = COMPOSABLE_RAW_RUN;
        *p++ = BLACK;   // Extra black pixel
        *p++ = 642 - 3; //
        *p++ = BLACK;   // Extra black pixel

        // For efficiency, compute two pixels at a time using a lookup table
        // p is now on a word boundary due to the extra pixels above
        uint32_t *q = (uint32_t *)p;

        if (vga80_ctrl1 & 0x08)
        {
            // Attribute mode enabled, attributes follow the characters in the frame buffer
            uint attr_addr = char_addr + 80 * 40;
            uint shift = (sub_row >> 1) & 0x06; // 0, 2 or 4
            // Compute these outside of the for loop for efficiency
            uint smask0 = 0x10 >> shift;
            uint smask1 = 0x20 >> shift;
            uint ulmask = (sub_row == 10) ? 0xFF : 0x00;
            for (int col = 0; col < 80; col++)
            {
                uint ch = eb_get(char_addr++);
                uint attr = eb_get(attr_addr++);
                uint32_t *vp = vga80_lut + ((attr & 0x77) << 2);
                if (attr & 0x80)
                {
                    // Semi Graphics
                    uint32_t p1 = (ch & smask1) ? *(vp + 3) : *vp;
                    uint32_t p0 = (ch & smask0) ? *(vp + 3) : *vp;
                    // Unroll the writing of the four pixel pairs
                    *q++ = p1;
                    *q++ = p1;
                    *q++ = p0;
                    *q++ = p0;
                }
                else
                {
#if (PLATFORM == PLATFORM_DRAGON)
                    ch ^= 0x60;
#endif
                    // Text
                    uint8_t b = fd[(ch & 0x7f) * 12];
                    if (ch >= 0x80)
                    {
                        b = ~b;
                    }
                    // Underlined
                    if (attr & 0x08)
                    {
                        b |= ulmask;
                    }
                    // Unroll the writing of the four pixel pairs
                    *q++ = *(vp + ((b >> 6) & 3));
                    *q++ = *(vp + ((b >> 4) & 3));
                    *q++ = *(vp + ((b >> 2) & 3));
                    *q++ = *(vp + ((b >> 0) & 3));
                }
            }
        }
        else
        {
            // Attribute mode disabled, use default colours from the VGA80 control registers:
            //   bits 2..0 of VGA80_CTRL1 (#BDE4) are the default foreground colour
            //   bits 2..0 of VGA80_CTRL2 (#BDE5) are the default background colour
            uint attr = ((vga80_ctrl2 & 7) << 4) | (vga80_ctrl1 & 7);
            uint32_t *vp = vga80_lut + (attr << 2);
            for (int col = 0; col < 80; col++)
            {
                uint ch = eb_get(char_addr++);
                bool inv = (ch & INV_MASK) ? true : false;

#if (PLATFORM == PLATFORM_DRAGON)
                ch ^= 0x40;
#endif
                uint8_t b = fd[(ch & 0x7f) * 12];
                if (inv)
                {
                    b = ~b;
                }
                // Unroll the writing of the four pixel pairs
                *q++ = *(vp + ((b >> 6) & 3));
                *q++ = *(vp + ((b >> 4) & 3));
                *q++ = *(vp + ((b >> 2) & 3));
                *q++ = *(vp + ((b >> 0) & 3));
            }
        }
    }
    // The above loops add 80 x 4 = 320 32-bit words, which is 640 16-bit words
    return p + 640;
}

void draw_color_bar_vga80(scanvideo_scanline_buffer_t *buffer)
{
    const uint line_num = scanvideo_scanline_number(buffer->scanline_id);

    uint16_t *p = do_text_vga80(buffer, line_num, (uint16_t *)buffer->data);

    if (line_num == 0)
    {
        check_command();
        //update_debug_text();
        check_reset();
    }

    // black pixel to end line
    *p++ = COMPOSABLE_RAW_1P;
    *p++ = 0;
    // end of line with alignment padding
    if (!(3u & (uintptr_t)p))
    {
        *p++ = COMPOSABLE_EOL_SKIP_ALIGN;
    }
    else
    {
        *p++ = COMPOSABLE_EOL_ALIGN;
    }
    *p++ = 0;

    buffer->data_used = ((uint32_t *)p) - buffer->data;
    assert(buffer->data_used < buffer->data_max);

    buffer->status = SCANLINE_OK;
}

void core1_func()
{
#ifdef GENLOCK
    static genlock_mode_t last_genlock_mode = GENLOCK_OFF;
    static genlock_t *genlock = NULL;
#endif
    // initialize video and interrupts on core 1
    initialize_vga80();
    scanvideo_setup(&vga_mode);

    // TODO - work out how to configure the video GPIO pins!
    // Grab the uart pins back from the video function
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);
    gpio_set_function(21, GPIO_FUNC_PWM);

#ifdef GENLOCK
    genlock_initialize();
#endif
    scanvideo_timing_enable(true);
    sem_release(&video_initted);

    uint last_vga80 = -1;
    while (true)
    {
        uint vga80 = eb_get(COL80_BASE) & COL80_ON;
        scanvideo_scanline_buffer_t *scanline_buffer = scanvideo_begin_scanline_generation(true);
        if (vga80)
        {
            draw_color_bar_vga80(scanline_buffer);
        }
        else
        {
            draw_color_bar(scanline_buffer);
        }
        scanvideo_end_scanline_generation(scanline_buffer);
        if (vga80 != last_vga80)
        {
            if (vga80)
            {
                gpio_set_outover(PICO_SCANVIDEO_SYNC_PIN_BASE, GPIO_OVERRIDE_INVERT);
                gpio_set_outover(PICO_SCANVIDEO_SYNC_PIN_BASE + 1, GPIO_OVERRIDE_INVERT);
            }
            else
            {
                gpio_set_outover(PICO_SCANVIDEO_SYNC_PIN_BASE, GPIO_OVERRIDE_NORMAL);
                gpio_set_outover(PICO_SCANVIDEO_SYNC_PIN_BASE + 1, GPIO_OVERRIDE_NORMAL);
            }
            last_vga80 = vga80;
        }
#ifdef GENLOCK
        genlock_mode_t genlock_mode = vga80 ? GENLOCK_OFF : genlock_setting;
        if (genlock_mode != last_genlock_mode)
        {
            if (genlock && genlock->destroy)
            {
                genlock->destroy(genlock);
            }
            genlock = genlock_factory(genlock_mode);
            if (genlock && genlock->init)
            {
                genlock->init(genlock);
            }
            last_genlock_mode = genlock_mode;
        }
        if (genlock && genlock->process_line)
        {
            int line = scanvideo_get_next_scanline_id() & 0xFFFF;
            genlock->process_line(genlock, line);
        }
#endif
    }
}
