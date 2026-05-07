#ifndef __WSTTS_EXAMPLE_H__
#define __WSTTS_EXAMPLE_H__

// Keep WebSocket++ on Boost types to match the Boost.Asio transport.
#ifndef _WEBSOCKETPP_NO_CPP11_MEMORY_
#define _WEBSOCKETPP_NO_CPP11_MEMORY_
#endif

#ifndef _WEBSOCKETPP_NO_CPP11_FUNCTIONAL_
#define _WEBSOCKETPP_NO_CPP11_FUNCTIONAL_
#endif

#ifndef _WEBSOCKETPP_NO_CPP11_SYSTEM_ERROR_
#define _WEBSOCKETPP_NO_CPP11_SYSTEM_ERROR_
#endif

#include <openssl/ssl.h>
#ifndef SSL_R_SHORT_READ
#define SSL_R_SHORT_READ 0
#endif

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <string>
#include <list>
#include <pthread.h>

typedef websocketpp::client<websocketpp::config::asio_tls_client> client;
typedef websocketpp::config::asio_tls_client::message_type::ptr message_ptr;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> context_ptr;
typedef client::connection_ptr connection_ptr;

struct asr_params
{
    /*����  ԭʼ��Ƶÿ���̶�5120�ֽڣ����һ������, ����ʹ��base64���б���*/
    std::string audio_data;
    /*����
    * ��Ƶ�����ʽ
        pcm
        wav
    */
    std::string audio_format;
    /* ����
    * ��Ƶ������
        8000
        16000
    */
    int sample_rate;
    /* ����
     * ��Ƶ�������������Ϊ1���� 0����ʼ��Ƶ֡ >0���м���Ƶ֡����1 2 3 4 �� 1000�� -n��������Ƶ֡����-1001)
     */
    int req_idx;
    /* �Ǳ���
    * ģ�����ƣ�ͨ��ģ�� "common"��
        Ӣ��ģ��"english"��
        Ĭ��ֵΪ��common��
    */
    std::string domain;
    /* �Ǳ���
     * true: �ӱ�㣬Ĭ��ֵ false�������ӱ��
     */
    bool add_pct;
    asr_params()
    {
        domain = "common";
        add_pct = true;
    }
};

class wssclient
{
public:
    //   std::string get_token(const std::string& client_id, const std::string& client_secret);
    std::string gen_json_request(const std::string &token, const asr_params &params);

public:
    wssclient(const asr_params &params);
    virtual ~wssclient();
    void run();
    bool open_connection(const std::string &uri, int *connected = NULL);
    // void send_request_frame(websocketpp::connection_hdl hdl, const std::string& data);
    bool send_request_frame(websocketpp::connection_hdl hdl, char *data, int datalen);
    void stop_io_service();
    void close_connection();

protected:
    context_ptr on_tls_init(websocketpp::connection_hdl);
    void on_open(websocketpp::connection_hdl hdl);
    void on_close(websocketpp::connection_hdl hdl);
    void on_fail(websocketpp::connection_hdl hdl);
    void on_message(websocketpp::connection_hdl hdl, message_ptr msg);

public:
    void work_thread();
    
private:
    void recv_asr_realtime_msg(const std::string &msg);

public:
    client ws_client_;
    websocketpp::connection_hdl hdl_;
    pthread_mutex_t hdl_mutex_;
    //    boost::thread*                      work_thread_;
    std::string access_token_;
    asr_params asr_params_;

public:
    std::string uuid_;
    std::string leg_;
    int bufidx_;
    time_t timeout_;
    int m_exit_;     //-1 init 1 exit, 0 run
    int *connected_; // 0 not connected, 1 connected
    pthread_t work_thread_id_; // 工作线程ID用于清理
};

#endif // __WSTTS_EXAMPLE_H__
