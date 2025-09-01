hello_fuse: hello_fuse.cpp fuse_d2.h
	gcc hello_fuse.cpp -fpermissive -o hello_fuse `pkg-config fuse3 --cflags --libs`

