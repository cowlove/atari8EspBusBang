#pragma once
// "proc:" disks: a stitched DOS disk whose files are functions (ProcFsFile),
// e.g. reading UPTIME returns the uptime, writing HALT stops the run.
#include "diskStitch.h"

// One file in a "proc:" disk: what reading it returns, what writing it does.
// The node list is the app's (the host's is main/procFsNodes.h).
struct ProcFsFile { 
    string name;
    std::function<void(string &s)> read = [](string &){ return -1; };
    std::function<void(const string &s)> write = [](const string &){ return -1; };
    bool convertAtascii = true;
    bool filterEmptyWrites = true;
    bool lineMode = true;
};

class ProcFsConnection : public StorageInterface {
public:
    using ProcFsFile = ::ProcFsFile;
    vector<ProcFsFile> files;
private:
    int curFile = -1;
    string curIo;
    int curMode;
    int readdir(const char *path, std::function<void(int, struct dirent *, size_t)>func) {
        for (auto f : files) { 
            struct dirent de = {};
            #pragma GCC diagnostic ignored "-Wstringop-truncation"
            strncpy(de.d_name, f.name.c_str(), sizeof(de.d_name));
            de.d_type = DT_REG;
            func(0, &de, 100);
        }
        return files.size();
    };
    int open(const char *path, int mode, int ac = 0) {
        for (int n = 0; n < files.size(); n++) {  
            if (path == files[n].name) { 
                curFile = n;
                curIo = "";
                curMode = mode;
                if (mode == O_RDONLY)   
                    files[curFile].read(curIo);
                return 0;
            }
        }
        return -ENOENT;
    };
    // todo add "bool eof" argument to function 
    int pwrite(const uint8_t *buf, size_t len, size_t pos, bool eof) {
        if (curFile < 0)
            return -EBADF;
        if (curMode == O_RDONLY)
            return -EBADF;
        if (!files[curFile].write)
            return -EROFS;
        //if (std::string_view((const char *)buf, len).find(0) == std::string_view::npos) 
        //    curIo += string(buf, buf + len); // no string-terminating null, add the whole buf
        //else
        curIo += (const char *)buf;
        if (eof) {
            curIo.erase(std::remove(curIo.begin(), curIo.end(), 
                        static_cast<char>('\n')), curIo.end());
            curIo.erase(std::remove(curIo.begin(), curIo.end(), 
                        static_cast<char>(155)), curIo.end());
            if (curIo.length() > 0 || files[curFile].filterEmptyWrites == false) 
                files[curFile].write(curIo);
        }
        return len;
    }; 
    int rename(const char *, const char *) override { return -EROFS; }
    int pread(uint8_t *buf, size_t len, size_t pos) {
        if (pos >= curIo.length()) {
            len = 0;
        } else { 
            len = std::min(curIo.length() - pos, len);
            memcpy(buf, curIo.c_str() + pos, len);
        }
        return len;
    }; 
public:
    ProcFsConnection(const char *url) {}
    void add(const ProcFsFile &f) { files.push_back(f); }
};

class DiskProcFs : public DiskStitchGeneric<ProcFsConnection> { 
    public:
    DiskProcFs(const vector<ProcFsConnection::ProcFsFile> &a) { 
        storage.files = a; 
        start(); 
    };
};
