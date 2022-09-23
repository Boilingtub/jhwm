#!/bin/bash


#Wayland variables
WAYLAND_PROTOCOLS=$(pkg-config --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER=$(pkg-config --variable=wayland_scanner wayland-scanner)
Wayland_libraries="$(pkg-config --cflags --libs wlroots) $(pkg-config --cflags --libs wayland-server) $(pkg-config --cflags --libs xkbcommon)"
Essential_libraries=" -I/usr/local/include -I/usr/include/libdrm -I/usr/include/pixman-1 -L/usr/local/lib/x86_64-linux-gnu"


#Configure
ProjectDir='/home/hendrik/Coding-Projects/C/Wayland-jewl'
SrcFiles="jewl.c"
Libraries=" ${Essential_libraries} ${Wayland_libraries}"
Exename='jewl'

#Procedure
  #Get Wayland headers
  ${WAYLAND_SCANNER} server-header ${WAYLAND_PROTOCOLS}/stable/xdg-shell/xdg-shell.xml xdg-shell-protocol.h

cd "${ProjectDir}"
if gcc -g -Werror -I. -DWLR_USE_UNSTABLE -o ${Exename} ${SrcFiles} ${Libraries}; then
        echo "compilation Success"
        #./${Exename}
else
        echo "compilation Failure"
fi  

