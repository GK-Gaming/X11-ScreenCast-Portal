#!/bin/sh
clang events.c -o xdg-desktop-portal-x11 -I/usr/include/spa-0.2 -I/usr/include/pipewire-0.3 -I/home/gaming/dev/libdrmtap/include -ldrmtap -lsystemd -ggdb
clang video-src-alloc.c -o xdg-desktop-portal-x11-session -I/usr/include/spa-0.2 -I/usr/include/pipewire-0.3 -I/home/gaming/dev/libdrmtap/include -lpipewire-0.3 -ldrmtap -lm -lsystemd -ggdb

