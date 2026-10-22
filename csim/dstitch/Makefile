dstitch_mount: dstitch_mount.cpp fuse_d2.h ../../main/diskStitch.h ../../main/diskStitchImageSDFS.h ../../main/diskStitchSmb.h ../../main/diskStitchProc.h ../../main/diskImage.h
	g++ -DCSIM ${CDEFS} -I ./libsmb2/include/ -I ./libsmb2/include/smb2 -g dstitch_mount.cpp libsmb2/build/lib/libsmb2.so -fpermissive -o dstitch_mount \
	`pkg-config fuse3 --cflags --libs` 

sdfs_test: test_sdfs.cpp ../../main/diskStitchImageSDFS.h ../../main/diskStitch.h ../../main/diskImage.h
	g++ -DCSIM -g test_sdfs.cpp -o sdfs_test

clean:
	rm -f dstitch_mount sdfs_test
