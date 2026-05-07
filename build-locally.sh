#!/bin/bash

# Check if .env file exists
if [ ! -f ".env" ]; then
  echo "Error: .env file not found"
  exit 1
fi

cmakeVersion=3.28.3
grpcVersion=1.64.2
libwebsocketsVersion=4.3.3
speechSdkVersion=1.37.0
spandspVersion=0d2e6ac
sofiaVersion=1.13.17
awsSdkCppVersion=1.11.345
freeswitchModulesVersion=1.2.22
freeswitchVersion=1.10.12

dockerImageRepo=ue-test.harbor.useasy.net/ue/freeswitch
dockerImageVersion=1.10.12

docker build \
  --build-arg CACHEBUST=$(date +%s) \
  --build-arg CMAKE_VERSION="${cmakeVersion}" \
  --build-arg GRPC_VERSION="${grpcVersion}" \
  --build-arg LIBWEBSOCKETS_VERSION="${libwebsocketsVersion}" \
  --build-arg SPEECH_SDK_VERSION="${speechSdkVersion}" \
  --build-arg SPANDSP_VERSION="${spandspVersion}" \
  --build-arg SOFIA_VERSION="${sofiaVersion}" \
  --build-arg AWS_SDK_CPP_VERSION="${awsSdkCppVersion}" \
  --build-arg FREESWITCH_MODULES_VERSION="${freeswitchModulesVersion}" \
  --build-arg FREESWITCH_VERSION="${freeswitchVersion}" \
  -t "${dockerImageRepo}:${dockerImageVersion}" --push .