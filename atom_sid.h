/*

Interface to resid emulation

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

#pragma once
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "atom_if.h"

// The Atom SID sound board uses #BDC0 to #BDDF
#define SID_BASE_ADDR 0xBDC0
#define SID_WRITEABLE 25
#define SID_LEN 29
#define AS_Q_LENGTH 32

#ifdef __cplusplus
extern "C"
{
#endif
    struct  as_element {
        int address;
        uint8_t data;
    };

    typedef struct as_element as_element_t;
    extern queue_t as_q;
    extern int as_count;

    void as_init();

    /// @brief reset the SID - so it stops making a noise
    static inline void as_reset()
    {
        as_element_t el;
        el.address = 0;
        el.data = 0;
        queue_add_blocking(&as_q, &el);
    }

    void as_run();

    void as_show_status();

    static inline void as_update_reg(uint8_t reg, uint8_t data)
    {
        eb_set(SID_BASE_ADDR + reg, data);
    }

    static inline void as_sid_write(int address)
    {
        as_element_t el;
        uint8_t data = *(uint8_t*)address;
        el.address = address;
        el.data = data;
        queue_try_add(&as_q, &el);
    }

#ifdef __cplusplus
}
#endif
