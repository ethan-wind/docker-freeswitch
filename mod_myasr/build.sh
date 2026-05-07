#/bin/sh
freeswitch_include_path="/opt/freeswitch/include/"
freeswitch_libs_path="/opt/freeswitch/lib"

g++ -std=c++11 -shared -fPIC -o mod_myasr.so httpclient.cpp mod_myasr.cpp  -pthread -I ${freeswitch_include_path} -I/usr/local/include -I/opt/websocketpp-master -I/usr/local/curl/include -I/usr/local/openssl/include/openssl -L/usr/local/lib -L/usr/local/curl/lib  -L/usr/local/openssl/lib -L ${freeswitch_libs_path} -ldl -lm -lssl -lcurl -lpthread -lboost_thread -lboost_system -lfreeswitch -Wl,-rpath-link=${freeswitch_libs_path},-rpath=${freeswitch_libs_path}
