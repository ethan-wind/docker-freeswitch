#!/bin/sh
set -eu

CXX="${CXX:-g++}"
OUTPUT="${OUTPUT:-mod_myasr.so}"

FREESWITCH_PREFIX="${FREESWITCH_PREFIX:-/opt/freeswitch}"
FREESWITCH_INCLUDE_PATH="${FREESWITCH_INCLUDE_PATH:-${FREESWITCH_PREFIX}/include/freeswitch}"
FREESWITCH_LIB_PATH="${FREESWITCH_LIB_PATH:-${FREESWITCH_PREFIX}/lib}"
WEBSOCKETPP_INCLUDE_PATH="${WEBSOCKETPP_INCLUDE_PATH:-/opt/KDXF_ASR/websocketpp-master}"

fail() {
    echo "错误：$*" >&2
    exit 1
}

command -v "${CXX}" >/dev/null 2>&1 || fail "找不到 C++ 编译器：${CXX}"
[ -f "${FREESWITCH_INCLUDE_PATH}/switch.h" ] || \
    fail "找不到 ${FREESWITCH_INCLUDE_PATH}/switch.h"
[ -d "${FREESWITCH_LIB_PATH}" ] || \
    fail "找不到 FreeSWITCH 库目录：${FREESWITCH_LIB_PATH}"
[ -f "${WEBSOCKETPP_INCLUDE_PATH}/websocketpp/client.hpp" ] || \
    fail "找不到 websocketpp/client.hpp，请安装 websocketpp-devel，或设置 WEBSOCKETPP_INCLUDE_PATH"

echo "FreeSWITCH 头文件：${FREESWITCH_INCLUDE_PATH}"
echo "FreeSWITCH 库目录：${FREESWITCH_LIB_PATH}"
echo "WebSocket++ 头文件：${WEBSOCKETPP_INCLUDE_PATH}"

"${CXX}" -std=c++11 -Wno-deprecated-declarations -shared -fPIC -o "${OUTPUT}" \
    httpclient.cpp mod_myasr.cpp \
    -pthread \
    -I"${FREESWITCH_INCLUDE_PATH}" \
    -I"${WEBSOCKETPP_INCLUDE_PATH}" \
    -L"${FREESWITCH_LIB_PATH}" \
    -ldl -lm -lssl -lcrypto -lcurl -lpthread \
    -lboost_thread -lboost_system -lfreeswitch \
    -Wl,-rpath-link,"${FREESWITCH_LIB_PATH}" \
    -Wl,-rpath,"${FREESWITCH_LIB_PATH}"

echo "编译完成：$(pwd)/${OUTPUT}"
