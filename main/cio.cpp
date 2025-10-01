#include "cio.h"

AtariIO *fakeFile; 

DRAM_ATTR const char *defaultProgram = 
#ifdef BOOT_SDX
        "10 PRINT \"HELLO FROM BASIC\" \233"
        "20 PRINT \"HELLO 2\" \233"
        "30 CLOSE #4:OPEN #4,8,0,\"J1:DUMPSCREEN\":PUT #4,0:CLOSE #4\233"
        "40 DOS\233 "
        "RUN \233"
#else
        //"1 DIM D$(255) \233"
        //"10 REM A=USR(1546, 1) \233"
        //"15 OPEN #1,4,0,\"J2:\" \233"
        //"20 GET #1,A  \233"
        //"38 CLOSE #1  \233"
        //"39 PRINT \"OK\" \233"
        //"40 GOTO 10 \233"
        //"41 OPEN #1,8,0,\"J\" \233"
        //"42 PUT #1,A + 1 \233"
        //"43 CLOSE #1 \233"
        //"50 PRINT \" -> \"; \233"
        //"52 PRINT COUNT; \233"
        //"53 COUNT = COUNT + 1 \233"
        //"60 OPEN #1,8,0,\"D1:DAT\":FOR I=0 TO 20:XIO 11,#1,8,0,D$:NEXT I:CLOSE #1 \233"
        //"61 TRAP 61: CLOSE #1: OPEN #1,4,0,\"D1:DAT\":FOR I=0 TO 10:XIO 7,#1,4,0,D$:NEXT I:CLOSE #1 \233"
        //"61 CLOSE #1: OPEN #1,4,0,\"D1:DAT\":FOR I=0 TO 10:XIO 7,#1,4,0,D$:NEXT I:CLOSE #1 \233"
        //"63 OPEN #1,4,0,\"D2:DAT\":FOR I=0 TO 10:XIO 7,#1,4,0,D$:NEXT I:CLOSE #1 \233"
        "60 TRAP 80:XIO 80,#1,0,0,\"D1:DIR D4:*.*/A\" \233"
        "70 TRAP 80:XIO 80,#1,0,0,\"D1:X.CMD\" \233"
        //"80 GOTO 10 \233"
        "RUN\233"
#endif
        ;
