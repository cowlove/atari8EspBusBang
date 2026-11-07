# Disk images and the SIO drives (Sio), shared by the host app (main/) and the Sidecar app
# (sidecar/): the DiskImage sector interface, ATR images on SPIFFS (plain or
# .atr.gz, inflated into PSRAM), the SDFS stitch images (SPIFFS, SMB, proc:),
# the SMB ATR image and the gzip inflater. The proc: file lists stay with each app (main/procFsNodes.h). csim compiles these
# sources directly (csim/a800/Makefile).
idf_component_register(
    SRCS "diskFlash.cpp" "gzipImage.cpp" "diskImageSmb.cpp" "sio.cpp"
    INCLUDE_DIRS "."
        # The raw SPIFFS API (spiffs.h, SPIFFS_open() ...), which the
        # images use through the spiffs_fs_by_label() handle.
        "${IDF_PATH}/components/spiffs/spiffs/src/"
    REQUIRES spiffs esp_rom heap log lwip libsmb2
)
