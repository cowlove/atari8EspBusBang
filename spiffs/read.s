        .include "atari.inc"
        .include "errno.inc"
        .globl read

read:
        jsr     __rwsetup
        beq     read_done
        cpx     #$FF
        beq     read_invalid
        lda     #GETCHR
        sta     ICCOM,x
        jsr     CIOV
        bpl     read_done
        cpy     #EOFERR
        beq     read_done
        tya
        jmp     __mappederrno

read_done:
        lda     ICBLL,x
        pha
        lda     ICBLH,x
        tax
        lda     #0
        sta     __oserror
        pla
        rts

read_invalid:
        lda     #<EINVAL
        jmp     __directerrno
