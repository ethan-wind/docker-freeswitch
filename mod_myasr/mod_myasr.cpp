#include <switch.h>
#include <string>
#include <signal.h>
#include <vector>
#include <iostream>
#include <sstream>
#include <cctype>
#include <time.h>
#include <pthread.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/md5.h>
#include "wssclient.h"
#include "httpclient.h"

using std::cout;
using std::endl;
using std::list;
using std::ostringstream;
using std::string;
using std::vector;

using websocketpp::lib::bind;
using namespace std;

SWITCH_MODULE_LOAD_FUNCTION(mod_myasr_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_myasr_shutdown);

extern "C"
{
	SWITCH_MODULE_DEFINITION(mod_myasr, mod_myasr_load, mod_myasr_shutdown, NULL);
};

#define MAX_PAYLOAD_SIZE 1280 // 1280
#define MAX_USER_CHANNEL 10000
#define MYASR_AUDIO_QUEUE_SIZE 8
#define MYASR_TARGET_SAMPLE_RATE 16000
#define MYASR_CHANNEL_FREE 0
#define MYASR_CHANNEL_RUNNING 1
#define MYASR_CHANNEL_STOPPING 2

int g_app_shutdown = 0; // 1 mod_myasr_load, 0 mod_myasr_shutdown

int g_user_channel_state[MAX_USER_CHANNEL] = {0}; // 0 free, 1 running, 2 stopping
time_t g_timeout[MAX_USER_CHANNEL] = {0};
int g_nCurr_channel_index = 0;
static int g_wss_thread_count = 0;

struct myasr_audio_queue
{
	unsigned char data[MYASR_AUDIO_QUEUE_SIZE][MAX_PAYLOAD_SIZE + 1];
	size_t len[MYASR_AUDIO_QUEUE_SIZE];
	uint32_t head;
	uint32_t tail;
	uint32_t count;
	uint32_t dropped;
};

// fs audio buffers for send
myasr_audio_queue *g_audio_queues[MAX_USER_CHANNEL] = {0};

pthread_mutex_t idx_mutex;
int idx_Lock() { return pthread_mutex_lock(&idx_mutex); }
int idx_Unlock() { return pthread_mutex_unlock(&idx_mutex); }
int idx_TryLock() { return pthread_mutex_trylock(&idx_mutex); }

static switch_bool_t myasr_channel_is_running(int aleg_idx)
{
	if (aleg_idx < 0 || aleg_idx >= MAX_USER_CHANNEL)
	{
		return SWITCH_FALSE;
	}

	idx_Lock();
	switch_bool_t running = g_user_channel_state[aleg_idx] == MYASR_CHANNEL_RUNNING ? SWITCH_TRUE : SWITCH_FALSE;
	idx_Unlock();
	return running;
}

struct user_data
{
	switch_media_bug_t *bug;
	int mode; // 1 aleg, 0 bleg

	char aleg_uuid[64];

	char aleg_buf[MAX_PAYLOAD_SIZE + 1];
	size_t aleg_buf_len;
	int aleg_bufidx;

	switch_audio_resampler_t *resampler;
	uint32_t read_sample_rate;
	int resample_unavailable_logged;
};

struct WsUserData
{
	char uuid[64];
	char leg[10];
	int sendcounter;
	int recvcounter;
	int bufidx;
	time_t timeout;
	int sendFlag;		  // 0 send firstData, 1 send audio, 2 send endData
	volatile int connected; // 0 not connected, 1 connected
	char custom_appid[64]; // 新增：自定义appid
	pthread_mutex_t ref_mutex;
	int refcount;
};

static switch_bool_t myasr_wud_init(struct WsUserData *wud)
{
	if (!wud)
	{
		return SWITCH_FALSE;
	}
	memset(wud, 0, sizeof(*wud));
	if (pthread_mutex_init(&wud->ref_mutex, NULL) != 0)
	{
		return SWITCH_FALSE;
	}
	wud->refcount = 1;
	return SWITCH_TRUE;
}

static void myasr_wud_retain(struct WsUserData *wud)
{
	if (!wud)
	{
		return;
	}
	pthread_mutex_lock(&wud->ref_mutex);
	wud->refcount++;
	pthread_mutex_unlock(&wud->ref_mutex);
}

static void myasr_wud_release(struct WsUserData *wud)
{
	int free_now = 0;

	if (!wud)
	{
		return;
	}

	pthread_mutex_lock(&wud->ref_mutex);
	if (wud->refcount > 0)
	{
		wud->refcount--;
	}
	free_now = (wud->refcount == 0);
	pthread_mutex_unlock(&wud->ref_mutex);

	if (free_now)
	{
		pthread_mutex_destroy(&wud->ref_mutex);
		free(wud);
	}
}

#define MAX_HTTP_RECV_BUFFER 5120
static struct
{
	switch_memory_pool_t *pool;
	char *client_id;
	char *client_secret;
	char access_token[512];
	int wait_timeout;
	int token_flag; // 1 valid, 0 invalid

	char *server_ip;
	int server_port;
	char *url_param;
	char *wss_server_ip;

	char http_recv_buffer[MAX_HTTP_RECV_BUFFER + 2];
	int http_recv_buffer_len;
	char *app_id;
	char *app_key;
	char *wss_url;
} globals;

static void myasr_audio_queue_reset_locked(myasr_audio_queue *queue)
{
	if (!queue)
	{
		return;
	}

	memset(queue->data, 0, sizeof(queue->data));
	memset(queue->len, 0, sizeof(queue->len));
	queue->head = 0;
	queue->tail = 0;
	queue->count = 0;
	queue->dropped = 0;
}

static switch_bool_t myasr_audio_queue_prepare(int aleg_idx)
{
	if (aleg_idx < 0 || aleg_idx >= MAX_USER_CHANNEL)
	{
		return SWITCH_FALSE;
	}

	idx_Lock();
	if (!g_audio_queues[aleg_idx])
	{
		g_audio_queues[aleg_idx] = (myasr_audio_queue *)calloc(1, sizeof(myasr_audio_queue));
	}
	myasr_audio_queue *queue = g_audio_queues[aleg_idx];
	myasr_audio_queue_reset_locked(queue);
	idx_Unlock();

	return queue ? SWITCH_TRUE : SWITCH_FALSE;
}

static void myasr_audio_queue_release_locked(int aleg_idx)
{
	if (aleg_idx < 0 || aleg_idx >= MAX_USER_CHANNEL)
	{
		return;
	}

	myasr_audio_queue *queue = g_audio_queues[aleg_idx];
	if (queue)
	{
		myasr_audio_queue_reset_locked(queue);
	}
}

static size_t myasr_audio_queue_peek_len_locked(int aleg_idx)
{
	if (aleg_idx < 0 || aleg_idx >= MAX_USER_CHANNEL)
	{
		return 0;
	}

	myasr_audio_queue *queue = g_audio_queues[aleg_idx];
	if (!queue || queue->count == 0)
	{
		return 0;
	}
	if (queue->count > MYASR_AUDIO_QUEUE_SIZE || queue->head >= MYASR_AUDIO_QUEUE_SIZE || queue->tail >= MYASR_AUDIO_QUEUE_SIZE)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "myasr audio queue invalid state on peek aleg_idx=%d count=%u head=%u tail=%u, reset\n",
						  aleg_idx, queue->count, queue->head, queue->tail);
		myasr_audio_queue_reset_locked(queue);
		return 0;
	}

	return queue->len[queue->head];
}

static size_t myasr_audio_queue_pop_locked(int aleg_idx, unsigned char *out, size_t out_size)
{
	if (aleg_idx < 0 || aleg_idx >= MAX_USER_CHANNEL || !out || out_size == 0)
	{
		return 0;
	}

	myasr_audio_queue *queue = g_audio_queues[aleg_idx];
	if (!queue || queue->count == 0)
	{
		return 0;
	}
	if (queue->count > MYASR_AUDIO_QUEUE_SIZE || queue->head >= MYASR_AUDIO_QUEUE_SIZE || queue->tail >= MYASR_AUDIO_QUEUE_SIZE)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "myasr audio queue invalid state on pop aleg_idx=%d count=%u head=%u tail=%u, reset\n",
						  aleg_idx, queue->count, queue->head, queue->tail);
		myasr_audio_queue_reset_locked(queue);
		return 0;
	}

	uint32_t slot = queue->head;
	size_t len = queue->len[slot];
	if (len > MAX_PAYLOAD_SIZE)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "myasr audio queue invalid packet len=%zu aleg_idx=%d, drop\n", len, aleg_idx);
		len = 0;
	}
	else if (len > out_size)
	{
		len = out_size;
	}

	if (len > 0)
	{
		memcpy(out, queue->data[slot], len);
	}
	memset(queue->data[slot], 0, sizeof(queue->data[slot]));
	queue->len[slot] = 0;
	queue->head = (queue->head + 1) % MYASR_AUDIO_QUEUE_SIZE;
	queue->count--;
	return len;
}

static void myasr_publish_audio_buffer(int aleg_idx, const char *data, size_t len)
{
	if (aleg_idx < 0 || aleg_idx >= MAX_USER_CHANNEL || !data || len == 0)
	{
		return;
	}

	size_t publish_len = len > MAX_PAYLOAD_SIZE ? MAX_PAYLOAD_SIZE : len;
	idx_Lock();
	myasr_audio_queue *queue = g_audio_queues[aleg_idx];
	if (!queue)
	{
		idx_Unlock();
		return;
	}
	if (queue->count > MYASR_AUDIO_QUEUE_SIZE || queue->head >= MYASR_AUDIO_QUEUE_SIZE || queue->tail >= MYASR_AUDIO_QUEUE_SIZE)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "myasr audio queue invalid state on push aleg_idx=%d count=%u head=%u tail=%u, reset\n",
						  aleg_idx, queue->count, queue->head, queue->tail);
		myasr_audio_queue_reset_locked(queue);
	}

	if (queue->count == MYASR_AUDIO_QUEUE_SIZE)
	{
		queue->head = (queue->head + 1) % MYASR_AUDIO_QUEUE_SIZE;
		queue->count--;
		queue->dropped++;
		if (queue->dropped == 1 || queue->dropped % 100 == 0)
		{
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "myasr audio queue full, drop oldest packet aleg_idx=%d dropped=%u\n",
							  aleg_idx, queue->dropped);
		}
	}

	uint32_t slot = queue->tail;
	memset(queue->data[slot], 0, sizeof(queue->data[slot]));
	memcpy(queue->data[slot], data, publish_len);
	queue->len[slot] = publish_len;
	queue->tail = (queue->tail + 1) % MYASR_AUDIO_QUEUE_SIZE;
	queue->count++;
	idx_Unlock();
}

static void myasr_append_audio_buffer(struct user_data *ud, int aleg_idx, const char *data, size_t len)
{
	if (!ud || !data || len == 0 || aleg_idx < 0 || aleg_idx >= MAX_USER_CHANNEL)
	{
		return;
	}

	size_t offset = 0;
	while (offset < len)
	{
		if (ud->aleg_buf_len > MAX_PAYLOAD_SIZE)
		{
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "myasr audio buffer overflow state, reset len=%zu\n", ud->aleg_buf_len);
			ud->aleg_buf_len = 0;
			memset(ud->aleg_buf, 0, sizeof(ud->aleg_buf));
		}

		size_t space = MAX_PAYLOAD_SIZE - ud->aleg_buf_len;
		if (space == 0)
		{
			myasr_publish_audio_buffer(aleg_idx, ud->aleg_buf, ud->aleg_buf_len);
			ud->aleg_buf_len = 0;
			memset(ud->aleg_buf, 0, sizeof(ud->aleg_buf));
			space = MAX_PAYLOAD_SIZE;
		}

		size_t copy_len = len - offset;
		if (copy_len > space)
		{
			copy_len = space;
		}

		memcpy(ud->aleg_buf + ud->aleg_buf_len, data + offset, copy_len);
		ud->aleg_buf_len += copy_len;
		offset += copy_len;

		if (ud->aleg_buf_len == MAX_PAYLOAD_SIZE)
		{
			myasr_publish_audio_buffer(aleg_idx, ud->aleg_buf, ud->aleg_buf_len);
			ud->aleg_buf_len = 0;
			memset(ud->aleg_buf, 0, sizeof(ud->aleg_buf));
		}
	}
}

static switch_bool_t myasr_get_frame_audio_16k(struct user_data *ud, switch_frame_t *frame, const char **frame_data, size_t *frame_len)
{
	if (!ud || !frame || !frame->data || !frame_data || !frame_len || frame->datalen == 0)
	{
		return SWITCH_FALSE;
	}

	*frame_data = NULL;
	*frame_len = 0;

	uint32_t input_samples = (uint32_t)(frame->datalen / sizeof(int16_t));
	if (input_samples == 0)
	{
		return SWITCH_FALSE;
	}

	if ((frame->datalen % sizeof(int16_t)) != 0)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "myasr resample : odd frame datalen=%u, ignore trailing byte\n",
						  (unsigned int)frame->datalen);
	}

	if (ud->resampler)
	{
		uint32_t output_samples = switch_resample_process(ud->resampler, (int16_t *)frame->data, input_samples);
		if (output_samples == 0 || !ud->resampler->to)
		{
			return SWITCH_FALSE;
		}

		*frame_data = (const char *)ud->resampler->to;
		*frame_len = (size_t)output_samples * sizeof(int16_t);
		return SWITCH_TRUE;
	}

	if (ud->read_sample_rate == MYASR_TARGET_SAMPLE_RATE)
	{
		*frame_data = (const char *)frame->data;
		*frame_len = input_samples * sizeof(int16_t);
		return SWITCH_TRUE;
	}

	if (!ud->resample_unavailable_logged)
	{
		ud->resample_unavailable_logged = 1;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "myasr resample : resampler unavailable, drop frame input_rate=%u target_rate=%d\n",
						  ud->read_sample_rate, MYASR_TARGET_SAMPLE_RATE);
	}
	return SWITCH_FALSE;
}

int GetKeyValue(const char *str, int addlen, const char *key, int maxlen, char *values)
{
	int outlen = 0;
	int nKeyLen = 0;
	char *p = NULL;
	char *ptr;

	if (NULL == str || 0x0 == str)
		return -1;
	if (NULL == key || 0x0 == key)
		return -1;
	if (NULL == values || 0x0 == values)
		return -1;

	nKeyLen = strlen(key) + addlen;
	ptr = (char *)strstr(str, key);
	if (ptr)
	{
		p = ptr + nKeyLen;
		while (1)
		{
			// if(*p=='\r' || *p==':' || *p=='\n' || *p=='\0')
			if (*p == '"' || *p == ';' || *p == ' ' || *p == '\r' || *p == '>' || *p == ':' || *p == '\n' || *p == ',' || *p == '\0')
			{
				outlen = p - ptr - nKeyLen;
				if (outlen > 0 && outlen < maxlen - 1)
					memcpy(values, ptr + nKeyLen, outlen);
				break;
			}
			else
				p++;
		}
	}
	return outlen;
}

static switch_status_t load_mymedia_config(switch_memory_pool_t *pool)
{
	switch_xml_t cfg, xml, settings, param;
	if (!(xml = switch_xml_open_cfg("myasr.conf", &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to open myasr.conf\n");
		return SWITCH_STATUS_FALSE;
	}
	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			const char *name = switch_xml_attr_soft(param, "name");
			const char *value = switch_xml_attr_soft(param, "value");
			if (!strcmp(name, "app_id"))
				globals.app_id = switch_core_strdup(pool, value);
			else if (!strcmp(name, "app_key"))
				globals.app_key = switch_core_strdup(pool, value);
			else if (!strcmp(name, "wss_url") || !strcmp(name, "ws_url"))
				globals.wss_url = switch_core_strdup(pool, value);
		}
	}
	switch_xml_free(xml);
	return SWITCH_STATUS_SUCCESS;
}

static void event_handler(switch_event_t *event)
{
	load_mymedia_config(globals.pool);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "myasr config reloaded\n");
}
void asrTextEvent(const char *uuid, char *msg, const char *asrType)
{
	switch_event_t *event = NULL;
	if (switch_event_create(&event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS)
	{
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-Subclass", "xfasr");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "ASR-Event", asrType);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "ASR-Response", msg);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-Call-UUID", uuid);
		switch_event_fire(&event);
	}
}

void handle_message(const char *uuid, const std::string &message)
{
	if (!uuid || message.empty()) {
		return;
	}

	char middleText[500] = {0};
	char finalResult[5000] = {0};
	cJSON *cjson_test = NULL;
	cJSON *cjson_action = NULL;
	cJSON *cjson_code = NULL;
	cJSON *cjson_data = NULL;
	cJSON *cjson_desc = NULL;
	cJSON *cjson_sid = NULL;
	cJSON *cjson_text = NULL;
	cJSON *cjson_segid = NULL;
	cJSON *cjson_cn = NULL;
	cJSON *cjson_st = NULL;
	cJSON *cjson_rt = NULL;
	cJSON *cjson_rt_item = NULL;
	cJSON *cjson_cw_item = NULL;
	cJSON *cjson_w_item = NULL;
	cJSON *cjson_type = NULL;
	cJSON *cjson_ws = NULL;
	cJSON *cjson_cw = NULL;
	cJSON *cjson_w = NULL;

	cjson_test = cJSON_Parse(message.c_str());
	if (!cjson_test) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to parse JSON message\n");
		return;
	}

	cjson_action = cJSON_GetObjectItem(cjson_test, "action");
	cjson_code = cJSON_GetObjectItem(cjson_test, "code");
	cjson_data = cJSON_GetObjectItem(cjson_test, "data");
	cjson_desc = cJSON_GetObjectItem(cjson_test, "desc");
	cjson_sid = cJSON_GetObjectItem(cjson_test, "sid");

	// Check for null pointers and valid string values
	if (!cjson_action || !cjson_action->valuestring || 
		!cjson_code || !cjson_code->valuestring ||
		!cjson_data || !cjson_data->valuestring) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid JSON structure\n");
		cJSON_Delete(cjson_test);
		return;
	}

	if (strcmp(cjson_action->valuestring, "result") == 0 && strcmp(cjson_code->valuestring, "0") == 0 && strlen(cjson_data->valuestring) > 0)
	{
		cjson_text = cJSON_Parse(cjson_data->valuestring);
		if (!cjson_text) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to parse data JSON\n");
			cJSON_Delete(cjson_test);
			return;
		}
		
		cjson_segid = cJSON_GetObjectItem(cjson_text, "seg_id");
		cjson_cn = cJSON_GetObjectItem(cjson_text, "cn");
		if (!cjson_cn) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing 'cn' field in JSON\n");
			cJSON_Delete(cjson_text);
			cJSON_Delete(cjson_test);
			return;
		}
		
		cjson_st = cJSON_GetObjectItem(cjson_cn, "st");
		if (!cjson_st) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing 'st' field in JSON\n");
			cJSON_Delete(cjson_text);
			cJSON_Delete(cjson_test);
			return;
		}
		
		cjson_rt = cJSON_GetObjectItem(cjson_st, "rt");
		cjson_type = cJSON_GetObjectItem(cjson_st, "type");
		
		if (!cjson_type || !cjson_type->valuestring || !cjson_rt) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing 'type' or 'rt' field in JSON\n");
			cJSON_Delete(cjson_text);
			cJSON_Delete(cjson_test);
			return;
		}

		if (strcmp(cjson_type->valuestring, "0") == 0)
		{
			int rt_array_size = cJSON_GetArraySize(cjson_rt);
			for (int i = 0; i < rt_array_size; i++)
			{
				cjson_rt_item = cJSON_GetArrayItem(cjson_rt, i);
				if (!cjson_rt_item) continue;
				
				cjson_ws = cJSON_GetObjectItem(cjson_rt_item, "ws");
				if (!cjson_ws) continue;

				int ws_array_size = cJSON_GetArraySize(cjson_ws);
				for (int j = 0; j < ws_array_size; j++)
				{
					cjson_cw_item = cJSON_GetArrayItem(cjson_ws, j);
					if (!cjson_cw_item) continue;
					
					cjson_cw = cJSON_GetObjectItem(cjson_cw_item, "cw");
					if (!cjson_cw) continue;

					int cw_array_size = cJSON_GetArraySize(cjson_cw);
					for (int k = 0; k < cw_array_size; k++)
					{
						cjson_w_item = cJSON_GetArrayItem(cjson_cw, k);
						if (!cjson_w_item) continue;
						
						cjson_w = cJSON_GetObjectItem(cjson_w_item, "w");
						if (!cjson_w || !cjson_w->valuestring) continue;
						
						size_t current_len = strlen(finalResult);
						size_t word_len = strlen(cjson_w->valuestring);
						if (current_len + word_len < sizeof(finalResult) - 1)
						{
							strncat(finalResult, cjson_w->valuestring, sizeof(finalResult) - current_len - 1);
						}
						else
						{
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "content too long!!!!!!\n");
							break;
						}
					}
				}
			}
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "asrFinalResult:%s\n", finalResult);
			asrTextEvent(uuid, finalResult, "finalResult");
		}
		else
		{
			int rt_array_size = cJSON_GetArraySize(cjson_rt);
			for (int i = 0; i < rt_array_size; i++)
			{
				cjson_rt_item = cJSON_GetArrayItem(cjson_rt, i);
				if (!cjson_rt_item) continue;
				
				cjson_ws = cJSON_GetObjectItem(cjson_rt_item, "ws");
				if (!cjson_ws) continue;

				int ws_array_size = cJSON_GetArraySize(cjson_ws);
				for (int j = 0; j < ws_array_size; j++)
				{
					cjson_cw_item = cJSON_GetArrayItem(cjson_ws, j);
					if (!cjson_cw_item) continue;
					
					cjson_cw = cJSON_GetObjectItem(cjson_cw_item, "cw");
					if (!cjson_cw) continue;

					int cw_array_size = cJSON_GetArraySize(cjson_cw);
					for (int k = 0; k < cw_array_size; k++)
					{
						cjson_w_item = cJSON_GetArrayItem(cjson_cw, k);
						if (!cjson_w_item) continue;
						
						cjson_w = cJSON_GetObjectItem(cjson_w_item, "w");
						if (!cjson_w || !cjson_w->valuestring) continue;
						
						size_t current_len = strlen(middleText);
						size_t word_len = strlen(cjson_w->valuestring);
						if (current_len + word_len < sizeof(middleText) - 1)
						{
							strncat(middleText, cjson_w->valuestring, sizeof(middleText) - current_len - 1);
						}
						else
						{
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "middleText content too long!!!!!!\n");
							break;
						}
					}
				}
			}
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "asrTempResult:%s\n", middleText);
			asrTextEvent(uuid, middleText, "tempResult");
		}
		cJSON_Delete(cjson_text);
	}
	else if (strcmp(cjson_action->valuestring, "error") == 0)
	{
		if (cjson_desc && cjson_desc->valuestring) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "asrErrorInfo:%s\n", cjson_desc->valuestring);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "asrErrorInfo: unknown error\n");
		}
	}
	cJSON_Delete(cjson_test);
}

// begin asr
void BeginAsr(char *uuid)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "begin asr uuid=%s\n", uuid);
	switch_event_t *event = NULL;
	if (switch_event_create(&event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS)
	{
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-Subclass", "myasr");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "ASR-Event", "BeginAsr");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "ASR-Response", "");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-Call-UUID", uuid);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-from", "ASR"); // 方便在ESL中判断是否是机器人事件
		switch_event_fire(&event);
	}
}

// end asr
void EndAsr(char *uuid)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "end asr uuid=%s\n", uuid);
	switch_event_t *event = NULL;
	if (switch_event_create(&event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS)
	{
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-Subclass", "myasr");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "ASR-Event", "EndAsr");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "ASR-Response", "");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Channel-Call-UUID", uuid);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-from", "ASR"); // 方便在ESL中判断是否是机器人事件
		switch_event_fire(&event);
	}
}

int getChannelIdx()
{
	idx_Lock();
	int i = 0;
	int nFindIdx = -1;
	for (i = g_nCurr_channel_index; i < MAX_USER_CHANNEL; i++)
	{
		if (g_user_channel_state[i] == MYASR_CHANNEL_FREE)
		{
			g_user_channel_state[i] = MYASR_CHANNEL_RUNNING;
			g_nCurr_channel_index = i;
			nFindIdx = i;
			g_timeout[i] = time(NULL);
			break;
		}
	}
	if (nFindIdx < 0)
	{
		for (i = 0; i < MAX_USER_CHANNEL; i++)
		{
			if (g_user_channel_state[i] == MYASR_CHANNEL_FREE)
			{
				g_user_channel_state[i] = MYASR_CHANNEL_RUNNING;
				g_nCurr_channel_index = i;
				nFindIdx = i;
				g_timeout[i] = time(NULL);
				break;
			}
		}
	}
	if (nFindIdx < 0)
	{
		time_t now = time(NULL);
		for (i = 0; i < MAX_USER_CHANNEL; i++)
		{
			if (g_user_channel_state[i] == MYASR_CHANNEL_RUNNING && (now - g_timeout[i] > 3600 * 2))
			{
				myasr_audio_queue_release_locked(i);
				g_user_channel_state[i] = MYASR_CHANNEL_RUNNING;
				g_nCurr_channel_index = i;
				nFindIdx = i;
				g_timeout[i] = time(NULL);
				break;
			}
		}
	}
	g_nCurr_channel_index++;
	if (g_nCurr_channel_index >= MAX_USER_CHANNEL)
		g_nCurr_channel_index = 0;
	idx_Unlock();

	return nFindIdx;
}

wssclient::wssclient(const asr_params &params)
{
	asr_params_ = params;
	if (asr_params_.sample_rate != 8000 && asr_params_.sample_rate != MYASR_TARGET_SAMPLE_RATE)
	{
		asr_params_.sample_rate = MYASR_TARGET_SAMPLE_RATE;
	}
	m_exit_ = -1;
	use_tls_ = true;
	connected_ = NULL;
	work_thread_id_ = 0;
	
	// 初始化pthread_mutex_t
	pthread_mutex_init(&hdl_mutex_, NULL);
	
	tls_client_.clear_access_channels(websocketpp::log::alevel::all);
	tls_client_.set_error_channels(websocketpp::log::elevel::all);
	tls_client_.init_asio();
	tls_client_.set_open_handshake_timeout(5000);
	tls_client_.set_tls_init_handler(bind(&wssclient::on_tls_init, this, websocketpp::lib::placeholders::_1));
	tls_client_.set_open_handler(bind(&wssclient::on_open, this, websocketpp::lib::placeholders::_1));
	tls_client_.set_close_handler(bind(&wssclient::on_close, this, websocketpp::lib::placeholders::_1));
	tls_client_.set_fail_handler(bind(&wssclient::on_fail, this, websocketpp::lib::placeholders::_1));
	tls_client_.set_message_handler(bind(&wssclient::on_tls_message, this, websocketpp::lib::placeholders::_1, websocketpp::lib::placeholders::_2));

	plain_client_.clear_access_channels(websocketpp::log::alevel::all);
	plain_client_.set_error_channels(websocketpp::log::elevel::all);
	plain_client_.init_asio();
	plain_client_.set_open_handshake_timeout(5000);
	plain_client_.set_open_handler(bind(&wssclient::on_open, this, websocketpp::lib::placeholders::_1));
	plain_client_.set_close_handler(bind(&wssclient::on_close, this, websocketpp::lib::placeholders::_1));
	plain_client_.set_fail_handler(bind(&wssclient::on_fail, this, websocketpp::lib::placeholders::_1));
	plain_client_.set_message_handler(bind(&wssclient::on_plain_message, this, websocketpp::lib::placeholders::_1, websocketpp::lib::placeholders::_2));
}

wssclient::~wssclient()
{
	m_exit_ = 1;
	
	// 等待工作线程结束，防止资源竞争
	if (work_thread_id_ != 0) {
		void *thread_result;
		int join_result = pthread_join(work_thread_id_, &thread_result);
		if (join_result != 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "~wssclient : pthread_join failed: %d uuid[%s] bufidx[%d]\n", join_result, uuid_.empty() ? "unknown" : uuid_.c_str(), bufidx_);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "~wssclient : work thread joined successfully uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
		}
		work_thread_id_ = 0;
	}
	
	// 销毁pthread_mutex_t
	pthread_mutex_destroy(&hdl_mutex_);
	
	// stop_io_service();
}

string wssclient::gen_json_request(const std::string &token, const asr_params &params)
{

	string json_asr_params = "{\"access_token\":\"";
	json_asr_params += token;
	json_asr_params += "\",\"version\":\"1.0\",\"asr_params\":{\"audio_format\":\"pcm\",\"sample_rate\":";
	json_asr_params += std::to_string(params.sample_rate);
	json_asr_params += ",\"req_idx\":";
	json_asr_params += std::to_string(params.req_idx);
	json_asr_params += ",\"speech_type\":1,\"add_pct\":true,\"domain\":\"common\",\"audio_data\":\"";
	json_asr_params += params.audio_data;
	json_asr_params += "\"}}";

	return json_asr_params;
}

void wssclient::recv_asr_realtime_msg(const std::string &msg)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "uuid[%s] bufidx[%d] recv asr message = %s\n", uuid_.c_str(), bufidx_, msg.c_str());
	if (msg.empty())
	{
		return;
	}
	handle_message(uuid_.c_str(), msg);
}

bool wssclient::connection_is_open(websocketpp::connection_hdl hdl)
{
	if (hdl.expired())
	{
		return false;
	}

	try {
		if (use_tls_) {
			tls_client::connection_ptr con = tls_client_.get_con_from_hdl(hdl);
			return con && con->get_state() == websocketpp::session::state::open;
		}

		plain_client::connection_ptr con = plain_client_.get_con_from_hdl(hdl);
		return con && con->get_state() == websocketpp::session::state::open;
	} catch (const std::exception& e) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "connection_is_open : exception=%s uuid[%s] bufidx[%d]\n", e.what(), uuid_.c_str(), bufidx_);
	} catch (...) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "connection_is_open : unknown exception uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
	}
	return false;
}

bool wssclient::send_request_frame(websocketpp::connection_hdl hdl, char *buf, int buflen)
{
	// 参数验证
	if (buf == NULL || buflen <= 0 || buflen > MAX_PAYLOAD_SIZE) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send_request_frame : invalid parameters buf=%p buflen=%d uuid[%s] bufidx[%d]\n", 
			buf, buflen, uuid_.empty() ? "unknown" : uuid_.c_str(), bufidx_);
		return false;
	}
	
	// 检查线程退出状态
	if (m_exit_ == 1 || m_exit_ == 2) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send_request_frame : thread marked for exit uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
		return false;
	}
	
	// 检查句柄有效性
	if (hdl.expired()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send_request_frame : hdl is expired uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
		m_exit_ = 2;
		return false;
	}
	
	try {
		if (!connection_is_open(hdl)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send_request_frame : connection not open uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
			m_exit_ = 2;
			return false;
		}
		
		// 安全的数据发送
		websocketpp::lib::error_code ec;
		std::vector<uint8_t> data;
		try {
			data.assign(buf, buf + buflen);
		} catch (const std::exception& e) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send_request_frame : data assignment std::exception: %s uuid[%s] bufidx[%d]\n", e.what(), uuid_.c_str(), bufidx_);
			return false;
		} catch (...) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send_request_frame : data assignment unknown exception uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
			return false;
		}
		
		if (use_tls_) {
			tls_client_.send(hdl, data.data(), data.size(), websocketpp::frame::opcode::BINARY, ec);
		} else {
			plain_client_.send(hdl, data.data(), data.size(), websocketpp::frame::opcode::BINARY, ec);
		}
		if (ec) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send_request_frame : send error=%s uuid[%s] bufidx[%d]\n", ec.message().c_str(), uuid_.c_str(), bufidx_);
			m_exit_ = 2;
			return false;
		}
		return true;
	} catch (const websocketpp::exception& e) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send_request_frame : websocketpp::exception=%s uuid[%s] bufidx[%d]\n", e.what(), uuid_.c_str(), bufidx_);
		m_exit_ = 2;
		return false;
	} catch (const std::exception& e) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send_request_frame : std::exception=%s uuid[%s] bufidx[%d]\n", e.what(), uuid_.c_str(), bufidx_);
		m_exit_ = 2;
		return false;
	} catch (...) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "send_request_frame : unknown exception uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
		m_exit_ = 2;
		return false;
	}
}

void wssclient::run()
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "wssclient run uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
	try
	{
		if (use_tls_) {
			tls_client_.run();
		} else {
			plain_client_.run();
		}
	}
	catch (websocketpp::exception &e)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "wssclient run : uuid[%s] bufidx[%d] catch websocketpp exception=%s\n", uuid_.c_str(), bufidx_, e.what());
	}
	catch (...)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "wssclient run : uuid[%s] bufidx[%d] catch other exception\n", uuid_.c_str(), bufidx_);
	}
}

static bool myasr_url_has_scheme(const std::string &uri, const char *scheme)
{
	size_t scheme_len = strlen(scheme);
	if (uri.size() < scheme_len)
	{
		return false;
	}

	for (size_t i = 0; i < scheme_len; i++)
	{
		if (std::tolower((unsigned char)uri[i]) != std::tolower((unsigned char)scheme[i]))
		{
			return false;
		}
	}
	return true;
}

bool wssclient::open_connection(const std::string &uri, volatile int *connected)
{
	try {
		websocketpp::lib::error_code ec;
		if (myasr_url_has_scheme(uri, "wss://")) {
			use_tls_ = true;
		} else if (myasr_url_has_scheme(uri, "ws://")) {
			use_tls_ = false;
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "wssclient unsupported websocket uri scheme uri=%s uuid[%s] bufidx[%d]\n", uri.c_str(), uuid_.c_str(), bufidx_);
			if (connected) {
				*connected = -1;
			}
			return false;
		}

		if (use_tls_) {
			tls_client::connection_ptr con = tls_client_.get_connection(uri, ec);
			if (!ec) {
				con->set_open_handshake_timeout(5000);
				tls_client_.connect(con);
			}
		} else {
			plain_client::connection_ptr con = plain_client_.get_connection(uri, ec);
			if (!ec) {
				con->set_open_handshake_timeout(5000);
				plain_client_.connect(con);
			}
		}
		if (ec)
		{
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "wssclient connection initialized failed-%s \n", ec.message().c_str());
			if (connected) {
				*connected = -1;
			}
			return false;
		}
		
		if (connected)
		{
			connected_ = connected;
			*connected = 0; // 初始化为连接中状态
		}
		else
		{
			connected_ = NULL;
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "wssclient start connecting scheme=%s uuid[%s] bufidx[%d]\n", use_tls_ ? "wss" : "ws", uuid_.c_str(), bufidx_);
		return true;
	}
	catch (const std::exception& e) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "wssclient open_connection exception: %s, uuid[%s] bufidx[%d]\n", e.what(), uuid_.c_str(), bufidx_);
		if (connected) {
			*connected = -1;
		}
		return false;
	}
	catch (...) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "wssclient open_connection unknown exception, uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
		if (connected) {
			*connected = -1;
		}
		return false;
	}
}

void wssclient::close_connection()
{
	bool locked = false;
	try
	{
		pthread_mutex_lock(&hdl_mutex_);
		locked = true;
		
		// 检查连接句柄是否有效
		if (!hdl_.expired()) {
			int close_code = websocketpp::close::status::normal;
			if (use_tls_) {
				tls_client_.close(hdl_, close_code, "");
			} else {
				plain_client_.close(hdl_, close_code, "");
			}
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "close_connection : uuid[%s] bufidx[%d] end\n", uuid_.c_str(), bufidx_);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "close_connection : uuid[%s] bufidx[%d] handle already expired\n", uuid_.c_str(), bufidx_);
		}
		
		pthread_mutex_unlock(&hdl_mutex_);
		locked = false;
	}
	catch (websocketpp::exception &e)
	{
		if (locked) {
			pthread_mutex_unlock(&hdl_mutex_);
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "close_connection : uuid[%s] bufidx[%d] catch websocketpp exception=%s\n", uuid_.c_str(), bufidx_, e.what());
	}
	catch (...)
	{
		if (locked) {
			pthread_mutex_unlock(&hdl_mutex_);
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "close_connection : uuid[%s] bufidx[%d] catch other exception\n", uuid_.c_str(), bufidx_);
	}
}

void wssclient::stop_io_service()
{
	try
	{
		if (use_tls_) {
			tls_client_.stop();
		} else {
			plain_client_.stop();
		}
	}
	catch (websocketpp::exception &e)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "stop_io_service : uuid[%s] bufidx[%d] catch websocketpp exception=%s\n", uuid_.c_str(), bufidx_, e.what());
	}
	catch (...)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "stop_io_service : uuid[%s] bufidx[%d] catch other exception\n", uuid_.c_str(), bufidx_);
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "stop_io_service : uuid[%s] bufidx[%d] end\n", uuid_.c_str(), bufidx_);
}

context_ptr wssclient::on_tls_init(websocketpp::connection_hdl)
{
	context_ptr ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12);

	try
	{
		ctx->set_options(boost::asio::ssl::context::default_workarounds |
						 boost::asio::ssl::context::no_sslv2 |
						 boost::asio::ssl::context::no_sslv3 |
						 boost::asio::ssl::context::single_dh_use);
	}
	catch (std::exception &e)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "on_tls_init : uuid[%s] bufidx[%d] catch fail\n", uuid_.c_str(), bufidx_);
	}
	return ctx;
}

// pthread wrapper function for work_thread
static void *wssclient_work_thread_wrapper(void *arg)
{
	wssclient *client = static_cast<wssclient*>(arg);
	if (client) {
		try {
			client->work_thread();
		} catch (...) {
			// 确保异常不会导致线程崩溃
		}
	}
	pthread_exit(NULL);
	return NULL;
}

void wssclient::on_open(websocketpp::connection_hdl hdl)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on_open : uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
	hdl_ = hdl;
	volatile int *connected = connected_;
	connected_ = NULL;
	
	// 使用pthread替代boost::thread，避免boost 1.63.0的线程问题
	pthread_t work_thread_id;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 512 * 1024); // 设置较小的栈大小512KB
	
	int ret = pthread_create(&work_thread_id, &attr, wssclient_work_thread_wrapper, this);
	pthread_attr_destroy(&attr);
	
	if (ret != 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "on_open : failed to create pthread uuid[%s] bufidx[%d] error: %d\n", uuid_.c_str(), bufidx_, ret);
		m_exit_ = 1;
		if (connected) {
			*connected = -1;
		}
	} else {
		// 存储线程ID，稍后用于清理
		work_thread_id_ = work_thread_id;
		if (connected) {
			*connected = 1;
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on_open : pthread created successfully uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
	}
}

void wssclient::on_close(websocketpp::connection_hdl hdl)
{
	try {
		m_exit_ = 1;

		std::stringstream s;
		if (use_tls_) {
			tls_client::connection_ptr con = tls_client_.get_con_from_hdl(hdl);
			if (!con) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "on_close : null tls connection uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
				return;
			}
			s << "close code: " << con->get_remote_close_code() << " ("
			  << websocketpp::close::status::get_string(con->get_remote_close_code())
			  << "), close reason: " << con->get_remote_close_reason();
		} else {
			plain_client::connection_ptr con = plain_client_.get_con_from_hdl(hdl);
			if (!con) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "on_close : null plain connection uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
				return;
			}
			s << "close code: " << con->get_remote_close_code() << " ("
			  << websocketpp::close::status::get_string(con->get_remote_close_code())
			  << "), close reason: " << con->get_remote_close_reason();
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on_close :[%s] uuid[%s] bufidx[%d]\n", s.str().c_str(), uuid_.c_str(), bufidx_);
	} catch (const std::exception& e) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "on_close : exception=%s uuid[%s] bufidx[%d]\n", e.what(), uuid_.c_str(), bufidx_);
	} catch (...) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "on_close : unknown exception uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
	}
}

void wssclient::on_fail(websocketpp::connection_hdl hdl)
{
	try {
		m_exit_ = 2;
		if (connected_) {
			*connected_ = -1;
			connected_ = NULL;
		}
		
		// 安全清理通道状态
		if (bufidx_ >= 0 && bufidx_ < MAX_USER_CHANNEL) {
			try {
				idx_Lock();
				g_user_channel_state[bufidx_] = MYASR_CHANNEL_FREE;
				myasr_audio_queue_release_locked(bufidx_);
				idx_Unlock();
			} catch (...) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "on_fail : exception during channel cleanup uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
				try { idx_Unlock(); } catch (...) {}
			}
		}
		
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "on_fail : WebSocket connection failed uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
		
		// 安全停止WebSocket客户端
		try {
			if (use_tls_) {
				tls_client_.stop();
			} else {
				plain_client_.stop();
			}
		} catch (const websocketpp::exception& e) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "on_fail : websocketpp::exception stopping client: %s uuid[%s] bufidx[%d]\n", e.what(), uuid_.c_str(), bufidx_);
		} catch (const std::exception& e) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "on_fail : std::exception stopping client: %s uuid[%s] bufidx[%d]\n", e.what(), uuid_.c_str(), bufidx_);
		} catch (...) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "on_fail : unknown exception stopping client uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
		}
	} catch (...) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "on_fail : critical exception in failure handler uuid[%s] bufidx[%d]\n", uuid_.empty() ? "unknown" : uuid_.c_str(), bufidx_);
		m_exit_ = 2;
		// 尽最大努力清理状态
		try {
			if (connected_) {
				*connected_ = -1;
				connected_ = NULL;
			}
		} catch (...) {}
		try {
			if (bufidx_ >= 0 && bufidx_ < MAX_USER_CHANNEL) {
				try { idx_Lock(); g_user_channel_state[bufidx_] = MYASR_CHANNEL_FREE; idx_Unlock(); } catch (...) { try { idx_Unlock(); } catch (...) {} }
			}
		} catch (...) {}
	}
}

void wssclient::on_tls_message(websocketpp::connection_hdl hdl, tls_message_ptr msg)
{
	//	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on_tls_message : uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
	recv_asr_realtime_msg(msg->get_payload());
}

void wssclient::on_plain_message(websocketpp::connection_hdl hdl, plain_message_ptr msg)
{
	//	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "on_plain_message : uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
	recv_asr_realtime_msg(msg->get_payload());
}

void wssclient::work_thread()
{
	// 增强整体异常保护
	try {
		// 基本参数验证
		if (uuid_.empty() || bufidx_ < 0 || bufidx_ >= MAX_USER_CHANNEL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "work_thread : invalid parameters uuid[%s] bufidx[%d]\n", 
				uuid_.empty() ? "empty" : uuid_.c_str(), bufidx_);
			m_exit_ = 2;
			return;
		}
		
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "work_thread : starting uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
		
		// 延迟初始化，确保对象稳定
		usleep(50000); // 50ms
		
		// 检查退出状态
		if (m_exit_ == 1 || m_exit_ == 2) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : already marked for exit uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
			return;
		}
	
	string json_data;
	int req_idx = 0;

	m_exit_ = 0;
	time_t last_activity = time(NULL);
	const int MAX_IDLE_TIME = 60; // 1分钟超时
	
	while (1)
	{		
		// 检查超时
		time_t current_time = time(NULL);
		if (current_time - last_activity > MAX_IDLE_TIME) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : idle timeout reached, exiting uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
			m_exit_ = 2;
			break;
		}
		// 首先检查线程退出条件，这是最高优先级
		if (m_exit_ == 1 || m_exit_ == 2)
		{
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "work_thread : exit signal received (m_exit_=%d) uuid[%s] bufidx[%d]\n", m_exit_, uuid_.c_str(), bufidx_);
			break;
		}
		
		// 检查连接句柄是否有效
		if (hdl_.expired()) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : connection handle expired, exiting thread uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
			m_exit_ = 2;
			break;
		}
		
		// 增强的WebSocket连接状态检查
		bool connection_valid = false;
		try {
			if (!hdl_.expired()) {
				if (connection_is_open(hdl_)) {
					connection_valid = true;
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : connection not in open state uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
				}
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : connection handle expired uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
			}
		} catch (const std::exception& e) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : std::exception checking connection: %s uuid[%s] bufidx[%d]\n", e.what(), uuid_.c_str(), bufidx_);
		} catch (...) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : unknown exception checking connection uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
		}
		
		if (!connection_valid) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : invalid connection, exiting thread uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
			m_exit_ = 2;
			break;
		}
		
		// 检查bufidx_有效性
		if (bufidx_ < 0 || bufidx_ >= MAX_USER_CHANNEL)
		{
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "work_thread bufidx_=%d invalid\n", bufidx_);
			break;
		}

			// 安全的互斥锁访问和数组边界检查
			int channel_state = 0;
			size_t audio_len = 0;
			bool lock_acquired = false;

			// 尝试获取锁，带超时保护
			for (int retry = 0; retry < 3; retry++) {
				if (idx_TryLock() == 0) {
					lock_acquired = true;
					// 双重检查数组边界
					if (bufidx_ >= 0 && bufidx_ < MAX_USER_CHANNEL) {
						channel_state = g_user_channel_state[bufidx_];
						audio_len = myasr_audio_queue_peek_len_locked(bufidx_);
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "work_thread : bufidx out of bounds %d\n", bufidx_);
						m_exit_ = 2;
					}
					break;
				} else {
					usleep(5000); // 等待5ms
				}
			}

			if (!lock_acquired) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : failed to acquire lock after retries uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
				usleep(10000);
				continue;
			}

			if (m_exit_ == 2) {
				idx_Unlock();
				break;
			}

			if (channel_state != MYASR_CHANNEL_RUNNING)
			{
				// 通道已停止，先把队列里的剩余音频按顺序发完，再发送结束信号。
				if (audio_len > 0)
				{
					unsigned char temp_buf[MAX_PAYLOAD_SIZE + 1];
					size_t temp_len = (audio_len > MAX_PAYLOAD_SIZE) ? MAX_PAYLOAD_SIZE : audio_len;
					bool is_last_audio_packet = true;

					memset(temp_buf, 0, sizeof(temp_buf));
					temp_len = myasr_audio_queue_pop_locked(bufidx_, temp_buf, sizeof(temp_buf));
					is_last_audio_packet = myasr_audio_queue_peek_len_locked(bufidx_) == 0;
					idx_Unlock();

					if (temp_len > 0 && !hdl_.expired()) {
						try {
							if (connection_is_open(hdl_)) {
								asr_params_.req_idx = req_idx++;
								if (is_last_audio_packet)
								{
									asr_params_.req_idx = 0 - asr_params_.req_idx;
								}
								if (!send_request_frame(hdl_, (char *)temp_buf, temp_len)) {
									switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : send failed, exiting uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
									break;
								}
								last_activity = time(NULL);
								if (!is_last_audio_packet)
								{
									continue;
								}
								usleep(400000);
							} else {
								switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : connection not open, skipping send uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
							}
						} catch (const std::exception& e) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : std::exception during send: %s uuid[%s] bufidx[%d]\n", e.what(), uuid_.c_str(), bufidx_);
							break;
						} catch (...) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : unknown exception during send uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
							break;
						}
					}
				}
				else
				{
					idx_Unlock();
				}

				// 安全发送结束信号
				if (m_exit_ != 2 && !hdl_.expired()) {
					try {
						if (connection_is_open(hdl_)) {
							const char *end_signal = "{\"end\": true}";
							char *end_data = const_cast<char *>(end_signal);
							size_t end_len = std::strlen(end_signal);
							if (!send_request_frame(hdl_, end_data, end_len)) {
								switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : send end signal failed uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
							}
						}
					} catch (const std::exception& e) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : std::exception during end signal: %s uuid[%s] bufidx[%d]\n", e.what(), uuid_.c_str(), bufidx_);
					} catch (...) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : unknown exception during end signal uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
					}
				}
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "work_thread : normal end exit uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
				break;
			}
			else if (audio_len > 0)
			{
				unsigned char temp_buf[MAX_PAYLOAD_SIZE + 1];
				size_t temp_len = (audio_len > MAX_PAYLOAD_SIZE) ? MAX_PAYLOAD_SIZE : audio_len;

				memset(temp_buf, 0, sizeof(temp_buf));
				temp_len = myasr_audio_queue_pop_locked(bufidx_, temp_buf, sizeof(temp_buf));
				idx_Unlock();

				if (m_exit_ == 1 || m_exit_ == 2 || temp_len == 0) {
					if (m_exit_ != 0) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "work_thread : exit signal during audio processing uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
					}
					break;
				}

				if (hdl_.expired()) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : connection expired during audio processing uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
					m_exit_ = 2;
					break;
				}

				try {
					if (!connection_is_open(hdl_)) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : connection not open during audio processing uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
						m_exit_ = 2;
						break;
					}

					asr_params_.req_idx = req_idx++;
					if (!send_request_frame(hdl_, (char *)temp_buf, temp_len)) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : send audio failed, exiting uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
						m_exit_ = 2;
						break;
					}
					last_activity = time(NULL);
				} catch (const std::exception& e) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : std::exception during audio processing: %s uuid[%s] bufidx[%d]\n", e.what(), uuid_.c_str(), bufidx_);
					m_exit_ = 2;
					break;
				} catch (...) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : unknown exception during audio processing uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
					m_exit_ = 2;
					break;
				}
			}
				else
				{
					idx_Unlock();
				}

				usleep(40000);
			}

				// 安全的线程退出清理
				try {
					if (bufidx_ >= 0 && bufidx_ < MAX_USER_CHANNEL) {
						try {
							idx_Lock();
							g_user_channel_state[bufidx_] = MYASR_CHANNEL_FREE;
							myasr_audio_queue_release_locked(bufidx_);
							idx_Unlock();
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "work_thread : cleaned up channel state uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
						} catch (...) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : exception during channel cleanup uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
						// 尽力释放锁
						try { idx_Unlock(); } catch (...) {}
					}
				}
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "work_thread : normal exit uuid[%s] bufidx[%d] count[%d]\n", uuid_.c_str(), bufidx_, req_idx);
				m_exit_ = 1;
				} catch (...) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "work_thread : exception during final cleanup uuid[%s] bufidx[%d]\n", uuid_.c_str(), bufidx_);
					m_exit_ = 2;
				}
	} catch (const websocketpp::exception& e) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "work_thread : caught websocketpp::exception uuid[%s] bufidx[%d]: %s\n", uuid_.empty() ? "unknown" : uuid_.c_str(), bufidx_, e.what());
		m_exit_ = 2;
		if (connected_) {
			*connected_ = -1;
		}
		// 安全清理通道状态
		if (bufidx_ >= 0 && bufidx_ < MAX_USER_CHANNEL) {
			try {
				idx_Lock();
				g_user_channel_state[bufidx_] = MYASR_CHANNEL_FREE;
				idx_Unlock();
			} catch (...) {
				try { idx_Unlock(); } catch (...) {}
			}
		}
	} catch (const std::exception& e) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "work_thread : caught std::exception uuid[%s] bufidx[%d]: %s\n", uuid_.empty() ? "unknown" : uuid_.c_str(), bufidx_, e.what());
		m_exit_ = 2;
		if (connected_) {
			*connected_ = -1;
		}
		// 安全清理通道状态
		if (bufidx_ >= 0 && bufidx_ < MAX_USER_CHANNEL) {
			try {
				idx_Lock();
				g_user_channel_state[bufidx_] = MYASR_CHANNEL_FREE;
				idx_Unlock();
			} catch (...) {
				try { idx_Unlock(); } catch (...) {}
			}
		}
	} catch (...) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "work_thread : caught unknown exception uuid[%s] bufidx[%d], thread exiting safely\n", uuid_.empty() ? "unknown" : uuid_.c_str(), bufidx_);
		m_exit_ = 2;
		if (connected_) {
			*connected_ = -1;
		}
		// 安全清理通道状态
		if (bufidx_ >= 0 && bufidx_ < MAX_USER_CHANNEL) {
			try {
				idx_Lock();
				g_user_channel_state[bufidx_] = MYASR_CHANNEL_FREE;
				idx_Unlock();
			} catch (...) {
				try { idx_Unlock(); } catch (...) {}
			}
		}
	}
	
	// 统一的最终清理，确保在任何情况下都执行
	if (bufidx_ >= 0 && bufidx_ < MAX_USER_CHANNEL) {
			try {
				idx_Lock();
				g_user_channel_state[bufidx_] = MYASR_CHANNEL_FREE;
				myasr_audio_queue_release_locked(bufidx_);
				idx_Unlock();
		} catch (...) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "work_thread : final cleanup exception uuid[%s] bufidx[%d]\n", uuid_.empty() ? "unknown" : uuid_.c_str(), bufidx_);
			try { idx_Unlock(); } catch (...) {}
		}
	}
	
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "work_thread : thread exiting safely uuid[%s] bufidx[%d]\n", uuid_.empty() ? "unknown" : uuid_.c_str(), bufidx_);
}

std::string urlencode(const std::string &s)
{
	std::ostringstream escaped;
	escaped.fill('0');
	escaped << std::hex;

	for (std::string::const_iterator i = s.begin(), n = s.end(); i != n; ++i)
	{
		std::string::value_type c = (*i);

		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
		{
			escaped << c;
			continue;
		}

		escaped << std::uppercase;
		escaped << '%' << std::setw(2) << int((unsigned char)c);
		escaped << std::nouppercase;
	}

	return escaped.str();
}

void base64_encode_block(char *inData, int inlen, char *outData, int *outlen)
{
	if (NULL == inData || NULL == outData || NULL == outlen)
	{
		if (outlen) *outlen = 0;
		return;
	}

	int blocksize;
	blocksize = inlen * 8 / 6 + 3;
	unsigned char *buffer = NULL;
	
	try {
		buffer = new unsigned char[blocksize];
		memset(buffer, 0, blocksize);
		*outlen = EVP_EncodeBlock(buffer, (const unsigned char *)inData, inlen);
		strcpy(outData, (char *)buffer);
		delete[] buffer;
	} catch (...) {
		if (buffer) delete[] buffer;
		*outlen = 0;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "base64_encode_block: memory allocation failed\n");
	}
}

void md5(unsigned char *data, char hex[36])
{
	unsigned char digest[MD5_DIGEST_LENGTH];
	MD5(data, std::strlen((const char *)data), digest);

	for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
	{
		snprintf(&hex[i * 2], 3, "%02x", (unsigned int)digest[i]);
	}
	hex[2 * MD5_DIGEST_LENGTH] = '\0';
}

////////////////////////////////////////////////////////////
// wss client thread
////////////////////////////////////////////////////////////
static void *wss_client_run(void *arg)
{
	struct WsUserData *wud;
	wud = (struct WsUserData *)arg;
	if (!wud)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " wss client get param null\n");
		pthread_exit(NULL);
		return 0;
	}
	const int local_bufidx = wud->bufidx;
	char local_uuid[sizeof(wud->uuid)] = {0};
	char local_leg[sizeof(wud->leg)] = {0};
	char local_custom_appid[sizeof(wud->custom_appid)] = {0};
	strncpy(local_uuid, wud->uuid, sizeof(local_uuid) - 1);
	strncpy(local_leg, wud->leg, sizeof(local_leg) - 1);
	strncpy(local_custom_appid, wud->custom_appid, sizeof(local_custom_appid) - 1);

	// 初始化为连接中，只有明确失败路径才置为 -1，避免主线程误判失败。
	wud->connected = 0;
	
	// init websocket client
	asr_params params;
	// 根据自己音频格式具体设置
	params.audio_format = "pcm";
	params.sample_rate = MYASR_TARGET_SAMPLE_RATE;

	char tempStr[200] = {0};
	unsigned char digest[EVP_MAX_MD_SIZE] = {'\0'};
	unsigned int digest_len = 0;
	char hex[36] = {0};
	char url[200] = {0};
	char outdata[200] = {0};
	int outlen;
	char *appid = globals.app_id;
	char *key = globals.app_key;
	if (zstr(appid) || zstr(key) || zstr(globals.wss_url)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "wss_client_run missing config appid/key/ws_url or wss_url bufidx=%d\n", local_bufidx);
		wud->connected = -1;
		__sync_sub_and_fetch(&g_wss_thread_count, 1);
		myasr_wud_release(wud);
		pthread_exit(NULL);
		return 0;
	}
	int64_t timeStamp = time(NULL);
	snprintf(tempStr, sizeof(tempStr), "%s%ld", appid, (long)timeStamp);

	md5((unsigned char *)tempStr, hex);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "tempStr=%s hex is =%s\n", tempStr, hex);
	HMAC(EVP_sha1(), key, strlen(key), (unsigned char *)hex, strlen(hex), digest, &digest_len);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " digest_len is =%u\n", digest_len);
	base64_encode_block((char *)digest, digest_len, (char *)outdata, &outlen);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " outdata is =%s\n", outdata);
	std::string signa(outdata);
	std::string signaEncode = urlencode(signa);
	
	// 决定使用哪个appid：如果有自定义appid则使用，否则使用默认的
	char *url_appid = (strlen(local_custom_appid) > 0) ? local_custom_appid : appid;
	// snprintf(url, sizeof(url), "wss://dev1-asr.useasy.cn/v1/ws?appid=%s&ts=%ld&signa=%s", url_appid, (long)timeStamp, signaEncode.c_str());
	snprintf(url, sizeof(url), "%s?appid=%s&ts=%ld&signa=%s", globals.wss_url, url_appid, (long)timeStamp, signaEncode.c_str());
	std::string ws_url = url;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " wss client begin connect url=%s bufidx=%d using appid=%s\n", ws_url.c_str(), local_bufidx, url_appid);

	// 启动websocket client
	try {
		wssclient ws_client(params);
		ws_client.uuid_ = local_uuid;
		ws_client.leg_ = local_leg;
		ws_client.bufidx_ = local_bufidx;
		
		// 尝试连接WebSocket服务器
		if (!ws_client.open_connection(ws_url, &wud->connected))
		{
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, " open websocket connection failed !!! bufidx=%d \n", local_bufidx);
			wud->connected = -1;
			// 清理通道状态
			if (local_bufidx >= 0 && local_bufidx < MAX_USER_CHANNEL) {
				idx_Lock();
				g_user_channel_state[local_bufidx] = MYASR_CHANNEL_FREE;
				myasr_audio_queue_release_locked(local_bufidx);
				idx_Unlock();
			}
			__sync_sub_and_fetch(&g_wss_thread_count, 1);
			myasr_wud_release(wud);
			return 0;
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " wss client run bufidx=%d\n", local_bufidx);
		ws_client.run();
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " wss client end bufidx=%d\n", local_bufidx);
	}
	catch (const std::exception& e) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "wss_client_run exception: %s, bufidx=%d\n", e.what(), local_bufidx);
		wud->connected = -1;
		// 清理通道状态
		if (local_bufidx >= 0 && local_bufidx < MAX_USER_CHANNEL) {
			idx_Lock();
			g_user_channel_state[local_bufidx] = MYASR_CHANNEL_FREE;
			myasr_audio_queue_release_locked(local_bufidx);
			idx_Unlock();
		}
	}
	catch (...) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "wss_client_run unknown exception, bufidx=%d\n", local_bufidx);
		wud->connected = -1;
		// 清理通道状态
		if (local_bufidx >= 0 && local_bufidx < MAX_USER_CHANNEL) {
			idx_Lock();
			g_user_channel_state[local_bufidx] = MYASR_CHANNEL_FREE;
			myasr_audio_queue_release_locked(local_bufidx);
			idx_Unlock();
		}
	}

	// 确保线程正常退出
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "wss_client_run thread exit, bufidx=%d\n", local_bufidx);
	__sync_sub_and_fetch(&g_wss_thread_count, 1);
	myasr_wud_release(wud);
	pthread_exit(NULL);
	return 0;
}

// media bug callback
static switch_bool_t myasr_callback(switch_media_bug_t *bug, void *userdata, switch_abc_type_t type)
{
	struct user_data *ud = (user_data *)userdata;
	if (!ud)
		return SWITCH_TRUE;
	if (!bug)
		return SWITCH_TRUE;
	switch_codec_implementation_t read_impl;
	switch_status_t status;
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
	if (!session)
	{
		return SWITCH_TRUE;
	}
	int aleg_idx = ud->aleg_bufidx;

	switch (type)
	{
	case SWITCH_ABC_TYPE_INIT:
	{
		switch_core_session_get_read_impl(session, &read_impl);
		ud->read_sample_rate = read_impl.actual_samples_per_second;
		status = switch_resample_create(&ud->resampler, read_impl.actual_samples_per_second, MYASR_TARGET_SAMPLE_RATE, 640, SWITCH_RESAMPLE_QUALITY, 1);
		if (status != SWITCH_STATUS_SUCCESS)
		{
			ud->resampler = NULL;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to allocate resampler from %u to %d\n",
							  ud->read_sample_rate, MYASR_TARGET_SAMPLE_RATE);
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "myasr callback : init aleg_idx=%d aleg_uuid=%s input_rate=%u target_rate=%d\n",
						  aleg_idx, ud->aleg_uuid, ud->read_sample_rate, MYASR_TARGET_SAMPLE_RATE);
	}
	break;

	case SWITCH_ABC_TYPE_CLOSE:
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "myasr callback : stop1 aleg_idx=%d aleg_uuid=%s\n", aleg_idx, ud->aleg_uuid);
		if (aleg_idx >= 0 && aleg_idx < MAX_USER_CHANNEL)
		{
			if (ud->aleg_buf_len <= MAX_PAYLOAD_SIZE)
			{
				myasr_publish_audio_buffer(aleg_idx, ud->aleg_buf, ud->aleg_buf_len);
			}
			else
			{
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "myasr callback : drop invalid tail buffer len=%zu aleg_idx=%d\n",
								  ud->aleg_buf_len, aleg_idx);
			}

				idx_Lock();
				g_user_channel_state[aleg_idx] = MYASR_CHANNEL_STOPPING;
				idx_Unlock();
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "myasr callback : stop1 aleg_idx=%d aleg_uuid=%s\n", aleg_idx, ud->aleg_uuid);
			// EndAsr(ud->aleg_uuid);
		}

		if (ud->resampler)
		{
			switch_resample_destroy(&ud->resampler);
		}
	}
	break;

	case SWITCH_ABC_TYPE_READ:
	{
		uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
		switch_frame_t frame = {0};

		if (aleg_idx < 0 || aleg_idx >= MAX_USER_CHANNEL || myasr_channel_is_running(aleg_idx) != SWITCH_TRUE)
		{
			break;
		}

		frame.data = data;
		frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

		if (switch_core_media_bug_read(bug, &frame, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS && !switch_test_flag((&frame), SFF_CNG) && frame.datalen)
		{
			const char *frame_data = NULL;
			size_t frame_len = 0;
			if (myasr_get_frame_audio_16k(ud, &frame, &frame_data, &frame_len))
			{
				myasr_append_audio_buffer(ud, aleg_idx, frame_data, frame_len);
			}
		}
	}
	break;

	case SWITCH_ABC_TYPE_WRITE:
	{
		uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
		switch_frame_t frame = {0};

		if (aleg_idx < 0 || aleg_idx >= MAX_USER_CHANNEL || myasr_channel_is_running(aleg_idx) != SWITCH_TRUE)
		{
			break;
		}

		frame.data = data;
		frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

		if (switch_core_media_bug_read(bug, &frame, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS && !switch_test_flag((&frame), SFF_CNG) && frame.datalen)
		{
			const char *frame_data = NULL;
			size_t frame_len = 0;
			if (myasr_get_frame_audio_16k(ud, &frame, &frame_data, &frame_len))
			{
				myasr_append_audio_buffer(ud, aleg_idx, frame_data, frame_len);
			}
		}
	}
	break;

	default:
		break;
	}

	return SWITCH_TRUE;
}

// stop_myasr
SWITCH_STANDARD_APP(stop_asr_session_function)
{
	user_data *ud;
	if (!session)
	{
		return;
	}
	switch_channel_t *channel = switch_core_session_get_channel(session);
	if (!channel)
	{
		return;
	}
	if ((ud = (user_data *)switch_channel_get_private(channel, "myasr")))
	{
		switch_channel_set_private(channel, "myasr", NULL);
		if (ud->aleg_bufidx >= 0 && ud->aleg_bufidx < MAX_USER_CHANNEL) {
			idx_Lock();
			g_user_channel_state[ud->aleg_bufidx] = MYASR_CHANNEL_STOPPING;
			idx_Unlock();
		}
		if (ud->bug) {
			switch_core_media_bug_remove(session, &ud->bug);
		}
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s Stop MYASR\n", switch_channel_get_name(channel));
	}
}

//<action application="start_myasr" data="0 123"/>
SWITCH_STANDARD_APP(start_asr_session_function)
{
	if (!session)
	{
		return;
	}
	switch_channel_t *channel = switch_core_session_get_channel(session);
	if (!channel)
	{
		return;
	}

	// switch_media_bug_t *bug;
	switch_status_t status;
	struct user_data *ud;
	struct WsUserData *aleg_wud = NULL;
	switch_codec_implementation_t read_impl;
	memset(&read_impl, 0, sizeof(switch_codec_implementation_t));
	switch_core_session_get_read_impl(session, &read_impl);
	int nMode = 1; // 1 识别aleg, 0 识别bleg
	const char *aleg_uuid;
	int aleg_idx = -1;
	char custom_appid[64] = {0}; // 存储自定义appid

	if (zstr(data))
		return;

	// 解析参数：第一个参数是模式，第二个参数是appid
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_myasr : data=%s\n", data);
	
	// 解析参数
	char *data_copy = strdup(data);
	if (!data_copy)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_myasr : strdup data failed\n");
		return;
	}
	char *mode_str = strtok(data_copy, " ");
	char *appid_str = strtok(NULL, " ");
	
	if (mode_str && mode_str[0] == '0')
		nMode = 0;
	
	// 如果提供了第二个参数，保存自定义appid
	if (appid_str && strlen(appid_str) > 0) {
		strncpy(custom_appid, appid_str, sizeof(custom_appid) - 1);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_myasr : using custom appid=%s\n", custom_appid);
	}
	
	free(data_copy);

	// create ud
	if (!(ud = (user_data *)switch_core_session_alloc(session, sizeof(user_data))))
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "session alloc aleg_ud fail\n");
		return;
	}
	memset(ud, 0, sizeof(user_data));

	// get aleg uuid
	aleg_uuid = switch_channel_get_variable(channel, "call_uuid");
	if (!aleg_uuid)
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "get uuid fail\n");
		return;
	}

	// create aleg userdata
	aleg_wud = (struct WsUserData *)calloc(1, sizeof(struct WsUserData));
	if (!aleg_wud)
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "get malloc fail\n");
		return;
	}
	if (myasr_wud_init(aleg_wud) != SWITCH_TRUE)
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "init wss userdata fail\n");
		free(aleg_wud);
		return;
	}
	strncpy(aleg_wud->uuid, aleg_uuid, sizeof(aleg_wud->uuid) - 1);
	strcpy(aleg_wud->leg, "aleg");
	aleg_wud->sendcounter = 0;
	aleg_wud->recvcounter = 0;
	aleg_wud->timeout = time(NULL);
	aleg_wud->sendFlag = 0;
	aleg_wud->connected = 0;
	
	// 设置自定义appid
	strncpy(aleg_wud->custom_appid, custom_appid, sizeof(aleg_wud->custom_appid) - 1);

	// get aleg channel index
	aleg_idx = getChannelIdx();
	if (aleg_idx < 0 || aleg_idx >= MAX_USER_CHANNEL)
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "getChannelIdx fail\n");
		myasr_wud_release(aleg_wud);
		return;
	}
	aleg_wud->bufidx = aleg_idx;

	if (myasr_audio_queue_prepare(aleg_idx) != SWITCH_TRUE)
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "prepare audio queue fail aleg_idx=%d\n", aleg_idx);
		idx_Lock();
		g_user_channel_state[aleg_idx] = MYASR_CHANNEL_FREE;
		idx_Unlock();
		myasr_wud_release(aleg_wud);
		return;
	}

	// set ud param
	ud->mode = nMode;
	strncpy(ud->aleg_uuid, aleg_uuid, sizeof(ud->aleg_uuid) - 1);
	memset(ud->aleg_buf, 0, sizeof(ud->aleg_buf));
	ud->aleg_buf_len = 0;
	ud->aleg_bufidx = aleg_idx;
 
	// create aleg wss thread
	pthread_t wss_thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 1024 * 1024); // 设置1MB栈大小
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED); // 分离线程，自动清理
	myasr_wud_retain(aleg_wud);
	__sync_add_and_fetch(&g_wss_thread_count, 1);

	int err = pthread_create(&wss_thread, &attr, wss_client_run, aleg_wud);
	pthread_attr_destroy(&attr);
	
	if (err)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_myasr -> create aleg wss thread fail err[%d]-%s \n", err, strerror(err));
		idx_Lock();
		g_user_channel_state[aleg_idx] = MYASR_CHANNEL_FREE;
		myasr_audio_queue_release_locked(aleg_idx);
		idx_Unlock();
		__sync_sub_and_fetch(&g_wss_thread_count, 1);
		myasr_wud_release(aleg_wud);
		myasr_wud_release(aleg_wud);
		return;
	}

	// 等待wss client连接成功
	int wait_count = 0;
	while (aleg_wud->connected != 1)
	{
		if (aleg_wud->connected < 0)
		{
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "wss client connect fail aleg_idx=%d aleg_uuid=%s, cleaning up channel state\n", aleg_idx, aleg_uuid);
			// 清理通道状态
			idx_Lock();
			g_user_channel_state[aleg_idx] = MYASR_CHANNEL_FREE;
			myasr_audio_queue_release_locked(aleg_idx);
			idx_Unlock();
			myasr_wud_release(aleg_wud);
			return;
		}
		if (wait_count > 50) // 减少等待时间，从10秒减少到5秒
		{
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "wss client connect timeout aleg_idx=%d aleg_uuid=%s, cleaning up channel state\n", aleg_idx, aleg_uuid);
			// 清理通道状态
			idx_Lock();
			g_user_channel_state[aleg_idx] = MYASR_CHANNEL_FREE;
			myasr_audio_queue_release_locked(aleg_idx);
			idx_Unlock();
			// 设置连接失败状态，让WebSocket线程知道需要退出
			aleg_wud->connected = -1;
			myasr_wud_release(aleg_wud);
			return;
		}
		usleep(100000); // 等待100毫秒
		wait_count++;
	}

	if (nMode == 0)
	{
		if ((status = switch_core_media_bug_add(session, "myasr", NULL, myasr_callback, ud, 0, SMBF_WRITE_STREAM | SMBF_NO_PAUSE, &(ud->bug))) != SWITCH_STATUS_SUCCESS)
		{
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "media bug add fail\n");
			idx_Lock();
			g_user_channel_state[aleg_idx] = MYASR_CHANNEL_FREE;
			myasr_audio_queue_release_locked(aleg_idx);
			idx_Unlock();
			myasr_wud_release(aleg_wud);
			return;
		}
	}
	else
	{
		if ((status = switch_core_media_bug_add(session, "myasr", NULL, myasr_callback, ud, 0, SMBF_READ_STREAM | SMBF_NO_PAUSE, &(ud->bug))) != SWITCH_STATUS_SUCCESS)
		{
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "media bug add fail\n");
			idx_Lock();
			g_user_channel_state[aleg_idx] = MYASR_CHANNEL_FREE;
			myasr_audio_queue_release_locked(aleg_idx);
			idx_Unlock();
			myasr_wud_release(aleg_wud);
			return;
		}
	}

	switch_channel_set_private(channel, "myasr", ud);
	myasr_wud_release(aleg_wud);
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "%s Start MYASR\n", switch_channel_get_name(channel));
}

// load
SWITCH_MODULE_LOAD_FUNCTION(mod_myasr_load)
{
	switch_application_interface_t *app_interface;

	globals.pool = pool;
	load_mymedia_config(pool);

	switch_event_bind(modname, SWITCH_EVENT_RELOADXML, NULL, event_handler, NULL);

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	SWITCH_ADD_APP(app_interface, "start_myasr", "myasr", "myasr", start_asr_session_function, "", SAF_MEDIA_TAP);
	SWITCH_ADD_APP(app_interface, "stop_myasr", "myasr", "myasr", stop_asr_session_function, "", SAF_NONE);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "myasr_load\n");

	// create https thread
	globals.wait_timeout = 0;

	pthread_mutex_init(&idx_mutex, NULL);

	g_app_shutdown = 1;

	return SWITCH_STATUS_SUCCESS;
}

// unload
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_myasr_shutdown)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "myasr_shutdown\n");

	switch_event_unbind_callback(event_handler);

	g_app_shutdown = 0;
	
	// 清理所有通道状态，确保工作线程正确退出
	idx_Lock();
	for (int i = 0; i < MAX_USER_CHANNEL; i++) {
		if (g_user_channel_state[i] == MYASR_CHANNEL_RUNNING) {
			g_user_channel_state[i] = MYASR_CHANNEL_FREE;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "myasr_shutdown: cleaning channel %d\n", i);
		}
		myasr_audio_queue_release_locked(i);
	}
	idx_Unlock();
	
	for (int wait_count = 0; wait_count < 50 && __sync_add_and_fetch(&g_wss_thread_count, 0) > 0; wait_count++) {
		usleep(100000);
	}
	if (__sync_add_and_fetch(&g_wss_thread_count, 0) > 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "myasr_shutdown: timeout waiting for %d wss threads\n", __sync_add_and_fetch(&g_wss_thread_count, 0));
	}

	pthread_mutex_destroy(&idx_mutex);
	
	return SWITCH_STATUS_SUCCESS;
}
