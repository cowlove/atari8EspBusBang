hello_fuse: hello_fuse.cpp fuse_d2.h ../../main/diskStitch.h ../../main/diskStitchSmb.h ../../main/diskStitchProc.h ../../main/diskImage.h
	g++ -DCSIM ${CDEFS} -I ./libsmb2/include/ -I ./libsmb2/include/smb2 -g hello_fuse.cpp libsmb2/build/lib/libsmb2.so -fpermissive -o hello_fuse \
	`pkg-config fuse3 --cflags --libs` 

clean:
	rm -f hello_fuse
