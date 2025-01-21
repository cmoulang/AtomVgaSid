/*

PIO/DMA interface to the 6502 bus

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

volatile uint16_t _Alignas(EB_BUFFER_LENGTH * 2) _eb_memory[EB_BUFFER_LENGTH] __attribute__((section(".uninitialized_dma_buffer"))) = {0};

#define EB_EVENT_QUEUE_BITS 7
#define EB_EVENT_QUEUE_LEN ((1 << EB_EVENT_QUEUE_BITS) / __SIZEOF_INT__)

static volatile _Alignas(1 << EB_EVENT_QUEUE_BITS) uint32_t eb_event_queue[EB_EVENT_QUEUE_LEN] = {0};
static PIO eb_pio;
static uint eb2_address_sm = 0;
static uint eb2_access_sm = 1;
uint eb_event_chan;

static void eb2_address_program_init(PIO pio, uint sm, bool r65c02mode)
{
    int offset;
    pio_sm_config c;

    if (r65c02mode)
    {
        offset = pio_add_program(pio, &eb2_addr_65C02_program);
        c = eb2_addr_65C02_program_get_default_config(offset);
    }
    else
    {
        offset = pio_add_program(pio, &eb2_addr_other_program);
        c = eb2_addr_other_program_get_default_config(offset);
    }
    hard_assert(offset >= 0);

    (pio)->input_sync_bypass = (0xFF << PIN_A0) | (1 << PIN_R_NW);

    for (int pin = PIN_A0; pin < PIN_A0 + 8; pin++)
    {
        pio_gpio_init(pio, pin);
        gpio_set_pulls(pin, true, false);
    }

    for (int pin = PIN_MUX_DATA; pin < PIN_MUX_DATA + 3; pin++)
    {
        pio_gpio_init(pio, pin);
        gpio_set_pulls(pin, true, false);
    }

    pio_gpio_init(pio, PIN_1MHZ);
    pio_gpio_init(pio, PIN_R_NW);

    pio_sm_set_pins_with_mask(pio, sm,
                              (0x07 << PIN_MUX_DATA),
                              (0x07 << PIN_MUX_DATA));

    pio_sm_set_consecutive_pindirs(pio, sm, PIN_MUX_DATA, 3, true);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_A0, 8, false);

    sm_config_set_jmp_pin(&c, PIN_A0 + 7); // == A15
    sm_config_set_in_pins(&c, PIN_A0);
    sm_config_set_out_pins(&c, PIN_A0, 8);
    sm_config_set_set_pins(&c, PIN_A0, 8);

    sm_config_set_sideset(&c, 4, true, false);
    sm_config_set_sideset_pins(&c, PIN_MUX_DATA);

    sm_config_set_in_shift(&c, false, true, 17);

    // Calculate address for PIO
    uint address = (uint)&_eb_memory >> 17;

    int status;
    status = pio_sm_init(pio, sm, offset, &c);
    hard_assert(status == PICO_OK);
    pio_sm_put(pio, sm, address);
    pio_sm_exec(pio, sm, pio_encode_pull(false, true) | pio_encode_sideset_opt(3, 0x7));
    pio_sm_exec(pio, sm, pio_encode_mov(pio_x, pio_osr) | pio_encode_sideset_opt(3, 0x7));
    pio_sm_exec(pio, sm, pio_encode_nop() | pio_encode_sideset_opt(3, 0x7));
}

static void eb2_access_program_init(PIO pio, int sm)
{
    int offset;

    offset = pio_add_program(pio, &eb2_access_program);

    pio_sm_config c = eb2_access_program_get_default_config(offset);
    sm_config_set_jmp_pin(&c, PIN_R_NW);
    sm_config_set_in_pins(&c, PIN_A0);
    sm_config_set_in_shift(&c, false, true, 8);

    sm_config_set_out_pins(&c, PIN_A0, 8);
    sm_config_set_set_pins(&c, PIN_A0, 8);

    sm_config_set_sideset(&c, 4, true, false);
    sm_config_set_sideset_pins(&c, PIN_MUX_DATA);

    pio_sm_init(pio, sm, offset, &c);
    pio_sm_exec(pio, sm, pio_encode_nop() | pio_encode_sideset_opt(3, 0x7));
}

static void eb_setup_dma(PIO pio, int eb2_address_sm,
                         int eb2_access_sm)
{
    uint address_chan = dma_claim_unused_channel(true);
    uint read_data_chan = dma_claim_unused_channel(true);
    uint address_chan2 = dma_claim_unused_channel(true);
    uint write_data_chan = dma_claim_unused_channel(true);
    eb_event_chan = dma_claim_unused_channel(true);

    dma_channel_config c;

    // Copies address from fifo to read_data_chan
    c = dma_channel_get_default_config(address_chan);
    channel_config_set_high_priority(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, eb2_address_sm, false));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);

    dma_channel_configure(
        address_chan,
        &c,
        &dma_channel_hw_addr(read_data_chan)->al3_read_addr_trig,
        &pio->rxf[eb2_address_sm],
        1,
        true);

    // Copies data from the memory to fifo
    c = dma_channel_get_default_config(read_data_chan);
    channel_config_set_high_priority(&c, true);
    // channel_config_set_dreq(&c, pio_get_dreq(pio, eb2_access_sm, true));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_chain_to(&c, address_chan2);

    dma_channel_configure(
        read_data_chan,
        &c,
        &pio->txf[eb2_access_sm],
        NULL, // read address set by DMA
        1,
        false);

    // Copies address from read_data_chan to write_data_chan
    c = dma_channel_get_default_config(address_chan2);
    channel_config_set_high_priority(&c, true);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_chain_to(&c, address_chan);

    dma_channel_configure(
        address_chan2,
        &c,
        &dma_channel_hw_addr(write_data_chan)->al2_write_addr_trig,
        &dma_channel_hw_addr(read_data_chan)->read_addr,
        1,
        false);

    // Copies data from fifo to memory
    c = dma_channel_get_default_config(write_data_chan);
    // channel_config_set_high_priority(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, eb2_access_sm, false));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_chain_to(&c, eb_event_chan);
    dma_channel_configure(
        write_data_chan,
        &c,
        NULL, // write address set by DMA
        &pio->rxf[eb2_access_sm],
        1,
        false);

    // Updates the event queue
    c = dma_channel_get_default_config(eb_event_chan);
    channel_config_set_high_priority(&c, true);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, EB_EVENT_QUEUE_BITS);
    dma_channel_configure(
        eb_event_chan,
        &c,
        &eb_event_queue,
        &dma_channel_hw_addr(write_data_chan)->write_addr,
        1,
        false);
}

void eb_init(PIO pio) //, irq_handler_t handler)
{
    bool r65c02mode = (watchdog_hw->scratch[0] == EB_65C02_MAGIC_NUMBER);
    eb_pio = pio;
    eb2_address_program_init(eb_pio, eb2_address_sm, r65c02mode);
    eb2_access_program_init(eb_pio, eb2_access_sm);
    eb_setup_dma(eb_pio, eb2_address_sm, eb2_access_sm);
    pio_enable_sm_mask_in_sync(eb_pio, 1u << eb2_address_sm | 1u << eb2_access_sm);
}

void eb_shutdown()
{
    pio_sm_set_enabled(eb_pio, eb2_access_sm, false);
}

void eb_set_exclusive_handler(irq_handler_t handler)
{
    // Tell the DMA to raise IRQ line 1 when the eb_event_chan finishes copying the address
    dma_channel_set_irq1_enabled(eb_event_chan, true);

    // Configure the processor to run dma_handler() when DMA IRQ 1 is asserted
    irq_set_exclusive_handler(DMA_IRQ_1, handler);
    irq_set_enabled(DMA_IRQ_1, true);
    irq_set_priority(DMA_IRQ_1, 0);
    dma_hw->ints1 = 1u << eb_event_chan;
}

int eb_get_event()
{
    static volatile uint32_t *out_ptr = &eb_event_queue[0];
    uint32_t *in_ptr = (uint32_t *)dma_channel_hw_addr(eb_event_chan)->write_addr;

    int result;
    if (out_ptr == in_ptr)
    {
        result = -1;
    }
    else
    {
        result = *out_ptr;
        assert(result >= (uint)&_eb_memory[0]);
        assert(result <= ((uint)&_eb_memory[0] + sizeof _eb_memory));
        out_ptr++;
        if (out_ptr > &eb_event_queue[EB_EVENT_QUEUE_LEN - 1])
        {
            // wrap out_ptr
            out_ptr = &eb_event_queue[0];
        }
    }
    return result;
}
