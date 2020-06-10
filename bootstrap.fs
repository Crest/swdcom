forgetram
compiletoflash

\ This code is specific to the STM32F103.
\ It assumes a 8MHz crystal like found on the Blue Pill board.
\ The code runs the core at 72MHz from the external crystal instead of internal oscillator
\ and updates the baud rate register to preserve the default rate of 115200 baud.
\ Remove/replace it for other chips.

8000000 variable clock-hz

$40013800 constant USART1
	USART1 $8 + constant USART1-BRR

$40021000 constant RCC
	RCC $00 + constant RCC-CR
	RCC $04 + constant RCC-CFGR
	RCC $10 + constant RCC-APB1RSTR
	RCC $14 + constant RCC-AHBENR
	RCC $18 + constant RCC-APB2ENR
	RCC $1C + constant RCC-APB1ENR

$40022000 constant FLASH
	FLASH $0 + constant FLASH-ACR

: bit ( u -- u ) \ turn a bit position into a single-bit mask
	1 swap lshift  1-foldable ;

: baud ( u -- u ) \ calculate baud rate divider, based on current clock rate
	clock-hz @ swap / ;

: 8MHz ( -- ) \ set the main clock back to 8 MHz, keep baud rate at 115200
	0 RCC-CFGR !					\ revert to HSI @ 8 MHz, no PLL
	$81 RCC-CR !					\ turn off HSE and PLL, power-up value
	$18 FLASH-ACR !					\ zero flash wait, enable half-cycle access
	8000000 clock-hz !  115200 baud USART1-BRR ! ;	\ fix console baud rate

: 72MHz ( -- ) \ set the main clock to 72 MHz, keep baud rate at 115200
    8MHz						\ make sure the PLL is off
    $12 FLASH-ACR !					\ two flash mem wait states
    16 bit RCC-CR bis!					\ set HSEON
    begin 17 bit RCC-CR bit@ until			\ wait for HSERDY
    1 16 lshift						\ HSE clock is 8 MHz Xtal source for PLL
    7 18 lshift or					\ PLL factor: 8 MHz * 9 = 72 MHz = HCLK
    4  8 lshift or					\ PCLK1 = HCLK/2
    2 14 lshift or					\ ADCPRE = PCLK2/6
              2 or  RCC-CFGR !				\ PLL is the system clock
    24 bit RCC-CR bis!					\ set PLLON
    begin 25 bit RCC-CR bit@ until			\ wait for PLLRDY
    72000000 clock-hz !  115200 baud USART1-BRR ! ;	\ fix console baud rate

\ End of STM32F103 specific code



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
	72MHz swd-console ;

reset

