#ifdef CSIM
#include "csim.h"
#endif

#include "diskImageSmb.h"
#include "esp_attr.h"
#include "esp_log.h"

#include <stdio.h>

// Todo factor our ATR header stuff into parent class

void wifiRun();

void DiskImageSMB::open(const char *f, bool cacheInPsram) {
    (void)cacheInPsram; (void)f;
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
