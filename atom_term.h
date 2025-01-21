/*

Serial keyboard/printer interface

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

#include "pico/util/queue.h"
#include "hardware/uart.h"
#include "atom_if.h"
#include <ctype.h>

#define UART_ID uart0

queue_t at_txq;
queue_t at_rxq;

void on_uart_rx()
{
    while (uart_is_readable(UART_ID))
    {
        int ch = uart_getc(UART_ID) & 0xFF;
        queue_try_add(&at_rxq, &ch);
        as_show_status();
    }
}

static void at_init()
{
    queue_init(&at_txq, 4, 40);
    queue_init(&at_rxq, 4, 40);
    eb_set_perm_byte(0xBDDF, EB_PERM_READ_WRITE);


    int UART_IRQ = UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;

    irq_set_exclusive_handler(UART_IRQ, on_uart_rx);
    irq_set_enabled(UART_IRQ, true);

    // Now enable the UART to send interrupts - RX only
    uart_set_irq_enables(UART_ID, true, false);
}

static void at_poll()
{
    int data;
    while (queue_try_remove(&at_txq, &data))
    {
        if (isprint(data))
        {
            putchar(data);
        }
        else if (data == 13)
        {
            puts("");
        }
    }
    uint8_t x = eb_get(0xBDDF);
    if (x == 0 && queue_try_remove(&at_rxq, &data))
    {
        eb_set(0xBDDF, data & 0xFF);
    }
}

static void at_putchar(int c)
{
    queue_try_add(&at_txq, &c);
}
