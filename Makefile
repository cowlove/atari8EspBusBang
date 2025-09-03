hello_fuse: hello_fuse.cpp fuse_d2.h
	g++ -I ./libsmb2/include/ -I ./libsmb2/include/smb2 -g hello_fuse.cpp libsmb2/build/lib/libsmb2.so -fpermissive -o hello_fuse \
	`pkg-config fuse3 --cflags --libs` 

