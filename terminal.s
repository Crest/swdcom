@
@    Mecrisp-Stellaris - A native code Forth implementation for ARM-Cortex M microcontrollers
@    Copyright (C) 2013  Matthias Koch
@
@    This program is free software: you can redistribute it and/or modify
@    it under the terms of the GNU General Public License as published by
@    the Free Software Foundation, either version 3 of the License, or
@    (at your option) any later version.
@
@    This program is distributed in the hope that it will be useful,
@    but WITHOUT ANY WARRANTY; without even the implied warranty of
@    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
@    GNU General Public License for more details.
@
@    You should have received a copy of the GNU General Public License
@    along with this program.  If not, see <http://www.gnu.org/licenses/>.
@

@ Terminalroutinen

@ Instead of the normal USART these terminal words use a pair of ring buffers
@ in SRAM. The PC (ab)uses the STLINK/V2 as remote memory access interface.
@ All four indicies are stores in a single 32 bit word allowing the host
@ to poll them all with a single 32 bit memory read. Updates to the indicies
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
   ldr r11, =SWD_Base @ Load the base address into R11. This makes the code
                      @ slightly fast and allows the host PC to autodiscover
		      @ the buffer address.
   eor r0, r0         @ Initialize all four indicies to zero.
   str r0, [r11]

   bx lr

@ -----------------------------------------------------------------------------
  Wortbirne Flag_visible, "swd-key?" @ ( -- ? )
@ -----------------------------------------------------------------------------
serial_qkey:         @ Hijack the serial_qkey symbol to minimize code changes
   push {lr}         @ Yield the CPU
   bl pause

   dup
   ldrb r0, [r11]    @ Load RX write index
   ldrb r1, [r11, 1] @ Load RX read index
   cmp r0, r1        @ Compare the RX read and write indicies for equality
   ite eq 
   eoreq tos, tos    @ Buffer empty =>  0   
   ornne tos, tos    @ Buffer used  => -1

   pop {pc}

@ -----------------------------------------------------------------------------
  Wortbirne Flag_visible, "swd-emit?" @ ( -- ? )
@ -----------------------------------------------------------------------------
serial_qemit:
   push {lr}         @ Yield the CPU
   bl pause

   dup
   ldrb r0, [r11, 2] @ Load TX write index
   ldrb r1, [r11, 3] @ Load TX read index
   add r0, 1         @ Check if RX write index + 1 == RX read index
   and r0, 255
   cmp r0, r1
   ite eq            
   eoreq tos, tos    @ Buffer full     =>  0
   ornne tos, tos    @ Space available => -1

   pop {pc}

@ -----------------------------------------------------------------------------
  Wortbirne Flag_visible, "swd-key" @ ( -- c )
@ -----------------------------------------------------------------------------
serial_key:
   dup

   ldrb r0, [r11, 1]   @ Cache RX read index
1: ldrb r1, [r11, 0]   @ Load RX write index
   cmp r0, r1          @ Wait while RX read == RX write
   beq 1b
   
   add r1, r0, 4       @ The next byte is at R11 + 4 + RX read
   ldrb tos, [r1, r11]

   add r0, 1           @ Advance the read index one byte
   strb r0, [r11, 1]

   bx lr
      
@ -----------------------------------------------------------------------------
  Wortbirne Flag_visible, "swd-emit" @ ( c -- )
@ -----------------------------------------------------------------------------
serial_emit:
   ldrb r0, [r11, 2]   @ Cache TX write index
   add r1, r0, 1       @ Increment TX write index % 256
   and r1, 255
1: ldrb r2, [r11, 3]   @ Load TX read index
   cmp r1, r2          @ Wait while TX write + 1 == TX read
   beq 1b

   add r0, 256+4       @ Store the next byte at R11 + 4 + 256 + TX write
   strb tos, [r0, r11]

   strb r1, [r11, 2]
   
   drop
   bx lr

.include "../common/terminalhooks.s"

  .ltorg @ Hier werden viele spezielle Hardwarestellenkonstanten gebraucht, schreibe sie gleich !
