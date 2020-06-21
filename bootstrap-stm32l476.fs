forgetram
compiletoflash

\ Allocate four 8 bit indicies and two 256 byte ring buffers
here 256 2* cell+ buffer: swd 0 swd !
swd 0 + constant swd-rx-w
swd 1 + constant swd-rx-r
swd 2 + constant swd-tx-w
swd 3 + constant swd-tx-r
swd cell+ dup constant swd-rx
256 + constant swd-tx

\ Advance the ring buffer indicies by one byte
: inc-rx-r ( -- ) 1 swd-rx-r c+! inline ;
: inc-tx-w ( -- ) 1 swd-tx-w c+! inline ;

\ The buffer is empty iff the read any write index are equal
: swd-key? ( -- flag ) swd h@ dup 8 rshift swap $ff and <> ;
: swd-key ( -- char ) begin swd-key? until swd-rx swd-rx-r c@ + c@ inc-rx-r ;

\ The buffer is full iff the write index one less than the read index
: swd-emit? ( -- flag ) swd-tx-w h@ dup 8 rshift swap $ff and 1+ $ff and <> ;
: swd-emit ( char -- ) begin swd-emit?  until swd-tx swd-tx-w c@ + c! inc-tx-w ;

\ Store the ringbuffer base address in R11
\ R11 was picked because it otherwise unused not saved on interrupt.
: >r11 ( x -- ) [ $46b3 h, ] drop ; \ $46b3 = mov r11, rTOS (aka r6)
: r11@ ( -- x ) dup [ $465e h, ] ; \ $465e = mov rTOS, r11
: swd-init ( -- ) 0 swd ! swd >r11 ;
	
\ Switch to the swd console
: swd-console ( -- )
	['] swd-key? hook-key? !
	['] swd-key hook-key !
	['] swd-emit? hook-emit? !
	['] swd-emit hook-emit ! ;

\ Switch (back) to the serial console
: serial-console ( -- )
	['] serial-key? hook-key? !
	['] serial-key hook-key !
	['] serial-emit? hook-emit? !
	['] serial-emit hook-emit ! ;

: init ( -- )
	swd-init
	." The swd buffer address is: " swd hex. cr
	swd-console ;

reset
