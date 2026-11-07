# Disk images, shared by the host app (main/) and the Sidecar app
# (sidecar/): the DiskImage sector interface, ATR images on SPIFFS (plain or
# .atr.gz, inflated into PSRAM), the SDFS stitch image and the gzip inflater.
# The host-only images (SMB, procfs) stay in main/. csim compiles these
# sources directly (csim/a800/Makefile).
idf_component_register(
    SRCS "diskFlash.cpp" "gzipImage.cpp"
    INCLUDE_DIRS "."
        # The raw SPIFFS API (spiffs.h, SPIFFS_open() ...), which the
        # images use through the spiffs_fs_by_label() handle.
        "${IDF_PATH}/components/spiffs/spiffs/src/"
    REQUIRES spiffs esp_rom heap
)
