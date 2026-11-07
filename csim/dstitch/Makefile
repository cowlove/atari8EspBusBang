dstitch_mount: dstitch_mount.cpp ../../components/disk_image/diskStitch.h ../../components/disk_image/diskStitchImageSDFS.h ../../main/diskStitchSmb.h ../../main/diskStitchProc.h ../../components/disk_image/diskImage.h
	g++ -DCSIM ${CDEFS} -I ../../components/disk_image -I ../libsmb2/include/ -I ../libsmb2/include/smb2 -g dstitch_mount.cpp ../libsmb2/build/lib/libsmb2.so -fpermissive -o dstitch_mount \
	`pkg-config fuse3 --cflags --libs` 

sdfs_test: test_sdfs.cpp ../../components/disk_image/diskStitchImageSDFS.h ../../components/disk_image/diskStitch.h ../../components/disk_image/diskImage.h
	g++ -DCSIM -I ../../components/disk_image -g test_sdfs.cpp -o sdfs_test

clean:
	rm -f dstitch_mount sdfs_test
