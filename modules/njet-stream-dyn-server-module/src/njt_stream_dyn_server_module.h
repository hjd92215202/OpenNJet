/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) 2021-2023  TMLake(Beijing) Technology Co., Ltd.
 */
#ifndef NJT_STREAM_DYN_SERVER_MODULE_H_
#define NJT_STREAM_DYN_SERVER_MODULE_H_
#include <njt_config.h>
#include <njt_core.h>


extern njt_module_t njt_stream_dyn_server_module;
typedef struct njt_stream_dyn_server_info_s {
    njt_str_t file;
    njt_str_t type;
    njt_str_t addr_port;
    njt_str_t listen_option;
    njt_str_t server_name;  //查找用。去掉开头结尾 "
    njt_str_t old_server_name;  // 原始的。
    njt_str_t server_body;
    njt_str_t listens;
    njt_pool_t *pool;
    njt_stream_core_srv_conf_t *cscf;
    njt_str_t     msg;
    njt_str_t buffer;
    njt_int_t   bind;
    unsigned                   ssl_certificate:1; 
    unsigned                   ssl_certificate_key:1;
    unsigned                   listen_count:2;
    njt_stream_addr_conf_t *addr_conf;
} njt_stream_dyn_server_info_t;

typedef struct njt_stream_dyn_server_loc_conf_s {
    njt_flag_t dyn_server_enable;
} njt_stream_dyn_server_loc_conf_t;

typedef void (*njt_proxy_close_handler_pt)(njt_stream_session_t *s, njt_uint_t rc);
typedef struct njt_stream_dyn_server_client_ctx_s {
    njt_queue_t queue;  //加入VS 的session 队列。
    njt_stream_session_t *s;
    //njt_stream_core_srv_conf_t *srv;  // 记录VS 的指针
} njt_stream_dyn_server_ctx_t;

typedef struct njt_stream_dyn_server_srv_s {
    njt_queue_t *session_queue;   // 记录客户端的session 列表
    njt_proxy_close_handler_pt proxy_close_handler;  //关闭proxy_pass 的回调指针。
} njt_stream_dyn_server_srv_t;

njt_stream_dyn_server_info_t * njt_stream_parser_server_data(njt_str_t json_str,njt_uint_t method);
void njt_stream_dyn_server_set_finalize(njt_conf_t *cf,njt_proxy_close_handler_pt  proxy_close_handler);
#endif
