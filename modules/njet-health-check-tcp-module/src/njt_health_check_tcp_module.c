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
#include "njt_health_check_tcp_module.h"


static void njt_health_check_tcp_module_start_check(void *hc_peer_info, interal_update_handler udpate_handler);
static void *njt_health_check_tcp_module_create_ctx(void *hhccf_info, void *api_data_info);
static void njt_health_check_tcp_dummy_handler(njt_event_t *ev);
static njt_int_t njt_health_check_tcp_module_init_process(njt_cycle_t *cycle);
static njt_int_t njt_health_check_tcp_module_test_connect(njt_connection_t *c);
static void njt_health_check_close_tcp_connection(njt_connection_t *c);
static void njt_health_check_tcp_module_free_peer_resource(njt_health_check_peer_t *hc_peer, njt_int_t status);

static njt_int_t njt_health_check_tcp_send_handler(njt_event_t *wev);
static njt_int_t njt_health_check_tcp_recv_handler(njt_event_t *rev);
static void njt_health_check_tcp_module_write_handler(njt_event_t *wev);
static void njt_health_check_tcp_module_read_handler(njt_event_t *rev);
static njt_int_t 
njt_health_check_tcp_match_block(njt_health_check_api_data_t *api_data, njt_health_check_conf_t *hhccf,
        njt_health_check_tcp_module_conf_ctx_t *tcp_ctx);
static void njt_health_check_tcp_module_connect_handler(njt_event_t *ev);
static njt_int_t
njt_health_check_tcp_ssl_init_connection(njt_connection_t *c, njt_health_check_peer_t *hc_peer);

extern njt_cycle_t *njet_master_cycle;

static njt_http_module_t njt_health_check_tcp_module_ctx = {
        NULL,                                   /* preconfiguration */
        NULL,          /* postconfiguration */

        NULL,                                   /* create main configuration */
        NULL,                                  /* init main configuration */

        NULL,                                  /* create server configuration */
        NULL,                                  /* merge server configuration */

        NULL,                                   /* create location configuration */
        NULL                                    /* merge location configuration */
};

njt_module_t njt_health_check_tcp_module = {
        NJT_MODULE_V1,
        &njt_health_check_tcp_module_ctx,      /* module context */
        NULL,                                   /* module directives */
        NJT_HTTP_MODULE,                        /* module type */
        NULL,                                   /* init master */
        NULL,                                   /* init module */
        njt_health_check_tcp_module_init_process,                                   /* init process */
        NULL,                                   /* init thread */
        NULL,                                   /* exit thread */
        NULL,                                   /* exit process */
        NULL,                                   /* exit master */
        NJT_MODULE_V1_PADDING
};


static njt_health_check_checker_t tcp_checker = {
        njt_health_check_tcp_send_handler,
        njt_health_check_tcp_recv_handler,
        NULL,
        NULL
};


static njt_int_t njt_health_check_tcp_module_init_process(njt_cycle_t *cycle){
    njt_health_check_reg_info_t             h;

    njt_str_t  server_type = njt_string("tcp");
    njt_memzero(&h, sizeof(njt_health_check_reg_info_t));

    h.server_type = &server_type;
    h.create_ctx = njt_health_check_tcp_module_create_ctx;
    h.start_check = njt_health_check_tcp_module_start_check;

    njt_health_check_reg_handler(&h);

    return NJT_OK;
}


static njt_int_t
njt_health_check_tcp_module_peek_one_byte(njt_connection_t *c) {
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

static void njt_health_check_tcp_module_start_check(void *hc_peer_info, interal_update_handler udpate_handler){
    njt_health_check_peer_t         *hc_peer = hc_peer_info;
    njt_peer_connection_t           *peer = &hc_peer->peer;
    njt_connection_t                *c;  
    njt_health_check_conf_t         *hhccf = hc_peer->hhccf;
    njt_int_t                       rc;

    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                    "health check connect to peer of %V.", peer->name);

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

    c->write->handler = njt_health_check_tcp_module_connect_handler;
    c->read->handler = njt_health_check_tcp_module_connect_handler;

    if(rc == NJT_AGAIN){
        if (hhccf->timeout){
            njt_add_timer(c->write, hhccf->timeout);
        }
    }

    if(rc == NJT_OK){
#if (NJT_HTTP_SSL)
        if (hhccf->ssl.ssl_enable && hhccf->ssl.ssl->ctx &&
            c->ssl == NULL) {
            rc = njt_health_check_tcp_ssl_init_connection(c, hc_peer);
            if (rc == NJT_ERROR) {
                if (hc_peer->peer.connection) {
                    njt_health_check_close_tcp_connection(hc_peer->peer.connection);
                }
                udpate_handler(hc_peer, NJT_ERROR);
            }
            return;
        }
#endif
        c->write->handler = njt_health_check_tcp_module_write_handler;
        c->read->handler = njt_health_check_tcp_module_read_handler;

        njt_health_check_tcp_module_write_handler(c->write);
    }

    return;
}


static void njt_health_check_tcp_module_connect_handler(njt_event_t *ev)
{
    njt_connection_t                    *c;
    njt_health_check_peer_t             *hc_peer;
    njt_int_t                           rc;
    njt_health_check_conf_t             *hhccf;

    c = ev->data;
    hc_peer = c->data;
    hhccf = hc_peer->hhccf;

    if (ev->timedout) {
        njt_health_check_tcp_module_free_peer_resource(hc_peer, NJT_ERROR);
        return;
    }
    if (hc_peer->hhccf->disable) {
        if (hc_peer->peer.connection) {
            njt_health_check_close_tcp_connection(hc_peer->peer.connection);
        }
        return;
    }

    if (ev->timer_set) {
        njt_del_timer(ev);
    }

    if (njt_health_check_tcp_module_test_connect(c) != NJT_OK) {
        njt_health_check_tcp_module_free_peer_resource(hc_peer, NJT_ERROR);
        return;
    }

#if (NJT_HTTP_SSL)
    if (hhccf->ssl.ssl_enable && hhccf->ssl.ssl->ctx &&
        c->ssl == NULL) {
        rc = njt_health_check_tcp_ssl_init_connection(c, hc_peer);
        if (rc == NJT_ERROR) {
            njt_health_check_tcp_module_free_peer_resource(hc_peer, NJT_ERROR);
        }
        return;
    }
#endif
    c->write->handler = njt_health_check_tcp_module_write_handler;
    c->read->handler = njt_health_check_tcp_module_read_handler;

    njt_health_check_tcp_module_write_handler(c->write);
}



static void *njt_health_check_tcp_module_create_ctx(void *hhccf_info, void *api_data_info){
    njt_health_check_conf_t                     *hhccf = hhccf_info;
    njt_health_check_tcp_module_conf_ctx_t      *tcp_ctx;
    njt_health_check_api_data_t                 *api_data = api_data_info;
    njt_int_t                                   rc;

    if (hhccf == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "health check alloc tcp_ctx error ");
        return NULL;
    }

    tcp_ctx = njt_pcalloc(hhccf->pool, sizeof(njt_health_check_tcp_module_conf_ctx_t));
    if (tcp_ctx == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "health check alloc tcp_ctx error ");
        return NULL;
    }

    tcp_ctx->checker = &tcp_checker;

    rc = njt_health_check_tcp_match_block(api_data, hhccf, tcp_ctx);
    if (rc != HC_SUCCESS) {
        return NULL;
    }

    return tcp_ctx;
}


#if (NJT_STREAM_SSL)

static njt_int_t
njt_health_check_tcp_ssl_name(njt_connection_t *c, njt_health_check_peer_t *hc_peer) {
    u_char                              *p, *last;
    njt_str_t                           name;
    njt_health_check_conf_t      *hhccf;
    njt_stream_upstream_srv_conf_t      *uscf;

    hhccf = hc_peer->hhccf;

    if (hhccf->ssl.ssl_name.len) {
        name = hhccf->ssl.ssl_name;
    } else {
        uscf = njt_health_check_find_stream_upstream_by_name(njet_master_cycle, &hhccf->upstream_name);
        if (uscf == NULL) {
            njt_log_error(NJT_LOG_ERR, c->log, 0,
                              "stream hc ssl name uscf has not exit");
            return NJT_ERROR;
        }
        name = uscf->host;
    }
    if (name.len == 0) {
        goto done;
    }

    /*
     * ssl name here may contain port, notably if derived from $proxy_host
     * or $http_host; we have to strip it
     */

    p = name.data;
    last = name.data + name.len;

    if (*p == '[') {
        p = njt_strlchr(p, last, ']');

        if (p == NULL) {
            p = name.data;
        }
    }

    p = njt_strlchr(p, last, ':');

    if (p != NULL) {
        name.len = p - name.data;
    }

    if (!hhccf->ssl.ssl_server_name) {
        goto done;
    }

#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME

    /* as per RFC 6066, literal IPv4 and IPv6 addresses are not permitted */

    if (name.len == 0 || *name.data == '[') {
        goto done;
    }

    if (njt_inet_addr(name.data, name.len) != INADDR_NONE) {
        goto done;
    }

    /*
     * SSL_set_tlsext_host_name() needs a null-terminated string,
     * hence we explicitly null-terminate name here
     */

    p = njt_pnalloc(c->pool, name.len + 1);
    if (p == NULL) {
        return NJT_ERROR;
    }

    (void) njt_cpystrn(p, name.data, name.len + 1);

    name.data = p;

    njt_log_error(NJT_LOG_DEBUG, c->log, 0, "upstream SSL server name: \"%s\"", name.data);

    if (SSL_set_tlsext_host_name(c->ssl->connection,
                                 (char *) name.data)
        == 0) {
        njt_ssl_error(NJT_LOG_ERR, c->log, 0,
                      "SSL_set_tlsext_host_name(\"%s\") failed", name.data);
        return NJT_ERROR;
    }

#endif

    done:

    hc_peer->ssl_name = name;

    return NJT_OK;
}
static njt_int_t
njt_health_check_tcp_ssl_handshake(njt_connection_t *c, njt_health_check_peer_t *hc_peer) {
    long rc;
    njt_health_check_conf_t *hhccf;

    hhccf = hc_peer->hhccf;

    if (c->ssl->handshaked) {

        if (hhccf->ssl.ssl_verify) {
            rc = SSL_get_verify_result(c->ssl->connection);
            if (rc != X509_V_OK) {
                njt_log_error(NJT_LOG_ERR, c->log, 0,
                              "upstream SSL certificate verify error: (%l:%s)",
                              rc, X509_verify_cert_error_string(rc));
                goto failed;
            }

            if (njt_ssl_check_host(c, &hc_peer->ssl_name) != NJT_OK) {
                njt_log_error(NJT_LOG_ERR, c->log, 0,
                              "hc SSL certificate does not match \"%V\"",
                              &hc_peer->ssl_name);
                goto failed;
            }
        }
        // hhccf->ref_count++;
        njt_log_error(NJT_LOG_DEBUG, c->log, 0,
            "====stream peer check ssl, peerid:%d ref_count=%d", hc_peer->peer_id, hhccf->ref_count);
        hc_peer->peer.connection->write->handler = njt_health_check_tcp_module_write_handler;
        hc_peer->peer.connection->read->handler = njt_health_check_tcp_module_read_handler;

        /*NJT_AGAIN or NJT_OK*/
        if (hhccf->timeout) {
            njt_add_timer(hc_peer->peer.connection->write, hhccf->timeout);
            njt_add_timer(hc_peer->peer.connection->read, hhccf->timeout);
        }
        return NJT_OK;
    }

    if (c->write->timedout) {
        return NJT_ERROR;
    }

failed:
    return NJT_ERROR;
}


static void
njt_health_check_tcp_ssl_handshake_handler(njt_connection_t *c) {
    njt_health_check_peer_t *hc_peer;
    njt_int_t rc;

    hc_peer = c->data;

    rc = njt_health_check_tcp_ssl_handshake(c, hc_peer);
    if (rc != NJT_OK) {
        njt_health_check_tcp_module_free_peer_resource(hc_peer, NJT_ERROR);
    }
}


static njt_int_t njt_health_check_tcp_module_test_connect(njt_connection_t *c) {
    int err;
    socklen_t len;

#if (NJT_HAVE_KQUEUE)
    if (njt_event_flags & NJT_USE_KQUEUE_EVENT)  {
        if (c->write->pending_eof || c->read->pending_eof) {
            if (c->write->pending_eof) {
                err = c->write->kq_errno;

            } else {
                err = c->read->kq_errno;
            }

            c->log->action = "connecting to upstream";
            (void) njt_connection_error(c, err,
                                    "kevent() reported that connect() failed");
            return NJT_ERROR;
        }

    } else
#endif
    {
        err = 0;
        len = sizeof(int);

        /*
         * BSDs and Linux return 0 and set a pending error in err
         * Solaris returns -1 and sets errno
         */

        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len)
            == -1) {
            err = njt_socket_errno;
        }

        if (err) {
            c->log->action = "connecting to hc";
            (void) njt_connection_error(c, err, "connect() failed");
            return NJT_ERROR;
        }
    }

    return NJT_OK;
}


static njt_int_t
njt_health_check_tcp_ssl_init_connection(njt_connection_t *c, njt_health_check_peer_t *hc_peer) {
    njt_int_t rc;
    njt_health_check_conf_t *hhccf;

    hhccf = hc_peer->hhccf;

    if (njt_ssl_create_connection(hhccf->ssl.ssl, c,
                                  NJT_SSL_BUFFER | NJT_SSL_CLIENT) != NJT_OK) {
        njt_log_error(NJT_LOG_DEBUG, c->log, 0, "ssl init create connection for health check error ");
        return NJT_ERROR;
    }

    c->sendfile = 0;

    if (hhccf->ssl.ssl_server_name || hhccf->ssl.ssl_verify) {
        if (njt_health_check_tcp_ssl_name(c, hc_peer) != NJT_OK) {
            njt_log_error(NJT_LOG_DEBUG, c->log, 0, "ssl init check ssl name for health check error ");
            return NJT_ERROR;
        }
    }

    c->log->action = "SSL handshaking to hc";

    rc = njt_ssl_handshake(c);
    if (rc == NJT_AGAIN) {
        if (!c->write->timer_set) {
            njt_add_timer(c->write, hhccf->timeout);
        }

        c->ssl->handler = njt_health_check_tcp_ssl_handshake_handler;
        return NJT_OK;
    }

    return njt_health_check_tcp_ssl_handshake(c, hc_peer);
}
#endif


/*
    by zhaokang
    stream : {
        "send"      : "xxx",
        "expect" : "yyy"
    }
*/
char* njt_hex2bin(njt_str_t *d, njt_str_t *s, int count);
static njt_int_t 
njt_health_check_tcp_match_block(njt_health_check_api_data_t *api_data, njt_health_check_conf_t *hhccf,
        njt_health_check_tcp_module_conf_ctx_t *tcp_ctx) {
    njt_health_check_tcp_match_t                 *match;
    njt_str_t                          val;
    char *p = NULL;


    tcp_ctx->match = njt_pcalloc(hhccf->pool, sizeof(njt_health_check_tcp_match_t)); 
    if (tcp_ctx->match == NULL) {
        njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "stream match create error");
        return HC_SERVER_ERROR;
    }

    match = tcp_ctx->match;
    if (api_data == NULL || api_data->hc_data == NULL || !api_data->hc_data->is_stream_set
        || api_data->hc_data->stream->send.len < 1) {
        njt_str_null(&match->send);
        njt_str_null(&tcp_ctx->send);
    } else {
        val = api_data->hc_data->stream->send;
        tcp_ctx->send.data = njt_pcalloc(hhccf->pool, val.len);
        tcp_ctx->send.len  = val.len;
        njt_memcpy(tcp_ctx->send.data, val.data, val.len);

        match->send.data = njt_pcalloc(hhccf->pool, val.len);
        match->send.len  = val.len;
        p = njt_hex2bin(&match->send, &tcp_ctx->send, val.len);
        if(NULL ==p) {
            njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "stream->send value is invalid");
            return HC_BODY_ERROR;
        }
        match->send.len = p - (char *)match->send.data;
    }

    if (api_data == NULL || api_data->hc_data == NULL || !api_data->hc_data->is_stream_set 
        || api_data->hc_data->stream->expect.len < 1) {
        njt_str_null(&match->expect);
        njt_str_null(&tcp_ctx->expect);
    } else {
        val = api_data->hc_data->stream->expect;
        tcp_ctx->expect.data = njt_pcalloc(hhccf->pool, val.len);
        tcp_ctx->expect.len  = val.len;
        njt_memcpy(tcp_ctx->expect.data, val.data, val.len);

        match->expect.data = njt_pcalloc(hhccf->pool, val.len);
        match->expect.len  = val.len;
        p = njt_hex2bin(&match->expect, &tcp_ctx->expect, val.len);
        if(NULL ==p) {
            njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "stream->send value is invalid");
            return HC_BODY_ERROR;
        }
        match->expect.len = p - (char *)match->expect.data;
    }

    return HC_SUCCESS;
}


static void njt_health_check_tcp_module_read_handler(njt_event_t *rev) {
    njt_connection_t                    *c;
    njt_health_check_peer_t             *hc_peer;
    njt_int_t                           rc;
    njt_health_check_tcp_module_conf_ctx_t *cf_ctx;
    njt_health_check_conf_t             *hhccf;

    c = rev->data;
    hc_peer = c->data;
    hhccf = hc_peer->hhccf;
    cf_ctx = hhccf->ctx;

    if (rev->timedout) {
        njt_health_check_tcp_module_free_peer_resource(hc_peer, NJT_ERROR);
        return;
    }
    if (hc_peer->hhccf->disable) {
        if (hc_peer->peer.connection) {
            njt_health_check_close_tcp_connection(hc_peer->peer.connection);
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
        njt_health_check_tcp_module_free_peer_resource(hc_peer, NJT_ERROR);
        return;
    } else if (rc == NJT_DONE || rc == NJT_OK) { //TODO
        njt_health_check_tcp_module_free_peer_resource(hc_peer, rc);
        return;
    } else {
        /*AGAIN*/
    }

    if (!rev->timer_set) {
        njt_add_timer(rev, hhccf->timeout);
    }

    return;
}


static void njt_health_check_tcp_module_write_handler(njt_event_t *wev) {
    njt_connection_t                   *c;
    njt_health_check_peer_t     *hc_peer;
    njt_int_t                           rc;
    njt_health_check_tcp_module_conf_ctx_t *cf_ctx;
    njt_health_check_conf_t     *hhccf;

    c = wev->data;
    hc_peer = c->data;
    hhccf = hc_peer->hhccf;
    cf_ctx = hhccf->ctx;

    if (wev->timedout) {
        /*log the case and update the peer status.*/
        njt_log_error(NJT_LOG_WARN, c->log, 0,
                       "write action for health check timeout");
        njt_health_check_tcp_module_free_peer_resource(hc_peer, NJT_ERROR);
        return;
    }

    if (wev->timer_set) {
        njt_del_timer(wev);
    }
    if (hc_peer->hhccf->disable) {
        if (hc_peer->peer.connection) {
            njt_health_check_close_tcp_connection(hc_peer->peer.connection);
        }

        return;
    }

    rc = cf_ctx->checker->write_handler(wev);
    wev->handler = njt_health_check_tcp_dummy_handler;
    if (rc == NJT_ERROR) {

        /*log the case and update the peer status.*/
        njt_log_error(NJT_LOG_WARN, c->log, 0,
                       "write action error for health check");
        njt_health_check_tcp_module_free_peer_resource(hc_peer, NJT_ERROR);
        return;
    } else if (rc == NJT_DONE || rc == NJT_OK) {
        if (cf_ctx->match == NULL || cf_ctx->match->expect.len == 0){
            njt_health_check_tcp_module_free_peer_resource(hc_peer, rc);
            return;
        }else{
            //should handle read
            njt_add_timer(hc_peer->peer.connection->read, hhccf->timeout);

            if (c->read->ready) {
                njt_post_event(c->read, &njt_posted_events);
            }
        }
    } else {
        /*AGAIN*/
    }

    if (!wev->timer_set) {
        njt_add_timer(wev, hhccf->timeout);
    }

    return;
}


static void njt_health_check_tcp_module_free_peer_resource(njt_health_check_peer_t *hc_peer, njt_int_t status) {
    njt_health_check_conf_t *hhccf = hc_peer->hhccf;

    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
        "free peer pool : upstream = %V   ref_count = %d",
        hc_peer->peer.name, hhccf->ref_count);
    
    if (hc_peer->peer.connection) {
        njt_health_check_close_tcp_connection(hc_peer->peer.connection);
    }

    hc_peer->update_handler(hc_peer, status);

   return;
}

static void njt_health_check_close_tcp_connection(njt_connection_t *c) {

#if (NJT_STREAM_SSL)
    if (c->ssl) {
        c->ssl->no_wait_shutdown = 1;
        c->ssl->no_send_shutdown = 1;

        (void) njt_ssl_shutdown(c);
    }
#endif

    c->destroyed = 1;
    njt_close_connection(c);

    return;
}


static void njt_health_check_tcp_dummy_handler(njt_event_t *ev) {
    njt_log_error(NJT_LOG_DEBUG_EVENT, ev->log, 0,
                        "stream health check dummy handler");
}

inline static njt_int_t 
njt_health_check_tcp_init_buf(njt_connection_t *c){
    njt_health_check_peer_t                 *hc_peer;
    njt_health_check_tcp_module_conf_ctx_t  *tcp_ctx;
    njt_health_check_conf_t                 *hhccf;
    njt_health_check_tcp_match_t            *match;
    njt_uint_t                              size;

    hc_peer = c->data;
    hhccf = hc_peer->hhccf;
    tcp_ctx = hhccf->ctx;
    match = tcp_ctx->match;

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
njt_health_check_tcp_match_all(njt_connection_t *c){
    njt_health_check_peer_t        *hc_peer;
    njt_buf_t                             *b;
    ssize_t                                n, size;
    njt_int_t                              rc;

    njt_health_check_tcp_module_conf_ctx_t    *tcp_ctx;
    njt_health_check_conf_t        *hhccf;
    njt_health_check_tcp_match_t                    *match;

    hc_peer = c->data;
    hhccf = hc_peer->hhccf;
    tcp_ctx = hhccf->ctx;
    match = tcp_ctx->match;

    rc = njt_health_check_tcp_init_buf(c);
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


static njt_int_t njt_health_check_tcp_recv_handler(njt_event_t *rev) {
    njt_connection_t                            *c;
    njt_health_check_peer_t                     *hc_peer;
    njt_health_check_tcp_module_conf_ctx_t      *tcp_ctx;
    njt_health_check_conf_t                     *hhccf;
    njt_health_check_tcp_match_t                *match;

    c = rev->data;
    hc_peer = c->data;
    hhccf = hc_peer->hhccf;
    tcp_ctx = hhccf->ctx;
    match = tcp_ctx->match;
    if( match == NULL || match->expect.len == 0 ) {
        return njt_health_check_tcp_module_peek_one_byte(c);
    }


    return njt_health_check_tcp_match_all(c);
}


static njt_int_t njt_health_check_tcp_send_handler(njt_event_t *wev) {
    njt_connection_t                    *c;
    njt_int_t                            rc;
    njt_health_check_peer_t             *hc_peer;
    njt_uint_t                           size;
    njt_int_t                            n;

    njt_health_check_tcp_module_conf_ctx_t  *stream_ctx;
    njt_health_check_conf_t                 *hhccf;
    njt_health_check_tcp_match_t            *match;

    c = wev->data;
    hc_peer = c->data;
    rc = NJT_OK;

    hhccf = hc_peer->hhccf;
    stream_ctx = hhccf->ctx;
    match = stream_ctx->match;

    if(match == NULL || match->send.len == 0){
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
            wev->handler = njt_health_check_tcp_dummy_handler;
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