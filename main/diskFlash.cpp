#include "diskFlash.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "spiffs.h"

#include <string>
#include <vector>
using std::vector;
using std::string;

// returns TRUE if text string matches wild pattern with * and ?
bool naive_recursive_match(const char *text, const char *wild) {
  while (*text != '\0') {
    if (*wild == '*') {
      // any number of stars act as one star
      while (*++wild == '*')
        continue;
      // a star at the end of the pattern matches any text
      if (*wild == '\0')
        return true;
      // star-loop: match the rest of the pattern and text
      while (naive_recursive_match(text, wild) == false && *text != '\0')
        text++;
      return *text != '\0';
    }
    // ? matches any character or we match the current non-NUL character
    if (*wild != '?' && toupper(*wild) != toupper(*text))
      return false;
    text++;
    wild++;
  }
  // ignore trailing stars
  while (*wild == '*')
    wild++;
  // at end of text means success if nothing else is left to match
  return *wild == '\0';
}

vector<string> spiffsDir(struct spiffs_t*fs, const char *d, const char *pat, bool icase) { 
    vector<string> result;
    spiffs_DIR dir;
    spiffs_dirent dirent; 
    SPIFFS_opendir(fs, d, &dir);
    while(SPIFFS_readdir(&dir, &dirent) != NULL){
        if (naive_recursive_match((const char *)dirent.name, pat))
            result.push_back((const char *)dirent.name);
    }
    SPIFFS_closedir(&dir);
    return result;
}

void DiskImageATR::open(const char *f, bool cacheInPsram) {
    image = NULL;
    spiffs_stat stat;
    if (SPIFFS_stat(fs, f, &stat) < 0 ||
        (fd = SPIFFS_open(fs, f, SPIFFS_O_RDWR, 0)) < 0) { 
        printf("AtariDisk::open('%s'): file open failed\n", f);
        return;
    }
    size_t fsize = stat.size;
    int r = SPIFFS_read(fs, fd, &header, sizeof(header));
    if (r != sizeof(header) || header.magic != 0x0296) { 
        printf("DiskImage::open('%s'): bad file or bad atr header\n", f);
        SPIFFS_close(fs, fd);
        return;
    }
    printf("DiskImage::open('%s'): sector size %d, header size %d\n", 
        f, header.sectorSize, sizeof(header));
    if (cacheInPsram) { 
        size_t dataSize = (header.pars + header.parsHigh * 0x10000) * 0x10;
        image = (uint8_t *)heap_caps_malloc(dataSize, MALLOC_CAP_SPIRAM);
        if (image == NULL) {
            printf("DiskImage::open('%s'): psram heap_caps_malloc(%d) failed!\n", f, dataSize);
            heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
            SPIFFS_close(fs, fd);
            return;
        }
        printf("Opened '%s' file size %zu bytes, reading data: ", f, fsize);
        SPIFFS_lseek(fs, fd, sizeof(header), SPIFFS_SEEK_SET);
        int r = SPIFFS_read(fs, fd, image, dataSize);
        SPIFFS_close(fs, fd); 
        if (r != dataSize) { 
            printf("wrong size, discarding\n");
            heap_caps_free(image);
            image = NULL;
            return;
        }
        printf("OK\n");
    } 
    filename = f;
}
bool IRAM_ATTR DiskImageATR::valid() { return header.magic == 0x0296; }

int IRAM_ATTR DiskImageATR::sectorSize() { return header.sectorSize; }

int IRAM_ATTR DiskImageATR::sectorCount() { return (header.pars + header.parsHigh * 256) * 0x10 / header.sectorSize; }

void DiskImageATR::close() {
    if (image != NULL) {
        heap_caps_free(image);
        image = NULL;
    } else { 
        SPIFFS_close(fs, fd); 
    }
}

size_t IRAM_ATTR DiskImageATR::read(uint8_t *buf, size_t sector) {
    int secSize = header.sectorSize;
    // first 3 sectors are always 128 bytes even on DD disks
    if (sector <= 3) secSize = 128;
    int offset = (sector - 1) * secSize;
    if (sector > 3) offset -= 3 * (secSize - 128);
    if(image != NULL) {
        for(int n = 0; n < secSize; n++) 
            buf[n] = image[offset + n];
        return secSize;
    } else if(filename.length() != 0) {
        SPIFFS_lseek(fs, fd, offset + sizeof(header), SPIFFS_SEEK_SET);
        return SPIFFS_read(fs, fd, buf, secSize);                                    
    }
    return 0;
}

size_t IRAM_ATTR DiskImageATR::write(const uint8_t *buf, size_t sector) { 
    int secSize = header.sectorSize;
    // first 3 sectors are always 128 bytes even on DD disks
    if (sector <= 3) secSize = 128;
    int offset = (sector - 1) * secSize;
    if (sector > 3) offset -= 3 * (secSize - 128);
    if(image != NULL) {
        for(int n = 0; n < secSize; n++) image[offset + n] = buf[n];
        return secSize;
    } else if(filename.length() != 0) {
        //printf("write %d at %d:\n", len, offset); 
        SPIFFS_lseek(fs, fd, offset + sizeof(header), SPIFFS_SEEK_SET);
        size_t r = SPIFFS_write(fs, fd, (void *)buf, secSize);  
        //SPIFFS_flush(fs, fd);
        return r;
    }
    return 0;
}


// Todo factor our ATR header stuff into parent class

void wifiRun();

void DiskImageSMB::open(const char *f, bool cacheInPsram) {
    smb2 = smb2_init_context();
    if (smb2 == NULL) {
            ESP_LOGE(TAG, "Failed to init context");
            //while(1){ vTaskDelay(1); }
            return;
    }

    ESP_LOGI(TAG, "CONFIG_SMB_USER=[%s]",CONFIG_SMB_USER);
    ESP_LOGI(TAG, "CONFIG_SMB_HOST=[%s]",CONFIG_SMB_HOST);
    ESP_LOGI(TAG, "CONFIG_SMB_PATH=[%s]",CONFIG_SMB_PATH);  

    char smburl[64];
    sprintf(smburl, "smb://%s@%s/%s/esp-idf-cat.txt", CONFIG_SMB_USER, CONFIG_SMB_HOST, CONFIG_SMB_PATH);
    ESP_LOGI(TAG, "smburl=%s", smburl);

#if CONFIG_SMB_NEED_PASSWORD
        smb2_set_password(smb2, CONFIG_SMB_PASSWORD);
#endif

    url = smb2_parse_url(smb2, smburl);
    if (url == NULL) {
            ESP_LOGE(TAG, "Failed to parse url: %s", smb2_get_error(smb2));
            //while(1){ vTaskDelay(1); }
            smb2_destroy_context(smb2);
            return;
    }

    smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
}

bool IRAM_ATTR DiskImageSMB::valid() { 
    if (header.magic == 0x0296) 
        return true; 
    wifiRun();
    // set up connection, read header. 
    return false;
}
