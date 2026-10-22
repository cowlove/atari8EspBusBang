#!/usr/bin/python
from fuse import FUSE, FuseOSError, Operations
from errno import ENOENT

class SimpleFS(Operations):
    def __init__(self):
        self.files = {
            '/': {'st_mode': (0o40755), 'st_nlink': 2},  # Root directory
            '/hello.txt': {'st_mode': (0o100644), 'st_size': len("Hello, FUSE!"), 'content': "Hello, FUSE!"}
        }

    def getattr(self, path, fh=None):
        if path not in self.files:
            raise FuseOSError(ENOENT)
        return self.files[path]

    def readdir(self, path, fh):
        if path == '/':
            return ['.', '..', 'hello.txt']
        raise FuseOSError(ENOENT)

    def open(self, path, flags):
        if path == '/hello.txt':
            return 0  # Success
        raise FuseOSError(ENOENT)

    def read(self, path, size, offset, fh):
        if path == '/hello.txt':
            content = self.files[path]['content']
            return content[offset:offset + size].encode('utf-8')
        raise FuseOSError(ENOENT)

if __name__ == '__main__':
    mount_point = './mnt'  # Create a directory named 'mnt'
    FUSE(SimpleFS(), mount_point, foreground=True)

