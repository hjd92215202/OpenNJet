/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) 2021-2023  TMLake(Beijing) Technology Co., Ltd.
 */


#include <njt_config.h>
#include <njt_core.h>
#include <njt_http.h>
#include <njet.h>
#include <njt_str_util.h>
#include <njt_json_util.h>
#include <njt_http_ext_module.h>
#include <njt_health_check_register_module.h>
#include <njt_health_check_common.h>
#include <njt_health_check_module.h>
#include <njt_health_check_parser.h>
#include <njt_health_check_util.h>
#include "njt_health_check_udp_module.h"


static void njt_health_check_udp_module_start_check(void *hc_peer_info, interal_update_handler udpate_handler);
static void *njt_health_check_udp_module_create_ctx(void *hhccf_info, void *api_data_info);
static void njt_health_check_udp_dummy_handler(njt_event_t *ev);
static njt_int_t njt_health_check_udp_module_init_process(njt_cycle_t *cycle);
static void njt_health_check_close_udp_connection(njt_connection_t *c);
static void njt_health_check_udp_module_free_peer_resource(njt_health_check_peer_t *hc_peer, njt_int_t status);

static njt_int_t njt_health_check_udp_send_handler(njt_event_t *wev);
static njt_int_t njt_health_check_udp_recv_handler(njt_event_t *rev);
static void njt_health_check_udp_module_write_handler(njt_event_t *wev);
static void njt_health_check_udp_module_read_handler(njt_event_t *rev);
static njt_int_t 
njt_health_check_udp_match_block(njt_health_check_api_data_t *api_data, njt_health_check_conf_t *hhccf,
        njt_health_check_udp_module_conf_ctx_t *udp_ctx);

extern njt_cycle_t *njet_master_cycle;

static njt_http_module_t njt_health_check_udp_module_ctx = {
        NULL,                                   /* preconfiguration */
        NULL,          /* postconfiguration */

        NULL,                                   /* create main configuration */
        NULL,                                  /* init main configuration */

        NULL,                                  /* create server configuration */
        NULL,                                  /* merge server configuration */

        NULL,                                   /* create location configuration */
        NULL                                    /* merge location configuration */
};

njt_module_t njt_health_check_udp_module = {
        NJT_MODULE_V1,
        &njt_health_check_udp_module_ctx,      /* module context */
        NULL,                                   /* module directives */
        NJT_HTTP_MODULE,                        /* module type */
        NULL,                                   /* init master */
        NULL,                                   /* init module */
        njt_health_check_udp_module_init_process,                                   /* init process */
        NULL,                                   /* init thread */
        NULL,                                   /* exit thread */
        NULL,                                   /* exit process */
        NULL,                                   /* exit master */
        NJT_MODULE_V1_PADDING
};


static njt_health_check_checker_t udp_checker = {
        njt_health_check_udp_send_handler,
        njt_health_check_udp_recv_handler,
        NULL,
        NULL
};


static njt_int_t njt_health_check_udp_module_init_process(njt_cycle_t *cycle){
    njt_health_check_reg_info_t             h;

    njt_str_t  server_type = njt_string("udp");
    njt_memzero(&h, sizeof(njt_health_check_reg_info_t));

    h.server_type = &server_type;
    h.create_ctx = njt_health_check_udp_module_create_ctx;
    h.start_check = njt_health_check_udp_module_start_check;

    njt_health_check_reg_handler(&h);

    return NJT_OK;
}


static njt_int_t
njt_health_check_udp_module_peek_one_byte(njt_connection_t *c) {
    u_char buf[1];
    njt_int_t n;
    njt_err_t err;

    n = c->recv(c,buf,1);

    err = njt_socket_errno;

    njt_log_error(NJT_LOG_DEBUG, c->log, err,
                   "http check upstream recv(): %i, fd: %d",
                   n, c->fd);

    if (n == 1 || (n == -1 && err == NJT_EAGAIN)) {
        return NJT_OK;
    }

    return NJT_ERROR;
}

static void njt_health_check_udp_module_start_check(void *hc_peer_info, interal_update_handler udpate_handler){
    njt_health_check_peer_t         *hc_peer = hc_peer_info;
    njt_peer_connection_t           *peer = &hc_peer->peer;
    njt_connection_t                *c;  
    njt_health_check_conf_t         *hhccf = hc_peer->hhccf;
    njt_int_t                       rc;

    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                    "health check connect to peer of %V.", peer->name);
    peer->type = SOCK_DGRAM;
    rc = njt_event_connect_peer(peer);
    if (rc == NJT_ERROR || rc == NJT_DECLINED || rc == NJT_BUSY) {
        njt_log_error(NJT_LOG_WARN, njt_cycle->log, 0,
                        "health check connect to peer of %V errror.", peer->name);

        udpate_handler(hc_peer, NJT_ERROR);

        return;
    }

    c = peer->connection;
    c->data = hc_peer;
    c->pool = hc_peer->pool;
    c->log->connection = c->number;

    njt_log_error(NJT_LOG_DEBUG, c->log, 0,
        "http peer check, peerid:%d ref_count=%d", hc_peer->peer_id, hhccf->ref_count);

    c->write->handler = njt_health_check_udp_module_write_handler;
    c->read->handler = njt_health_check_udp_module_read_handler;

    if(rc == NJT_AGAIN){
        if (hhccf->timeout){
            njt_add_timer(c->write, hhccf->timeout);
        }
    }

    if(rc == NJT_OK){
        njt_health_check_udp_module_write_handler(c->write);
    }

    return;
}


static void *njt_health_check_udp_module_create_ctx(void *hhccf_info, void *api_data_info){
    njt_health_check_conf_t                     *hhccf = hhccf_info;
    njt_health_check_udp_module_conf_ctx_t      *udp_ctx;
    njt_health_check_api_data_t                 *api_data = api_data_info;
    njt_int_t                                   rc;

    if (hhccf == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "health check alloc udp_ctx error ");
        return NULL;
    }

    udp_ctx = njt_pcalloc(hhccf->pool, sizeof(njt_health_check_udp_module_conf_ctx_t));
    if (udp_ctx == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "health check alloc udp_ctx error ");
        return NULL;
    }

    udp_ctx->checker = &udp_checker;

    rc = njt_health_check_udp_match_block(api_data, hhccf, udp_ctx);
    if (rc != HC_SUCCESS) {
        return NULL;
    }

    return udp_ctx;
}


/*
    by zhaokang
    stream : {
        "send"      : "xxx",
        "expect" : "yyy"
    }
*/
char* njt_hex2bin(njt_str_t *d, njt_str_t *s, int count);
static njt_int_t 
njt_health_check_udp_match_block(njt_health_check_api_data_t *api_data, njt_health_check_conf_t *hhccf,
        njt_health_check_udp_module_conf_ctx_t *udp_ctx) {
    njt_health_check_udp_match_t        *match;
    njt_str_t                           val;
    char                                *p = NULL;

    udp_ctx->match = njt_pcalloc(hhccf->pool, sizeof(njt_health_check_udp_match_t)); 
    if (udp_ctx->match == NULL) {
        njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "stream match create error");
        return HC_SERVER_ERROR;
    }

    match = udp_ctx->match;
    if (api_data == NULL || api_data->hc_data == NULL || !api_data->hc_data->is_stream_set
        || api_data->hc_data->stream->send.len < 1) {
        njt_str_null(&match->send);
        njt_str_null(&udp_ctx->send);
    } else {
        val = api_data->hc_data->stream->send;
        udp_ctx->send.data = njt_pcalloc(hhccf->pool, val.len);
        udp_ctx->send.len  = val.len;
        njt_memcpy(udp_ctx->send.data, val.data, val.len);

        match->send.data = njt_pcalloc(hhccf->pool, val.len);
        match->send.len  = val.len;
        p = njt_hex2bin(&match->send, &udp_ctx->send, val.len);
        if(NULL ==p) {
            njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "stream->send value is invalid");
            return HC_BODY_ERROR;
        }
        match->send.len = p - (char *)match->send.data;
    }

    if (api_data == NULL || api_data->hc_data == NULL || !api_data->hc_data->is_stream_set 
        || api_data->hc_data->stream->expect.len < 1) {
        njt_str_null(&match->expect);
        njt_str_null(&udp_ctx->expect);
    } else {
        val = api_data->hc_data->stream->expect;
        udp_ctx->expect.data = njt_pcalloc(hhccf->pool, val.len);
        udp_ctx->expect.len  = val.len;
        njt_memcpy(udp_ctx->expect.data, val.data, val.len);

        match->expect.data = njt_pcalloc(hhccf->pool, val.len);
        match->expect.len  = val.len;
        p = njt_hex2bin(&match->expect, &udp_ctx->expect, val.len);
        if(NULL ==p) {
            njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "stream->send value is invalid");
            return HC_BODY_ERROR;
        }
        match->expect.len = p - (char *)match->expect.data;
    }

    return HC_SUCCESS;
}


static void njt_health_check_udp_module_read_handler(njt_event_t *rev) {
    njt_connection_t                   *c;
    njt_health_check_peer_t     *hc_peer;
    njt_int_t                           rc;
    njt_health_check_udp_module_conf_ctx_t *cf_ctx;
    njt_health_check_conf_t     *hhccf;

    c = rev->data;
    hc_peer = c->data;
    hhccf = hc_peer->hhccf;
    cf_ctx = hhccf->ctx;

    if (rev->timedout) {
        if(cf_ctx->match == NULL || cf_ctx->match->expect.len == 0){
            rc = NJT_OK;
            njt_log_error(NJT_LOG_DEBUG, c->log, 0,
                        "read action for health check timeout");
        } else {
            /*log the case and update the peer status.*/
            rc = NJT_ERROR;
            njt_log_error(NJT_LOG_WARN, c->log, 0,
                       "read action for health check timeout");
        }

        njt_health_check_udp_module_free_peer_resource(hc_peer, rc);
        return;
    }
    if (hc_peer->hhccf->disable) {
        if (hc_peer->peer.connection) {
            njt_health_check_close_udp_connection(hc_peer->peer.connection);
        }
        return;
    }

    if (rev->timer_set) {
        njt_del_timer(rev);
    }

    rc = cf_ctx->checker->read_handler(rev);
    if (rc == NJT_ERROR) {
        /*log the case and update the peer status.*/
        njt_log_error(NJT_LOG_WARN, c->log, 0,
                       "read action error for health check");
        njt_health_check_udp_module_free_peer_resource(hc_peer, NJT_ERROR);
        return;
    } else if (rc == NJT_DONE || rc == NJT_OK) { //TODO
        njt_health_check_udp_module_free_peer_resource(hc_peer, rc);
        return;
    } else {
        /*AGAIN*/
    }

    if (!rev->timer_set) {
        njt_add_timer(rev, hhccf->timeout);
    }

    return;
}


static void njt_health_check_udp_module_write_handler(njt_event_t *wev) {
    njt_connection_t                   *c;
    njt_health_check_peer_t     *hc_peer;
    njt_int_t                           rc;
    njt_health_check_udp_module_conf_ctx_t *cf_ctx;
    njt_health_check_conf_t     *hhccf;

    c = wev->data;
    hc_peer = c->data;
    hhccf = hc_peer->hhccf;
    cf_ctx = hhccf->ctx;

    if (wev->timedout) {
        /*log the case and update the peer status.*/
        njt_log_error(NJT_LOG_WARN, c->log, 0,
                       "write action for health check timeout");
        njt_health_check_udp_module_free_peer_resource(hc_peer, NJT_ERROR);
        return;
    }

    if (wev->timer_set) {
        njt_del_timer(wev);
    }
    if (hc_peer->hhccf->disable) {
        if (hc_peer->peer.connection) {
            njt_health_check_close_udp_connection(hc_peer->peer.connection);
        }

        return;
    }

    rc = cf_ctx->checker->write_handler(wev);
    wev->handler = njt_health_check_udp_dummy_handler;
    if (rc == NJT_ERROR) {

        /*log the case and update the peer status.*/
        njt_log_error(NJT_LOG_WARN, c->log, 0,
                       "write action error for health check");
        njt_health_check_udp_module_free_peer_resource(hc_peer, NJT_ERROR);
        return;
    } else if (rc == NJT_DONE || rc == NJT_OK) {
        //should handle read
        njt_add_timer(hc_peer->peer.connection->read, hhccf->timeout);

        if (c->read->ready) {
            njt_post_event(c->read, &njt_posted_events);
        }
    } else {
        /*AGAIN*/
    }

    if (!wev->timer_set) {
        njt_add_timer(wev, hhccf->timeout);
    }

    return;
}


static void njt_health_check_udp_module_free_peer_resource(njt_health_check_peer_t *hc_peer, njt_int_t status) {
    njt_health_check_conf_t *hhccf = hc_peer->hhccf;

    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
        "free peer pool : upstream = %V   ref_count = %d",
        hc_peer->peer.name, hhccf->ref_count);
    
    if (hc_peer->peer.connection) {
        njt_health_check_close_udp_connection(hc_peer->peer.connection);
    }

    hc_peer->update_handler(hc_peer, status);

   return;
}

static void njt_health_check_close_udp_connection(njt_connection_t *c) {
    c->destroyed = 1;
    njt_close_connection(c);

    return;
}


static void njt_health_check_udp_dummy_handler(njt_event_t *ev) {
    njt_log_error(NJT_LOG_DEBUG_EVENT, ev->log, 0,
                        "stream health check dummy handler");
}

inline static njt_int_t 
njt_health_check_udp_init_buf(njt_connection_t *c){
    njt_health_check_peer_t                 *hc_peer;
    njt_health_check_udp_module_conf_ctx_t  *udp_ctx;
    njt_health_check_conf_t                 *hhccf;
    njt_health_check_udp_match_t            *match;
    njt_uint_t                              size;

    hc_peer = c->data;
    hhccf = hc_peer->hhccf;
    udp_ctx = hhccf->ctx;
    match = udp_ctx->match;

    //plus max is 0x40000LL
    size = njt_pagesize;
    if(hc_peer->recv_buf == NULL){
        size = match->expect.len;
        hc_peer->recv_buf = njt_create_temp_buf(hc_peer->pool, size);
        if(hc_peer->recv_buf == NULL){
            njt_log_error(NJT_LOG_EMERG, c->log, 0,"cannot alloc ngx_buf_t in check match all");
            return NJT_ERROR;
        }

        hc_peer->recv_buf->last = hc_peer->recv_buf->pos = hc_peer->recv_buf->start;
    }

    return NJT_OK;
}


static njt_int_t 
njt_health_check_udp_match_all(njt_connection_t *c){
    njt_health_check_peer_t        *hc_peer;
    njt_buf_t                             *b;
    ssize_t                                n, size;
    njt_int_t                              rc;

    njt_health_check_udp_module_conf_ctx_t    *udp_ctx;
    njt_health_check_conf_t        *hhccf;
    njt_health_check_udp_match_t                    *match;

    hc_peer = c->data;
    hhccf = hc_peer->hhccf;
    udp_ctx = hhccf->ctx;
    match = udp_ctx->match;

    rc = njt_health_check_udp_init_buf(c);
    if(rc != NJT_OK){
        return rc;
    }

    b = hc_peer->recv_buf;
    size = b->end - b->last;
    n = c->recv(c, b->last, size);
    if (n > 0) {
        b->last += n;
        /*link chain buffer*/
        if (b->last == b->end) {

            if(njt_strncmp(match->expect.data, hc_peer->recv_buf->start,
                            match->expect.len) == 0){
                return NJT_OK;
            }
        }
    }
    if (n == NJT_AGAIN) {
        if (njt_handle_read_event(c->read, 0) != NJT_OK) {
            njt_log_error(NJT_LOG_ERR, c->log, 0,"read event handle error for health check");
            return NJT_ERROR;
        }
        return NJT_AGAIN;
    }
    if (n == NJT_ERROR) {
        njt_log_error(NJT_LOG_ERR, c->log, 0,"read error for health check");
    }

    return NJT_ERROR;
}


static njt_int_t njt_health_check_udp_recv_handler(njt_event_t *rev) {
    njt_connection_t                            *c;
    njt_health_check_peer_t                     *hc_peer;
    njt_health_check_udp_module_conf_ctx_t      *udp_ctx;
    njt_health_check_conf_t                     *hhccf;
    njt_health_check_udp_match_t                *match;

    c = rev->data;
    hc_peer = c->data;
    hhccf = hc_peer->hhccf;
    udp_ctx = hhccf->ctx;
    match = udp_ctx->match;
    if( match == NULL || match->expect.len == 0 ) {
        return njt_health_check_udp_module_peek_one_byte(c);
    }


    return njt_health_check_udp_match_all(c);
}


static u_char* test_str=(u_char*)"njet health check";
static njt_int_t njt_health_check_udp_send_handler(njt_event_t *wev) {
    njt_connection_t                    *c;
    njt_int_t                            rc;
    njt_health_check_peer_t             *hc_peer;
    njt_uint_t                           size;
    njt_int_t                            n;

    njt_health_check_udp_module_conf_ctx_t  *stream_ctx;
    njt_health_check_conf_t                 *hhccf;
    njt_health_check_udp_match_t            *match;

    c = wev->data;
    hc_peer = c->data;
    rc = NJT_OK;

    hhccf = hc_peer->hhccf;
    stream_ctx = hhccf->ctx;
    match = stream_ctx->match;

    if(match == NULL || match->send.len == 0){
        n = c->send(c,test_str,18);
        if(n<=0){
            rc = NJT_ERROR;
        }

        return rc;
    }

    if (hc_peer->send_buf == NULL) {
        hc_peer->send_buf = njt_pcalloc(hc_peer->pool, sizeof(njt_buf_t));
        if (hc_peer->send_buf == NULL) {
            /*log the send buf allocation failure*/
            njt_log_error(NJT_LOG_ERR, c->log, 0,
                           "malloc failure of the send buffer for health check.");
            return NJT_ERROR;
        }

        hc_peer->send_buf->pos = match->send.data;
        hc_peer->send_buf->last = hc_peer->send_buf->pos + match->send.len;
    }
    size = hc_peer->send_buf->last - hc_peer->send_buf->pos;
    n = c->send(c, hc_peer->send_buf->pos,size);
    if (n == NJT_ERROR) {
        return NJT_ERROR;
    }
    if (n > 0) {
        hc_peer->send_buf->pos += n;
        if (n == (njt_int_t)size) {
            wev->handler = njt_health_check_udp_dummy_handler;
            if (njt_handle_write_event(wev, 0) != NJT_OK) {
                /*LOG the failure*/
                njt_log_error(NJT_LOG_ERR, c->log, 0,
                               "write event handle error for health check");
                return NJT_ERROR;
            }
            return NJT_DONE;
        }
    }
    return rc;
}