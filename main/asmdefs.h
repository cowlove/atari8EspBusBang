//#define HW_600XL
;;// trying to run HW_600XL on 800XL demonstrates a repeatRead timeout very reliably 

#ifdef HW_600XL
#define HALT_6502
#define PERM_EXTSEL
#else
#undef HALT_6502
#undef PERM_EXTSEL
#endif




;;// flags for ESP32_IOCB_REQ
;;//////////////////////////////
#define REQ_FLAG_NORMAL       1             // 
#define REQ_FLAG_DETACHSAFE   2         // 6502 is ready for bus detach 
#define REQ_FLAG_COPYIN       4             // data has been copied in per ESP32_IOCB_COPYBUF/LEN
#define REQ_FLAG_STACKWAIT    8

;;// flags for ESP32_IOCB_RESULT
////////////////////////////////
#define RES_FLAG_COMPLETE         1           // command complete
#define RES_FLAG_NEED_DETACHSAFE  2    // re-issue command, need REQ_FLAG_DETACHSAFE
#define RES_FLAG_NEED_COPYIN      4        // re-issue command, need copyin from ESP32_IOCB32_COPYBUF and REQ_FLAG_COPYIN
#define RES_FLAG_COPYOUT          8            // command complete, data is in ESP32_IOCB_COPYBUF for copyout 
#define RES_FLAG_MONITOR          128            // immediately reissue monitor command, repeat until clear 

#define REQ_MAX_COPYLEN 512

#define PBICMD_WAIT_VBLANK        10
#define PBICMD_INTERRUPT          11
#define PBICMD_UNMAP_NATIVE_BLOCK 12
#define PBICMD_REMAP_NATIVE_BLOCK 13

#define NATIVE_BLOCK_ADDR 4096
#define NATIVE_BLOCK_LEN 2048
