# Copyright 2021-2025 Chris Moulang
# 
# This file is part of AtomVgaSid
# 
# AtomVgaSid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
# 
# AtomVgaSid is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License along with
# AtomVgaSid. If not, see <https://www.gnu.org/licenses/>.
# 

cmake_minimum_required(VERSION 3.12)

set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)
project(atomvga_project)

pico_sdk_init()

add_compile_definitions(PICO_NO_FPGA_CHECK=1)
add_compile_definitions(PICO_BARE_METAL=1)
add_compile_definitions(PICO_CXX_ENABLE_EXCEPTIONS=0)

set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter" )

#
# Atom Build
#

add_executable(atomvga
  main.cc
  atomvga.c
  genlock.c
  atom_if.c
  atom_sid.cc
  resid-0.16/envelope.cc
  resid-0.16/extfilt.cc
  resid-0.16/pot.cc
  resid-0.16/filter.cc
  resid-0.16/sid.cc
  resid-0.16/voice.cc
  resid-0.16/wave.cc
  resid-0.16/wave6581__ST.cc
  resid-0.16/wave6581_P_T.cc
  resid-0.16/wave6581_PS_.cc
  resid-0.16/wave6581_PST.cc
  resid-0.16/wave8580__ST.cc
  resid-0.16/wave8580_P_T.cc
  resid-0.16/wave8580_PS_.cc
  resid-0.16/wave8580_PST.cc
)

pico_generate_pio_header(atomvga ${CMAKE_CURRENT_LIST_DIR}/sm.pio)
pico_generate_pio_header(atomvga ${CMAKE_CURRENT_LIST_DIR}/genlock.pio)

target_compile_definitions(atomvga PUBLIC -DGENLOCK=1 -DRESID_HAVE_BOOL -DINCLUDE_BRIGHT_ORANGE=1 -DPLATFORM=PLATFORM_ATOM -DPICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS=800 -DPICO_SCANVIDEO_SCANLINE_BUFFER_COUNT=16)
target_compile_definitions(atomvga PRIVATE PICO_MALLOC_PANIC=1)
target_compile_definitions(atomvga PRIVATE PICO_USE_MALLOC_MUTEX=0)
target_compile_definitions(atomvga PRIVATE PICO_DEBUG_MALLOC=0)
target_compile_options(atomvga PRIVATE -save-temps -fverbose-asm)

target_link_libraries(atomvga PRIVATE
  pico_multicore
  pico_stdlib
  pico_scanvideo_dpi
  hardware_pio
  hardware_pwm
  )

pico_add_extra_outputs(atomvga)
pico_enable_stdio_uart(atomvga 1)
pico_set_linker_script(atomvga ${CMAKE_CURRENT_LIST_DIR}/memmap_default.ld)

#
# Dragon Build
#

add_executable(dragonvga
  main.cc
  atomvga.c
  eeprom.c
  atom_if.c
  )

pico_generate_pio_header(dragonvga ${CMAKE_CURRENT_LIST_DIR}/sm.pio)
  
pico_generate_pio_header(dragonvga ${CMAKE_CURRENT_LIST_DIR}/atomvga.pio)

pico_generate_pio_header(dragonvga ${CMAKE_CURRENT_LIST_DIR}/atomvga_out.pio)

target_compile_definitions(dragonvga PUBLIC -DPLATFORM=PLATFORM_DRAGON -DPICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS=800 -DPICO_SCANVIDEO_SCANLINE_BUFFER_COUNT=16)

target_link_libraries(dragonvga PRIVATE
  pico_multicore
  pico_stdlib
  pico_scanvideo_dpi
  hardware_pio
  hardware_i2c
  hardware_pwm
  )

pico_add_extra_outputs(dragonvga)

pico_enable_stdio_uart(dragonvga 1)

pico_set_binary_type(dragonvga copy_to_ram)
