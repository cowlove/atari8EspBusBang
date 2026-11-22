# The Sidecar command/response protocol over i2slink (../i2slink), shared by
# the host app (../../main) and the Sidecar app (../../sidecar).

idf_component_register(
    SRCS "sidecar.cpp" "sidecar_file_server.cpp"
    INCLUDE_DIRS "."
    REQUIRES a8log i2slink
    PRIV_REQUIRES esp_timer esp_rom
)
