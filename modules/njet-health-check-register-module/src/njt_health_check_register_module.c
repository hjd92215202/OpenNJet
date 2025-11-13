/*
 * Copyright (C) 2021-2023  TMLake(Beijing) Technology Co., Ltd.
 */
#include <njt_config.h>
#include <njt_core.h>
#include <njt_http.h>
#include <njt_hash_util.h>
#include "njt_health_check_register_module.h"

static njt_lvlhsh_t njt_health_check_register_modules;
static njt_pool_t *njt_hc_register_pool = NULL;


static njt_int_t njt_health_check_register_init_worker(njt_cycle_t *cycle);

static void njt_health_check_register_exit_worker(njt_cycle_t *cycle);



static njt_int_t
njt_hc_register_lvlhsh_test(njt_lvlhsh_query_t *lhq, void *data)
{
    //ignore value compare, just return ok
    return NJT_OK;
}

const njt_lvlhsh_proto_t  njt_hc_register_lvlhsh_proto = {
    NJT_LVLHSH_LARGE_MEMALIGN,
    njt_hc_register_lvlhsh_test,
    njt_lvlhsh_pool_alloc,
    njt_lvlhsh_pool_free,
};


static njt_http_module_t njt_health_check_register_module_ctx = {
        NULL,                               /* preconfiguration */
        NULL,                               /* postconfiguration */

        NULL,                               /* create main configuration */
        NULL,                               /* init main configuration */

        NULL,                               /* create server configuration */
        NULL,                               /* merge server configuration */

        NULL,                               /* create location configuration */
        NULL                                /* merge location configuration */
};


njt_module_t njt_health_check_register_module = {
        NJT_MODULE_V1,
        &njt_health_check_register_module_ctx, /* module context */
        NULL,    /* module directives */
        NJT_HTTP_MODULE,                    /* module type */
        NULL,                               /* init master */
        NULL,                               /* init module */
        njt_health_check_register_init_worker, /* init process */
        NULL,                               /* init thread */
        NULL,                               /* exit thread */
        njt_health_check_register_exit_worker,  /* exit process */
        NULL,                               /* exit master */
        NJT_MODULE_V1_PADDING
};


njt_health_check_reg_info_t *
njt_health_check_register_find_handler(njt_str_t *server_type){
    njt_lvlhsh_query_t                  lhq;
    njt_uint_t                          rc;

    //check param
    if(server_type == NULL){
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0, 
                    "input health check module key is null");

        return NULL;
    }

    if(njt_hc_register_pool == NULL){
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0, 
                    "health_check register error, pool is null");

        return NULL;
    }

    //servername to peers
    lhq.key = *server_type;
    lhq.key_hash = njt_murmur_hash2(lhq.key.data, lhq.key.len);
    lhq.proto = &njt_hc_register_lvlhsh_proto;

    //find
    rc = njt_lvlhsh_find(&njt_health_check_register_modules, &lhq);
    if(rc == NJT_OK){
        return lhq.value;
    }

    return NULL;
}

static njt_int_t
njt_health_check_register_init_worker(njt_cycle_t *cycle) {
    njt_uint_t                          rc;

    if(njt_process != NJT_PROCESS_HELPER){
        return NJT_OK;
    }


    njt_hc_register_pool = njt_create_pool(njt_pagesize, njt_cycle->log);
    if (njt_hc_register_pool == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "health check register module create pool error");

        return NJT_ERROR;
    }

    rc = njt_sub_pool(njt_cycle->pool, njt_hc_register_pool);
    if (rc != NJT_OK) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "njt_sub_pool error in health check register");
        njt_destroy_pool(njt_hc_register_pool);
        return NJT_ERROR;
    }

    njt_lvlhsh_init(&njt_health_check_register_modules);

    return NJT_OK;
}


njt_int_t njt_health_check_register_reg_handler(njt_health_check_reg_info_t *reg_info){
    njt_health_check_reg_info_t         *saved_reg_info;
    njt_lvlhsh_query_t                  lhq;
    njt_uint_t                          rc;

    if(njt_process != NJT_PROCESS_HELPER){
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0, 
                    "health_check register module just run in helper worker");

        return NJT_ERROR;
    }

    //check param
    if(reg_info == NULL || reg_info->server_type == NULL 
        || reg_info->server_type->len < 1
        || reg_info->start_check == NULL || reg_info->create_ctx == NULL){
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0, 
                    "health_check register error, param error");

        return NJT_ERROR;
    }

    //check param
    if(njt_hc_register_pool == NULL){
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0, 
                    "health_check register error, pool is null");

        return NJT_ERROR;
    }

    //servername to peers
    lhq.key = *reg_info->server_type;
    lhq.key_hash = njt_murmur_hash2(lhq.key.data, lhq.key.len);
    lhq.proto = &njt_hc_register_lvlhsh_proto;

    //find
    rc = njt_lvlhsh_find(&njt_health_check_register_modules, &lhq);
    if(rc == NJT_OK){
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0, 
                "%V already register in health_check", reg_info->server_type);

        return NJT_ERROR;
    }

    //register module
    lhq.key = *reg_info->server_type;
    lhq.key_hash = njt_murmur_hash2(lhq.key.data, lhq.key.len);
    lhq.proto = &njt_hc_register_lvlhsh_proto;
    lhq.pool = njt_hc_register_pool;

    saved_reg_info = njt_pcalloc(njt_hc_register_pool, sizeof(njt_health_check_reg_info_t));
    if(saved_reg_info == NULL){
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "health check register module malloc reg info error");
        return NJT_ERROR;
    }

    *saved_reg_info = *reg_info;

    lhq.value = saved_reg_info;
    rc = njt_lvlhsh_insert(&njt_health_check_register_modules, &lhq);
    if(rc != NJT_OK){
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "health check register module lvlhash insert fail");

        return NJT_ERROR;
    }


    return NJT_OK;
}


static void njt_health_check_register_exit_worker(njt_cycle_t *cycle)
{
    if(njt_hc_register_pool != NULL){
        njt_destroy_pool(njt_hc_register_pool);
        njt_hc_register_pool = NULL;
    }
}
