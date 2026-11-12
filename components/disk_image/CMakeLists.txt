# Disk images and the SIO drives (Sio), shared by the host app (main/) and the Sidecar app
# (sidecar/): the DiskImage sector interface, ATR images (plain or .atr.gz,
# inflated into PSRAM), the SDFS stitch images (a directory, SMB, proc:),
# the SMB ATR image and the gzip inflater. The proc: file lists stay with each app (main/procFsNodes.h). csim compiles these
# sources directly (csim/a800/Makefile).
idf_component_register(
    SRCS "diskFlash.cpp" "gzipImage.cpp" "diskImageSmb.cpp" "sio.cpp"
    INCLUDE_DIRS "."
    # Files through the VFS (open(), read(), opendir() ...), whatever is
    # mounted: SPIFFS, an SD card.
    REQUIRES vfs esp_rom heap log lwip libsmb2
)
