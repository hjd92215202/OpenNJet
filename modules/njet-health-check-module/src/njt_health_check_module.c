/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) 2021-2023  TMLake(Beijing) Technology Co., Ltd.
 */


#include <njt_config.h>
#include <njt_core.h>
#include <njt_http.h>
#include <njet.h>
#include <njt_stream.h>
#include <njt_str_util.h>
#include <njt_json_util.h>
#include <njt_http_sendmsg_module.h>
#include <njt_http_ext_module.h>
#include <njt_health_check_register_module.h>
#include <njt_health_check_parser.h>
#include <njt_health_check_ctrl_parser.h>
#include "njt_health_check_util.h"
#include "njt_http_api_register_module.h"
#include "njt_health_check_module.h"


#define NJT_HTTP_SERVER_PORT        5688
#define NJT_HTTP_HC_INTERVAL        5000
#define NJT_HTTP_HC_CONNECT_TIMEOUT 5000


#define  NJT_HEALTH_CHECK_FILTER_TYPE_NONE 0
#define  NJT_HEALTH_CHECK_FILTER_TYPE_HTTP 1
#define  NJT_HEALTH_CHECK_FILTER_TYPE_STREAM 2

#define HTTP_HEALTH_CHECK_SEPARATOR "$"
#define HTTP_UPSTREAM_KEYS "helper_hc_http_upstreams"
#define UPSTREAM_NAME_PREFIX "helper_hc_http_upstream" HTTP_HEALTH_CHECK_SEPARATOR
#define HTTP_HEALTH_CHECK_CONFS "helper_hc_confs"
#define HTTP_HEALTH_CHECK_CONF_INFO "helper_hc_conf_info" HTTP_HEALTH_CHECK_SEPARATOR

/* by zhaokang */
#define STREAM_HEALTH_CHECK_SEPARATOR     "$"
#define STREAM_UPSTREAM_KEYS              "stream_helper_hc_stream_upstreams"
#define STREAM_UPSTREAM_NAME_PREFIX       "stream_helper_hc_stream_upstream"     STREAM_HEALTH_CHECK_SEPARATOR
#define STREAM_HEALTH_CHECK_CONFS         "stream_helper_hc_confs"
#define STREAM_HEALTH_CHECK_CONF_INFO     "stream_helper_hc_conf_info"             STREAM_HEALTH_CHECK_SEPARATOR


// NJT_HTTP_API_HC_ERROR will be njt_health_check_error_msg's index, so please add pair 
static njt_str_t njt_health_check_error_msg[] = {
        njt_string("success"),
        njt_string("Type not allow"),
        njt_string("not find upstream"),
        njt_string("version not match , please update njet"),
        njt_string("Trusted CA certificate must be configured"),
        njt_string("Certificate must be configured"),
        njt_string("Certificate key must be configured"),
        njt_string("Unknown server error, please check the error log"),
        njt_string("The health check already configured "),
        njt_string("The request body parse error "),
        njt_string("The request uri not found "),
        njt_string("The request method not allow "),
        njt_string("Not found health check "),
        njt_string("port only allowed in 1-65535"),
        njt_string("UDP does not support tls"),
        njt_string("SMYSQL not allow config ssl block info"),
        njt_string("SMYSQL db must be set and should not be empty"),
        njt_string("")
};

extern njt_cycle_t *njet_master_cycle;

static njt_int_t
njt_health_check_lvlhsh_test(njt_lvlhsh_query_t *lhq, void *data)
{
    //ignore value compare, just return ok
    return NJT_OK;
}

const njt_lvlhsh_proto_t  njt_health_check_lvlhsh_proto = {
    NJT_LVLHSH_LARGE_MEMALIGN,
    njt_health_check_lvlhsh_test,
    njt_lvlhsh_pool_alloc,
    njt_lvlhsh_pool_free,
};

/**
 *This module provided directive: health_check url interval=100 timeout=300 port=8080 type=TCP.
 **/
static njt_int_t   njt_health_check_postconfiguration(njt_conf_t *cf);
static void njt_health_check_api_read_data(njt_http_request_t *r);
static njt_int_t njt_health_check_conf_handler(njt_http_request_t *r);
static njt_int_t
njt_health_check_api_parse_path(njt_http_request_t *r, njt_array_t *path);
static njt_int_t njt_health_check_add(njt_health_check_conf_t *hhccf, njt_str_t *msg, njt_int_t sync);
static njt_int_t njt_health_check_api_get_hcs(njt_http_request_t *r);
static void njt_health_check_http_timer_handler(njt_event_t *ev);
static njt_int_t njt_health_check_api_delete_conf(njt_http_request_t *r, njt_health_check_api_data_t *api_data);
static njt_int_t njt_health_check_api_get_conf_info(njt_http_request_t *r, njt_health_check_api_data_t *api_data);

static njt_int_t njt_health_check_conf_out_handler(njt_http_request_t *r, njt_int_t hrc);
njt_http_upstream_srv_conf_t* njt_health_check_find_http_upstream_by_name(njt_cycle_t *cycle,njt_str_t *name);
static njt_int_t njt_health_check_api_data2_common_cf(njt_health_check_api_data_t *api_data, njt_health_check_conf_t *hhccf);
static void njt_health_check_clean_peers_map(njt_health_check_conf_t *hhccf);
static void njt_health_check_loop_http_upstream_peer(njt_health_check_conf_t *hhccf, njt_http_upstream_rr_peers_t *peers,
        njt_flag_t backup, njt_flag_t op, bool map_recreate);
void njt_health_check_loop_stream_upstream_peer(njt_health_check_conf_t *hhccf, njt_stream_upstream_rr_peers_t *peers,
        njt_flag_t backup, njt_flag_t op, bool map_recreate);

static void njt_health_check_http_common_update(void *peer_info, njt_int_t status);
static void njt_health_check_http_common_update_bylock(void *peer_info, njt_int_t status);

static void njt_health_check_stream_common_update_bylock(void *peer_info, njt_int_t status);
static void njt_health_check_stream_common_update(void *peer_info, njt_int_t status);

static njt_int_t njt_health_check_helper_module_init(njt_cycle_t *cycle);

static void njt_health_check_http_free_peer_resource(njt_health_check_peer_t *hc_peer);
static void njt_health_check_stream_free_peer_resource(njt_health_check_peer_t *hc_peer);

static njt_int_t njt_health_check_init_process(njt_cycle_t *cycle);

static void njt_health_check_kv_flush_http_confs(njt_health_check_main_conf_t *hmcf);

static void njt_health_check_kv_flush_http_conf_info(njt_health_check_conf_t *hhccf, njt_str_t *msg, njt_flag_t is_add);

static njt_int_t njt_health_check_api_add_conf(njt_log_t *pool, njt_health_check_api_data_t *api_data, njt_str_t *msg, njt_int_t sync);

static njt_health_check_conf_t *njt_health_check_find_hc(njt_cycle_t *cycle, njt_health_check_api_data_t *api_data);

static njt_health_check_conf_t *njt_health_check_find_hc_by_name_and_type(njt_cycle_t *cycle, njt_str_t * hc_type,njt_str_t *upstream_name);

static njt_int_t njt_health_check_http_peer_add_map(njt_health_check_conf_t *hhccf, 
                njt_http_upstream_rr_peer_t *peer);

static njt_int_t njt_health_check_stream_peer_add_map(njt_health_check_conf_t *hhccf, 
            njt_stream_upstream_rr_peer_t *stream_peer);

static bool njt_health_check_http_check_peer(njt_health_check_conf_t *hhccf, 
                njt_http_upstream_rr_peer_t *peer);

static bool njt_health_check_stream_check_peer(njt_health_check_conf_t *hhccf, 
                njt_stream_upstream_rr_peer_t *peer);

static void njt_health_check_stream_timer_handler(njt_event_t *ev);

static void njt_health_check_kv_flush_stream_conf_info(njt_health_check_conf_t *hhccf, njt_str_t *msg, njt_flag_t is_add);

static void njt_health_check_kv_flush_stream_confs(njt_health_check_main_conf_t *hmcf);

static void njt_stream_health_check_recovery_confs();

njt_stream_upstream_srv_conf_t *njt_health_check_find_stream_upstream_by_name(njt_cycle_t *cycle, njt_str_t *name);

void njt_http_upstream_traver(void *ctx,njt_int_t (*item_handle)(void *ctx,njt_http_upstream_srv_conf_t *));

void njt_stream_upstream_traver(void *ctx,njt_int_t (*item_handle)(void *ctx,njt_stream_upstream_srv_conf_t *));

static njt_uint_t njt_health_check_get_module_type(njt_str_t *hc_type);
static njt_health_check_reg_info_t * njt_health_check_get_reg_module(njt_str_t *hc_type);

static njt_http_module_t njt_health_check_module_ctx = {
        NULL,                                   /* preconfiguration */
        njt_health_check_postconfiguration,          /* postconfiguration */

        NULL,                                   /* create main configuration */
        NULL,                                  /* init main configuration */

        NULL,                                  /* create server configuration */
        NULL,                                  /* merge server configuration */

        NULL,                                   /* create location configuration */
        NULL                                    /* merge location configuration */
};

njt_module_t njt_helper_health_check_module = {
        NJT_MODULE_V1,
        &njt_health_check_module_ctx,      /* module context */
        NULL,                                   /* module directives */
        NJT_HTTP_MODULE,                        /* module type */
        NULL,                                   /* init master */
        njt_health_check_helper_module_init,              /* init module */
        njt_health_check_init_process,/* init process */
        NULL,                                   /* init thread */
        NULL,                                   /* exit thread */
        NULL,                                   /* exit process */
        NULL,                                   /* exit master */
        NJT_MODULE_V1_PADDING
};


static njt_int_t   njt_health_check_postconfiguration(njt_conf_t *cf){
    njt_http_api_reg_info_t             h;

    njt_str_t  module_key = njt_string("/v1/hc");
    njt_memzero(&h, sizeof(njt_http_api_reg_info_t));
    h.key = &module_key;
    h.handler = njt_health_check_conf_handler;
    njt_http_api_module_reg_handler(&h);

    return NJT_OK;
}


static njt_int_t njt_health_check_conf_handler(njt_http_request_t *r) {
    njt_int_t                       rc;
    njt_int_t                       hrc;
    njt_str_t                       *uri;
    njt_health_check_api_data_t     *api_data = NULL;
    njt_array_t                     *path;

    hrc = HC_SUCCESS;
    if (r->method == NJT_HTTP_GET || r->method == NJT_HTTP_DELETE) {
        api_data = njt_pcalloc(r->pool, sizeof(njt_health_check_api_data_t));
        if (api_data == NULL) {
            njt_log_error(NJT_LOG_ERR, r->connection->log, 0, "alloc api_data error.");
            hrc = HC_SERVER_ERROR;
            goto out;
        }
    } else if (r->method == NJT_HTTP_POST) {
        rc = njt_http_read_client_request_body(r, njt_health_check_api_read_data);

        if (rc == NJT_ERROR || rc >= NJT_HTTP_SPECIAL_RESPONSE) {
            return rc;
        }
        return NJT_DONE;
    } else {
        hrc = HC_METHOD_NOT_ALLOW;
        goto out;
    }

    path = njt_array_create(r->pool, 4, sizeof(njt_str_t));
    if (path == NULL) {
        njt_log_error(NJT_LOG_ERR, r->connection->log, 0, "array init of path error.");
        hrc = HC_SERVER_ERROR;
        goto out;
    }

    rc = njt_health_check_api_parse_path(r, path);
    if (rc != HC_SUCCESS || path->nelts <= 0) {
        hrc = HC_PATH_NOT_FOUND;
        goto out;
    }

    uri = path->elts;
    if (path->nelts < 2 
        || (uri[1].len != 2 || njt_strncmp(uri[1].data, "hc", 2) != 0)) {
        hrc = HC_PATH_NOT_FOUND;
        goto out;
    }

    hrc = HC_PATH_NOT_FOUND;
    if (path->nelts == 2 && r->method == NJT_HTTP_GET) {
        hrc = njt_health_check_api_get_hcs(r);
    }

    if (path->nelts == 4) {
        api_data->hc_type = uri[2];
        api_data->upstream_name = uri[3];
        if (r->method == NJT_HTTP_DELETE) {
            hrc = njt_health_check_api_delete_conf(r, api_data);
        }
        if (r->method == NJT_HTTP_GET) {
            hrc = njt_health_check_api_get_conf_info(r, api_data);
        }
    }

    out:
    if (hrc == HC_RESP_DONE) {
        return NJT_OK;
    }

    return njt_health_check_conf_out_handler(r, hrc);
}


/**
 * 它读取请求体并将其解析为 njt_health_check_api_data_t 结构
 *
 * @param r http 请求对象
 */
static void njt_health_check_api_read_data(njt_http_request_t *r){
    njt_str_t                   json_str;
    njt_int_t                   hrc;
    njt_chain_t                 *body_chain, *tmp_chain;
    njt_int_t                   rc;
    njt_health_check_api_data_t    *api_data = NULL;
    njt_uint_t                  len, size;
    njt_str_t                   *uri;
    njt_array_t                 *path;
    js2c_parse_error_t          err_info;

    body_chain = r->request_body->bufs;
    /*check the sanity of the json body*/
    if(NULL == body_chain){
        hrc = HC_SERVER_ERROR;
        goto out;
    }

    api_data = njt_pcalloc(r->pool, sizeof(njt_health_check_api_data_t));
    if (api_data == NULL) {
        njt_log_error(NJT_LOG_DEBUG, r->connection->log, 0,
                       "could not alloc buffer in function %s", __func__);
        hrc = HC_SERVER_ERROR;
        goto out;
    }

    len = 0;
    tmp_chain = body_chain;
    while (tmp_chain != NULL) {
        len += tmp_chain->buf->last - tmp_chain->buf->pos;
        tmp_chain = tmp_chain->next;
    }

    json_str.len = len;
    json_str.data = njt_pcalloc(r->pool, len);
    if (json_str.data == NULL) {
        njt_log_error(NJT_LOG_DEBUG, r->connection->log, 0,
                       "could not alloc buffer in function %s", __func__);
        hrc = HC_SERVER_ERROR;
        goto out;
    }

    len = 0;
    tmp_chain = r->request_body->bufs;
    while (tmp_chain != NULL) {
        size = tmp_chain->buf->last - tmp_chain->buf->pos;
        njt_memcpy(json_str.data + len, tmp_chain->buf->pos, size);
        tmp_chain = tmp_chain->next;
        len += size;
    }

    api_data->hc_data = json_parse_health_check(r->pool, &json_str, &err_info);
    if(api_data->hc_data == NULL){
        njt_log_error(NJT_LOG_ERR, r->connection->log, 0, "health check json parse error:%V", &err_info.err_str);
        api_data->rc = NJT_ERROR;
        api_data->success = 0;
        hrc = HC_BODY_ERROR;
        goto out;
    } else {
        api_data->success = 1;
    }

    njt_http_set_ctx(r, api_data, njt_helper_health_check_module);

    path = njt_array_create(r->pool, 4, sizeof(njt_str_t));
    if (path == NULL) {
        njt_log_error(NJT_LOG_ERR, r->connection->log, 0, "array init of path error.");
        hrc = HC_SERVER_ERROR;
        goto out;
    }

    rc = njt_health_check_api_parse_path(r, path);
    if (rc != HC_SUCCESS || path->nelts <= 0) {
        hrc = HC_PATH_NOT_FOUND;
        goto out;
    }

    uri = path->elts;
    if (path->nelts < 2
        || (uri[1].len != 2 || njt_strncmp(uri[1].data, "hc", 2) != 0)) {
        hrc = HC_PATH_NOT_FOUND;
        goto out;
    }

    hrc = HC_PATH_NOT_FOUND;
    if (path->nelts == 4) {
        api_data->hc_type = uri[2];
        api_data->upstream_name = uri[3];
    }

    if (r->method == NJT_HTTP_POST) {
        hrc = njt_health_check_api_add_conf(r->pool->log, api_data, &json_str, 1);
    }

    if (hrc == HC_RESP_DONE) {
        rc = NJT_OK;
        goto end;
    }

    out:
    rc = njt_health_check_conf_out_handler(r, hrc);

    end:
    njt_http_finalize_request(r, rc);
}


static njt_health_check_reg_info_t * njt_health_check_get_reg_module(njt_str_t *hc_type){
    njt_str_t           sub_type;

    if(hc_type->len < 2){
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0, " health check type not valid !!");
        return NULL;
    }

    if(hc_type->data[0] != 's' && hc_type->data[0] != 'h'){
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0, " health check type not valid !!");
        return NULL;
    }

    //http is just used for http upstream do http check
    if(hc_type->len == 4 && njt_strncasecmp(hc_type->data, (u_char *)"http", 4) == 0){
        return njt_health_check_register_find_handler(hc_type);
    }

    sub_type.len = hc_type->len - 1;
    sub_type.data = hc_type->data + 1;

    return njt_health_check_register_find_handler(&sub_type);
}

static njt_uint_t njt_health_check_get_module_type(njt_str_t *hc_type){
    // [stcp|sudp|smysql] start with s, return NJT_STREAM_MODULE
    if(hc_type->len > 1 && hc_type->data[0] == 's'){
        return NJT_STREAM_MODULE;
    }

    return NJT_HTTP_MODULE;
}


static njt_int_t njt_health_check_api_add_conf(njt_log_t *log, njt_health_check_api_data_t *api_data, 
        njt_str_t *msg, njt_int_t sync) {
    njt_http_upstream_srv_conf_t            *uscf;
    njt_health_check_conf_t                 *hhccf;
    njt_cycle_t                             *cycle = (njt_cycle_t *) njt_cycle;
    njt_pool_t                              *hc_pool;
    njt_http_upstream_rr_peers_t            *peers;
    njt_stream_upstream_rr_peers_t          *stream_peers;
    njt_int_t                               rc = HC_SUCCESS;
    njt_stream_upstream_srv_conf_t          *suscf;
    njt_health_check_reg_info_t             *reg;

    if (api_data->hc_type.len == 0 || api_data->upstream_name.len == 0) {
        njt_log_error(NJT_LOG_ERR, log, 0, " type and upstream must be set !!");
        return HC_BODY_ERROR;
    }

    if(api_data->hc_data == NULL){
        njt_log_error(NJT_LOG_ERR, log, 0, " hc data must be set !!");
        return HC_BODY_ERROR;
    }

    if (api_data->hc_data->is_port_set && (api_data->hc_data->port < 0 || api_data->hc_data->port > 65535)) {
        njt_log_error(NJT_LOG_ERR, log, 0, " port is %i , port only allowed in 1-65535", api_data->hc_data->port);
        return PORT_NOT_ALLOW;
    }

    reg = njt_health_check_get_reg_module(&api_data->hc_type);
    if (reg == NULL) {
        njt_log_error(NJT_LOG_ERR, log, 0, "not support:%V in health check, please load %V's module",
            &api_data->hc_type, &api_data->hc_type);
        return HC_TYPE_NOT_FOUND;
    }

    hhccf = njt_health_check_find_hc(cycle, api_data);
    if (hhccf != NULL) {
        njt_log_error(NJT_LOG_ERR, log, 0, "find upstream %V hc, double set", &api_data->upstream_name);
        return HC_DOUBLE_SET;
    }

    hc_pool = njt_create_dynamic_pool(NJT_MIN_POOL_SIZE, cycle->log);
    if (hc_pool == NULL) {
        njt_log_error(NJT_LOG_ERR, log, 0, "health check helper create dynamic pool error ");
        rc = HC_SERVER_ERROR;
        goto err;
    }

    njt_sub_pool(cycle->pool, hc_pool);
    hhccf = njt_pcalloc(hc_pool, sizeof(njt_health_check_conf_t));
    if (hhccf == NULL) {
        njt_log_error(NJT_LOG_ERR, log, 0, "health check helper alloc hhccf mem error");
        rc = HC_SERVER_ERROR;
        goto err;
    }
    njt_queue_init(&hhccf->queue);
    hhccf->pool = hc_pool;
    hhccf->log = cycle->log;
    hhccf->reg = reg;

    hhccf->module_type = njt_health_check_get_module_type(&api_data->hc_type);
    if (hhccf->module_type == 0) {
        rc = HC_TYPE_NOT_FOUND;
        njt_log_error(NJT_LOG_ERR, log, 0, "type is not valid in health check");
        goto err;
    }

    hhccf->server_type.data =  njt_pcalloc(hc_pool, api_data->hc_type.len);
    if (hhccf->server_type.data == NULL) {
        njt_log_error(NJT_LOG_ERR, log, 0, "health check malloc hc_type:%V error", &api_data->hc_type);
        rc = HC_SERVER_ERROR;
        goto err;
    }

    njt_memcpy(hhccf->server_type.data, api_data->hc_type.data, api_data->hc_type.len);
    hhccf->server_type.len = api_data->hc_type.len;

    //set upstream name
    hhccf->upstream_name.data =  njt_pcalloc(hc_pool, api_data->upstream_name.len);
    if (hhccf->upstream_name.data == NULL) {
        njt_log_error(NJT_LOG_ERR, log, 0, "health check malloc upstream_name:%V error", &api_data->upstream_name);
        rc = HC_SERVER_ERROR;
        goto err;
    }

    njt_memcpy(hhccf->upstream_name.data, api_data->upstream_name.data, api_data->upstream_name.len);
    hhccf->upstream_name.len = api_data->upstream_name.len;

    hhccf->first = 1;

    hhccf->ctx = reg->create_ctx(hhccf, api_data);
    if (hhccf->ctx == NULL) {
        njt_log_error(NJT_LOG_EMERG, log, 0, "health check helper create ctx error ");
        rc = HC_SERVER_ERROR;
        goto err;
    }

    if (hhccf->module_type == NJT_HTTP_MODULE) {
        uscf = njt_health_check_find_http_upstream_by_name(njet_master_cycle, &api_data->upstream_name);
        if (uscf == NULL) {
            njt_log_error(NJT_LOG_ERR, log, 0, "not find http upstream: %V", &api_data->upstream_name);
            rc = HC_UPSTREAM_NOT_FOUND;
            goto err;
        }

        hhccf->mandatory = uscf->mandatory;
        hhccf->persistent = uscf->persistent;
        peers = uscf->peer.data;
        hhccf->update_id = peers->update_id;
    }

    if (hhccf->module_type == NJT_STREAM_MODULE) {
        suscf = njt_health_check_find_stream_upstream_by_name(njet_master_cycle, &api_data->upstream_name);
        if (suscf == NULL) {
            njt_log_error(NJT_LOG_ERR, log, 0, "not find stream upstream: %V", &api_data->upstream_name);
            rc = HC_UPSTREAM_NOT_FOUND;
            goto err;
        }

        hhccf->mandatory = suscf->mandatory;
        hhccf->persistent = suscf->persistent;
        stream_peers = suscf->peer.data;
        hhccf->update_id = stream_peers->update_id;
    }

    rc = njt_health_check_api_data2_common_cf(api_data, hhccf);
    if (rc != HC_SUCCESS) {
        njt_log_error(NJT_LOG_ERR, log, 0, "health check helper data transter error, rc:%d", rc);
        goto err;
    }

    njt_health_check_add(hhccf, msg, sync);
    return rc;

    err:
    if (hc_pool) {
        njt_destroy_pool(hc_pool);
    }
    return rc;
}


static njt_int_t njt_health_check_add(njt_health_check_conf_t *hhccf, njt_str_t *msg, njt_int_t sync) {
    njt_event_t *hc_timer;
    njt_uint_t refresh_in;
    njt_health_check_main_conf_t *hmcf;

    njt_cycle_t *cycle = (njt_cycle_t *) njt_cycle;

    hmcf = (njt_health_check_main_conf_t *) njt_get_conf(cycle->conf_ctx, njt_helper_health_check_module);
    hc_timer = &hhccf->hc_timer;
   
    /* by zhaokang */
    if (hhccf->module_type == NJT_HTTP_MODULE) {
         hc_timer->handler = njt_health_check_http_timer_handler;
    } 

    /* by zhaokang */
    if (hhccf->module_type == NJT_STREAM_MODULE) {
        hc_timer->handler = njt_health_check_stream_timer_handler;
    }

    hc_timer->log = hhccf->log;
    hc_timer->data = hhccf;
    hc_timer->cancelable = 1;
    refresh_in = njt_random() % 1000;
    njt_queue_insert_tail(&hmcf->hc_queue, &hhccf->queue);

    if (sync) {
        if (hhccf->module_type == NJT_HTTP_MODULE) {
            njt_health_check_kv_flush_http_conf_info(hhccf, msg, 1);
            njt_health_check_kv_flush_http_confs(hmcf);
        }

        /* by zhaokang */
        if ((hhccf->module_type == NJT_STREAM_MODULE)) {
            njt_health_check_kv_flush_stream_conf_info(hhccf, msg, 1);
            njt_health_check_kv_flush_stream_confs(hmcf);
        }
    }
    njt_add_timer(hc_timer, refresh_in);

    return NJT_OK;
}

/*define the status machine stage*/
static void njt_health_check_http_timer_handler(njt_event_t *ev) {
    njt_health_check_conf_t  *hhccf;
    njt_http_upstream_srv_conf_t    *uscf;
    njt_http_upstream_rr_peers_t    *peers;
    njt_uint_t                      jitter;
    njt_flag_t                      op = 0;
    bool                            map_recreate = false;
    
    hhccf = ev->data;
    if (hhccf == NULL) {
        njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0, "health check http timer no valid data");
        return;
    }

    if(ev->timer_set){
        njt_del_timer(ev);
    }

    if (hhccf->disable) {
        if(hhccf->ref_count == 0 ){
            njt_log_error(NJT_LOG_INFO, njt_cycle->log, 0,
                    "active probe cleanup for disable, type:%V upstream:%V", &hhccf->server_type, &hhccf->upstream_name);
            njt_destroy_pool(hhccf->pool);
        } else {
            njt_log_error(NJT_LOG_INFO, njt_cycle->log, 0,
                    "active probe cleanup for disable, type:%V upstream:%V", &hhccf->server_type, &hhccf->upstream_name);
            njt_add_timer(&hhccf->hc_timer, 1000);
        }

        return;
    }

    if (njt_quit || njt_terminate || njt_exiting) {
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0, "active probe cleanup for quiting in http health check");
        return;
    }

    uscf = njt_health_check_find_http_upstream_by_name(njet_master_cycle, &hhccf->upstream_name);
    if (uscf == NULL) {
        njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                "not found http upstream:%V data in health check", &hhccf->upstream_name);
        return;
    }

    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0, "Health check http timer is triggered.");

    if (hhccf->mandatory == 1 && hhccf->persistent == 0 && hhccf->curr_delay != 0) {
        hhccf->curr_frame += 1000;
    }

    if (hhccf->curr_delay != 0 && hhccf->curr_delay <= hhccf->curr_frame) {
        hhccf->curr_frame = 0;
        op = 1;
    } else if (hhccf->curr_delay == 0) {
        op = 1;
    }

    peers = uscf->peer.data;

    njt_http_upstream_rr_peers_wlock(peers);
    if(hhccf->first || hhccf->update_id != peers->update_id){
        hhccf->first = 0;
        hhccf->update_id = peers->update_id;
        //clear map
        njt_health_check_clean_peers_map(hhccf);
        map_recreate = true;
    }

    if (peers->peer) {
        njt_health_check_loop_http_upstream_peer(hhccf, peers, 0, op, map_recreate);
    }
    if (peers->next) {
        njt_health_check_loop_http_upstream_peer(hhccf, peers, 1, op, map_recreate);
    }
    njt_http_upstream_rr_peers_unlock(peers);

    jitter = 0;
    if (hhccf->jitter) {
        jitter = njt_random() % hhccf->jitter;
        njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                       "delay %u for the health check http timer.", jitter);
    }

    if(hhccf->mandatory == 1 && hhccf->persistent == 0) {
        hhccf->curr_delay = hhccf->interval + jitter;
        njt_add_timer(&hhccf->hc_timer, 1000);
    } else {
        njt_add_timer(&hhccf->hc_timer, hhccf->interval + jitter);
    }

    return;
}

/*
    by zhaokang
*/
static void
njt_health_check_stream_timer_handler(njt_event_t *ev) {
    njt_health_check_conf_t         *hhccf;    
    njt_stream_upstream_srv_conf_t         *uscf;
    njt_stream_upstream_rr_peers_t         *peers;
    njt_uint_t                              jitter;
    njt_flag_t                              op;
    bool                                    map_recreate = false;


    hhccf = ev->data;
    if (hhccf == NULL) {
        njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0, "no valid data");
        return;
    }

    if(ev->timer_set){
        njt_del_timer(ev);
        njt_log_error(NJT_LOG_INFO, njt_cycle->log, 0,
                      "del timer ");
    }

    if (hhccf->disable) {
        if(hhccf->ref_count == 0 ){
            njt_log_error(NJT_LOG_INFO, njt_cycle->log, 0,
                          "active probe cleanup for disable:/%V/%V ", &hhccf->server_type, &hhccf->upstream_name);
            njt_destroy_pool(hhccf->pool);
        } else {
            njt_log_error(NJT_LOG_INFO, njt_cycle->log, 0,
                          "active probe cleanup for disable,cleanup is delayed:/%V/%V ref_count = %d ",
                          &hhccf->server_type, &hhccf->upstream_name, hhccf->ref_count);
            njt_add_timer(&hhccf->hc_timer, 1000);
        }

        return;
    }

    uscf = njt_health_check_find_stream_upstream_by_name(njet_master_cycle, &hhccf->upstream_name);
    if (uscf == NULL) {
        njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0, "no stream upstream data");
        return;
    }

    if (njt_quit || njt_terminate || njt_exiting) {
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0, "stream active probe cleanup for quiting");
        return;
    }

    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0, "Stream health check timer is triggered.");
    
    op = 0;
    if (hhccf->curr_delay != 0 
            && hhccf->curr_delay <= hhccf->curr_frame) {
        
        hhccf->curr_frame = 0;
        op = 1;

    } else if (hhccf->curr_delay == 0) {
        op = 1;
    }

    peers = uscf->peer.data;
    njt_stream_upstream_rr_peers_wlock(peers);
    if(hhccf->first || hhccf->update_id != peers->update_id){
        hhccf->first = 0;
        hhccf->update_id = peers->update_id;
        //clear map
        njt_health_check_clean_peers_map(hhccf);
        
        map_recreate = true;
    }

    if (peers->peer) {
        njt_health_check_loop_stream_upstream_peer(hhccf, peers, 0, op, map_recreate);
    }
    if (peers->next) {
        njt_health_check_loop_stream_upstream_peer(hhccf, peers, 1, op, map_recreate);
    }
    njt_stream_upstream_rr_peers_unlock(peers);

    jitter = 0;
    if (hhccf->jitter) {
        jitter = njt_random() % hhccf->jitter;
        njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                       "delay %u for the health check timer.", jitter);
    }
    njt_add_timer(&hhccf->hc_timer, hhccf->interval + jitter);

    return;    
}




static void njt_health_check_update_http_peer(njt_http_upstream_srv_conf_t *uscf,
                            njt_http_upstream_rr_peer_t *peer,
                            njt_int_t status, njt_uint_t passes, njt_uint_t fails) {
    peer->hc_check_in_process = 0;
    // 如果是持久化，并且是reload后第一次进入不做。  如果peer已经down状态也不做
    if((uscf->mandatory == 1 && uscf->reload == 1 &&  uscf->persistent == 1 &&  peer->set_first_check == 0) || peer->down == 1 ) {
        peer->set_first_check = 1;
        return;
    }

    peer->hc_checks++;
    if (status == NJT_OK || status == NJT_DONE) {

        peer->hc_consecutive_fails = 0;
        peer->hc_last_passed = 1; //zyg

        if (peer->hc_last_passed) {
            peer->hc_consecutive_passes++;
        }

        if (peer->hc_consecutive_passes >= passes) {
            peer->hc_down = (peer->hc_down / 100 * 100);

            if (peer->hc_downstart != 0) {
                peer->hc_upstart = njt_time();
                peer->hc_downtime = peer->hc_downtime + (((njt_uint_t) ((njt_timeofday())->sec) * 1000 +
                                                          (njt_uint_t) ((njt_timeofday())->msec)) - peer->hc_downstart);
            }
            peer->hc_downstart = 0;//(njt_timeofday())->sec;

        }
//        if (uscf->mandatory == 1 && uscf->reload != 1 &&
//            peer->hc_checks == 1) {  //hclcf->plcf->upstream.upstream->reload
//            if (peer->down == 0) {
//                peer->hc_down = (peer->hc_down / 100 * 100);
//            }
//        }


    } else {

        peer->hc_fails++;
        peer->hc_consecutive_passes = 0;
        peer->hc_consecutive_fails++;


        /*Only change the status at the first time when fails number mets*/
        if (peer->hc_consecutive_fails == fails) {
            peer->hc_unhealthy++;

            peer->hc_down = (peer->hc_down / 100 * 100) + 1;

            peer->hc_downstart = (njt_uint_t) ((njt_timeofday())->sec) * 1000 +
                                 (njt_uint_t) ((njt_timeofday())->msec); //(peer->hc_downstart == 0 ?(njt_current_msec):(peer->hc_downstart));
        }
        peer->hc_last_passed = 0;

    }

    return;
}


static void njt_health_check_update_stream_peer(njt_stream_upstream_srv_conf_t *uscf,
                            njt_stream_upstream_rr_peer_t *peer,
                            njt_int_t status, njt_uint_t passes, njt_uint_t fails) {
    peer->hc_check_in_process = 0;

    // 如果是持久化，并且是reload后第一次进入不做。  如果peer已经down状态也不做
    if((uscf->mandatory == 1 && uscf->reload == 1 &&  uscf->persistent == 1 &&  peer->set_first_check == 0 ) || peer->down == 1 ) {
        peer->set_first_check = 1;
        return;
    }
    peer->hc_checks++;

    if (status == NJT_OK || status == NJT_DONE) {
//        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
//            "enable check peer: %V ",
//            &peer->name);

        peer->hc_consecutive_fails = 0;
        peer->hc_last_passed = 1; //zyg

        if (peer->hc_last_passed) {
            peer->hc_consecutive_passes++;
        }

        if (peer->hc_consecutive_passes >= passes) {
            peer->hc_down = (peer->hc_down / 100 * 100);

            if (peer->hc_downstart != 0) {
                peer->hc_upstart = njt_time();
                peer->hc_downtime = peer->hc_downtime + (((njt_uint_t) ((njt_timeofday())->sec) * 1000 +
                                                          (njt_uint_t) ((njt_timeofday())->msec)) - peer->hc_downstart);
            }
            peer->hc_downstart = 0;//(njt_timeofday())->sec;

        }
//        if (uscf->mandatory == 1 && uscf->reload != 1 &&
//            peer->hc_checks == 1) {  //hclcf->plcf->upstream.upstream->reload
//            if (peer->down == 0) {
//                peer->hc_down = (peer->hc_down / 100 * 100);
//            }
//        }


    } else {
        njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0, 
            "disable check peer: %V ",
            &peer->name);

        peer->hc_fails++;
        peer->hc_consecutive_passes = 0;
        peer->hc_consecutive_fails++;

    /*TODO*/
        /*Only change the status at the first time when fails number mets*/
        if (peer->hc_consecutive_fails == fails) {
            peer->hc_unhealthy++;

            peer->hc_down = (peer->hc_down / 100 * 100) + 1;

            peer->hc_downstart = (njt_uint_t) ((njt_timeofday())->sec) * 1000 +
                                 (njt_uint_t) ((njt_timeofday())->msec); //(peer->hc_downstart == 0 ?(njt_current_msec):(peer->hc_downstart));
        }
        peer->hc_last_passed = 0;

    }

    return;
}

static void njt_health_check_http_common_update_bylock(void *peer_info, njt_int_t status){
    njt_http_upstream_srv_conf_t    *uscf;
    njt_http_upstream_rr_peers_t    *peers;
    njt_health_check_peer_t         *hc_peer = peer_info;

    uscf = njt_health_check_find_http_upstream_by_name(njet_master_cycle, &hc_peer->hhccf->upstream_name);
    if (uscf == NULL) {
        njt_log_error(NJT_LOG_ERR, hc_peer->pool->log, 0, "upstream %V isn't found", &hc_peer->hhccf->upstream_name);

        return;
    }
    peers = (njt_http_upstream_rr_peers_t *) uscf->peer.data;

    njt_http_upstream_rr_peers_unlock(peers);
    njt_health_check_http_common_update(hc_peer, status);
    njt_http_upstream_rr_peers_wlock(peers);
}


static void njt_health_check_http_common_update(void *peer_info, njt_int_t status) {
    njt_health_check_peer_t         *hc_peer = peer_info;
    // njt_uint_t                      peer_id;
    njt_http_upstream_srv_conf_t    *uscf;
    njt_http_upstream_rr_peers_t    *peers;
    njt_http_upstream_rr_peer_t     *peer;
    njt_lvlhsh_query_t              lhq;
    njt_uint_t                      rc;
    njt_health_check_http_same_peer_t         *http_lvlhsh_value;
    njt_queue_t                     *q;
    njt_health_check_http_peer_element        *http_ele;
    njt_uint_t                      log_level = NJT_LOG_DEBUG;

    /*Find the right peer and update it's status*/
    // peer_id = hc_peer->peer_id;
    uscf = njt_health_check_find_http_upstream_by_name(njet_master_cycle, &hc_peer->hhccf->upstream_name);
    if (uscf == NULL) {
        njt_log_error(NJT_LOG_ERR, hc_peer->pool->log, 0, "upstream %V isn't found", &hc_peer->hhccf->upstream_name);

        goto end;
    }
    peers = (njt_http_upstream_rr_peers_t *) uscf->peer.data;

    if(status == NJT_ERROR){
        log_level = NJT_LOG_WARN;
    }

    njt_http_upstream_rr_peers_wlock(peers);

    //compare update_id
    if(hc_peer->update_id == peers->update_id){
        //just use saved map for update
        //get all peers of has same servername
        lhq.key = *hc_peer->peer.name;
        lhq.key_hash = njt_murmur_hash2(lhq.key.data, lhq.key.len);
        lhq.proto = &njt_health_check_lvlhsh_proto;

        //find
        rc = njt_lvlhsh_find(&hc_peer->hhccf->servername_to_peers, &lhq);
        if(rc != NJT_OK){
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                "njt_http_health_check_common_update not found  from map");
        
            njt_http_upstream_rr_peers_unlock(peers);
            njt_health_check_http_free_peer_resource(hc_peer);
            return;
        }else{
            http_lvlhsh_value = lhq.value;
            //update self
            njt_log_error(log_level, njt_cycle->log, 0,
                    "hc check peer update status peer:%V peerid:%d status:%d",
                    &http_lvlhsh_value->peer->name, http_lvlhsh_value->peer->id, status);
            njt_health_check_update_http_peer(uscf, http_lvlhsh_value->peer, status, hc_peer->hhccf->passes, hc_peer->hhccf->fails);

            //update others
            q = njt_queue_head(&http_lvlhsh_value->datas);
            for (; q != njt_queue_sentinel(&http_lvlhsh_value->datas); q = njt_queue_next(q)) {
                http_ele = njt_queue_data(q, njt_health_check_http_peer_element, ele_queue);
                njt_log_error(log_level, njt_cycle->log, 0,
                    "hc same peer update status peer:%V peerid:%d status:%d",
                    &http_ele->peer->name, http_ele->peer->id, status);
                njt_health_check_update_http_peer(uscf, http_ele->peer, status, hc_peer->hhccf->passes, hc_peer->hhccf->fails);
            }
        }
    }else{
        //list all peers of has same servername
        for (peer = peers->peer; peer != NULL; peer = peer->next) {
            if(peer->name.len == hc_peer->peer.name->len
                && njt_memcmp(peer->name.data, hc_peer->peer.name->data, peer->name.len) == 0){
                njt_health_check_update_http_peer(uscf, peer, status, hc_peer->hhccf->passes, hc_peer->hhccf->fails);
                njt_log_error(log_level, njt_cycle->log, 0,
                    "hc update peer update status peer:%V peerid:%d status:%d",
                    &peer->name, peer->id, status);
            }
        }
        if (peer == NULL && peers->next) {
            for (peer = peers->next->peer; peer != NULL; peer = peer->next) {
                if(peer->name.len == hc_peer->peer.name->len
                    && njt_memcmp(peer->name.data, hc_peer->peer.name->data, peer->name.len) == 0){
                    njt_health_check_update_http_peer(uscf, peer, status, hc_peer->hhccf->passes, hc_peer->hhccf->fails);
                    njt_log_error(log_level, njt_cycle->log, 0,
                        "hc update peer update status peer:%V peerid:%d status:%d",
                        &peer->name, peer->id, status);
                }
            }
        }
    }

    njt_http_upstream_rr_peers_unlock(peers);

    end:
    njt_health_check_http_free_peer_resource(hc_peer);
    return;
}


static void njt_health_check_stream_common_update_bylock(void *peer_info,
            njt_int_t status) {
    njt_stream_upstream_srv_conf_t  *uscf;
    njt_stream_upstream_rr_peers_t  *peers;
    njt_health_check_peer_t         *hc_peer = peer_info;

    uscf = njt_health_check_find_stream_upstream_by_name(njet_master_cycle, &hc_peer->hhccf->upstream_name);
    if (uscf == NULL) {
        njt_log_error(NJT_LOG_ERR, hc_peer->pool->log, 0, "upstream %V isn't found", &hc_peer->hhccf->upstream_name);
        return ;
    }

    peers = (njt_stream_upstream_rr_peers_t *) uscf->peer.data;


    njt_stream_upstream_rr_peers_unlock(peers);
    njt_health_check_stream_common_update(hc_peer, status);
    njt_stream_upstream_rr_peers_wlock(peers);
}


static void njt_health_check_stream_common_update(void *peer_info, njt_int_t status) {

    njt_health_check_peer_t         *hc_peer = peer_info;
    njt_stream_upstream_srv_conf_t *uscf;
    njt_stream_upstream_rr_peers_t *peers;
    njt_stream_upstream_rr_peer_t  *peer;
    njt_lvlhsh_query_t              lhq;
    njt_uint_t                      rc;
    njt_health_check_stream_same_peer_t       *stream_lvlhsh_value;
    njt_queue_t                     *q;
    njt_health_check_stream_peer_element      *stream_ele;
    njt_uint_t                      log_level = NJT_LOG_DEBUG;


    /*Find the right peer and update it's status*/
    // peer_id = hc_peer->peer_id;
    uscf = njt_health_check_find_stream_upstream_by_name(njet_master_cycle, &hc_peer->hhccf->upstream_name);
    if (uscf == NULL) {
        njt_log_error(NJT_LOG_ERR, hc_peer->pool->log, 0, "upstream %V isn't found", &hc_peer->hhccf->upstream_name);
        goto end;
    }

    if(status == NJT_ERROR){
        log_level = NJT_LOG_WARN;
    }

    peers = (njt_stream_upstream_rr_peers_t *) uscf->peer.data;

    njt_stream_upstream_rr_peers_wlock(peers);
    //compare update_id
    if(hc_peer->hhccf->update_id == peers->update_id){
        //just use saved map for update
        //get all peers of has same servername
        lhq.key = *hc_peer->peer.name;
        lhq.key_hash = njt_murmur_hash2(lhq.key.data, lhq.key.len);
        lhq.proto = &njt_health_check_lvlhsh_proto;

        //find
        rc = njt_lvlhsh_find(&hc_peer->hhccf->servername_to_peers, &lhq);
        if(rc != NJT_OK){
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "njt_stream_health_check_common_update not found  from map");
                njt_stream_upstream_rr_peers_unlock(peers);
                return;
        }else{
            stream_lvlhsh_value = lhq.value;
            //update self
            njt_log_error(log_level, njt_cycle->log, 0,
                "hc stream check peer update status peer:%V peerid:%d status:%d",
                &stream_lvlhsh_value->peer->name, stream_lvlhsh_value->peer->id, status);
            njt_health_check_update_stream_peer(uscf, stream_lvlhsh_value->peer, status, hc_peer->hhccf->passes, hc_peer->hhccf->fails);

            //update others
            q = njt_queue_head(&stream_lvlhsh_value->datas);
            for (; q != njt_queue_sentinel(&stream_lvlhsh_value->datas); q = njt_queue_next(q)) {
                stream_ele = njt_queue_data(q, njt_health_check_stream_peer_element, ele_queue);
                njt_log_error(log_level, njt_cycle->log, 0,
                    "hc stream same peer update status peer:%V peerid:%d status:%d",
                    &stream_ele->peer->name, stream_ele->peer->id, status);
                njt_health_check_update_stream_peer(uscf, stream_ele->peer, status, hc_peer->hhccf->passes, hc_peer->hhccf->fails);
            }
        }
    }else{
        //list all peers of has same servername
        for (peer = peers->peer; peer != NULL; peer = peer->next) {
            if(peer->name.len == hc_peer->peer.name->len
                && njt_memcmp(peer->name.data, hc_peer->peer.name->data, peer->name.len) == 0){
                njt_health_check_update_stream_peer(uscf, peer, status, hc_peer->hhccf->passes, hc_peer->hhccf->fails);
                njt_log_error(log_level, njt_cycle->log, 0,
                    "hc stream update peer update status peer:%V peerid:%d status:%d", &peer->server, peer->id, status);
            }
        }
        if (peer == NULL && peers->next) {
            for (peer = peers->next->peer; peer != NULL; peer = peer->next) {
                if(peer->name.len == hc_peer->peer.name->len
                    && njt_memcmp(peer->name.data, hc_peer->peer.name->data, peer->name.len) == 0){
                    njt_health_check_update_stream_peer(uscf, peer, status, hc_peer->hhccf->passes, hc_peer->hhccf->fails);
                    njt_log_error(log_level, njt_cycle->log, 0,
                        "hc stream update peer update status peer:%V peerid:%d status:%d", &peer->server, peer->id, status);
                }
            }
        }
    }

    njt_stream_upstream_rr_peers_unlock(peers);

end:
    njt_health_check_stream_free_peer_resource(hc_peer);

    return;
}

static njt_int_t njt_health_check_api_data2_ssl_cf(njt_health_check_api_data_t *api_data, njt_health_check_conf_t *hhccf) {
    njt_str_t      tmp_str;

    if(!api_data->hc_data->is_ssl_set){
        return HC_SUCCESS;
    }

    if(api_data->hc_data->ssl->enable){
        hhccf->ssl.ssl_enable = 1;
    }

    if(api_data->hc_data->ssl->ntls){
        hhccf->ssl.ssl_enable = 1;
        hhccf->ssl.ntls_enable = 1;
    }

    hhccf->ssl.ssl_session_reuse = api_data->hc_data->ssl->session_reuse ? 1 : 0;

    if (api_data->hc_data->ssl->ciphers.len <= 0) {
        njt_str_set(&hhccf->ssl.ssl_ciphers, "DEFAULT");
    } else {
        njt_str_copy_pool(hhccf->pool, hhccf->ssl.ssl_ciphers, api_data->hc_data->ssl->ciphers, return HC_SERVER_ERROR);
    }

    if(api_data->hc_data->ssl->protocols.len > 0){
        njt_str_copy_pool(hhccf->pool, hhccf->ssl.ssl_protocol_str, api_data->hc_data->ssl->protocols,
                    return HC_SERVER_ERROR);
        
        if(NJT_OK != njt_health_check_json_parse_ssl_protocols(api_data->hc_data->ssl->protocols, &hhccf->ssl.ssl_protocols)){
            return HC_BODY_ERROR;
            // hhccf->ssl.ssl_protocols = (NJT_CONF_BITMASK_SET | NJT_SSL_TLSv1 | NJT_SSL_TLSv1_1 | NJT_SSL_TLSv1_2);
        }
    }else{
        hhccf->ssl.ssl_protocols = (NJT_CONF_BITMASK_SET| NJT_SSL_TLSv1 | NJT_SSL_TLSv1_1 | NJT_SSL_TLSv1_2 | NJT_SSL_TLSv1_3);
    }

    if(api_data->hc_data->ssl->name.len > 0){
        njt_str_copy_pool(hhccf->pool, hhccf->ssl.ssl_name, api_data->hc_data->ssl->name, return HC_SERVER_ERROR);
    }

    hhccf->ssl.ssl_server_name = api_data->hc_data->ssl->serverName ? 1 : 0;
    hhccf->ssl.ssl_verify = api_data->hc_data->ssl->verify ? 1 : 0;
    hhccf->ssl.ssl_verify_depth = api_data->hc_data->ssl->verifyDepth <= 0 ? 1 : api_data->hc_data->ssl->verifyDepth;
    if(api_data->hc_data->ssl->trustedCertificate.len > 0){
        tmp_str = api_data->hc_data->ssl->trustedCertificate;
        njt_str_copy_pool(hhccf->pool, hhccf->ssl.ssl_trusted_certificate, tmp_str, return HC_SERVER_ERROR);
    }

    if(api_data->hc_data->ssl->crl.len > 0){
        tmp_str = api_data->hc_data->ssl->crl;
        njt_str_copy_pool(hhccf->pool, hhccf->ssl.ssl_crl, tmp_str, return HC_SERVER_ERROR);
    }

    if(api_data->hc_data->ssl->certificate.len > 0){
        tmp_str = api_data->hc_data->ssl->certificate;
        njt_str_copy_pool(hhccf->pool, hhccf->ssl.ssl_certificate, tmp_str, return HC_SERVER_ERROR);
    }

    if(api_data->hc_data->ssl->certificateKey.len > 0){
        tmp_str = api_data->hc_data->ssl->certificateKey;
        njt_str_copy_pool(hhccf->pool, hhccf->ssl.ssl_certificate_key, tmp_str, return HC_SERVER_ERROR);
    }

    if(api_data->hc_data->ssl->encCertificate.len > 0){
        tmp_str = api_data->hc_data->ssl->encCertificate;
        njt_str_copy_pool(hhccf->pool, hhccf->ssl.ssl_enc_certificate, tmp_str, return HC_SERVER_ERROR);
    }

    if(api_data->hc_data->ssl->encCertificate.len > 0){
        tmp_str = api_data->hc_data->ssl->encCertificate;
        njt_str_copy_pool(hhccf->pool, hhccf->ssl.ssl_enc_certificate, tmp_str, return HC_SERVER_ERROR);
    }

    if(api_data->hc_data->ssl->encCertificateKey.len > 0){
        tmp_str = api_data->hc_data->ssl->encCertificateKey;
        njt_str_copy_pool(hhccf->pool, hhccf->ssl.ssl_enc_certificate_key, tmp_str, return HC_SERVER_ERROR);
    }
//    njt_array_t *ssl_passwords;
//    njt_array_t *ssl_conf_commands;

    hhccf->ssl.ssl = njt_pcalloc(hhccf->pool, sizeof(njt_ssl_t));
    if (hhccf->ssl.ssl == NULL) {
        return HC_SERVER_ERROR;
    }
    hhccf->ssl.ssl->log = hhccf->log;
    if (njt_health_check_set_ssl(hhccf, &hhccf->ssl) != NJT_OK) {
        return HC_BODY_ERROR;
    } else {
        return HC_SUCCESS;
    }
}


static njt_int_t njt_health_check_api_data2_common_cf(njt_health_check_api_data_t *api_data, njt_health_check_conf_t *hhccf) {
    njt_int_t rc;

    if(api_data->hc_data->is_interval_set && api_data->hc_data->interval.len > 0){
        njt_int_t i_interval;
        i_interval = njt_parse_time(&api_data->hc_data->interval, 0);
        if(NJT_ERROR == i_interval  || i_interval <= 0){
            return HC_BODY_ERROR;
        }else{
            hhccf->interval = i_interval;
        }
    }else{
        hhccf->interval = NJT_HTTP_HC_INTERVAL;
    }

    if(api_data->hc_data->is_visit_interval_set && api_data->hc_data->visit_interval.len > 0){
        njt_int_t i_visit_interval;
        i_visit_interval = njt_parse_time(&api_data->hc_data->visit_interval, 0);
        if(NJT_ERROR == i_visit_interval  || i_visit_interval < 0){
            return HC_BODY_ERROR;
        }else{
            hhccf->visit_interval = i_visit_interval;
        }
    }else{
        hhccf->visit_interval = 0;
    }    

    if(api_data->hc_data->is_jitter_set && api_data->hc_data->jitter.len > 0){
        njt_int_t i_jitter;
        i_jitter = njt_parse_time(&api_data->hc_data->jitter, 0);
        if(NJT_ERROR == i_jitter  || i_jitter < 0){
            return HC_BODY_ERROR;
        }else{
            hhccf->jitter = i_jitter;
        }
    }else{
        hhccf->jitter = 0;
    }

    if(api_data->hc_data->is_timeout_set && api_data->hc_data->timeout.len > 0){
        njt_int_t i_timeout;
        i_timeout = njt_parse_time(&api_data->hc_data->timeout, 0);
        if(NJT_ERROR == i_timeout || i_timeout <= 0){
            return HC_BODY_ERROR;
        }else{
            hhccf->timeout = i_timeout;
        }
    }else{
        hhccf->timeout = NJT_HTTP_HC_CONNECT_TIMEOUT;
    }    

    // hhccf->interval = api_data->hc_data->interval <= 0 ? NJT_HTTP_HC_INTERVAL : api_data->hc_data->interval;
    // hhccf->jitter = api_data->hc_data->jitter <= 0 ? 0 : api_data->hc_data->jitter;
    // hhccf->timeout = api_data->hc_data->timeout <= 0 ? NJT_HTTP_HC_CONNECT_TIMEOUT : api_data->hc_data->timeout;
    hhccf->port = api_data->hc_data->port;
    hhccf->passes = api_data->hc_data->passes == 0 ? 1 : api_data->hc_data->passes;
    hhccf->fails = api_data->hc_data->fails == 0 ? 1 : api_data->hc_data->fails;

    rc = njt_health_check_api_data2_ssl_cf(api_data, hhccf);
    return rc;
}


static void njt_health_check_http_free_peer_resource(njt_health_check_peer_t *hc_peer) {
    njt_health_check_conf_t     *hhccf = hc_peer->hhccf;
    njt_uint_t                  local_peer_id = hc_peer->peer_id;
    

    if(hhccf->ref_count>0){
        hhccf->ref_count--;
    }

    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                "====http free peer check, peerid:%d ref_count=%d", local_peer_id, hhccf->ref_count); 

    if (hc_peer->pool) {
        njt_destroy_pool(hc_peer->pool);
    }

   return;
}


static void njt_health_check_stream_free_peer_resource(njt_health_check_peer_t *hc_peer) {
    njt_health_check_conf_t  *hhccf = hc_peer->hhccf;

    if(hhccf->ref_count > 0){
        hhccf->ref_count--;
    }

    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
        "====stream free peer check, peerid:%d ref_count=%d", hc_peer->peer_id, hhccf->ref_count);

    if (hc_peer->pool) {
        njt_destroy_pool(hc_peer->pool);
    }
    return;
}


static void
njt_health_check_loop_http_upstream_peer(njt_health_check_conf_t *hhccf, njt_http_upstream_rr_peers_t *peers,
                          njt_flag_t backup, njt_flag_t op, bool map_recreate) {
    njt_int_t                       rc;
    njt_health_check_peer_t    *hc_peer;
    njt_http_upstream_rr_peer_t     *peer;
    njt_http_upstream_rr_peers_t    *hu_peers;
    njt_pool_t                      *pool;
    njt_msec_t                      now_time;

    hu_peers = peers;
    if (backup == 1) {
        hu_peers = peers->next;
    }
    if (hu_peers == NULL) {
        return;
    }
    peer = hu_peers->peer;
    for (; peer != NULL; peer = peer->next) {

        if (peer->down == 1) //zyg
        {
            continue;
        }
        if ((peer->hc_down == 2) || (op == 1)) {  //checking
            if (peer->hc_check_in_process) {
                njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                               "peer's health check is in process.");
                continue;
            }

            if(hhccf->visit_interval > 0){
                now_time = ((njt_timeofday())->sec)*1000 + (njt_uint_t)((njt_timeofday())->msec);
                if((now_time - peer->selected_time) <= hhccf->visit_interval){
                    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                               "this peer is visited last, and intervel is less than visit_interval, not check");
                    continue;
                }
            }

            if(map_recreate){
                rc = njt_health_check_http_peer_add_map(hhccf, peer);
                switch (rc)
                {
                case NJT_ERROR:
                    njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                            " add http peer:%V peerid:%d to map error", &peer->name, peer->id);
                    continue;
                case NJT_DECLINED:
                    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                            " same http peer:%V peerid:%d exist", &peer->name, peer->id);
                    continue;
                case NJT_OK:
                    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                            " add http peer:%V peerid:%d to map", &peer->name, peer->id);
                    break;
                }
            }else{
                //if not check peer, just continue
                if(!njt_health_check_http_check_peer(hhccf, peer)){
                    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                            " http, not check peer:%V peerid:%d just continue", &peer->name, peer->id);
                    continue;
                }
            }

            njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                    " http check peer:%V peerid:%d", &peer->name, peer->id);

            peer->hc_check_in_process = 1;
            pool = njt_create_pool(njt_pagesize, njt_cycle->log);
            if (pool == NULL) {
                /*log the malloc failure*/
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                                "create pool failure for health check.");
                continue;
            }
            hc_peer = njt_pcalloc(pool, sizeof(njt_health_check_peer_t));
            if (hc_peer == NULL) {
                /*log the malloc failure*/
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                                "memory allocate failure for health check.");
                njt_destroy_pool(pool);
                continue;
            }
            hc_peer->pool = pool;

            hc_peer->peer_id = peer->id;
            hc_peer->hhccf = hhccf;
            hc_peer->update_id = hhccf->update_id;

            hc_peer->peer.sockaddr = njt_pcalloc(pool, sizeof(struct sockaddr));
            if (hc_peer->peer.sockaddr == NULL) {
                /*log the malloc failure*/
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                                "memory allocate failure for health check.");
                njt_destroy_pool(pool);
                continue;
            }
            njt_memcpy(hc_peer->peer.sockaddr, peer->sockaddr, sizeof(struct sockaddr));

            /*customized the peer's port*/
            if (hhccf->port) {
                njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                            "health check use config port:%d", hhccf->port);
                njt_inet_set_port(hc_peer->peer.sockaddr, hhccf->port);
            }

            hc_peer->peer.socklen = peer->socklen;
            hc_peer->peer.name = njt_pcalloc(pool, sizeof(njt_str_t));
            if (hc_peer->peer.name == NULL) {
                /*log the malloc failure*/
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                                "memory allocate peer name failure for health check.");
                njt_destroy_pool(pool);
                continue;
            }
            hc_peer->peer.name->len = peer->name.len;
            hc_peer->peer.name->data = njt_pcalloc(pool, peer->name.len);
            if (hc_peer->peer.name->data == NULL) {
                /*log the malloc failure*/
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                                "memory allocate peer name data failure for health check.");
                njt_destroy_pool(pool);
                continue;
            }
            njt_memcpy(hc_peer->peer.name->data, peer->name.data, peer->name.len);

            hc_peer->server.len = peer->server.len;
            hc_peer->server.data = njt_pcalloc(pool, peer->server.len);
            njt_memcpy(hc_peer->server.data, peer->server.data, peer->server.len);
            hc_peer->peer.get = njt_event_get_peer;
            hc_peer->peer.log = njt_cycle->log;
            hc_peer->peer.log_error = NJT_ERROR_ERR;
            hc_peer->update_handler = njt_health_check_http_common_update;

            //cal reg start check
            hhccf->ref_count++;
            hhccf->reg->start_check(hc_peer, njt_health_check_http_common_update_bylock);
        }
    }
}

void njt_health_check_loop_stream_upstream_peer(njt_health_check_conf_t *hhccf, njt_stream_upstream_rr_peers_t *peers,
                                njt_flag_t backup, njt_flag_t op, bool map_recreate) {
    njt_int_t                       rc;
    njt_health_check_peer_t *hc_peer;
    njt_stream_upstream_rr_peer_t  *peer;
    njt_stream_upstream_rr_peers_t *hu_peers;
    njt_pool_t                     *pool;
    njt_msec_t                      now_time;

    hu_peers = peers;
    if (backup == 1) {
        hu_peers = peers->next;
    }

    if (hu_peers == NULL) {
        return;
    }

    peer = hu_peers->peer;
    for (; peer != NULL; peer = peer->next) {
        if (peer->down == 1) 
        {
            continue;
        }
        if ((peer->hc_down == 2) || (op == 1)) {  
            if (peer->hc_check_in_process) {
                njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                               "peer's health check is in process.");
                continue;
            }

            if(hhccf->visit_interval > 0){
                now_time = ((njt_timeofday())->sec)*1000 + (njt_uint_t)((njt_timeofday())->msec);
                if((now_time - peer->selected_time) <= hhccf->visit_interval){
                    njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                               "this peer is visited last, and intervel is less than visit_interval, not check");
                    continue;
                }
            }

            if(map_recreate){
                rc = njt_health_check_stream_peer_add_map(hhccf, peer);
                switch (rc)
                {
                case NJT_ERROR:
                    njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                            " add stream peer:%V peerid:%d to map error", &peer->name, peer->id);
                    continue;
                case NJT_DECLINED:
                    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                            " same stream peer:%V peerid:%d exist", &peer->name, peer->id);
                    continue;
                case NJT_OK:
                    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                            " add stream peer:%V peerid:%d to map", &peer->name, peer->id);
                    break;
                }
            }else{
                //if not check peer, just continue
                if(!njt_health_check_stream_check_peer(hhccf, peer)){
                    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                            " not stream check peer:%V peerid:%d just continue", &peer->name, peer->id);
                    continue;
                }
            }

            njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                    " stream check peer:%V peerid:%d", &peer->name, peer->id);

            peer->hc_check_in_process = 1;
            pool = njt_create_pool(njt_pagesize, njt_cycle->log);
            if (pool == NULL) {
                /*log the malloc failure*/
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                              "create pool failure for health check.");
                continue;
            }
                
            hc_peer = njt_pcalloc(pool, sizeof(njt_health_check_peer_t));
            if (hc_peer == NULL) {
                /*log the malloc failure*/
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                              "memory allocate failure for health check.");
                njt_destroy_pool(pool);
                continue;
            }

            hc_peer->pool = pool;
            hc_peer->peer_id = peer->id;
            hc_peer->hhccf = hhccf;
            hc_peer->update_id = hhccf->update_id;

            hc_peer->peer.sockaddr = njt_pcalloc(pool, sizeof(struct sockaddr));
            if (hc_peer->peer.sockaddr == NULL) {
                 /*log the malloc failure*/
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                                  "memory allocate failure for health check.");
                njt_destroy_pool(pool);
                continue;
            }
                
            njt_memcpy(hc_peer->peer.sockaddr, peer->sockaddr, sizeof(struct sockaddr));

             /*customized the peer's port*/
            if (hhccf->port) {
                njt_inet_set_port(hc_peer->peer.sockaddr, hhccf->port);
            }

            hc_peer->peer.socklen = peer->socklen;
            hc_peer->peer.name = njt_pcalloc(pool, sizeof(njt_str_t));
            if (hc_peer->peer.name == NULL) {
                /*log the malloc failure*/
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                                "memory allocate peer name failure for health check.");
                njt_destroy_pool(pool);
                continue;
            }
            hc_peer->peer.name->len = peer->name.len;
            hc_peer->peer.name->data = njt_pcalloc(pool, peer->name.len);
            if (hc_peer->peer.name->data == NULL) {
                /*log the malloc failure*/
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                                "memory allocate peer name data failure for health check.");
                njt_destroy_pool(pool);
                continue;
            }
            njt_memcpy(hc_peer->peer.name->data, peer->name.data, peer->name.len);


            hc_peer->server.len = peer->server.len;
            hc_peer->server.data = njt_pcalloc(pool, peer->server.len);
            if (hc_peer->server.data == NULL) {
                /*log the malloc failure*/
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                                "memory allocate peer server data failure for health check.");
                njt_destroy_pool(pool);
                continue;
            }
            njt_memcpy(hc_peer->server.data, peer->server.data, peer->server.len);
            hc_peer->peer.log = njt_cycle->log;
            hc_peer->peer.log_error = NJT_ERROR_ERR;
            hc_peer->peer.type = SOCK_STREAM;
            hc_peer->peer.get = njt_event_get_peer;
            hc_peer->update_handler = njt_health_check_stream_common_update;

            //cal reg start check
            hhccf->reg->start_check(hc_peer, njt_health_check_stream_common_update_bylock);
        }
    }
}



static njt_int_t njt_health_check_helper_module_init(njt_cycle_t *cycle) {
    njt_health_check_main_conf_t *hmcf;

    hmcf = njt_pcalloc(cycle->pool, sizeof(njt_health_check_main_conf_t));
    if (hmcf == NULL) {
        njt_log_error(NJT_LOG_EMERG, cycle->log, 0, "health check helper alloc main conf error ");
        return NJT_ERROR;
    }

    njt_queue_init(&hmcf->hc_queue);
    hmcf->first = 1;
    cycle->conf_ctx[njt_helper_health_check_module.index] = (void *) hmcf;

    return NJT_OK;
}

static void njt_health_check_recovery_conf_info(njt_pool_t *pool, njt_str_t *msg, njt_str_t *name, njt_str_t *type) {
    njt_int_t                   rc;
    njt_health_check_api_data_t    *api_data = NULL;
    js2c_parse_error_t          err_info;

    api_data = njt_pcalloc(pool, sizeof(njt_health_check_api_data_t));
    if (api_data == NULL) {
        njt_log_error(NJT_LOG_EMERG, pool->log, 0, "could not alloc buffer in function %s", __func__);
        return;
    }

    api_data->hc_data = json_parse_health_check(pool, msg, &err_info);
    if (api_data->hc_data == NULL)
    {
        njt_log_error(NJT_LOG_ERR, pool->log, 0, 
                "json_parse_health_check err: %V",  &err_info.err_str);

        rc = NJT_ERROR;
        return;
    }

    njt_str_copy_pool(pool, api_data->upstream_name, (*name), return);
    njt_str_copy_pool(pool, api_data->hc_type, (*type), return);
    rc = njt_health_check_api_add_conf(pool->log, api_data, msg, 0);
    if (rc != HC_SUCCESS) {
        njt_log_error(NJT_LOG_EMERG, pool->log, 0, "recovery conf info info error");
    }

}

static njt_int_t njt_health_check_recovery_conf_of_upstream(njt_str_t *upstream, njt_flag_t is_http_upstream){
    njt_health_check_main_conf_t  *hmcf;
    njt_str_t               msg;
    njt_str_t               tkey1, tkey2;
    njt_pool_t              *pool;
    njt_uint_t              i;
    health_checks_t         *hc_datas;    
    health_checks_item_t    *item;
    njt_int_t               rc = NJT_OK;
    js2c_parse_error_t      err_info;
    njt_str_t               hc_type, hc_upstream;
    njt_http_upstream_srv_conf_t *uscf;
    njt_stream_upstream_srv_conf_t *suscf;
    njt_str_t               key;
    njt_str_t               key_pre;
    njt_str_t               key_separator;

    if(is_http_upstream){
        // key = njt_string(HTTP_HEALTH_CHECK_CONFS);
        njt_str_set(&key, HTTP_HEALTH_CHECK_CONFS);
        njt_str_set(&key_pre, HTTP_HEALTH_CHECK_CONF_INFO);
        njt_str_set(&key_separator, HTTP_HEALTH_CHECK_SEPARATOR);
    }else{
        njt_str_set(&key, STREAM_HEALTH_CHECK_CONFS);
        njt_str_set(&key_pre, STREAM_HEALTH_CHECK_CONF_INFO);
        njt_str_set(&key_separator, STREAM_HEALTH_CHECK_SEPARATOR);
    }

    if(upstream == NULL){
        return NJT_ERROR;
    }

    hmcf = (void *) njt_get_conf(njt_cycle->conf_ctx, njt_helper_health_check_module);
    if (hmcf == NULL) {
        return NJT_ERROR;
    }

    njt_memzero(&msg, sizeof(njt_str_t));
    njt_dyn_kv_get(&key, &msg);
    if (msg.len <= 2) {
        return NJT_ERROR;
    }

    pool = njt_create_pool(NJT_MIN_POOL_SIZE, njt_cycle->log);
    if (pool == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "create pool error in function %s", __func__);
        rc = NJT_ERROR;

        goto end;
    }

    njt_log_error(NJT_LOG_DEBUG, pool->log, 0, 
                "http json_parse_health_checks msg: %V",  &msg);

    hc_datas = json_parse_health_checks(pool, &msg, &err_info);
    if (hc_datas == NULL)
    {
        njt_log_error(NJT_LOG_ERR, pool->log, 0, 
                "json_parse_health_checks err: %V",  &err_info.err_str);

        rc = NJT_ERROR;
        goto end;
    }

    rc = NJT_AGAIN;
    for (i = 0; i < hc_datas->nelts; ++i) {
        item = get_health_checks_item(hc_datas, i);
        hc_upstream = item->upstream_name;

        if(upstream->len != hc_upstream.len
            || njt_strncmp(upstream->data, hc_upstream.data, upstream->len) != 0){
            continue;
        }

        hc_type = item->hc_type;

        njt_str_concat(pool, tkey1, key_pre, hc_type, goto end);
        njt_str_concat(pool, tkey2, tkey1, key_separator, goto end);
        njt_str_concat(pool, tkey1, tkey2, hc_upstream, goto end);
        njt_memzero(&msg, sizeof(njt_str_t));
        njt_dyn_kv_get(&tkey1, &msg);
        if (msg.len <= 0) {
            njt_log_error(NJT_LOG_ERR, pool->log, 0, 
                        "upstream:%V hc msg len is less than 0",  &msg);

            rc = NJT_AGAIN;
            break;
        }

        if(is_http_upstream){
            //this time must be exist
            uscf = njt_health_check_find_http_upstream_by_name(njet_master_cycle, &item->upstream_name);
            if (uscf == NULL) {
                njt_log_error(NJT_LOG_ERR, pool->log, 0, 
                    "hc upstream:%V is not exist",  &item->upstream_name);

                rc = NJT_ERROR;
                break;
            }
        }else{            
            //this time must be exist
            suscf = njt_health_check_find_stream_upstream_by_name(njet_master_cycle, &item->upstream_name);
            if (suscf == NULL) {
                njt_log_error(NJT_LOG_ERR, pool->log, 0, 
                    "hc upstream:%V is not exist",  &item->upstream_name);

                rc = NJT_ERROR;
                break;
            }
        }

        njt_health_check_recovery_conf_info(pool, &msg, &item->upstream_name, &item->hc_type);
        rc = NJT_OK;

        break;
    }

    end:
    if(pool != NULL){
        njt_destroy_pool(pool);
    }

    return rc;
}




static void njt_health_check_recovery_confs(){
    njt_health_check_main_conf_t    *hmcf;
    njt_str_t                       msg;
    njt_str_t                       tkey1, tkey2;
    njt_pool_t                      *pool;
    njt_uint_t                      i;
    health_checks_t                 *hc_datas;    
    health_checks_item_t            *item;
    js2c_parse_error_t              err_info;
    njt_str_t                       hc_type, hc_upstream;
    njt_http_upstream_srv_conf_t    *uscf;
    njt_stream_upstream_srv_conf_t  *suscf;

    njt_str_t key_pre = njt_string(HTTP_HEALTH_CHECK_CONF_INFO);
    njt_str_t key_separator = njt_string(HTTP_HEALTH_CHECK_SEPARATOR);
    njt_str_t key = njt_string(HTTP_HEALTH_CHECK_CONFS);

    hmcf = (void *) njt_get_conf(njt_cycle->conf_ctx, njt_helper_health_check_module);
    if (hmcf == NULL) {
        return;
    }

    njt_memzero(&msg, sizeof(njt_str_t));
    njt_dyn_kv_get(&key, &msg);
    if (msg.len <= 2) {
        return;
    }
    pool = njt_create_pool(NJT_MIN_POOL_SIZE, njt_cycle->log);
    if (pool == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "create pool error in function %s", __func__);
        goto end;
    }

    njt_log_error(NJT_LOG_INFO, pool->log, 0, 
                "http json_parse_health_checks msg: %V",  &msg);
    hc_datas = json_parse_health_checks(pool, &msg, &err_info);
    if (hc_datas == NULL)
    {
        njt_log_error(NJT_LOG_ERR, pool->log, 0, 
                "json_parse_health_checks err: %V",  &err_info.err_str);

        goto end;
    }

    for (i = 0; i < hc_datas->nelts; ++i) {
        item = get_health_checks_item(hc_datas, i);
        hc_type = item->hc_type;
        hc_upstream = item->upstream_name;
        njt_str_concat(pool, tkey1, key_pre, hc_type, goto end);
        njt_str_concat(pool, tkey2, tkey1, key_separator, goto end);
        njt_str_concat(pool, tkey1, tkey2, hc_upstream, goto end);
        njt_memzero(&msg, sizeof(njt_str_t));
        njt_dyn_kv_get(&tkey1, &msg);
        if (msg.len <= 0) {
            continue;
        }

        if(item->hc_type.len >= 2){
            //upstream maybe dyn, add hc later
            if(item->hc_type.data[0] == 'h'){
                uscf = njt_health_check_find_http_upstream_by_name(njet_master_cycle, &item->upstream_name);
                if (uscf == NULL) {
                    njt_log_error(NJT_LOG_DEBUG, pool->log, 0, 
                        "hc http upstream:%V maybe dyn, recover later",  &item->upstream_name);
                    continue;
                }
            }else if(item->hc_type.data[0] == 's'){
                suscf = njt_health_check_find_stream_upstream_by_name(njet_master_cycle, &item->upstream_name);
                if (suscf == NULL) {
                    njt_log_error(NJT_LOG_DEBUG, pool->log, 0, 
                        "hc stream upstream:%V maybe dyn, recover later",  &item->upstream_name);
                    continue;
                }
            }

            njt_health_check_recovery_conf_info(pool, &msg, &item->upstream_name, &item->hc_type);
        }
    }

    end:
    if(pool != NULL){
        njt_destroy_pool(pool);
    }
}


/* by zhaokang */
static void njt_stream_health_check_recovery_confs(){
    njt_health_check_main_conf_t            *hmcf;
    njt_str_t                               msg;
    njt_str_t                               tkey1, tkey2;
    njt_pool_t                              *pool;
    njt_uint_t                              i;
    health_checks_t                         *hc_datas;    
    health_checks_item_t                    *item;
    js2c_parse_error_t                      err_info;
    njt_str_t                               hc_type, hc_upstream;
    njt_stream_upstream_srv_conf_t          *suscf;
    njt_http_upstream_srv_conf_t            *uscf;


    njt_str_t key_pre         = njt_string(STREAM_HEALTH_CHECK_CONF_INFO);
    njt_str_t key_separator = njt_string(STREAM_HEALTH_CHECK_SEPARATOR);
    njt_str_t key             = njt_string(STREAM_HEALTH_CHECK_CONFS);

    hmcf = (void *) njt_get_conf(njt_cycle->conf_ctx, njt_helper_health_check_module);
    if (hmcf == NULL) {
        return;
    }

    njt_memzero(&msg, sizeof(njt_str_t));

    njt_dyn_kv_get(&key, &msg);
    if (msg.len <= 2) {
        return;
    }

    pool = njt_create_pool(NJT_MIN_POOL_SIZE, njt_cycle->log);
    if (pool == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "create pool error in function %s", __func__);
        goto end;
    }
    njt_log_error(NJT_LOG_INFO, pool->log, 0, 
                "stream json_parse_health_checks msg: %V",  &msg);
    hc_datas = json_parse_health_checks(pool, &msg, &err_info);
    if (hc_datas == NULL)
    {
        njt_log_error(NJT_LOG_ERR, pool->log, 0, 
                "json_parse_health_checks err: %V",  &err_info.err_str);

        goto end;
    }

    for (i = 0; i < hc_datas->nelts; ++i) {
        item = get_health_checks_item(hc_datas, i);
        hc_type = item->hc_type;
        hc_upstream = item->upstream_name;
        njt_str_concat(pool, tkey1, key_pre, hc_type,         goto end);
        njt_str_concat(pool, tkey2, tkey1,      key_separator,         goto end);
        njt_str_concat(pool, tkey1, tkey2,   hc_upstream, goto end);

        njt_memzero(&msg, sizeof(njt_str_t));

        njt_dyn_kv_get(&tkey1, &msg);
        if (msg.len <= 0) {
            continue;
        }

        if(item->hc_type.len >= 2){
            //upstream maybe dyn, add hc later
            if(item->hc_type.data[0] == 'h'){
                uscf = njt_health_check_find_http_upstream_by_name(njet_master_cycle, &item->upstream_name);
                if (uscf == NULL) {
                    njt_log_error(NJT_LOG_DEBUG, pool->log, 0, 
                        "hc http upstream:%V maybe dyn, recover later",  &item->upstream_name);
                    continue;
                }
            }else if(item->hc_type.data[0] == 's'){
                suscf = njt_health_check_find_stream_upstream_by_name(njet_master_cycle, &item->upstream_name);
                if (suscf == NULL) {
                    njt_log_error(NJT_LOG_DEBUG, pool->log, 0, 
                        "hc stream upstream:%V maybe dyn, recover later",  &item->upstream_name);
                    continue;
                }
            }

            njt_health_check_recovery_conf_info(pool, &msg, &item->upstream_name, &item->hc_type);
        }
    }

end:
    if(pool != NULL){
        njt_destroy_pool(pool);
    }
}


static char njt_health_check_resp_body[] = "{\n  \"code\": %d,\n   \"msg\": \"%V\"\n }";

static njt_int_t njt_health_check_conf_out_handler(njt_http_request_t *r, njt_int_t hrc) {
    njt_uint_t buf_len;
    njt_buf_t *buf;
    njt_chain_t out;
    njt_int_t rc;

    switch (hrc) {
        case HC_SUCCESS:
            r->headers_out.status = NJT_HTTP_OK;
            break;
        case HC_TYPE_NOT_FOUND:
        case HC_UPSTREAM_NOT_FOUND:
        case HC_VERSION_NOT_SUPPORT:
        case HC_PATH_NOT_FOUND:
        case HC_NOT_FOUND:
            r->headers_out.status = NJT_HTTP_NOT_FOUND;
            break;
        case HC_METHOD_NOT_ALLOW:
            r->headers_out.status = NJT_HTTP_NOT_ALLOWED;
            break;
        case HC_CA_NOT_CONF:
        case HC_CERTIFICATE_NOT_CONF:
        case HC_CERTIFICATE_KEY_NOT_CONF:
        case HC_BODY_ERROR:
            r->headers_out.status = NJT_HTTP_BAD_REQUEST;
            break;
        case HC_DOUBLE_SET:
            r->headers_out.status = NJT_HTTP_CONFLICT;
            break;
        case HC_SERVER_ERROR:
        default:
            r->headers_out.status = NJT_HTTP_INTERNAL_SERVER_ERROR;
            break;
    }
    buf_len = sizeof(njt_health_check_resp_body) - 1 + 9 + njt_health_check_error_msg[hrc].len;
    buf = njt_create_temp_buf(r->pool, buf_len);
    if (buf == NULL) {
        njt_log_error(NJT_LOG_DEBUG, r->connection->log, 0,
                       "could not alloc buffer in function %s", __func__);
        return NJT_ERROR;
    }
    buf->last = njt_snprintf(buf->last, buf_len, njt_health_check_resp_body, hrc, njt_health_check_error_msg + hrc);
    njt_str_t type = njt_string("application/json");
    r->headers_out.content_type = type;
    r->headers_out.content_length_n = buf->last - buf->pos;
    if (r->headers_out.content_length) {
        r->headers_out.content_length->hash = 0;
        r->headers_out.content_length = NULL;
    }
    rc = njt_http_send_header(r);

    // r->header_only  when method is HEAD ,header_only is set.
    if (rc == NJT_ERROR || rc > NJT_OK) {
        return rc;
    }
    buf->last_buf = 1;
    out.buf = buf;
    out.next = NULL;
    return njt_http_output_filter(r, &out);
}



/*!
    路由解析
*/
static njt_int_t
njt_health_check_api_parse_path(njt_http_request_t *r, njt_array_t *path) {
    u_char *p, *sub_p,*end;
    njt_uint_t len;
    njt_str_t *item;
    njt_http_core_loc_conf_t *clcf;
    njt_str_t uri;

    /*the uri is parsed and delete all the duplidated '/' characters.
     * for example, "/api//7//http///upstreams///////" will be parse to
     * "/api/7/http/upstreams/" already*/

    clcf = njt_http_get_module_loc_conf(r, njt_http_core_module);

    uri = r->uri;
    p = uri.data + clcf->name.len;
    end = uri.data + uri.len;
    len = uri.len - clcf->name.len;

    if (len != 0 && *p != '/') {
        return HC_PATH_NOT_FOUND;
    }
    if (*p == '/') {
        len--;
        p++;
    }

    while (len > 0) {
        item = njt_array_push(path);
        if (item == NULL) {
            njt_log_error(NJT_LOG_ERR, r->connection->log, 0,
                          "zack: array item of path push error.");
            return NJT_ERROR;
        }

        item->data = p;
        sub_p = (u_char *) njt_strlchr(p,end, '/');

        if (sub_p == NULL || (njt_uint_t) (sub_p - uri.data) > uri.len) {
            item->len = uri.data + uri.len - p;
            break;

        } else {
            item->len = sub_p - p;
        }

        len -= item->len;
        p += item->len;

        if (*p == '/') {
            len--;
            p++;
        }

    }
    return NJT_OK;
}


static njt_str_t *njt_health_check_confs_to_json(njt_pool_t *pool, njt_health_check_main_conf_t *hmcf, njt_int_t filter_module_type) {
    njt_health_check_conf_t             *hhccf;
    njt_queue_t                         *q;
    health_checks_t                     *dynjson_obj;
    health_checks_item_t*               hc_item;       

    dynjson_obj = create_health_checks(pool, 4);
    if(dynjson_obj == NULL){
        goto err;
    }

    q = njt_queue_head(&hmcf->hc_queue);
    for (; q != njt_queue_sentinel(&hmcf->hc_queue); q = njt_queue_next(q)) {
        hhccf = njt_queue_data(q, njt_health_check_conf_t, queue);

        if(filter_module_type != NJT_HEALTH_CHECK_FILTER_TYPE_NONE){
            if(NJT_HEALTH_CHECK_FILTER_TYPE_HTTP == filter_module_type && hhccf->module_type != NJT_HTTP_MODULE){
                continue;
            }

            if(NJT_HEALTH_CHECK_FILTER_TYPE_STREAM == filter_module_type && hhccf->module_type != NJT_STREAM_MODULE){
                continue;
            }
        }

        hc_item = create_health_checks_item(pool);
        if(hc_item == NULL ){
            goto err;
        }

        //set upstream name
        set_health_checks_item_upstream_name(hc_item, &hhccf->upstream_name);

        //set real server type
        //get server type str by type
        set_health_checks_item_hc_type(hc_item, &hhccf->server_type);
        add_item_health_checks(dynjson_obj, hc_item);
    }

    return to_json_health_checks(pool, dynjson_obj, OMIT_NULL_ARRAY | OMIT_NULL_OBJ | OMIT_NULL_STR);

    err:
    return NULL;
}

static njt_int_t njt_health_check_api_get_hcs(njt_http_request_t *r) {
    njt_cycle_t             *cycle;
    njt_int_t               rc;
    njt_health_check_main_conf_t  *hmcf;
    njt_buf_t               *buf;
    njt_chain_t             out;
    njt_str_t               *json;

    rc = njt_http_discard_request_body(r);
    if (rc == NJT_ERROR || rc >= NJT_HTTP_SPECIAL_RESPONSE) {
        return HC_SERVER_ERROR;
    }

    cycle = (njt_cycle_t *) njt_cycle;
    hmcf = (njt_health_check_main_conf_t *) njt_get_conf(cycle->conf_ctx, njt_helper_health_check_module);

    json = njt_health_check_confs_to_json(r->pool, hmcf, NJT_HEALTH_CHECK_FILTER_TYPE_NONE);
    if (json == NULL || json->len == 0) {
        return HC_SERVER_ERROR;
    }

    buf = njt_create_temp_buf(r->pool, json->len);
    if(buf == NULL){
        njt_log_error(NJT_LOG_ERR, r->connection->log, 0, "njt_create_temp_buf error , size :%ui" ,json->len);
        return HC_SERVER_ERROR;
    }
    buf->last = buf->pos + json->len;
    njt_memcpy(buf->pos, json->data, json->len);

    r->headers_out.status = NJT_HTTP_OK;
    njt_str_t type = njt_string("application/json");
    r->headers_out.content_type = type;
    r->headers_out.content_length_n = buf->last - buf->pos;
    if (r->headers_out.content_length) {
        r->headers_out.content_length->hash = 0;
        r->headers_out.content_length = NULL;
    }

    rc = njt_http_send_header(r);
    if (rc == NJT_ERROR || rc > NJT_OK || r->header_only) {
        return rc;
    }
    buf->last_buf = 1;
    out.buf = buf;
    out.next = NULL;

    rc = njt_http_output_filter(r, &out);
    if (rc == NJT_OK) {
        return HC_RESP_DONE;
    }

    return HC_SERVER_ERROR;
}




static njt_health_check_conf_t *njt_health_check_find_hc_by_name_and_type(njt_cycle_t *cycle,
        njt_str_t *hc_type, njt_str_t *upstream_name){
    njt_health_check_main_conf_t    *hmcf;
    njt_health_check_conf_t         *hhccf;
    njt_queue_t                     *q;

    hmcf = (njt_health_check_main_conf_t *) njt_get_conf(cycle->conf_ctx, njt_helper_health_check_module);

    q = njt_queue_head(&hmcf->hc_queue);
    for (; q != njt_queue_sentinel(&hmcf->hc_queue); q = njt_queue_next(q)) {
        hhccf = njt_queue_data(q, njt_health_check_conf_t, queue);
        if (hhccf->server_type.len == hc_type->len
            && njt_strncmp(hhccf->server_type.data, hc_type->data, hhccf->server_type.len) == 0
            && hhccf->upstream_name.len == upstream_name->len
            && njt_strncmp(hhccf->upstream_name.data, upstream_name->data, hhccf->upstream_name.len) == 0) {
            return hhccf;
        }
    }

    return NULL;
}


static njt_health_check_conf_t *njt_health_check_find_stream_hc_by_upstream(njt_cycle_t *cycle,
        njt_str_t *upstream_name){
    njt_health_check_main_conf_t *hmcf;
    njt_health_check_conf_t *hhccf;
    njt_queue_t *q;

    hmcf = (njt_health_check_main_conf_t *) njt_get_conf(cycle->conf_ctx, njt_helper_health_check_module);

    q = njt_queue_head(&hmcf->hc_queue);
    for (; q != njt_queue_sentinel(&hmcf->hc_queue); q = njt_queue_next(q)) {
        hhccf = njt_queue_data(q, njt_health_check_conf_t, queue);
        if (hhccf->module_type == NJT_STREAM_MODULE
            && hhccf->upstream_name.len == upstream_name->len
            && njt_strncmp(hhccf->upstream_name.data, upstream_name->data, hhccf->upstream_name.len) == 0) {
            return hhccf;
        }
    }

    return NULL;
}


static njt_health_check_conf_t *njt_health_check_find_hc(njt_cycle_t *cycle, njt_health_check_api_data_t *api_data){
    return njt_health_check_find_hc_by_name_and_type(cycle, &api_data->hc_type, &api_data->upstream_name);
}


static njt_int_t njt_health_check_api_delete_conf(njt_http_request_t *r, njt_health_check_api_data_t *api_data) {
    njt_cycle_t *cycle;
    njt_health_check_conf_t *hhccf;
    njt_health_check_main_conf_t *hmcf;

    cycle = (njt_cycle_t *) njt_cycle;
    hmcf = (njt_health_check_main_conf_t *) njt_get_conf(cycle->conf_ctx, njt_helper_health_check_module);

    if (api_data->hc_type.len == 0 || api_data->upstream_name.len == 0) {
        njt_log_error(NJT_LOG_ERR, r->connection->log, 0, " type and upstream must be set !!");
        return HC_BODY_ERROR;
    }

    hhccf = njt_health_check_find_hc(cycle, api_data);
    if (hhccf == NULL) {
        njt_log_error(NJT_LOG_ERR, r->connection->log, 0, "not find upstream %V hc", &api_data->upstream_name);
        return HC_NOT_FOUND;
    }

    njt_queue_remove(&hhccf->queue);
    hhccf->disable = 1;
    switch (hhccf->module_type)
    {
    case NJT_HTTP_MODULE:
        njt_health_check_kv_flush_http_conf_info(hhccf, NULL, 0);
        njt_health_check_kv_flush_http_confs(hmcf);
        break;
    
    case NJT_STREAM_MODULE:
        njt_health_check_kv_flush_stream_conf_info(hhccf, NULL, 0);
        njt_health_check_kv_flush_stream_confs(hmcf);
        break;
    default:
        break;
    }

    return HC_SUCCESS;
}


static njt_int_t njt_health_check_api_get_conf_info(njt_http_request_t *r, njt_health_check_api_data_t *api_data) {
    njt_cycle_t                     *cycle;
    njt_health_check_conf_t         *hhccf;
    njt_buf_t                       *buf;
    njt_chain_t                     out;
    njt_int_t                       rc;
    njt_str_t                       json;
    njt_str_t                       type = njt_string("application/json");
    njt_str_t                       tkey1, tkey2;
    njt_str_t                       key;
    njt_str_t                       key_pre;
    njt_str_t                       key_separator;


    cycle = (njt_cycle_t *) njt_cycle;
    if (api_data->hc_type.len == 0 || api_data->upstream_name.len == 0) {
        njt_log_error(NJT_LOG_ERR, r->connection->log, 0, " type and upstream must be set !!");
        return HC_BODY_ERROR;
    }

    hhccf = njt_health_check_find_hc(cycle, api_data);
    if (hhccf == NULL) {
        njt_log_error(NJT_LOG_ERR, r->connection->log, 0, "not find upstream %V hc", &api_data->upstream_name);
        return HC_NOT_FOUND;
    }

    if(hhccf->module_type == NJT_HTTP_MODULE){
        // key = njt_string(HTTP_HEALTH_CHECK_CONFS);
        njt_str_set(&key, HTTP_HEALTH_CHECK_CONFS);
        njt_str_set(&key_pre, HTTP_HEALTH_CHECK_CONF_INFO);
        njt_str_set(&key_separator, HTTP_HEALTH_CHECK_SEPARATOR);
    }else{
        njt_str_set(&key, STREAM_HEALTH_CHECK_CONFS);
        njt_str_set(&key_pre, STREAM_HEALTH_CHECK_CONF_INFO);
        njt_str_set(&key_separator, STREAM_HEALTH_CHECK_SEPARATOR);
    }

    //just get from kv
    njt_str_concat(r->pool, tkey1, key_pre, hhccf->server_type, return HC_SERVER_ERROR);
    njt_str_concat(r->pool, tkey2, tkey1, key_separator, return HC_SERVER_ERROR);
    njt_str_concat(r->pool, tkey1, tkey2, hhccf->upstream_name, return HC_SERVER_ERROR);
    
    njt_memzero(&json, sizeof(njt_str_t));
    rc = njt_dyn_kv_get(&tkey1, &json);
    if (rc != NJT_OK || json.len <= 0) {
        njt_log_error(NJT_LOG_ERR, r->connection->log, 0, 
                    "upstream:%V hc msg len is less than 0",  &json);

        return HC_SERVER_ERROR;
    }

    buf = njt_create_temp_buf(r->pool, json.len);
    if(buf == NULL){
        njt_log_error(NJT_LOG_ERR, r->connection->log, 0, "njt_create_temp_buf error , size :%ui" , json.len);
        return HC_SERVER_ERROR;
    }
    buf->last = buf->pos + json.len;
    njt_memcpy(buf->pos, json.data, json.len);
    r->headers_out.status = NJT_HTTP_OK;
    
    r->headers_out.content_type = type;
    r->headers_out.content_length_n = buf->last - buf->pos;
    if (r->headers_out.content_length) {
        r->headers_out.content_length->hash = 0;
        r->headers_out.content_length = NULL;
    }

    rc = njt_http_send_header(r);
    if (rc == NJT_ERROR || rc > NJT_OK || r->header_only) {
        return rc;
    }

    buf->last_buf = 1;
    out.buf = buf;
    out.next = NULL;
    rc = njt_http_output_filter(r, &out);
    if (rc == NJT_OK) {
        return HC_RESP_DONE;
    }

    return HC_SERVER_ERROR;
}




static bool njt_health_check_stream_check_peer(njt_health_check_conf_t *hhccf, 
                njt_stream_upstream_rr_peer_t *peer){
    njt_lvlhsh_query_t                  lhq;
    njt_int_t                           rc;
    njt_health_check_stream_same_peer_t             *stream_lvlhsh_value;
    
    //servername to peers
    lhq.key = peer->name;
    lhq.key_hash = njt_murmur_hash2(lhq.key.data, lhq.key.len);
    lhq.proto = &njt_health_check_lvlhsh_proto;

    //find
    rc = njt_lvlhsh_find(&hhccf->servername_to_peers, &lhq);
    if(rc != NJT_OK){
        return false;
    }

    stream_lvlhsh_value = lhq.value;
    if(stream_lvlhsh_value->peer->id == peer->id){
        return true;
    }

    return false;
}

static bool njt_health_check_http_check_peer(njt_health_check_conf_t *hhccf, 
                njt_http_upstream_rr_peer_t *peer){
    njt_lvlhsh_query_t                  lhq;
    njt_int_t                           rc;
    njt_health_check_http_same_peer_t             *http_lvlhsh_value;
    
    //servername to peers
    lhq.key = peer->name;
    lhq.key_hash = njt_murmur_hash2(lhq.key.data, lhq.key.len);
    lhq.proto = &njt_health_check_lvlhsh_proto;

    //find
    rc = njt_lvlhsh_find(&hhccf->servername_to_peers, &lhq);
    if(rc != NJT_OK){
        return false;
    }

    http_lvlhsh_value = lhq.value;
    if(http_lvlhsh_value->peer->id == peer->id){
        return true;
    }

    return false;
}

static njt_int_t njt_health_check_http_peer_add_map(njt_health_check_conf_t *hhccf, 
                njt_http_upstream_rr_peer_t *peer){
    njt_lvlhsh_query_t                  lhq;
    njt_int_t                           rc, rc2;
    njt_health_check_http_same_peer_t             *http_lvlhsh_value;
    njt_health_check_http_peer_element            *http_ele;
    
    //servername to peers
    lhq.key = peer->name;
    lhq.key_hash = njt_murmur_hash2(lhq.key.data, lhq.key.len);
    lhq.proto = &njt_health_check_lvlhsh_proto;

    //find
    rc = njt_lvlhsh_find(&hhccf->servername_to_peers, &lhq);
    if(rc != NJT_OK){
        //if not exist, insert and update current peer
        lhq.key = peer->name;
        lhq.key_hash = njt_murmur_hash2(lhq.key.data, lhq.key.len);
        lhq.proto = &njt_health_check_lvlhsh_proto;
        lhq.pool = hhccf->map_pool;

        http_lvlhsh_value = njt_pcalloc(hhccf->map_pool, sizeof(njt_health_check_http_same_peer_t));
        if(http_lvlhsh_value == NULL){
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                        "njt_create_peers_map_by_peer malloc error");
            return NJT_ERROR;
        }

        http_lvlhsh_value->peer = peer;
        njt_queue_init(&http_lvlhsh_value->datas);

        lhq.value = http_lvlhsh_value;
        rc2 = njt_lvlhsh_insert(&hhccf->servername_to_peers, &lhq);
        if(rc2 != NJT_OK){
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                        "njt_create_peers_map_by_peer servername2peers lvlhash insert fail");
            return NJT_ERROR;
        }

        return NJT_OK;
    }else{
        //if exist, update peers list
        http_lvlhsh_value = lhq.value;
        http_ele = njt_pcalloc(hhccf->map_pool, sizeof(njt_health_check_http_peer_element));
        if(http_ele == NULL){
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                        "njt_create_peers_map_by_peer http_ele malloc error");
            return NJT_ERROR;
        }

        http_ele->peer = peer;
        njt_queue_insert_tail(&http_lvlhsh_value->datas, &http_ele->ele_queue);

        return NJT_DECLINED;
    }

    return NJT_OK;
}

static njt_int_t njt_health_check_stream_peer_add_map(njt_health_check_conf_t *hhccf, 
            njt_stream_upstream_rr_peer_t *stream_peer){
    njt_lvlhsh_query_t                  lhq;
    njt_int_t                           rc, rc2;
    njt_health_check_stream_same_peer_t           *stream_lvlhsh_value;
    njt_health_check_stream_peer_element          *stream_ele;

    //servername to peers
    lhq.key = stream_peer->name;
    lhq.key_hash = njt_murmur_hash2(lhq.key.data, lhq.key.len);
    lhq.proto = &njt_health_check_lvlhsh_proto;

    //find
    rc = njt_lvlhsh_find(&hhccf->servername_to_peers, &lhq);
    if(rc != NJT_OK){
        //if not exist, insert and update current peer
        lhq.key = stream_peer->name;
        lhq.key_hash = njt_murmur_hash2(lhq.key.data, lhq.key.len);
        lhq.proto = &njt_health_check_lvlhsh_proto;
        lhq.pool = hhccf->map_pool;
        
        //if(hhccf->type == NJT_STREAM_MODULE){
        stream_lvlhsh_value = njt_pcalloc(hhccf->map_pool, sizeof(njt_health_check_stream_same_peer_t));
        if(stream_lvlhsh_value == NULL){
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                        "njt_create_peers_map_by_peer malloc error");
            return NJT_ERROR;
        }

        stream_lvlhsh_value->peer = stream_peer;
        njt_queue_init(&stream_lvlhsh_value->datas);

        lhq.value = stream_lvlhsh_value;

        rc2 = njt_lvlhsh_insert(&hhccf->servername_to_peers, &lhq);
        if(rc2 != NJT_OK){
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                        "njt_create_peers_map_by_peer servername2peers lvlhash insert fail");
            return NJT_ERROR;
        }

        return NJT_OK;
    }else{
        //if exist, update peers list
        stream_lvlhsh_value = lhq.value;
        stream_ele = njt_pcalloc(hhccf->map_pool, sizeof(njt_health_check_stream_peer_element));
        if(stream_ele == NULL){
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                        "njt_create_peers_map_by_peer stream_ele malloc error");
            return NJT_ERROR;
        }

        stream_ele->peer = stream_peer;
        njt_queue_insert_tail(&stream_lvlhsh_value->datas, &stream_ele->ele_queue);

        return NJT_DECLINED;
    }

    return NJT_OK;
}


static void njt_health_check_clean_peers_map(njt_health_check_conf_t *hhccf){
    njt_int_t rc;
    if(hhccf->map_pool){
        njt_destroy_pool(hhccf->map_pool);
        hhccf->map_pool = NULL;
    }

    hhccf->map_pool = njt_create_pool(njt_pagesize, njt_cycle->log);
    if (hhccf->map_pool == NULL) {
        njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0, "njt_create_peer_map error");
        return;
    }

    rc = njt_sub_pool(njt_cycle->pool, hhccf->map_pool);
    if (rc != NJT_OK) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "njt_sub_pool error in function %s", __func__);
        njt_destroy_pool(hhccf->map_pool);
        return;
    }

    njt_lvlhsh_init(&hhccf->servername_to_peers);
}


static njt_int_t njt_traver_http_upstream_item_handle(void *ctx,njt_http_upstream_srv_conf_t * uscfp){
    njt_cycle_t                     *cycle;
    njt_str_t                       hc_type, upstream_name;
    njt_health_check_conf_t  *hhccf;
    njt_pool_t                      *pool;
    njt_str_t                       msg;

    cycle = (njt_cycle_t *)njt_cycle;

    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0, "http_upstream_item: %V,mandatory:%ud,persistent:%ud",&uscfp->host,uscfp->mandatory,uscfp->persistent);
    if(uscfp->mandatory == 1) {
        njt_str_set(&hc_type,"http");
        njt_str_set(&msg,"{\"interval\": \"10s\",\n"
                         "\"visit_interval\": \"10s\",\n"
                         "\"jitter\": \"1s\",\n"
                         "\"timeout\": \"10s\",\n"
                         "\"passes\": 2,\n"
                         "\"fails\": 1,\n"
                         "\"http\": {\n"
                         "\t\"uri\": \"/robots.txt\",\n"
                         "\t\"status\": \"200-299\"\n"
                         "}}");
        upstream_name.data = uscfp->host.data;
        upstream_name.len = uscfp->host.len;
        hhccf = njt_health_check_find_hc_by_name_and_type(cycle, &hc_type, &upstream_name);
        if(NULL != hhccf) {
            njt_log_error(NJT_LOG_INFO, njt_cycle->log, 0, "http upstream %V has added by kv",&uscfp->host);
            return 0;
        }
        pool = (njt_pool_t*)ctx;
        njt_health_check_recovery_conf_info(pool, &msg, &upstream_name, &hc_type);
    }
    return 0;
}

static njt_int_t njt_traver_stream_upstream_item_handle(void *ctx,njt_stream_upstream_srv_conf_t * uscfp){
    njt_cycle_t                     *cycle;
    njt_str_t                       hc_type, upstream_name;
    njt_health_check_conf_t         *hhccf;
    njt_pool_t                      *pool;
    njt_str_t                       msg;

    cycle = (njt_cycle_t *)njt_cycle;

    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0, "http_upstream_item: %V,mandatory:%ud,persistent:%ud",&uscfp->host,uscfp->mandatory,uscfp->persistent);
    if(uscfp->mandatory == 1) {
        njt_str_set(&hc_type,"stcp");
        njt_str_set(&msg,"{\n"
                         "\"interval\": \"10s\",\n"
                         "\"visit_interval\": \"10s\",\n"
                         "\"jitter\": \"1s\",\n"
                         "\"timeout\": \"10s\",\n"
                         "\"passes\": 2,\n"
                         "\"fails\": 1,\n"
                         "\"mandatory\": true,\n"
                         "\"stream\": {\n"
                         "\t\"send\":\"\",\n"
                         "\t\"expect\": \"\"\n"
                         "}\n"
                         "}");
        upstream_name.data = uscfp->host.data;
        upstream_name.len = uscfp->host.len;
        hhccf = njt_health_check_find_hc_by_name_and_type(cycle, &hc_type, &upstream_name);
        if(NULL != hhccf) {
            njt_log_error(NJT_LOG_INFO, njt_cycle->log, 0, "http upstream %V has added by kv.",&uscfp->host);
            return 0;
        }
        pool = (njt_pool_t*)ctx;
        njt_health_check_recovery_conf_info(pool, &msg, &upstream_name, &hc_type);
    }
    return 0;
}

#if (NJT_HTTP_ADD_DYNAMIC_UPSTREAM)
static void njt_health_check_dyn_http_upstream_add(void *data) {
    njt_http_upstream_srv_conf_t            *upstream = data;
    njt_health_check_conf_t          *hhccf;
    njt_cycle_t                             *cycle = (njt_cycle_t *) njt_cycle;
    njt_pool_t                              *pool; 
    njt_str_t                                hc_type = njt_string("http");

    if(upstream) {
        //check wether exist
        hhccf = njt_health_check_find_hc_by_name_and_type(cycle, &hc_type, &upstream->host);
        if(hhccf != NULL){
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                "http upstream:%V hc has already exist, just recover checking", &upstream->host);
        
            return;
        }

        //try kv recover, because already exist hc config, but need wait dyn http upstream
        if(NJT_OK == njt_health_check_recovery_conf_of_upstream(&upstream->host, 1)){
            return;
        }

        //check mandatory
        if(upstream->mandatory == 1){
            pool = njt_create_pool(njt_pagesize,cycle->log);
            if(NULL == pool){
                njt_log_error(NJT_LOG_EMERG, cycle->log, 0, "dyn upstream, health check helper alloc force hc memory error ");
                return ;
            }

            njt_traver_http_upstream_item_handle(pool, upstream);

            if(pool != NULL){
                njt_destroy_pool(pool);
            }   
        }

        return;
    } 
}


static void njt_health_check_dyn_http_upstream_del(void *data) {
    njt_http_upstream_srv_conf_t            *upstream = data;
    njt_health_check_conf_t          *hhccf;
    njt_cycle_t                             *cycle = (njt_cycle_t *) njt_cycle;
    njt_str_t                                hc_type = njt_string("http");
    njt_health_check_main_conf_t                  *hmcf;

    if(upstream) {
        //check wether exist
        hhccf = njt_health_check_find_hc_by_name_and_type(cycle, &hc_type, &upstream->host);
        if(hhccf == NULL){
            njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                "http upstream:%V has no hc config", &upstream->host);
        
            return;
        }

        hmcf = (njt_health_check_main_conf_t *) njt_get_conf(cycle->conf_ctx, njt_helper_health_check_module);

        //remove from hc queue
        njt_queue_remove(&hhccf->queue);
        hhccf->disable = 1;
        njt_health_check_kv_flush_http_conf_info(hhccf, NULL, 0);
        njt_health_check_kv_flush_http_confs(hmcf);

        return;
    } 
}
#endif


#if (NJT_STREAM_ADD_DYNAMIC_UPSTREAM)
static void njt_health_check_dyn_stream_upstream_add(void *data) {
    njt_stream_upstream_srv_conf_t          *upstream = data;
    njt_health_check_conf_t          *hhccf = NULL;
    njt_cycle_t                             *cycle = (njt_cycle_t *) njt_cycle;
    njt_pool_t                              *pool; 

    if(upstream) {
        //check wether exist
        hhccf = njt_health_check_find_stream_hc_by_upstream(cycle, &upstream->host);
        if(hhccf != NULL){
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                "stream upstream:%V hc has already exist, just recover checking", &upstream->host);
        
            return;
        }

        //try kv recover, because already exist hc config, but need wait dyn stream upstream
        if(NJT_OK == njt_health_check_recovery_conf_of_upstream(&upstream->host, 0)){
            return;
        }

        //check mandatory
        if(upstream->mandatory == 1){
            pool = njt_create_pool(njt_pagesize,cycle->log);
            if(NULL == pool){
                njt_log_error(NJT_LOG_EMERG, cycle->log, 0, "dyn upstream, health check helper alloc force hc memory error ");
                return ;
            }

            njt_traver_stream_upstream_item_handle(pool, upstream);

            if(pool != NULL){
                njt_destroy_pool(pool);
            }   
        }

        return;
    } 
}


static void njt_health_check_dyn_stream_upstream_del(void *data) {
    njt_stream_upstream_srv_conf_t            *upstream = data;
    njt_health_check_conf_t          *hhccf;
    njt_cycle_t                             *cycle = (njt_cycle_t *) njt_cycle;
    njt_health_check_main_conf_t                  *hmcf;


    if(upstream) {
        //check wether exist
        hhccf = njt_health_check_find_stream_hc_by_upstream(cycle, &upstream->host);
        if(hhccf == NULL){
            njt_log_error(NJT_LOG_INFO, njt_cycle->log, 0,
                "stream upstream:%V has no hc config", &upstream->host);
        
            return;
        }

        hmcf = (njt_health_check_main_conf_t *) njt_get_conf(cycle->conf_ctx, njt_helper_health_check_module);

        //remove from hc queue
        njt_queue_remove(&hhccf->queue);
        hhccf->disable = 1;
        njt_health_check_kv_flush_stream_conf_info(hhccf, NULL, 0);
        njt_health_check_kv_flush_stream_confs(hmcf);

        return;
    } 
}
#endif


static njt_int_t njt_health_check_init_process(njt_cycle_t *cycle) {
    njt_health_check_main_conf_t *hmcf;
    njt_pool_t * pool = NULL;

    hmcf = (njt_health_check_main_conf_t *) njt_get_conf(cycle->conf_ctx, njt_helper_health_check_module);
    if (hmcf == NULL) {
        njt_log_error(NJT_LOG_EMERG, cycle->log, 0, "health check helper alloc main conf error ");
        return NJT_ERROR;
    }

#if (NJT_HTTP_ADD_DYNAMIC_UPSTREAM)
    //register dyn http upstream handler
    njt_str_t keyy = njt_string("upstream");
    njt_http_object_change_reg_info_t reg;
    reg.add_handler = njt_health_check_dyn_http_upstream_add;
    reg.del_handler = njt_health_check_dyn_http_upstream_del;
    reg.update_handler = NULL;


    njt_http_object_register_notice(&keyy,&reg);
#endif


#if (NJT_STREAM_ADD_DYNAMIC_UPSTREAM)
    //register dyn stream upstream handler
    njt_str_t s_keyy = njt_string(STREAM_UPSTREAM_OBJ);
    njt_http_object_change_reg_info_t s_reg;
    s_reg.add_handler = njt_health_check_dyn_stream_upstream_add;
    s_reg.del_handler = njt_health_check_dyn_stream_upstream_del;
    s_reg.update_handler = NULL;

    njt_http_object_register_notice(&s_keyy, &s_reg);
#endif

    if (hmcf->first) {
        njt_health_check_recovery_confs();
    
        /* by zhaokang */
        njt_stream_health_check_recovery_confs();

        // 遍历upstream us->mandatory为1时添加健康检查项   interval   jitter  timeout passes  fails
        pool = njt_create_pool(njt_pagesize,cycle->log);
        if(NULL == pool){
            njt_log_error(NJT_LOG_EMERG, cycle->log, 0, "health check helper alloc force hc memory error ");
            return NJT_ERROR;
        }
        njt_http_upstream_traver(pool,njt_traver_http_upstream_item_handle);
        njt_stream_upstream_traver(pool,njt_traver_stream_upstream_item_handle);

        hmcf->first = 0;
    }

    if(NULL != pool){
        njt_destroy_pool(pool);
    }

    return NJT_OK;
}

static void njt_health_check_kv_flush_http_confs(njt_health_check_main_conf_t *hmcf) {
    njt_pool_t  *pool;
    njt_str_t   *msg;
    njt_str_t   key = njt_string(HTTP_HEALTH_CHECK_CONFS);

    pool = njt_create_pool(NJT_MIN_POOL_SIZE, njt_cycle->log);
    if (pool == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "create pool error when flush confs in health check");
        return;
    }
    msg = njt_health_check_confs_to_json(pool, hmcf, NJT_HEALTH_CHECK_FILTER_TYPE_HTTP);
    if (msg == NULL || msg->len == 0) {
        goto end;
    }

    njt_dyn_kv_set(&key, msg);

    end:
    njt_destroy_pool(pool);
}

static void njt_health_check_kv_flush_stream_confs(njt_health_check_main_conf_t *hmcf) {
    njt_pool_t     *pool;
    njt_str_t      *msg;

    njt_str_t key = njt_string(STREAM_HEALTH_CHECK_CONFS);

    pool = njt_create_pool(NJT_MIN_POOL_SIZE, njt_cycle->log);
    if (pool == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "create pool error in function %s", __func__);
        return;
    }

    msg = njt_health_check_confs_to_json(pool, hmcf, NJT_HEALTH_CHECK_FILTER_TYPE_STREAM);
    if (msg == NULL || msg->len == 0) {
        goto end;
    }

    njt_dyn_kv_set(&key, msg);

end:
    njt_destroy_pool(pool);
}

static void njt_health_check_kv_flush_http_conf_info(njt_health_check_conf_t *hhccf, njt_str_t *msg, njt_flag_t is_add) {
    njt_pool_t *pool;
    njt_str_t tkey1, tkey2;
    njt_str_t key_pre = njt_string(HTTP_HEALTH_CHECK_CONF_INFO);
    njt_str_t key_separator = njt_string(HTTP_HEALTH_CHECK_SEPARATOR);

    pool = njt_create_pool(NJT_MIN_POOL_SIZE, njt_cycle->log);
    if (pool == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "create pool error in function %s", __func__);
        return;
    }

    njt_str_concat(pool, tkey1, key_pre, hhccf->server_type, goto end);
    njt_str_concat(pool, tkey2, tkey1, key_separator, goto end);
    njt_str_concat(pool, tkey1, tkey2, hhccf->upstream_name, goto end);

    if(is_add){
        if (msg == NULL || msg->len == 0 ) {
            njt_log_error(NJT_LOG_ERR, pool->log, 0, "njt_health_check_conf_info_to_json error");
            goto end;
        }
        njt_dyn_kv_set(&tkey1, msg);
    }else{
        njt_dyn_kv_del(&tkey1);
    }

end:
    njt_destroy_pool(pool);
}

static void njt_health_check_kv_flush_stream_conf_info(njt_health_check_conf_t *hhccf, njt_str_t *msg, njt_flag_t is_add) {
    njt_pool_t         *pool;
    njt_str_t          tkey1, tkey2;

    njt_str_t key_pre         = njt_string(STREAM_HEALTH_CHECK_CONF_INFO);
    njt_str_t key_separator   = njt_string(STREAM_HEALTH_CHECK_SEPARATOR);

    pool = njt_create_pool(NJT_MIN_POOL_SIZE, njt_cycle->log);
    if (pool == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "create pool error in function %s", __func__);
        return;
    }

    njt_str_concat(pool, tkey1, key_pre, hhccf->server_type,      goto end);
    njt_str_concat(pool, tkey2, tkey1,   key_separator,        goto end);
    njt_str_concat(pool, tkey1, tkey2,   hhccf->upstream_name, goto end);

    if(is_add){
        if (msg == NULL || msg->len == 0 ) {
            njt_log_error(NJT_LOG_ERR, pool->log, 0, "njt_health_check_conf_info_to_json error");
            goto end;
        }

        njt_dyn_kv_set(&tkey1, msg);
    }else{
        njt_dyn_kv_del(&tkey1);
    }

end:
    if(pool != NULL){
        njt_destroy_pool(pool);
    }
}




