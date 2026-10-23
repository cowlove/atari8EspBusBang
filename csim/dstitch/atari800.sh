#!/bin/bash
video_args=(-stretch 3)
if [[ -n ${SSH_CONNECTION:-} || ${ATARI800_HEADLESS:-0} -eq 1 ]]; then
	video_args=(-no-video-accel -stretch 1)
fi

exec atari800 -xl "${video_args[@]}" \
	-cart ../../spiffs/SDX450_maxflash1.car  \
	./fuse_d1.atr ./mnt/fuse_d2.img \
	-turbo \
    -dump-screen-after 900 
