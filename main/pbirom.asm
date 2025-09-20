#include "asmdefs.h"

PDVMSK  =   $0247   //;Parallel device mask (indicates which are
NDEVREQ =   $0248   //;Shadow of PDVS ($D1FF), currently activated PBI device
PDIMSK  =   $0249   //;Parallel interrupt mask
PDVS    =   $D1FF
GPDVV   =   $E48F   //;Generic Parallel Device Vector, placed in HATABS by init routine 
HATABS  =   $031A   //;Device handler table
CRITIC  =   $0042   //;Critical code section flag
RTCLOK  =   $0012   //;Real time clock, 3 bytes
NMIEN   =   $D40E   //;NMI enable mask on Antic
NEWDEV  =   $E486   //;routine to add device to HATABS, doesn't seem to work, see below 
IOCBCHIDZ = $0020   //;page 0 copy of current IOCB 
SDMCTL  =   $022F
DMACTL  =   $D400
CONSOL  =   53279
KBCODE  =   $D209
SKCTL   =   $D20F
SKSTAT  =   $D20F
COPYBUF =   $DC00
BASICF  =   $03F8
PORTB   =   $D301
MEMLO   =   $02E7

#if 0
COPYSRC = $F5 
COPYDST = $F7 
COPYLEN = $F9
#else
COPYSRC = $DA 
COPYDST = $DC 
COPYLEN = $DE
#endif

DEVNAM  =   'J'     //;device letter J drive in this device's case
PDEVNUM =  2       //;Parallel device bit mask - $2 in this device's case.  $1,2,4,8,10,20,40, or $80   

#ifndef BASE_ADDR
BASE_ADDR = $d800
#endif
* = BASE_ADDR

.word   $ffff,                      // D800 ROM cksum lo
.byt    $01,                        // D802 ROM version
MPID1
.byt    $80,                        // D803 ID num 1, must be $80
.byt    $01,                        // D804 Device Type
jmp PBI_IO                          // D805-D807 Jump vector entry point for SIO-similar IO 
jmp PBI_ISR                         // D808-D80A Jump vector entry point for external PBI interrupt handler
MPID2
.byt    $91,                        // D80B ID num 2, must be $91
.byt    DEVNAM,                     // D80C Device Name (ASCII)
.word PBI_OPEN - 1
.word PBI_CLOSE - 1
.word PBI_GETB - 1
.word PBI_PUTB - 1
.word PBI_STATUS - 1
.word PBI_SPECIAL - 1
jmp PBI_INIT                        // D819-D81B Jump vector for device initialization 
.byt $ff
.byt $0
.byt $0
.byt $0                             // Pad out to $D820

IOCB_BMON_TRIGGER
PBI_INIT_COMPLETE
.byt $1
.byt $0
.byt $0
.byt $0

.byt $0
.byt $0
.byt $0
.byt $0

.byt $0
.byt $0
.byt $0
.byt $0

.byt $de
.byt $ad
.byt $be
.byt $ef                // Pad out to $D830


;; $D830 iocb #1
ESP32_IOCB
ESP32_IOCB_REQ                      // Private IOCB structure for passing info to ESP32
    .byt $0     ;  request - 6502 sets to 1 after filling out ESP32_IOCB struct, esp32 clears after handling
ESP32_IOCB_CMD
    .byt $ee      
ESP32_IOCB_A
    .byt $ee     ;  A - iocb index * $20 
ESP32_IOCB_X
    .byt $ee     ;  X -  

ESP32_IOCB_Y
    .byt $ee     ;  Y -  
ESP32_IOCB_CARRY
    .byt $ee
ESP32_IOCB_RESULT
    .byt $ee
ESP32_IOCB_6502PSP
    .byt $ee

ESP32_IOCB_COPYBUFL
    .byt $ee
ESP32_IOCB_COPYBUFH
    .byt $ee
ESP32_IOCB_COPYLENL
    .byt $ee
ESP32_IOCB_COPYLENH
    .byt $de

ESP32_IOCB_KBCODE
    .byt $ad
ESP32_IOCB_SDMCTL
    .byt $be
ESP32_IOCB_STACKPROG
    .byt $ef
ESP32_IOCB_CONSOL
    .byt $0

;; $D840 iocb #2
// todo - figure out how to reserve this much space for a second IOCB without
// replicating it all here
IESP32_IOCB
IESP32_IOCB_REQ                      // Private IOCB structure for passing info to ESP32
    .byt $0     ;  request - 6502 sets to 1 after filling out ESP32_IOCB struct, esp32 clears after handling
IESP32_IOCB_CMD
    .byt $ee      
IESP32_IOCB_A
    .byt $ee     ;  A -  
IESP32_IOCB_X
    .byt $ee     ;  X -  
IESP32_IOCB_Y
    .byt $ee     ;  Y -  
IESP32_IOCB_CARRY
    .byt $ee
IESP32_IOCB_RESULT
    .byt $ee
IESP32_IOCB_6502PSP
    .byt $ee
IESP32_IOCB_COPYBUFL
    .byt $ee
IESP32_IOCB_COPYBUFH
    .byt $ee
IESP32_IOCB_COPYLENL
    .byt $ee
IESP32_IOCB_COPYLENH
    .byt $de
IESP32_IOCB_KBCODE
    .byt $ad
IESP32_IOCB_SDMCTL
    .byt $be
IESP32_IOCB_STACKPROG
    .byt $ef
IESP32_IOCB_CONSOL
    .byt $0

;; $D850 
;; 0x10 bytes of canary data
.byt $de
.byt $ad
.byt $be
.byt $ef                
.byt $de
.byt $ad
.byt $be
.byt $ef            
.byt $de
.byt $ad
.byt $be
.byt $ef          
.byt $de
.byt $ad
.byt $be
.byt $ef             

;; 6 bytes to take up the original TEST_ code 
    nop
    nop
    nop
    nop
    nop
    nop

PBI_INIT
#if 1
    ;; try to disable basic 
    lda PORTB
    ora #02
    sta PORTB
    lda #0
    sta BASICF
#endif
    inc PBI_INIT_COMPLETE

    lda PDVMSK  // enable this device's bit in PDVMSK
    ora #PDEVNUM
    sta PDVMSK  

;Put device name in Handler table HATABS
     LDX #0
;        Top of loop
 SEARCH
     LDA HATABS,X ;Get a byte from table
     BEQ FNDIT   ;0? Then we found space.
     INX 
     INX 
     INX 
     CPX #36     ;Length of HATABS
     BCC SEARCH  ;Still looking
     RTS         ;No room in HATABS; device not initialized
 ;
 ;         We found a spot.
 FNDIT
     LDA #DEVNAM ;Get device name.
     STA HATABS,X ;Put it in blank spot.
     LDA #GPDVV & $FF ;Get lo byte of vector.
     STA HATABS+1,X
     LDA #GPDVV / $0100 ;Get hi byte of vector.
     STA HATABS+2,X

     ;; put it x3, xdos blindly overwrites the first two entries
     INX
     INX
     INX
     LDA #DEVNAM ;Get device name.
     STA HATABS,X ;Put it in blank spot.
     LDA #GPDVV & $FF ;Get lo byte of vector.
     STA HATABS+1,X
     LDA #GPDVV / $0100 ;Get hi byte of vector.
     STA HATABS+2,X

     ;; put it x3, xdos blindly overwrites the first two entries
     INX
     INX
     INX
     LDA #DEVNAM ;Get device name.
     STA HATABS,X ;Put it in blank spot.
     LDA #GPDVV & $FF ;Get lo byte of vector.
     STA HATABS+1,X
     LDA #GPDVV / $0100 ;Get hi byte of vector.
     STA HATABS+2,X

    //; NEWDEV routine was reported to do the above search and insert for us,
    //; but I couldn't get it to work 
    //;ldx #DEVNAM
    //;lda GENDEV / $100
    //;ldy GENDEV & $ff
    //;jsr NEWDEV		//; returns: N = 1 - failed, C = 0 - success, C =1 - entry already exists


    lda PDIMSK  // enable this device's bit in PDIMSK
    ora #PDEVNUM 
    //sta PDIMSK

    lda MEMLO+1  ;; hi byte of MEMLO
    clc           
    adc #4       ;; reserve 1K
    ;;sta MEMLO+1 
    ;; TODO stuff display list and screen mem in the reserved mem
    ;; Modify SAVSMC, copy display list and update SDLSTL, 
    ;; modify new display list to point to new screen mem 

    inc PBI_INIT_COMPLETE
    sec
    rts

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; SIO ENTRY ROUTINES 

PBI_IO
    sta ESP32_IOCB_A
    lda #7
    jmp PBI_COMMAND_COMMON

PBI_ISR     
    sta IESP32_IOCB_A
    stx IESP32_IOCB_X
    sty IESP32_IOCB_Y

    ldy #IESP32_IOCB - ESP32_IOCB 
    lda #8
    jsr PBI_ALL

L10
    bit IESP32_IOCB_RESULT
    bpl NO_MONITOR

    lda #$00 ;; change screen color, will be restored from shadow registers by VBI
    sta $d018
    lda #$ff
    sta $d01a
    lda #14
    sta $d017

    ldy #IESP32_IOCB - ESP32_IOCB 
    lda #11
    jsr PBI_ALL

#if 0 
    php   ;; clear interrupts for a short delay, give refresh loop a chance 
    cli
    ldx #0
L11
    inx 
    bne L11
    plp
#endif

    clc
    bcc L10 
    
NO_MONITOR
    rts

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
// CIO ROUTINES 

PBI_OPEN
// check IOCBCHIDZ see if this is for us
    sta ESP32_IOCB_A
    lda #1 
    JMP PBI_COMMAND_COMMON

PBI_CLOSE
    sta ESP32_IOCB_A
    lda #2 // cmd close
    JMP PBI_COMMAND_COMMON

PBI_GETB
    sta ESP32_IOCB_A
    lda #3 // cmd getb
    JMP PBI_COMMAND_COMMON

PBI_PUTB
    sta ESP32_IOCB_A
    lda #4 // cmd close
    JMP PBI_COMMAND_COMMON

PBI_STATUS
    sta ESP32_IOCB_A
    lda #5 // cmd status
    JMP PBI_COMMAND_COMMON
    
PBI_SPECIAL
    sta ESP32_IOCB_A
    lda #6 // cmd special
    // JMP PBI_COMMAND_COMMON
    // fall through to PBI_COMMAND_COMMON

PBI_COMMAND_COMMON
    stx ESP32_IOCB_X
    sty ESP32_IOCB_Y
    ldy #0 
    
    // jmp PBI_ALL
    // fall through to PBI_ALL

PBI_ALL  
    // Shared code between commands and interrupts 
    // A contains the command selected by entry stubs above 
    // Y contains the IOCB offset, selecting either normal IOCB or the interrupt IOCB 
    // on return - A,X,Y restored to original values from IOCB save locations

//#define VBLANK_SYNC
#ifdef VBLANK_SYNC
    // Issue cmd 10 - wait for good vblank timing
    pha
    lda #10
    sta ESP32_IOCB_CMD,Y
    jsr SAFE_WAIT
    pla
#endif 

    sta ESP32_IOCB_CMD,y
    lda CONSOL
    sta ESP32_IOCB_CONSOL,Y
    lda KBCODE
    sta ESP32_IOCB_KBCODE,Y
    lda SKSTAT
    and #$4
    beq STILL_PRESSED
    lda #0
    sta ESP32_IOCB_KBCODE,Y
STILL_PRESSED

//;; USE_NMIEN currently required, see stackprog TODO below 
#define USE_NMIEN
#ifdef USE_NMIEN 
    php
    pla
    sta ESP32_IOCB_6502PSP,y
    sei 
    ;; 5 bytes to take up the lda #$c0 and sta #xxxx
    nop
    nop
    nop
    nop
    nop
    lda #$00
    sta NMIEN
#endif

//#define USE_DMACTL
#ifdef USE_DMACTL
    lda SDMCTL
    sta ESP32_IOCB_SDMCTL,y
    and #$df
    ;;//sta SDMCTL  ;;// TODO understand why things hangs turbobasic
    sta DMACTL
#endif

    lda #REQ_FLAG_DETACHSAFE  ;; REQ_FLAGS in Acc 
RETRY_COMMAND

#ifdef HALT_6502
    sta ESP32_IOCB_REQ,y 
WAIT_FOR_REQ
;;    lda #PDEVNUM
;;    sta PDVS ;; trigger halt 
    lda ESP32_IOCB_REQ,y 
    bne WAIT_FOR_REQ
#else 
    jsr SAFE_WAIT
#endif 

    ;;// Copyin requested?  Do the copyin and reissue the command
    ;;///////////////////////////////////////////////////////////
    lda ESP32_IOCB_RESULT,y 
    and #RES_FLAG_NEED_COPYIN
    beq NO_COPYIN
    jsr COPYIN
    lda #(REQ_FLAG_DETACHSAFE | REQ_FLAG_COPYIN)
    jmp RETRY_COMMAND

NO_COPYIN
    lda ESP32_IOCB_RESULT,y 
    and #RES_FLAG_COPYOUT
    beq NO_COPYOUT
    jsr COPYOUT

NO_COPYOUT

#ifdef USE_DMACTL 
    lda ESP32_IOCB_SDMCTL,y
    ;;//sta SDMCTL
    sta DMACTL
#endif

#ifdef USE_NMIEN
    lda ESP32_IOCB_6502PSP,y
    and #$04
    bne NO_CLI 
    cli
NO_CLI
    lda #$c0 // TODO find the NMIEN shadow register and restore proper value
    sta NMIEN
#endif

    lda ESP32_IOCB_CARRY,y
    ror

RESTORE_REGS_AND_RETURN  
    lda ESP32_IOCB_A,y
    pha
    ldx ESP32_IOCB_X,y
    lda ESP32_IOCB_Y,y
    tay 
    pla 
    rts 

COPYOUT 
    lda COPYSRC
    pha
    lda COPYSRC+1
    pha
    lda COPYDST
    pha
    lda COPYDST+1
    pha
    lda COPYLEN
    pha
    lda COPYLEN+1
    pha

    lda #<COPYBUF
    sta COPYSRC
    lda #>COPYBUF
    sta COPYSRC+1
    lda ESP32_IOCB_COPYBUFL,y
    sta COPYDST
    lda ESP32_IOCB_COPYBUFH,y
    sta COPYDST+1
    lda ESP32_IOCB_COPYLENL,y
    sta COPYLEN
    lda ESP32_IOCB_COPYLENH,y
    sta COPYLEN+1
    jsr MEMCPY

    pla
    sta COPYLEN+1
    pla
    sta COPYLEN
    pla
    sta COPYDST+1
    pla
    sta COPYDST
    pla
    sta COPYSRC+1
    pla
    sta COPYSRC
    rts

COPYIN 
    lda COPYSRC
    pha
    lda COPYSRC+1
    pha
    lda COPYDST
    pha
    lda COPYDST+1
    pha
    lda COPYLEN
    pha
    lda COPYLEN+1
    pha

    lda #<COPYBUF
    sta COPYDST
    lda #>COPYBUF
    sta COPYDST+1
    lda ESP32_IOCB_COPYBUFL,y
    sta COPYSRC
    lda ESP32_IOCB_COPYBUFH,y
    sta COPYSRC+1
    lda ESP32_IOCB_COPYLENL,y
    sta COPYLEN
    lda ESP32_IOCB_COPYLENH,y
    sta COPYLEN+1
    jsr MEMCPY

    pla
    sta COPYLEN+1
    pla
    sta COPYLEN
    pla
    sta COPYDST+1
    pla
    sta COPYDST
    pla
    sta COPYSRC+1
    pla
    sta COPYSRC
    rts

MEMCPY
    tya 
    pha
    ldy   	#0  		            ; set up to move 256
	ldx   	COPYLEN+1       		; hi byte of len
  	beq   	L2

L1
	lda   	(COPYSRC),y	            ; get a byte
   	sta   	(COPYDST),y	            ; store it
   	iny
    bne    	L1
   	inc   	COPYSRC+1		        ; bump ptrs
   	inc   	COPYDST+1
    dex
  	bne   	L1  		            ; do another block

L2	
    ldx	    COPYLEN     		    ; get low byte of n
  	beq     COPY_DONE	            ; jump if done

L3
	lda	(COPYSRC),y	; get a byte
  	sta	(COPYDST),y	; store it
  	iny
  	dex
  	bne	L3
COPY_DONE
    pla
    tay 
    rts


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; Busy wait in RAM while the PBI ROM is mapped out
;; To avoid having to find free ram to do this, put the small 6-byte
;; program on the stack and call it
;;
;; Y   - contains the offset into PCB_IOCB structure were setting and then waiting on, preserved
;; A   - contains the REQ_FLAGS to be set when issuing the command
;; X   - no meaning 
;; 
;; Y   - preserved
;; A,X - not preserved 

SAFE_WAIT
    sta ESP32_IOCB_RESULT,y // stash the req flags here temporarily 

    ;; HACK - patch the STACK_RES_WAIT program to change the ldx #ff
    ;; instruction so that it will contain the stack pointer needed to pop
    ;; the REQ flag off the stack during the busy wait.  
    tsx 
    txa 
    sec
    sbc #(stack_res_wait_end - stack_res_wait + 3)
    sta stack_res_loop + 1  

    // push mini-program on stack in reverse order
    ldx #(stack_res_wait_end - stack_res_wait - 1)
push_prog_loop
    lda stack_res_wait,x
    pha
    dex
    bpl push_prog_loop

    tsx       ; stack pointer now points to newly-placed program - 1 

    lda #(RETURN_FROM_STACKPROG - 1) / $100     ;;// push (return_from_stackprog - 1) onto stack for RTS
    pha                                         // from mini-program
    lda #(RETURN_FROM_STACKPROG - 1) & $ff      //
    pha    

    lda #$01      //;; push 16-bit address of stack-resident mini-prog, minus 1, onto stack 
    pha
    txa 
    pha
    sta ESP32_IOCB_STACKPROG,y

    lda ESP32_IOCB_RESULT,y     // retrieve the REQ_FLAGS argument we stashed above   
    ora #REQ_FLAG_STACKWAIT     // add the stackwait flag 
    rts                         // jump to mini-prog

RETURN_FROM_STACKPROG
    tsx //;; pop the stackprog off the stack by doing arithmetic on the stack pointer
    txa
    clc
    adc #stack_res_wait_end - stack_res_wait
    tax
    txs
    rts        

//;; TODO - checking of the stack-resident req trigger by using PLA
//;;        requires that interrupts be masked for STACKWAIT.  If 
//;;        stackprog could peek at the stack location without a PLA
//;;        this routine would be interrupt-safe 

stack_res_wait
    pha                       //;; called with req value in A
    sta ESP32_IOCB_REQ,y      
;;    lda #PDEVNUM
;;    sta PDVS ;; trigger halt 
stack_res_loop
    ldx #$ff
    txs                       //;; reset stack pointer back to req value for next loop
    pla                       //;; pull req value and check if its been zeroed yet 
    bne stack_res_loop

    rts                       //;; return to spoofed return address RETURN_FROM_STACKPROG
stack_res_wait_end


