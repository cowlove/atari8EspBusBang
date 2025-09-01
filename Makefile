hello_fuse: hello_fuse.c d1.h
	gcc hello_fuse.c -o hello_fuse `pkg-config fuse3 --cflags --libs`

