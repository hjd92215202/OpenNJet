/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) 2021-2023  TMLake(Beijing) Technology Co., Ltd.
 */

#ifndef NJT_HEALTH_CHECK_COMMON_H
#define NJT_HEALTH_CHECK_COMMON_H

#include <njt_config.h>
#include <njt_core.h>
#include <njet.h>
#include <njt_event.h>
#include <njt_json_api.h>
#include <njt_json_util.h>
#include <njt_http.h>
#include <njt_stream.h>
#include "njt_health_check_register_module.h"


enum {
    HC_SUCCESS = 0,
    HC_TYPE_NOT_FOUND,
    HC_UPSTREAM_NOT_FOUND,
    HC_VERSION_NOT_SUPPORT,
    HC_CA_NOT_CONF,
    HC_CERTIFICATE_NOT_CONF,
    HC_CERTIFICATE_KEY_NOT_CONF,
    HC_SERVER_ERROR,
    HC_DOUBLE_SET,
    HC_BODY_ERROR,
    HC_PATH_NOT_FOUND,
    HC_METHOD_NOT_ALLOW,
    HC_NOT_FOUND,
    PORT_NOT_ALLOW,
    UDP_NOT_SUPPORT_TLS,
    SMYSQL_NOT_SUPPORT_SSL_CONFIG,
    HC_SMYSQL_DB_NOT_SET_ERROR,
    HC_RESP_DONE
} NJT_HTTP_API_HC_ERROR;






#if (NJT_OPENSSL)
typedef struct njt_health_check_ssl_conf_s {
    njt_flag_t ssl_enable;
    njt_flag_t ntls_enable;
    njt_flag_t ssl_session_reuse;
    njt_uint_t ssl_protocols;
    njt_str_t ssl_protocol_str;
    njt_str_t ssl_ciphers;
    njt_str_t ssl_name;
    njt_flag_t ssl_server_name;
    njt_flag_t ssl_verify;
    njt_int_t ssl_verify_depth;
    njt_str_t ssl_trusted_certificate;
    njt_str_t ssl_crl;
    njt_str_t ssl_certificate;
    njt_str_t ssl_certificate_key;
    njt_str_t ssl_enc_certificate;
    njt_str_t ssl_enc_certificate_key;
    njt_array_t *ssl_passwords;
    njt_array_t *ssl_conf_commands;
    njt_ssl_t *ssl;
} njt_health_check_ssl_conf_t;
#endif


typedef struct {
    njt_http_upstream_rr_peer_t *peer;   //current peer
    njt_queue_t  datas;     //other peers which has same servername of the current peer
} njt_health_check_http_same_peer_t;

typedef struct {
    njt_stream_upstream_rr_peer_t *peer;   //current peer
    njt_queue_t  datas;     //other peers which has same servername of the current peer
} njt_health_check_stream_same_peer_t;



//every hc has one this config, record current hc running info
typedef struct njt_health_check_conf_s {
    njt_pool_t  *pool;
    njt_log_t   *log;
    njt_health_check_reg_info_t *reg;
    njt_queue_t queue;
    njt_uint_t  module_type;
    njt_str_t   server_type;
    njt_uint_t  curr_delay;
    njt_uint_t  curr_frame;
    njt_str_t   upstream_name;
    njt_msec_t  interval;
    njt_msec_t  visit_interval;
    njt_msec_t  jitter;
    njt_msec_t  timeout;
    njt_uint_t  port;
    njt_uint_t  passes;
    njt_uint_t  fails;
#if (NJT_OPENSSL)
    njt_health_check_ssl_conf_t ssl;
#endif
    njt_event_t hc_timer;
    void        *ctx;    // http 或stream 特异化字段
    njt_int_t   ref_count;    //record all running peers count, used for release resource when delete current hc
    unsigned    persistent: 1;
    unsigned    mandatory: 1;
    unsigned    disable: 1;    //this flag used for release resouce when delete hc or delete upstream
    unsigned    first: 1;      //if first, need recreate map

    njt_lvlhsh_t    servername_to_peers; //1 vs more, key:servername value: peers which hash same servername
    njt_uint_t      update_id;           //check wether upstream has modified, update_id is not equel when upstream has member modify
    njt_pool_t      *map_pool;           //used for map
} njt_health_check_conf_t;

typedef struct {
    njt_str_t uri;
    njt_str_t status;
    njt_array_t headers;
    njt_str_t body;
} njt_health_check_add_data_t;

typedef struct {
    njt_str_t send;
    njt_str_t expect;
} njt_health_check_stream_add_data_t;

#if (NJT_OPENSSL)
typedef struct {
    bool ssl_enable;
    bool ntls_enable;
    bool ssl_session_reuse;
    njt_int_t ssl_protocols;
    njt_str_t ssl_protocols_str;
    njt_str_t ssl_ciphers;
    njt_str_t ssl_name;
    bool ssl_server_name;
    bool ssl_verify;
    njt_int_t ssl_verify_depth;
    njt_str_t ssl_trusted_certificate;
    njt_str_t ssl_crl;
    njt_str_t ssl_certificate;
    njt_str_t ssl_certificate_key;
    njt_str_t ssl_enc_certificate;
    njt_str_t ssl_enc_certificate_key;
    njt_str_t ssl_passwords;
    njt_str_t ssl_conf_commands;
} njt_health_check_ssl_add_data_t;
#endif



typedef void (*njt_health_check_interal_update_handler)(void *hc_peer, njt_int_t status);

typedef struct njt_health_check_peer_s {
    njt_uint_t                      peer_id;
    njt_uint_t                      update_id;
    njt_str_t                       server;
    njt_health_check_conf_t         *hhccf;
    njt_pool_t                      *pool;
    njt_peer_connection_t           peer;
    njt_health_check_interal_update_handler    update_handler;
    njt_buf_t                       *send_buf;
    njt_buf_t                       *recv_buf;
    njt_chain_t                     *recv_chain;
    njt_chain_t                     *last_chain_node;
    void                            *parser;
#if (NJT_HTTP_SSL)
    njt_str_t                       ssl_name;
#endif
    void                            *data;       //can used as customer data
} njt_health_check_peer_t;

/*Type of callbacks of the checker*/

typedef njt_int_t (*njt_health_check_process_pt)(njt_health_check_peer_t *peer);

typedef njt_int_t (*njt_health_check_event_handler)(njt_event_t *ev);

typedef void (*njt_health_check_update_pt)(njt_health_check_peer_t *hc_peer, njt_int_t status);

typedef struct {
    njt_health_check_event_handler write_handler;
    njt_health_check_event_handler read_handler;
    njt_health_check_process_pt process;
    njt_health_check_update_pt update;

} njt_health_check_checker_t;




#endif //NJT_HEALTH_CHECK_COMMON_H
