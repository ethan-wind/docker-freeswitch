#!/bin/sh
set -eu

FREESWITCH_PREFIX="${FREESWITCH_PREFIX:-/usr/local/freeswitch}"
WEBSOCKETPP_INCLUDE_PATH="${WEBSOCKETPP_INCLUDE_PATH:-/usr/local/include}"

export PKG_CONFIG_PATH="${FREESWITCH_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

FS_CFLAGS="$(pkg-config --cflags freeswitch 2>/dev/null || echo "-I${FREESWITCH_PREFIX}/include/freeswitch")"
FS_LIBS="$(pkg-config --libs freeswitch 2>/dev/null || echo "-L${FREESWITCH_PREFIX}/lib -lfreeswitch")"

g++ -std=c++11 -shared -fPIC -o mod_myasr.so \
    httpclient.cpp mod_myasr.cpp \
    -pthread \
    ${FS_CFLAGS} \
    -I"${WEBSOCKETPP_INCLUDE_PATH}" \
    -I/usr/local/include \
    -L/usr/local/lib \
    ${FS_LIBS} \
    -ldl -lm -lssl -lcrypto -lcurl -lpthread -lboost_thread -lboost_system \
    -Wl,-rpath-link="${FREESWITCH_PREFIX}/lib",-rpath="${FREESWITCH_PREFIX}/lib"
