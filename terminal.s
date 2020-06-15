@ swdcom terminal functions

@ Copyright (c) 2020, Jan Bramkamp <crest+swdcom@rlwinm.de>
@ Copyright (c) 2020, Robert Clausecker <fuz@fuz.su>
@ All rights reserved.
@ 
@ Redistribution and use in source and binary forms, with or without
@ modification, are permitted provided that the following conditions are met:
@ 
@ 1. Redistributions of source code must retain the above copyright notice, this
@    list of conditions and the following disclaimer.
@ 
@ 2. Redistributions in binary form must reproduce the above copyright notice,
@    this list of conditions and the following disclaimer in the documentation
@    and/or other materials provided with the distribution.
@ 
@ THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
@ AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
@ IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
@ DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
@ FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
@ DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
@ SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
@ CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
@ OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
@ OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

@ Instead of the normal USART these terminal words use a pair of ring buffers
@ in SRAM. The PC (ab)uses the STLINK/V2 as remote memory access interface.
@ All four indices are stores in a single 32 bit word allowing the host
@ to poll them all with a single 32 bit memory read. Updates to the indices
@ use 8 bit writes.
@ The ring buffers are used as single producer, single consumer queues to
@ decouple bursty producers from their consumer.
@ The buffer base address is permanently stored in register R11 allowing the
@ host PC to stop the core, read the address from R11 and resume the core.
@
@ In a simple benchmark this code transfered >90 kilobyte/second from a
@ STM32L476 at 48MHz through the STLINK/V2 to the host PC running swd2
@ on FreeBSD 12.1 and >100 kilobyte/second in the opposite direction.
@ The "words" word finishes in a fraction of a second.
@
@ Compared to normal USART initialization this code is almost hardware
@ independent and a lot easier to port.
@
@ The terminal program for the host PC that goes with this code is hosted at:
@
@     https://github.com/Crest/swdcom
@
@ Memory Layout of the buffer pair:
@ 
@   SWD_Base + 0x0000 : RX buffer write index
@   SWD_Base + 0x0001 : RX buffer read index
@   SWD_Base + 0x0002 : TX buffer write index
@   SWD_Base + 0x0003 : TX buffer read index
@   SWD_Base + 0x0104 : 256 bytes of RX buffer space
@   SWD_Base + 0x0204 : 256 bytes of TX buffer space
@
@ The host PC is only allowed to write to these locations:
@   * The RX buffer write index
@   * The TX buffer read index
@   * The unallocated range of the RX buffer space wrapping around at the end

@ TODO: Replace ramallot for with a call to ram_allot.

@ -----------------------------------------------------------------------------
  Wortbirne Flag_visible, "swd"
@ -----------------------------------------------------------------------------
   dup                @ In theory the rest could be written in forth
   ldr tos, =SWD_Base @ with just knowledge of this constant, but
   bx lr              @ it would require a two stage bootstrap.

@ -----------------------------------------------------------------------------
  Wortbirne Flag_visible, "swd-init"
@ -----------------------------------------------------------------------------
uart_init:            @ Hijack the usart_init symbol to minimize code changes
   ldr r1, =SWD_Base  @ SWD buffer base address
   movs r0, 0         @ Initialize all four indices to zero.
   str r0, [r1]
   mov r11, r1        @ Load the base address into R11. This makes the code
                      @ slightly faster and allows the host PC to autodiscover
		      @ the buffer address.
   bx lr

@ -----------------------------------------------------------------------------
  Wortbirne Flag_visible, "swd-key?" @ ( -- ? )
@ -----------------------------------------------------------------------------
serial_qkey:         @ Hijack the serial_qkey symbol to minimize code changes
   push {lr}         @ Yield the CPU
   bl pause

   dup
   mov r2, r11       @ Load SWD buffer base address into a low register
   ldrb r0, [r2]     @ Load RX write index
   ldrb r1, [r2, 1]  @ Load RX read index

   movs tos, 0	     @ Assume that the RX buffer is empty (read == write)
   cmp r0, r1        @ Test the assumption
   it ne             @ Change from 0 to -1 if the assumption was incorrect 
   subne tos, 1

   pop {pc}

@ -----------------------------------------------------------------------------
  Wortbirne Flag_visible, "swd-emit?" @ ( -- ? )
@ -----------------------------------------------------------------------------
serial_qemit:        @ Hijack the serial_qemit symbol to minimize code changes
   push {lr}         @ Yield the CPU
   bl pause

   dup
   mov r2, r11       @ Load SWD buffer base address into a low register
   ldrb r0, [r2, 2]  @ Load TX write index
   ldrb r1, [r2, 3]  @ Load TX read index
   adds r0, 1        @ Check if RX write index + 1 == RX read index
   uxtb r0, r0       @ clear possible carry
   movs tos, 0       @ Assume that the TX buffer is full (write + 1 == RX)
   cmp r0, r1        @ Test the assumption
   it ne             @ Change from 0 to -1 if the assumption was incorrect
   subne tos, 1

   pop {pc}

@ -----------------------------------------------------------------------------
  Wortbirne Flag_visible, "swd-key" @ ( -- c )
@ -----------------------------------------------------------------------------
serial_key:            @ Hijack the serial_key symbol to minimize code changes
   dup

   mov r2, r11         @ Load SWD buffer base address into a low register
   ldrb r0, [r2, 1]    @ Cache RX read index
1: ldrb r1, [r2, 0]    @ Load RX write index
   cmp r0, r1          @ Wait while RX read == RX write
   beq 1b
   
   adds r1, r0, 4      @ The next byte is at R11 + 4 + RX read
   ldrb tos, [r1, r2]  @ Load the byte that was written

   adds r0, 1          @ Advance the read index one byte
   strb r0, [r2, 1]    @ And write it to the ring buffer

   bx lr
      
@ -----------------------------------------------------------------------------
  Wortbirne Flag_visible, "swd-emit" @ ( c -- )
@ -----------------------------------------------------------------------------
serial_emit:           @ Hijack the serial_emit symbol to minimize code changes
   mov r3, r11         @ Load SWD buffer base address into a low register
   ldrb r0, [r3, 2]    @ Cache TX write index
   adds r1, r0, 1      @ Increment TX write index % 256
   uxtb r1, r1
1: ldrb r2, [r3, 3]    @ Load TX read index
   cmp r1, r2          @ Wait while TX write + 1 == TX read
   beq 1b

   add r0, 256+4       @ Store the next byte at R11 + 4 + 256 + TX write
   strb tos, [r0, r3]

   strb r1, [r3, 2]
   
   drop
   bx lr

.include "../common/terminalhooks.s"

  .ltorg @ Flush constant pool
