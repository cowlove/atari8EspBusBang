PDVMSK  =   $0247   //;Parallel device mask (indicates which are
NDEVREQ =   $0248   //;Shadow of PDVS ($D1FF), currently activated PBI device
PDIMSK  =   $0249   //;Parallel interrupt mask
GPDVV   =   $E48F   //;Generic Parallel Device Vector, placed in HATABS by init routine 
HATABS  =   $031A   //;Device handler table
CRITIC  =   $0042   //;Critical code section flag
RTCLOK  =   $0012   //;Real time clock, 3 bytes
NMIEN   =   $D40E   //;NMI enable mask on Antic
NEWDEV  =   $E486   //;routine to add device to HATABS, doesn't seem to work, see below 
IOCBCHIDZ = $0020   //;page 0 copy of current IOCB 

DEVNAM  =   'J'     //;device letter J drive in this device's case
PDEVNUM =   1       //;Parallel device bit mask - 1 in this device's case.  $1,2,4,8,10,20,40, or $80   


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
ESP32_IOCB_CRITIC
    .byt $ee
ESP32_IOCB_6502PSP
    .byt $ee

ESP32_IOCB_NMIEN
    .byt $ee
ESP32_IOCB_RTCLOK1
    .byt $ee
ESP32_IOCB_RTCLOK2
    .byt $ee
ESP32_IOCB_RTCLOK3
    .byt $de

ESP32_IOCB_LOC004D
    .byt $ad
ESP32_IOCB_LOC004E
    .byt $be
ESP32_IOCB_LOC004F
    .byt $ef
ESP32_IOCB_PDIMSK 
    .byt $0

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
IESP32_IOCB_CRITIC
    .byt $ee
IESP32_IOCB_6502PSP
    .byt $ee
IESP32_IOCB_NMIEN
    .byt $ee
IESP32_IOCB_RTCLOK1
    .byt $ee
IESP32_IOCB_RTCLOK2
    .byt $ee
IESP32_IOCB_RTCLOK3
    .byt $de
IESP32_IOCB_LOC004D
    .byt $ad
IESP32_IOCB_LOC004E
    .byt $be
IESP32_IOCB_LOC004F
    .byt $ef
IESP32_IOCB_PDIMSK 
    .byt $0


TEST_ENTRY
    PLA
    LDA #7
    JMP PBI_PUTB

PBI_INIT
    lda #$0f    //; set border white as indicator 
    sta 712 
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

    //; NEWDEV routine was reported to do the above search and insert for us,
    //; but I couldn't get it to work 
    //;ldx #DEVNAM
    //;lda GENDEV / $100
    //;ldy GENDEV & $ff
    //;jsr NEWDEV		//; returns: N = 1 - failed, C = 0 - success, C =1 - entry already exists

//#define PAGE6_TEST_PROG
#ifdef PAGE6_TEST_PROG
    ldx #COPY_END-COPY_BEGIN-1
L1
    lda COPY_BEGIN,x
    sta $0600,x
    dex
    bpl L1
#endif 

    lda PDIMSK  // enable this device's bit in PDIMSK
    // XXX disable interrupts until working
    ora #PDEVNUM 
    sta PDIMSK
    sec
    rts

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; SIO ENTRY ROUTINES 

PBI_IO
    sta ESP32_IOCB_A
    lda #7
    jmp PBI_COMMAND_COMMON

PBI_ISR     
    // TODO: When bus is detached, 0xd1ff will read high and we will be 
    // called to handle all NMI interrupts.  Need to mask off  Need to check  
    //return with clc, it hangs earlier with just 2 io requests.
    //clc
    //rts
#if 1
    pha
    //lda PDIMSK
    //and #$ff - PDEVNUM 
    //sta PDIMSK
    lda #8
    sta IESP32_IOCB_CMD
    lda #1
    sta IESP32_IOCB_REQ
WAIT11
    lda IESP32_IOCB_REQ
    bne WAIT11
    //lda PDIMSK
    //ora IESP32_IOCB_PDIMSK
    //sta PDIMSK
    lda IESP32_IOCB_CARRY,y
    ror
    pla
    rts
#endif

    sta IESP32_IOCB_A
    stx IESP32_IOCB_X
    sty IESP32_IOCB_Y

    // save and clear PDIMSK. paranoid in case we could interrupt one of our normal driver commands 
    lda PDIMSK  
    ora #PDEVNUM
    sta IESP32_IOCB_PDIMSK
    lda PDIMSK
    and #$ff - PDEVNUM 
    sta PDIMSK

    ldy #IESP32_IOCB - ESP32_IOCB 
    lda #8
    jsr PBI_ALL

    pha
    lda PDIMSK
    ora IESP32_IOCB_PDIMSK
    sta PDIMSK
    pla 

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
    // fall through to PBI_COMMAND_COMMON

PBI_COMMAND_COMMON
    stx ESP32_IOCB_X
    sty ESP32_IOCB_Y
    ldy #0 
    
    pha
    lda PDIMSK
    and #$ff - PDEVNUM 
    sta PDIMSK
    pla 

    jsr PBI_ALL

    pha 
    lda PDIMSK
    ora #PDEVNUM 
    sta PDIMSK
    pla
        
    rts 

PBI_ALL  
    // Shared code between commands and interrupts 
    // A contains the command selected by entry stubs above 
    // Y contains the IOCB offset, selecting either normal IOCB or the interrupt IOCB 
    // TODO: need to make sure this request is for this device by checking 
    // for this device's bit to be set in NDEVREQ

    sta ESP32_IOCB_CMD,y
    
    // lda NDEVREQ   // it has to be us, how else would be be here with this ROM active 
    // ora #PDEVNUM
    // bne CONTINUE
    // clc // not us, return 
    // jmp RESTORE_REGS_AND_RETURN  
    //CONTINUE 
    
    lda CRITIC
    sta ESP32_IOCB_CRITIC,y

    jsr SAFE_WAIT 

    // save and then mask interrupts
    php 
    pla 
    sta ESP32_IOCB_6502PSP,y

    lda #$40 // TODO find the NMIEN shadow register and restore proper value
    sta ESP32_IOCB_NMIEN,y

    sei 
    lda #$00
    sta NMIEN

    // only page 0xd800 is remapped now.  copy out changed memory locations and make
    // remap call to restore normal esp32 RAM
    // The remap call leaves the results portion of the ESP32_IOCB unchanged 
    lda RTCLOK
    sta ESP32_IOCB_RTCLOK1,y
    lda RTCLOK + 1
    sta ESP32_IOCB_RTCLOK1 + 1,y
    lda RTCLOK + 2
    sta ESP32_IOCB_RTCLOK1 + 2,y
    lda $4d
    sta ESP32_IOCB_LOC004D,y
    lda $4e
    sta ESP32_IOCB_LOC004E,y
    lda $4f
    sta ESP32_IOCB_LOC004F,y

    lda #9 // remap command
    STA ESP32_IOCB_CMD,y

    jsr SAFE_WAIT

    lda ESP32_IOCB_NMIEN,y
    sta NMIEN
    lda ESP32_IOCB_6502PSP,y
    and #$04
    bne NO_CLI
    cli
NO_CLI

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

#ifdef PAGE6_TEST_PROG
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
// Simple test code copied into page 6 by PBI_INIT 

COPY_BEGIN
TEST_MPD
    pla
    ldx #$ff
L2
    ldy #$ff
L3
   lda #1
    sta $d1ff
    lda #0
    sta $d1ff
    dey
    bpl L3
    dex
    bpl L2
    rts
COPY_END
#endif // PAGE6_TEST_PROG

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; Busy wait in RAM while the PBI ROM is mapped out
;; To avoid having to find free ram to do this, put the small 6-byte
;; program on the stack and call it
;;
;; Y contains the offset into PCB_IOCB structure were setting and then waiting on

SAFE_WAIT
    // push mini-program on stack in reverse order
    ldx #(stack_res_wait_end - stack_res_wait - 1)
push_prog_loop
    lda stack_res_wait,x
    pha
    dex
    bpl push_prog_loop

    tsx       ; stack pointer now points to newly-placed program - 1 

    lda #(return_from_stackprog - 1) / $100     // push (return_from_stackprog - 1) onto stack for RTS 
    pha                                         // from mini-program
    lda #(return_from_stackprog - 1) & $ff      // 
    pha    

    lda #$01      // push 16-bit address of stack-resident mini-prog onto stack 
    pha
    txa 
    pha

    lda #1                      //  
    rts                         // jump to mini-prog

return_from_stackprog
    tsx
    txa
    clc
    adc #stack_res_wait_end - stack_res_wait
    tax
    txs
    rts        

stack_res_wait
    sta ESP32_IOCB,y      // called with req value in A
stack_res_loop
    lda ESP32_IOCB,y
    bne stack_res_loop
    rts
stack_res_wait_end


