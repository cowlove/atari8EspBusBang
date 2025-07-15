* = $0600

TESTAREA = $8000
SCREENMEM = $9c40
CMD
.byt $00
HEARTBEAT
.byt $00 
MAGIC1
.byt $00
.byt $00
.byt $00
.byt $00
ERRCOUNT
.byt $00
.byt $00
.byt $00
.byt $00


    pla
    lda #$de
    sta MAGIC1
    lda #$ad
    sta MAGIC1 + 1
    lda #$be
    sta MAGIC1 + 2
    lda #$ef
    sta MAGIC1 + 3

    // get ESP32 to modify memory 
    lda #1
    sta CMD
WAIT4
    lda CMD
    bne WAIT4   

    ldy #255
LOOP1
    ldx #0
    lda #0

    // x, y, a: wrapped around to starting point on branches to here 
    LOOP2 // loop2: write incrementing pattern 
        sta TESTAREA,x
        sta TESTAREA+$100,x
        sta TESTAREA+$200,x
        sta TESTAREA+$300,x
        jsr TESTJSR
        jsr TESTJSR
        jsr TESTJSR

        clc
        adc #1
        inx
        bne LOOP2

    inc HEARTBEAT

    // x, a: wrapped back to starting point for next loop
    LOOP3 // compare pattern, should appear in remapped pages 
        cmp TESTAREA+$400,x
        bne ERR1
        cmp TESTAREA+$500,x
        bne ERR1
        cmp TESTAREA+$600,x
        bne ERR1
        cmp TESTAREA+$700,x
        beq OK1
    ERR1
        jsr LOG_ERROR1
    OK1    
        clc
        adc #1
        inx
        bne LOOP3

    pha // write page 6 command byte and wait for response 
    lda #3
    sta CMD
    WAIT5
        lda CMD
        bne WAIT5   
    pla
    iny
    bne LOOP2

    clc
    adc #1
    sta SCREENMEM
//    jmp LOOP2
    rts

LOG_ERROR1
    pha

    // wiggle the screen error indicator 
    inc SCREENMEM+2

    // increment 24-bit ERRCOUNT value
    sec
    lda #0
    adc ERRCOUNT
    sta ERRCOUNT
    lda #0
    adc ERRCOUNT+1
    sta ERRCOUNT+1
    lda #0
    adc ERRCOUNT+2
    sta ERRCOUNT+2
    lda #0
    adc ERRCOUNT+3
    sta ERRCOUNT+3
    pla
    rts


// test sequence of rapid jsr instructions to stress the double-writes of the stack pushes 
TESTJSR
    jsr TESTJSR1
    rts 

TESTJSR1
    jsr TESTJSR2
    rts 

TESTJSR2
    jsr TESTJSR3
    rts 

TESTJSR3
    rts 
