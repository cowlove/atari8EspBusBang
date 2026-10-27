#pragma once
#include "diskStitch.h"
#ifndef CSIM
#include "util.h"
#endif

class ProcFsConnection : public StorageInterface {
public:
    struct ProcFsFile { 
        string name;
        std::function<void(string &s)> read = [](string &){ return -1; };
        std::function<void(const string &s)> write = [](const string &){ return -1; };
    };
    vector<ProcFsFile> files;
private:
    int curFile = -1;
    string curIo;
    int curMode;
    int readdir(const char *path, std::function<void(int, struct dirent *, size_t)>func) {
        for (auto f : files) { 
            struct dirent de = {0};
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
        if (eof) 
            files[curFile].write(curIo);
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

static const vector<ProcFsConnection::ProcFsFile> procFsNodes =
{
    {
        .name = "WIFI",
        .read = [](string &buf) {},
        .write = [](const string &buf) {
            if (buf == "1") {
                wifiRun();
            } else {
                // TODO figure out how to shutdown WIFI
            }
        },
    },
    {   .name = "LOG",
        .read = [](string &buf) {},
        .write = [](const string &buf) { 
            string str = buf; 
            str.erase(std::remove(str.begin(), str.end(), static_cast<char>(155)), str.end());
            printf("LOG: '%s'\n", str.c_str()); 
        },
    },
    {   .name = "HALT",
        .write = [](const string &buf) { 
            string str = buf; 
            str.erase(std::remove(str.begin(), str.end(), static_cast<char>(155)), str.end());
            if (str.length() > 0) {
                printf("LOG: HALT written '%s'\n", str.c_str()); 
                exitReason = "ProcFS HALT file written :" + str; 
                exitFlag = -5; 
            }
        },
    },
    {
        .name = "FOOP",
        .read = [](string &buf) {
            buf = "foop contents\233";
            printf("read from FOOP\n");
        },
        .write = [](const string &buf) {
            printf("write to FOOP: '%s'\n", buf.c_str());
        },
    },
};
