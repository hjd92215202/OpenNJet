
#ifndef __NJT_HEALTH_CHECK_MODULE_H__
#define __NJT_HEALTH_CHECK_MODULE_H__

#include <njt_health_check_parser.h>


typedef struct {
    njt_str_t           upstream_name;
    njt_str_t           hc_type;
    health_check_t      *hc_data;
    bool                persistent;
    bool                mandatory;
    unsigned            success: 1;
    njt_int_t           rc;
} njt_health_check_api_data_t;


typedef struct {
    njt_queue_t hc_queue; // 健康检查列表
   // njt_event_t check_upstream; //
    unsigned first:1;
} njt_health_check_main_conf_t;


typedef struct {
    njt_http_upstream_rr_peer_t *peer;
    njt_queue_t      ele_queue;
} njt_health_check_http_peer_element;

typedef struct {
    njt_stream_upstream_rr_peer_t *peer;
    njt_queue_t      ele_queue;
} njt_health_check_stream_peer_element;



#endif