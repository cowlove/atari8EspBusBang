# Main's second-stage bootloader hook (hooks.c): GPIO43/44 (Atari D5/D6,
# the pads' reset function U0TXD/U0RXD) made plain inputs first thing.
idf_component_register(SRCS "hooks.c"
                       REQUIRES soc hal esp_rom)
