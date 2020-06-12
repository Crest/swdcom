compile-to-flash

here 256 2* cell+ buffer: swd 0 swd !
swd 0 + constant swd-rx-w
swd 1 + constant swd-rx-r
swd 2 + constant swd-tx-w
swd 3 + constant swd-tx-r
swd cell+ dup constant swd-rx
256 + constant swd-tx

: b+! dup b@ rot + swap b! [inlined] ;
: b-inc ( c-addr -- ) 1 swap b+! [inlined] ;
: inc-rx-w ( -- ) swd-rx-w b-inc ;
: inc-rx-r ( -- ) swd-rx-r b-inc ;
: inc-tx-w ( -- ) swd-tx-w b-inc ;
: inc-tx-r ( -- ) swd-tx-r b-inc ;

: swd-key? ( -- flag ) swd h@ dup 8 rshift swap $ff and <> ;
: swd-key ( -- char ) [: swd-key? ;] wait swd-rx swd-rx-r b@ + b@ inc-rx-r ;

: swd-emit? ( -- flag ) swd-tx-w h@ dup 8 rshift swap $ff and 1+ $ff and <> ;
: swd-emit ( char -- ) [: swd-emit? ;] wait swd-tx swd-tx-w b@ + b! inc-tx-w ;

: >r11 ( x -- ) [ $46b3 h, ] drop ; \ $46b3 = mov r11, r6
: swd-init ( -- ) 0 swd ! swd >r11  ;

: swd-console ( -- )
  ['] swd-key? key?-hook !
  ['] swd-key key-hook !
  ['] swd-emit? emit?-hook !
  ['] swd-emit emit-hook !
;

: serial-console ( -- )
  ['] do-key? key?-hook !
  ['] do-key key-hook !
  ['] do-emit? emit?-hook !
  ['] do-emit emit-hook !
;

: init ( -- )
  init
  swd-init
  swd-console
;

warm

