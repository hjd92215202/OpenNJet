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
#include <njt_health_check_parser.h>
#include <njt_health_check_module.h>
#include <njt_health_check_util.h>
#include "njt_health_check_http_module.h"



#define  njt_health_check_http_match_CONTAIN       0
#define  njt_health_check_http_match_NOT_CONTAIN   1
#define  njt_health_check_http_match_EQUAL         2
#define  njt_health_check_http_match_NOT_EQUAL     4
#define  njt_health_check_http_match_REG_MATCH     8
#define  njt_health_check_http_match_NOT_REG_MATCH 16

#define NJT_HTTP_PARSE_INIT           0
#define NJT_HTTP_PARSE_STATUS_LINE    1
#define NJT_HTTP_PARSE_HEADER         2
#define NJT_HTTP_PARSE_BODY           4


static njt_int_t   njt_health_check_http_module_postconfiguration(njt_conf_t *cf);
static void
njt_health_check_http_module_http_update_status(njt_health_check_peer_t *hc_peer, njt_int_t status);

static void njt_health_check_http_module_start_check(void *hc_peer_info, interal_update_handler update_handler);
static void *njt_health_check_http_module_create_ctx(void *hhccf_info, void *api_data_info);
static void njt_health_check_http_module_only_check_port_handler(njt_event_t *ev);
static void njt_health_check_http_module_free_peer_resource(njt_health_check_peer_t *hc_peer, njt_int_t status);
static njt_int_t
njt_health_check_http_ssl_init_connection(njt_connection_t *c, njt_health_check_peer_t *hc_peer);
static njt_int_t njt_health_check_http_match_block(njt_health_check_api_data_t *api_data, njt_health_check_conf_t *hhccf);
static void njt_health_check_close_http_connection(njt_connection_t *c);
static njt_int_t njt_health_check_http_module_http_write_handler(njt_event_t *wev);
static njt_int_t njt_health_check_http_module_http_read_handler(njt_event_t *rev);
static njt_int_t njt_health_check_http_module_http_process(njt_health_check_peer_t *hc_peer);

static void njt_health_check_http_module_write_handler(njt_event_t *wev);
static void njt_health_check_http_module_read_handler(njt_event_t *rev);
static njt_int_t njt_health_check_http_process_headers(njt_health_check_peer_t *hc_peer);
static njt_int_t njt_health_check_http_process_body(njt_health_check_peer_t *hc_peer);


extern njt_cycle_t *njet_master_cycle;

static njt_http_module_t njt_health_check_http_module_ctx = {
        NULL,                                   /* preconfiguration */
        njt_health_check_http_module_postconfiguration,          /* postconfiguration */

        NULL,                                   /* create main configuration */
        NULL,                                  /* init main configuration */

        NULL,                                  /* create server configuration */
        NULL,                                  /* merge server configuration */

        NULL,                                   /* create location configuration */
        NULL                                    /* merge location configuration */
};

njt_module_t njt_health_check_http_module = {
        NJT_MODULE_V1,
        &njt_health_check_http_module_ctx,      /* module context */
        NULL,                                   /* module directives */
        NJT_HTTP_MODULE,                        /* module type */
        NULL,                                   /* init master */
        NULL,                                   /* init module */
        NULL,                                   /* init process */
        NULL,                                   /* init thread */
        NULL,                                   /* exit thread */
        NULL,                                   /* exit process */
        NULL,                                   /* exit master */
        NJT_MODULE_V1_PADDING
};


static njt_health_check_checker_t http_checker = {
        njt_health_check_http_module_http_write_handler,
        njt_health_check_http_module_http_read_handler,
        njt_health_check_http_module_http_process,
        njt_health_check_http_module_http_update_status
};


static njt_int_t   njt_health_check_http_module_postconfiguration(njt_conf_t *cf){
    njt_health_check_reg_info_t             h;

    njt_str_t  server_type = njt_string("http");
    njt_memzero(&h, sizeof(njt_health_check_reg_info_t));

    h.server_type = &server_type;
    h.create_ctx = njt_health_check_http_module_create_ctx;
    h.start_check = njt_health_check_http_module_start_check;


    njt_health_check_reg_handler(&h);


    return NJT_OK;
}

static void njt_health_check_http_module_free_peer_resource(njt_health_check_peer_t *hc_peer, njt_int_t status) {
    njt_health_check_conf_t *hhccf = hc_peer->hhccf;

    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
        "free peer pool : upstream = %V   ref_count = %d",
        hc_peer->peer.name, hhccf->ref_count);
    
    if (hc_peer->peer.connection) {
        njt_health_check_close_http_connection(hc_peer->peer.connection);
    }

    hc_peer->update_handler(hc_peer, status);

   return;
}



static void njt_health_check_http_module_start_check(void *hc_peer_info, interal_update_handler update_handler){
    njt_health_check_peer_t         *hc_peer = hc_peer_info;
    njt_peer_connection_t           *peer = &hc_peer->peer;
    njt_connection_t                *c;  
    njt_health_check_conf_t         *hhccf = hc_peer->hhccf;
    njt_health_check_http_module_conf_ctx_t    *http_ctx;
    njt_int_t                       rc;

    njt_log_error(NJT_LOG_DEBUG, njt_cycle->log, 0,
                    "health check connect to peer of %V.", &peer->name);

    rc = njt_event_connect_peer(peer);

    if (rc == NJT_ERROR || rc == NJT_DECLINED || rc == NJT_BUSY) {
        njt_log_error(NJT_LOG_WARN, njt_cycle->log, 0,
                        "health check connect to peer of %V errror.", &peer->name);

        update_handler(hc_peer, NJT_ERROR);

        return;
    }

    c = peer->connection;
    c->data = hc_peer;
    c->pool = hc_peer->pool;
    c->log->connection = c->number;

    //if just check port
    http_ctx = (njt_health_check_http_module_conf_ctx_t *)hhccf->ctx;
    if(http_ctx && http_ctx->only_check_port){
        if(NJT_OK == rc){
            if (hc_peer->peer.connection) {
                njt_health_check_close_http_connection(hc_peer->peer.connection);
            }
            update_handler(hc_peer, NJT_OK);
        }else{
            //here just is NJT_AGAIN
            c->write->handler = njt_health_check_http_module_only_check_port_handler;
            c->read->handler = njt_health_check_http_module_only_check_port_handler;

            if (hhccf->timeout) {
                //just add write event
                njt_add_timer(c->write, hhccf->timeout);
            }
        }

        return;
    }


#if (NJT_HTTP_SSL)

    if (hhccf->ssl.ssl_enable && hhccf->ssl.ssl->ctx &&
        c->ssl == NULL) {
        rc = njt_health_check_http_ssl_init_connection(c, hc_peer);
        if (rc == NJT_ERROR) {
            if (hc_peer->peer.connection) {
                njt_health_check_close_http_connection(hc_peer->peer.connection);
            }
            update_handler(hc_peer, NJT_ERROR);
        }
        return;
    }

#endif

    njt_log_error(NJT_LOG_DEBUG, c->log, 0,
        "http peer check, peerid:%d ref_count=%d", hc_peer->peer_id, hhccf->ref_count);

    c->write->handler = njt_health_check_http_module_write_handler;
    c->read->handler = njt_health_check_http_module_read_handler;

    if(rc == NJT_AGAIN){
        if (hhccf->timeout){
            njt_add_timer(c->write, hhccf->timeout);
        }
    }

    if(rc == NJT_OK){
        njt_health_check_http_module_write_handler(c->write);
    }

    return;
}


static void *njt_health_check_http_module_create_ctx(void *hhccf_info, void *api_data_info){
    njt_health_check_conf_t                     *hhccf = hhccf_info;
    njt_health_check_http_module_conf_ctx_t     *http_ctx;
    njt_health_check_api_data_t                 *api_data = api_data_info;
    njt_int_t                                   rc;

    if (hhccf == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "health check alloc http_ctx error ");
        return NULL;
    }

    http_ctx = njt_pcalloc(hhccf->pool, sizeof(njt_health_check_http_module_conf_ctx_t));

    http_ctx->checker = &http_checker;
    if(api_data->hc_data->http->is_only_check_port_set){
        http_ctx->only_check_port = get_health_check_http_only_check_port(api_data->hc_data->http);
    }

    if(http_ctx->only_check_port)
    {
        //do nothing for other param
    }else if(api_data->hc_data->http->uri.len > 0){
        njt_str_copy_pool(hhccf->pool, http_ctx->uri, api_data->hc_data->http->uri, return NULL);
        rc = njt_health_check_http_match_block(api_data, hhccf);
        if (rc != HC_SUCCESS) {
            return NULL;
        }
    }

    return http_ctx;
}


static njt_int_t njt_health_check_http_match_parse_code(njt_str_t *code, njt_health_check_http_match_code_t *match_code) {
    u_char *dash, *last;
    njt_uint_t status;

    last = code->data + code->len;
    dash = njt_strlchr(code->data, last, '-');

    if (dash) {

        status = njt_atoi(code->data, dash - code->data);
        if (status < 100 || status >= 600) {
            return NJT_ERROR;
        }
        match_code->code = status;

        status = njt_atoi(dash + 1, last - dash - 1);
        if (status < 100 || status >= 600) {
            return NJT_ERROR;
        }
        match_code->last_code = status;

        if (match_code->last_code < match_code->code) {
            return NJT_ERROR;
        }

        match_code->single = 0;

    } else {

        status = njt_atoi(code->data, code->len);
        if (status < 100 || status >= 600) {
            return NJT_ERROR;
        }
        match_code->code = status;
        match_code->single = 1;
    }
    return NJT_OK;

}

static njt_regex_t *
njt_health_check_http_match_regex_value(njt_conf_t *cf, njt_str_t *regex) {
#if (NJT_PCRE)
    njt_regex_compile_t rc;
    u_char errstr[NJT_MAX_CONF_ERRSTR];

    njt_memzero(&rc, sizeof(njt_regex_compile_t));


    rc.pattern = *regex;
    rc.err.len = NJT_MAX_CONF_ERRSTR;
    rc.err.data = errstr;
    rc.pool = cf->pool;
    rc.options = NJT_REGEX_CASELESS;


    if (njt_regex_compile(&rc) != NJT_OK) {
        return NULL;
    }

    return rc.regex;

#else

    njt_conf_log_error(NJT_LOG_EMERG, cf, 0,
                       "using regex \"%V\" requires PCRE library",
                       regex);
    return NULL;

#endif
}


static njt_int_t
njt_health_check_http_match_parse_dheaders(njt_array_t *arr, njt_health_check_http_match_t *match, njt_health_check_conf_t *hhccf) {
    njt_uint_t nelts;
    njt_health_check_http_match_header_t *header;
    njt_uint_t i;
    njt_conf_t cf;
    njt_str_t *args;

    cf.pool = hhccf->pool;
    i = 0;
    args = arr->elts;
    nelts = arr->nelts;
    header = njt_array_push(&match->headers);
    if (header == NULL) {
        njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "header array push error.");
        return NJT_ERROR;
    }
    njt_memzero(header, sizeof(njt_health_check_http_match_header_t));

    /*header ! abc;*/
    if (args[i].data != NULL && njt_strncmp(args[i].data, "!", 1) == 0) {
        header->operation = njt_health_check_http_match_NOT_CONTAIN;
        i++;
        if (nelts != 2) {
            njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "parameter number %u of header ! error.", nelts);
            return NJT_ERROR;
        }
        njt_str_copy_pool(hhccf->pool, header->key, args[1], return NJT_ERROR);
//        header->key = args[2];
        return NJT_OK;
    }
    njt_str_copy_pool(hhccf->pool, header->key, args[i], return NJT_ERROR);
//    header->key = args[i];
    i++;

    /*header abc;*/
    if (nelts == 1) {
        header->operation = njt_health_check_http_match_CONTAIN;
        return NJT_OK;
    }

    if (nelts != 3) {
        njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "header parse error.");
        return NJT_ERROR;
    }

    if (args[i].len == 1) {
        njt_str_copy_pool(hhccf->pool, header->key, args[2], return NJT_ERROR);
        if (njt_strncmp(args[i].data, "=", 1) == 0) {
            header->operation = njt_health_check_http_match_EQUAL;
        } else if (njt_strncmp(args[i].data, "~", 1) == 0) {

            header->operation = njt_health_check_http_match_REG_MATCH;
            header->regex = njt_health_check_http_match_regex_value(&cf, &header->key);

            if (header->regex == NULL) {
                njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "header regex %V parse error.",
                              &args[2]);
                return NJT_ERROR;
            }

        } else {
            njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "header operation parse error.");
            return NJT_ERROR;
        }

//        header->value = args[3];

        return NJT_OK;

    } else if (args[i].len == 2) {
        njt_str_copy_pool(hhccf->pool, header->value, args[2], return NJT_ERROR);
        if (njt_strncmp(args[i].data, "!=", 2) == 0) {
            header->operation = njt_health_check_http_match_NOT_EQUAL;

        } else if (njt_strncmp(args[i].data, "!~", 2) == 0) {

            header->operation = njt_health_check_http_match_NOT_REG_MATCH;
            header->regex = njt_health_check_http_match_regex_value(&cf, &header->value);

            if (header->regex == NULL) {
                njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "header regex %V parse error.",
                              &args[2]);
                return NJT_ERROR;
            }

        } else {
            return NJT_ERROR;
        }

//        header->value = args[3];
        return NJT_OK;
    } else {
        njt_log_error(NJT_LOG_EMERG, hhccf->log, 0,
                      "header operation %V isn't supported.", &args[i]);
        return NJT_ERROR;
    }
}


static njt_int_t njt_health_check_http_match(njt_health_check_api_data_t *api_data, njt_health_check_conf_t *hhccf) {
    njt_str_t                       *args;
    njt_health_check_http_match_code_t           *code, tmp_code;
    njt_uint_t                      i;
    njt_int_t                       rc;
    njt_health_check_http_match_t                *match;
    njt_conf_t                      cf;
    njt_array_t                     *array;
    health_check_http_header_item_t *header_item;

    njt_health_check_http_module_conf_ctx_t *http_ctx = hhccf->ctx;
    match = http_ctx->match;

    match->conditions = 1;
    if(api_data->hc_data == NULL || !api_data->hc_data->is_http_set || api_data->hc_data->http == NULL){
        return NJT_OK;
    }

    if (api_data->hc_data->http->is_status_set && api_data->hc_data->http->status.len > 0) {
        i = 0;
        array = njt_array_create(hhccf->pool,4, sizeof(njt_str_t));
        njt_str_split(&api_data->hc_data->http->status,array,' ');
        if(array->nelts < 1 ){
            njt_log_error(NJT_LOG_ERR, hhccf->log, 0, "code array create error.");
            return NJT_ERROR;
        }
        args = array->elts;
        if (njt_strncmp(args[0].data, "!", 1) == 0) {
            match->status.not_operation = 1;
            i++;
        }
        for (; i < array->nelts;++i) {
            njt_memzero(&tmp_code, sizeof(njt_health_check_http_match_code_t));
            rc = njt_health_check_http_match_parse_code(&args[i], &tmp_code);
            if (rc == NJT_ERROR) {
                continue;
            }
            code = njt_array_push(&match->status.codes);
            if (code == NULL) {
                njt_log_error(NJT_LOG_ERR, hhccf->log, 0, "code array push error.");
                return NJT_ERROR;
            }
            *code = tmp_code;
        }

    }
    if (api_data->hc_data->http->is_header_set && api_data->hc_data->http->header->nelts > 0) {
        njt_array_t *arr;
        for (i = 0; i < api_data->hc_data->http->header->nelts; i++) {
            header_item = get_health_check_http_header_item(api_data->hc_data->http->header, i);
            arr = njt_array_create(hhccf->pool, 4, sizeof(njt_str_t));
            if(arr == NULL ){
                njt_log_error(NJT_LOG_ERR, hhccf->log, 0, "header array create error.");
                return NJT_ERROR;
            }
            njt_str_split(header_item, arr, ' ');
            if(arr->nelts > 0 ){
                njt_health_check_http_match_parse_dheaders(arr, match, hhccf);
            }
        }
    }
    if (api_data->hc_data->http->is_body_set && api_data->hc_data->http->body.len > 0) {
        array = njt_array_create(hhccf->pool,4, sizeof(njt_str_t));
        njt_str_split(&http_ctx->body, array,' ');
        if(array->nelts < 1 ){
            njt_log_error(NJT_LOG_ERR, hhccf->log, 0, "code array create error.");
            return NJT_ERROR;
        }
        args = array->elts;
        if (array->nelts < 2) {
            njt_log_error(NJT_LOG_ERR, hhccf->log, 0, "body parameter number error.");
            return NJT_ERROR;
        }

        if (njt_strncmp(args[0].data, "!~", 2) == 0) {
            match->body.operation = njt_health_check_http_match_NOT_REG_MATCH;
        } else if (njt_strncmp(args[0].data, "~", 1) == 0) {
            match->body.operation = njt_health_check_http_match_REG_MATCH;
        } else {
            /*log the case*/
            njt_log_error(NJT_LOG_ERR, hhccf->log, 0, "body operation %V isn't supported error.", &args[0]);
            return NJT_ERROR;
        }
        cf.pool = hhccf->pool;
//        njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "body regex %V parse error.",args+1);
        match->body.regex = njt_health_check_http_match_regex_value(&cf, &args[1]);
        if (match->body.regex == NULL) {
            njt_log_error(NJT_LOG_ERR, hhccf->log, 0, "body regex %V parse error.",&args[1]);
            return NJT_ERROR;
        }
        match->body.value = args[1];
    }

    return NJT_OK;
}

static njt_int_t njt_health_check_http_module_test_connect(njt_connection_t *c) {
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

#if (NJT_HTTP_SSL)


static njt_int_t
njt_http_hc_ssl_name(njt_connection_t *c, njt_health_check_peer_t *hc_peer) {
    u_char                              *p, *last;
    njt_str_t                           name;
    njt_health_check_conf_t      *hhccf;
    njt_http_upstream_srv_conf_t        *uscf;

    hhccf = hc_peer->hhccf;

    if (hhccf->ssl.ssl_name.len) {
        name = hhccf->ssl.ssl_name;
    } else {
        uscf = njt_health_check_find_http_upstream_by_name(njet_master_cycle, &hhccf->upstream_name);
        if (uscf == NULL) {
            njt_log_error(NJT_LOG_ERR, c->log, 0,
                              "http hc ssl name uscf has not exit");
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
njt_http_hc_ssl_handshake(njt_connection_t *c, njt_health_check_peer_t *hc_peer) {
    long                    rc;
    njt_health_check_conf_t *hhccf;
    njt_peer_connection_t   *peer;

    peer = &hc_peer->peer;
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
            "http peer check ssl, peerid:%d ref_count=%d", hc_peer->peer_id, hhccf->ref_count);
        peer->connection->write->handler = njt_health_check_http_module_write_handler;
        peer->connection->read->handler = njt_health_check_http_module_read_handler;

        /*NJT_AGAIN or NJT_OK*/
        if(hhccf->timeout) {
            njt_add_timer(peer->connection->write, hhccf->timeout);
            njt_add_timer(peer->connection->read, hhccf->timeout);
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
njt_http_hc_ssl_handshake_handler(njt_connection_t *c) {
    njt_health_check_peer_t    *hc_peer;
    njt_int_t                        rc;

    hc_peer = c->data;

    rc = njt_http_hc_ssl_handshake(c, hc_peer);
    if (rc != NJT_OK) {
        njt_health_check_http_module_free_peer_resource(hc_peer, NJT_ERROR);
    }
}

static njt_int_t
njt_health_check_http_ssl_init_connection(njt_connection_t *c, njt_health_check_peer_t *hc_peer) {
    njt_int_t rc;
    njt_health_check_conf_t *hhccf;

    hhccf = hc_peer->hhccf;

    if (njt_health_check_http_module_test_connect(c) != NJT_OK) {
        return NJT_ERROR;
    }

    if (njt_ssl_create_connection(hhccf->ssl.ssl, c,
            NJT_SSL_BUFFER | NJT_SSL_CLIENT) != NJT_OK) {
        njt_log_error(NJT_LOG_DEBUG, c->log, 0, "ssl init create connection for health check error ");
        return NJT_ERROR;
    }

    c->sendfile = 0;

    if (hhccf->ssl.ssl_server_name || hhccf->ssl.ssl_verify) {
        if (njt_http_hc_ssl_name(c, hc_peer) != NJT_OK) {
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

        c->ssl->handler = njt_http_hc_ssl_handshake_handler;
        return NJT_OK;
    }

    return njt_http_hc_ssl_handshake(c, hc_peer);
//    return NJT_OK;
}

#endif

static njt_health_check_http_match_t *njt_health_check_http_match_create(njt_health_check_conf_t *hhccf) {
    njt_health_check_http_match_t *match;

    match = njt_pcalloc(hhccf->pool, sizeof(njt_health_check_http_match_t));
    if (match == NULL) {
        return NULL;
    }
    njt_memzero(match, sizeof(njt_health_check_http_match_t));

    return match;
}



static njt_int_t njt_health_check_http_match_block(njt_health_check_api_data_t *api_data, njt_health_check_conf_t *hhccf) {
    njt_health_check_http_match_t        *match;
    njt_int_t               rc;
    njt_uint_t              i;
    njt_str_t               *header;
    health_check_http_header_item_t *header_item;
    njt_str_t               tmp_str;

    njt_health_check_http_module_conf_ctx_t *http_ctx = hhccf->ctx;
    if(api_data->hc_data == NULL || !api_data->hc_data->is_http_set){
        return HC_SUCCESS;
    }

    if(api_data->hc_data->http->is_body_set && api_data->hc_data->http->body.len > 0){
        tmp_str = api_data->hc_data->http->body;
        njt_str_copy_pool(hhccf->pool, http_ctx->body, tmp_str, return HC_SERVER_ERROR);
    }
    
    if(api_data->hc_data->http->is_status_set && api_data->hc_data->http->status.len > 0){
        tmp_str = api_data->hc_data->http->status;
        njt_str_copy_pool(hhccf->pool, http_ctx->status, tmp_str, return HC_SERVER_ERROR);
    }

    if(api_data->hc_data->http->is_header_set && api_data->hc_data->http->header->nelts > 0){
        njt_array_init(&http_ctx->headers, hhccf->pool, api_data->hc_data->http->header->nelts, sizeof(njt_str_t));
        for (i = 0; i < api_data->hc_data->http->header->nelts; ++i) {
            header_item = get_health_check_http_header_item(api_data->hc_data->http->header, i);
            header = njt_array_push(&http_ctx->headers);
            tmp_str = *(header_item);
            njt_str_copy_pool(hhccf->pool, header[i], tmp_str, return HC_SERVER_ERROR);
        }
    }

    match = njt_health_check_http_match_create(hhccf);
    if (match == NULL) {
        njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "match create error");
        return HC_SERVER_ERROR;
    }
    http_ctx->match = match;
    match->defined = 1;
    if (njt_array_init(&match->status.codes, hhccf->pool, 4,
                       sizeof(njt_health_check_http_match_code_t)) != NJT_OK) {
        njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "status code array init error");
        return HC_SERVER_ERROR;
    }
    if (njt_array_init(&match->headers, hhccf->pool, 4,
                       sizeof(njt_health_check_http_match_header_t)) != NJT_OK) {
        njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, "match header array init error");
        return HC_SERVER_ERROR;
    }
    rc = njt_health_check_http_match(api_data, hhccf);
    if (rc == NJT_OK) {
        return HC_SUCCESS;
    } else {
        return HC_BODY_ERROR;
    }

}


static void njt_health_check_http_module_read_handler(njt_event_t *rev) {
    njt_connection_t                    *c;
    njt_health_check_peer_t             *hc_peer;
    njt_int_t                           rc;
    njt_health_check_http_module_conf_ctx_t *cf_ctx;
    njt_health_check_conf_t             *hhccf;

    c = rev->data;
    hc_peer = c->data;
    hhccf = hc_peer->hhccf;
    cf_ctx = hhccf->ctx;

    njt_log_error(NJT_LOG_DEBUG, c->log, 0,
            "read handler : upstream = %V   ref_count = %d", hc_peer->peer.name, hhccf->ref_count);

    if (rev->timedout) {

        /*log the case and update the peer status.*/
        njt_log_error(NJT_LOG_WARN, c->log, 0,
                       "read action for health check timeout");
        njt_health_check_http_module_free_peer_resource(hc_peer, NJT_ERROR);
        return;
    }

    if (rev->timer_set) {
        njt_del_timer(rev);
    }

    if (hhccf->disable) {
        if (hc_peer->peer.connection) {
            njt_health_check_close_http_connection(hc_peer->peer.connection);
        }
        return;
    }

    rc = cf_ctx->checker->read_handler(rev);
    if (rc == NJT_ERROR) {
        /*log the case and update the peer status.*/
        njt_log_error(NJT_LOG_WARN, c->log, 0,
                       "read action error for health check");
        njt_health_check_http_module_free_peer_resource(hc_peer, NJT_ERROR);
        return;
    } else if (rc == NJT_DONE) {
        cf_ctx->checker->update(hc_peer, rc);
        return;
    } else {
        /*AGAIN*/
    }

    if (!rev->timer_set) {
        njt_add_timer(rev, hhccf->timeout);
    }

    return;
}

static void njt_health_check_close_http_connection(njt_connection_t *c)
{
    njt_log_error(NJT_LOG_DEBUG, c->log, 0,
                   "close http connection: %d", c->fd);

#if (NJT_HTTP_SSL)

    if (c->ssl) {
        if (njt_ssl_shutdown(c) == NJT_AGAIN) {
            c->ssl->handler = njt_health_check_close_http_connection;
            return;
        }
    }

#endif

#if (NJT_HTTP_V3)
    if (c->quic) {
        njt_http_v3_reset_stream(c);
    }
#endif

    c->destroyed = 1;

    njt_close_connection(c);
}


static void njt_health_check_http_module_only_check_port_handler(njt_event_t *ev){
    njt_connection_t                *c;
    njt_health_check_peer_t    *hc_peer;

    c = ev->data;
    hc_peer = c->data;

    if (ev->timedout) {
        njt_log_debug0(NJT_LOG_DEBUG_HTTP, c->log, 0,
                       "write action for health check timeout");
        njt_health_check_http_module_free_peer_resource(hc_peer, NJT_ERROR);
        return;
    }

    if (ev->timer_set) {
        njt_del_timer(ev);
    }

    if (hc_peer->hhccf->disable) {
        if (hc_peer->peer.connection) {
            njt_health_check_close_http_connection(hc_peer->peer.connection);
        }
        return;
    }

    if (njt_health_check_http_module_test_connect(c) != NJT_OK) {
        njt_health_check_http_module_free_peer_resource(hc_peer, NJT_ERROR);

    }else{
        njt_health_check_http_module_free_peer_resource(hc_peer, NJT_OK);
    }
}



static void njt_health_check_http_module_write_handler(njt_event_t *wev) {
    njt_connection_t                        *c;
    njt_health_check_peer_t                 *hc_peer;
    njt_peer_connection_t                   *peer;
    njt_int_t                               rc;
    njt_health_check_http_module_conf_ctx_t *cf_ctx;
    njt_health_check_conf_t                 *hhccf;

    c = wev->data;
    hc_peer = c->data;
    peer = &hc_peer->peer;
    hhccf = hc_peer->hhccf;
    cf_ctx = hhccf->ctx;

    njt_log_error(NJT_LOG_DEBUG, c->log, 0,
            "write handler : upstream = %V   ref_count = %d",
            peer->name, hhccf->ref_count);

    if (wev->timedout) {
        /*log the case and update the peer status.*/
        njt_log_error(NJT_LOG_WARN, c->log, 0,
                       "write action for health check timeout");
        njt_health_check_http_module_free_peer_resource(hc_peer, NJT_ERROR);
        return;
    }

    if (wev->timer_set) {
        njt_del_timer(wev);
    }
    if (hhccf->disable) {
        if (hc_peer->peer.connection) {
            njt_health_check_close_http_connection(hc_peer->peer.connection);
        }
        return;
    }

    rc = cf_ctx->checker->write_handler(wev);
    if (rc == NJT_ERROR) {

        /*log the case and update the peer status.*/
        njt_log_error(NJT_LOG_WARN, c->log, 0,
                       "write action error for health check");
        njt_health_check_http_module_free_peer_resource(hc_peer, NJT_ERROR);
        return;
    } else if (rc == NJT_DONE || rc == NJT_OK) {
        //should handle read
        njt_add_timer(peer->connection->read, hhccf->timeout);

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




static njt_int_t njt_health_check_http_module_http_process(njt_health_check_peer_t *hc_peer) {
    njt_health_check_http_parse_t *parse;
    njt_int_t rc;

    parse = hc_peer->parser;
    rc = parse->process(hc_peer);

    return rc;
}

/*We assume the status line and headers are located in one buffer*/
static njt_int_t
njt_health_check_http_parse_status_line(njt_health_check_peer_t *hc_peer) {
    u_char ch;
    u_char *p;
    njt_health_check_http_parse_t *hp;
    njt_buf_t *b;

    enum {
        sw_start = 0,
        sw_H,
        sw_HT,
        sw_HTT,
        sw_HTTP,
        sw_first_major_digit,
        sw_major_digit,
        sw_first_minor_digit,
        sw_minor_digit,
        sw_status,
        sw_space_after_status,
        sw_status_text,
        sw_almost_done
    } state;

    hp = hc_peer->parser;
    b = hc_peer->recv_buf;
    state = hp->state;

    for (p = b->pos; p < b->last; p++) {
        ch = *p;

        switch (state) {

            /* "HTTP/" */
            case sw_start:
                switch (ch) {
                    case 'H':
                        state = sw_H;
                        break;
                    default:
                        return NJT_ERROR;
                }
                break;

            case sw_H:
                switch (ch) {
                    case 'T':
                        state = sw_HT;
                        break;
                    default:
                        return NJT_ERROR;
                }
                break;

            case sw_HT:
                switch (ch) {
                    case 'T':
                        state = sw_HTT;
                        break;
                    default:
                        return NJT_ERROR;
                }
                break;

            case sw_HTT:
                switch (ch) {
                    case 'P':
                        state = sw_HTTP;
                        break;
                    default:
                        return NJT_ERROR;
                }
                break;

            case sw_HTTP:
                switch (ch) {
                    case '/':
                        state = sw_first_major_digit;
                        break;
                    default:
                        return NJT_ERROR;
                }
                break;

                /* the first digit of major HTTP version */
            case sw_first_major_digit:
                if (ch < '1' || ch > '9') {
                    return NJT_ERROR;
                }

                state = sw_major_digit;
                break;

                /* the major HTTP version or dot */
            case sw_major_digit:
                if (ch == '.') {
                    state = sw_first_minor_digit;
                    break;
                }

                if (ch < '0' || ch > '9') {
                    return NJT_ERROR;
                }

                break;

                /* the first digit of minor HTTP version */
            case sw_first_minor_digit:
                if (ch < '0' || ch > '9') {
                    return NJT_ERROR;
                }

                state = sw_minor_digit;
                break;

                /* the minor HTTP version or the end of the request line */
            case sw_minor_digit:
                if (ch == ' ') {
                    state = sw_status;
                    break;
                }

                if (ch < '0' || ch > '9') {
                    return NJT_ERROR;
                }

                break;

                /* HTTP status code */
            case sw_status:
                if (ch == ' ') {
                    break;
                }

                if (ch < '0' || ch > '9') {
                    return NJT_ERROR;
                }

                hp->code = hp->code * 10 + (ch - '0');

                if (++hp->count == 3) {
                    state = sw_space_after_status;
                }

                break;

                /* space or end of line */
            case sw_space_after_status:
                switch (ch) {
                    case ' ':
                        state = sw_status_text;
                        break;
                    case '.':                    /* IIS may send 403.1, 403.2, etc */
                        state = sw_status_text;
                        break;
                    case CR:
                        break;
                    case LF:
                        goto done;
                    default:
                        return NJT_ERROR;
                }
                break;

                /* any text until end of line */
            case sw_status_text:
                switch (ch) {
                    case CR:
                        hp->status_text_end = p;
                        state = sw_almost_done;
                        break;
                    case LF:
                        hp->status_text_end = p;
                        goto done;
                }

                if (hp->status_text == NULL) {
                    hp->status_text = p;
                }

                break;

                /* end of status line */
            case sw_almost_done:
                switch (ch) {
                    case LF:
                        goto done;
                    default:
                        return NJT_ERROR;
                }
        }
    }

    b->pos = p;
    hp->state = state;

    return NJT_AGAIN;

    done:

    b->pos = p + 1;
    hp->state = sw_start;

    /*begin to process headers*/

    hp->stage = NJT_HTTP_PARSE_HEADER;
    hp->process = njt_health_check_http_process_headers;

    hp->process(hc_peer);

    return NJT_OK;
}

static njt_int_t
njt_health_check_http_parse_header_line(njt_health_check_peer_t *hc_peer) {
    u_char c, ch, *p;
    njt_health_check_http_parse_t *hp;
    njt_buf_t *b;

    enum {
        sw_start = 0,
        sw_name,
        sw_space_before_value,
        sw_value,
        sw_space_after_value,
        sw_almost_done,
        sw_header_almost_done
    } state;

    b = hc_peer->recv_buf;
    hp = hc_peer->parser;
    state = hp->state;

    for (p = b->pos; p < b->last; p++) {
        ch = *p;

        switch (state) {

            /* first char */
            case sw_start:

                switch (ch) {
                    case CR:
                        hp->header_end = p;
                        state = sw_header_almost_done;
                        break;
                    case LF:
                        hp->header_end = p;
                        goto header_done;
                    default:
                        state = sw_name;
                        hp->header_name_start = p;

                        c = (u_char) (ch | 0x20);
                        if (c >= 'a' && c <= 'z') {
                            break;
                        }

                        if (ch >= '0' && ch <= '9') {
                            break;
                        }

                        return NJT_ERROR;
                }
                break;

                /* header name */
            case sw_name:
                c = (u_char) (ch | 0x20);
                if (c >= 'a' && c <= 'z') {
                    break;
                }

                if (ch == ':') {
                    hp->header_name_end = p;
                    state = sw_space_before_value;
                    break;
                }

                if (ch == '-') {
                    break;
                }

                if (ch >= '0' && ch <= '9') {
                    break;
                }

                if (ch == CR) {
                    hp->header_name_end = p;
                    hp->header_start = p;
                    hp->header_end = p;
                    state = sw_almost_done;
                    break;
                }

                if (ch == LF) {
                    hp->header_name_end = p;
                    hp->header_start = p;
                    hp->header_end = p;
                    goto done;
                }

                return NJT_ERROR;

                /* space* before header value */
            case sw_space_before_value:
                switch (ch) {
                    case ' ':
                        break;
                    case CR:
                        hp->header_start = p;
                        hp->header_end = p;
                        state = sw_almost_done;
                        break;
                    case LF:
                        hp->header_start = p;
                        hp->header_end = p;
                        goto done;
                    default:
                        hp->header_start = p;
                        state = sw_value;
                        break;
                }
                break;

                /* header value */
            case sw_value:
                switch (ch) {
                    case ' ':
                        hp->header_end = p;
                        state = sw_space_after_value;
                        break;
                    case CR:
                        hp->header_end = p;
                        state = sw_almost_done;
                        break;
                    case LF:
                        hp->header_end = p;
                        goto done;
                }
                break;

                /* space* before end of header line */
            case sw_space_after_value:
                switch (ch) {
                    case ' ':
                        break;
                    case CR:
                        state = sw_almost_done;
                        break;
                    case LF:
                        goto done;
                    default:
                        state = sw_value;
                        break;
                }
                break;

                /* end of header line */
            case sw_almost_done:
                switch (ch) {
                    case LF:
                        goto done;
                    default:
                        return NJT_ERROR;
                }

                /* end of header */
            case sw_header_almost_done:
                switch (ch) {
                    case LF:
                        goto header_done;
                    default:
                        return NJT_ERROR;
                }
        }
    }

    b->pos = p;
    hp->state = state;

    return NJT_AGAIN;

    done:

    b->pos = p + 1;
    hp->state = sw_start;

    return NJT_OK;

    header_done:

    b->pos = p + 1;
    hp->state = sw_start;
    hp->body_start = b->pos;

    return NJT_DONE;
}

static njt_int_t njt_health_check_http_process_headers(njt_health_check_peer_t *hc_peer) {
    njt_int_t                       rc;
    njt_table_elt_t                 *h;
    njt_health_check_http_parse_t   *hp;
    njt_connection_t                *c;
    c = hc_peer->peer.connection;

    njt_log_error(NJT_LOG_DEBUG, c->log, 0, "http process header.");

    hp = hc_peer->parser;

    if (hp->headers.size == 0) {
        rc = njt_array_init(&hp->headers, hc_peer->pool, 4,
                            sizeof(njt_table_elt_t));
        if (rc != NJT_OK) {
            return NJT_ERROR;
        }
    }

    for (;;) {

        rc = njt_health_check_http_parse_header_line(hc_peer);

        if (rc == NJT_OK) {
            h = njt_array_push(&hp->headers);
            if (h == NULL) {
                return NJT_ERROR;
            }

            njt_memzero(h, sizeof(njt_table_elt_t));
            h->hash = 1;
            h->key.data = hp->header_name_start;
            h->key.len = hp->header_name_end - hp->header_name_start;

            h->value.data = hp->header_start;
            h->value.len = hp->header_end - hp->header_start;

            njt_log_error(NJT_LOG_DEBUG, c->log, 0,
                           "http header \"%*s: %*s\"",
                           h->key.len, h->key.data, h->value.len,
                           h->value.data);

            if (h->key.len == njt_strlen("Transfer-Encoding")
                && h->value.len == njt_strlen("chunked")
                && njt_strncasecmp(h->key.data, (u_char *) "Transfer-Encoding",
                                   h->key.len) == 0
                && njt_strncasecmp(h->value.data, (u_char *) "chunked",
                                   h->value.len) == 0) {
                hp->chunked = 1;
            }

            if (h->key.len == njt_strlen("Content-Length")
                && njt_strncasecmp(h->key.data, (u_char *) "Content-Length",
                                   h->key.len) == 0) {
                hp->content_length_n = njt_atoof(h->value.data, h->value.len);

                if (hp->content_length_n == NJT_ERROR) {

                    njt_log_error(NJT_LOG_WARN, c->log, 0,
                                   "invalid fetch content length");
                    return NJT_ERROR;
                }
            }

            continue;
        }

        if (rc == NJT_DONE) {
            break;
        }

        if (rc == NJT_AGAIN) {
            return NJT_AGAIN;
        }

        /*http header parse error*/
        njt_log_error(NJT_LOG_WARN, c->log, 0,
                       "http process header error.");
        return NJT_ERROR;
    }

    /*TODO check if the first buffer is used out*/
    hp->stage = NJT_HTTP_PARSE_BODY;
    hp->process = njt_health_check_http_process_body;

    return hp->process(hc_peer);
}

static njt_int_t njt_health_check_http_process_body(njt_health_check_peer_t *hc_peer) {
    njt_health_check_http_parse_t *hp;

    hp = hc_peer->parser;

    if (hp->done) {
        return NJT_DONE;
    }
    return NJT_OK;
}


static njt_int_t njt_health_check_http_module_http_read_handler(njt_event_t *rev) {
    njt_connection_t                *c;
    njt_health_check_peer_t         *hc_peer;
    ssize_t                         n, size;
    njt_buf_t                       *b;
    njt_int_t                       rc;
    njt_chain_t                     *chain, *node;
    njt_health_check_http_parse_t   *hp;
    njt_health_check_http_module_conf_ctx_t *cf_ctx;
    njt_health_check_conf_t         *hhccf;

    c = rev->data;
    hc_peer = c->data;
    hhccf = hc_peer->hhccf;
    cf_ctx = hhccf->ctx;

    njt_log_error(NJT_LOG_DEBUG, c->log, 0, "http check recv.");


    /*Init the internal parser*/
    if (hc_peer->parser == NULL) {
        hp = njt_pcalloc(hc_peer->pool, sizeof(njt_health_check_http_parse_t));
        if (hp == NULL) {
            /*log*/
            njt_log_error(NJT_LOG_ERR, c->log, 0,
                           "memory allocation error for health check.");
            return NJT_ERROR;
        }

        hp->stage = NJT_HTTP_PARSE_STATUS_LINE;
        hp->process = njt_health_check_http_parse_status_line;

        hc_peer->parser = hp;
    }

    for (;;) {

        if (hc_peer->recv_buf == NULL) {
            b = njt_create_temp_buf(hc_peer->pool, njt_pagesize);
            if (b == NULL) {
                /*log*/
                njt_log_error(NJT_LOG_ERR, c->log, 0,
                               "recv buffer memory allocation error for health check.");
                return NJT_ERROR;
            }
            hc_peer->recv_buf = b;
        }

        b = hc_peer->recv_buf;
        size = b->end - b->last;
        n = c->recv(c, b->last, size);

        if (n > 0) {
            b->last += n;

            rc = cf_ctx->checker->process(hc_peer);
            if (rc == NJT_ERROR) {
                njt_log_error(NJT_LOG_WARN, c->log, 0, "hc process error");
                return NJT_ERROR;
            }

            /*link chain buffer*/
            if (b->last == b->end) {

                hp = hc_peer->parser;

                if (hp->stage != NJT_HTTP_PARSE_BODY) {
                    /*log. The status and headers are too large to be hold in one buffer*/
                    njt_log_error(NJT_LOG_WARN, c->log, 0,
                                   "status and headers exceed one page size");
                    return NJT_ERROR;
                }

                chain = njt_alloc_chain_link(hc_peer->pool);
                if (chain == NULL) {
                    /*log and process the error*/
                    njt_log_error(NJT_LOG_ERR, c->log, 0,
                                   "memory allocation of the chain buf failed.");
                    return NJT_ERROR;
                }

                chain->buf = b;
                chain->next = NULL;

                node = hc_peer->recv_chain;
                if (node == NULL) {
                    hc_peer->recv_chain = chain;
                } else {
                    hc_peer->last_chain_node->next = chain;
                }
                hc_peer->last_chain_node = chain;

                /*Reset the recv buffer*/
                hc_peer->recv_buf = NULL;
            }

            continue;
        }

        if (n == NJT_AGAIN) {
            if (njt_handle_read_event(rev, 0) != NJT_OK) {
                /*log*/
                njt_log_error(NJT_LOG_ERR, c->log, 0,
                               "read event handle error for health check");
                return NJT_ERROR;
            }
            return NJT_AGAIN;
        }

        if (n == NJT_ERROR) {
            /*log*/
            njt_log_error(NJT_LOG_ERR, c->log, 0,
                           "read error for health check");
            return NJT_ERROR;
        }

        break;
    }


    hp = hc_peer->parser;
    hp->done = 1;
    rc = cf_ctx->checker->process(hc_peer);

    if (rc == NJT_DONE) {
        return NJT_DONE;
    }

    if (rc == NJT_AGAIN) {
        /* the connection is shutdown*/
        njt_log_error(NJT_LOG_ERR, c->log, 0,
                       "connection is shutdown");
        return NJT_ERROR;
    }

    return NJT_OK;
}


static njt_int_t
njt_health_check_http_module_http_match_header(njt_health_check_http_match_header_t *input,
                                        njt_health_check_peer_t *hc_peer) {
    njt_health_check_http_parse_t   *hp;
    njt_int_t                       rc;
    njt_uint_t                      i;
    njt_int_t                       n;
    njt_table_elt_t                 *headers, *header;

    hp = hc_peer->parser;
    headers = hp->headers.elts;

    rc = NJT_OK;

    switch (input->operation) {

        case njt_health_check_http_match_CONTAIN:

            rc = NJT_ERROR;
            for (i = 0; i < hp->headers.nelts; i++) {
                header = headers + i;
                if (header->key.len != input->key.len) {
                    continue;
                }

                if (njt_strncmp(header->key.data, input->key.data, input->key.len) == 0) {
                    return NJT_OK;
                }
            }
            break;

        case njt_health_check_http_match_NOT_CONTAIN:
            rc = NJT_OK;
            for (i = 0; i < hp->headers.nelts; i++) {
                header = headers + i;
                if (header->key.len != input->key.len) {
                    continue;
                }

                if (njt_strncmp(header->key.data, input->key.data, input->key.len) == 0) {
                    return NJT_ERROR;
                }
            }

            break;

            /*If the header doesn't occur means failure*/
        case njt_health_check_http_match_EQUAL:

            rc = NJT_ERROR;

            for (i = 0; i < hp->headers.nelts; i++) {

                header = headers + i;
                if (header->key.len != input->key.len) {
                    continue;
                }

                if (njt_strncmp(header->key.data, input->key.data, input->key.len) == 0) {
                    if (header->value.len != input->value.len) {
                        return NJT_ERROR;
                    }

                    if (njt_strncmp(header->value.data, input->value.data,
                                    input->value.len) == 0) {
                        return NJT_OK;
                    }

                    return NJT_ERROR;
                }
            }

            break;

        case njt_health_check_http_match_NOT_EQUAL:

            rc = NJT_ERROR;

            for (i = 0; i < hp->headers.nelts; i++) {

                header = headers + i;
                if (header->key.len != input->key.len) {
                    continue;
                }

                if (njt_strncmp(header->key.data, input->key.data, input->key.len) == 0) {

                    if (header->value.len != input->value.len) {
                        return NJT_OK;
                    }

                    if (njt_strncmp(header->value.data, input->value.data,
                                    input->value.len) == 0) {
                        return NJT_ERROR;
                    }

                    return NJT_OK;
                }
            }

            break;

#if (NJT_PCRE)
        case njt_health_check_http_match_REG_MATCH:

            if (input->regex == NULL) {
                return NJT_ERROR;
            }

            rc = NJT_ERROR;

            for (i = 0; i < hp->headers.nelts; i++) {

                header = headers + i;
                if (header->key.len != input->key.len) {
                    continue;
                }

                if (njt_strncmp(header->key.data, input->key.data, input->key.len) == 0) {

                    n = njt_regex_exec(input->regex, &header->value, NULL, 0);

                    if (n == NJT_REGEX_NO_MATCHED) {
                        return NJT_ERROR;
                    }

                    return NJT_OK;
                }
            }

            break;

        case njt_health_check_http_match_NOT_REG_MATCH:

            if (input->regex == NULL) {
                return NJT_ERROR;
            }

            rc = NJT_ERROR;

            for (i = 0; i < hp->headers.nelts; i++) {

                header = headers + i;
                if (header->key.len != input->key.len) {
                    continue;
                }

                if (njt_strncmp(header->key.data, input->key.data, input->key.len) == 0) {

                    n = njt_regex_exec(input->regex, &header->value, NULL, 0);

                    if (n == NJT_REGEX_NO_MATCHED) {
                        return NJT_OK;
                    }

                    return NJT_ERROR;
                }
            }

            break;
#endif
        default:
            njt_log_error(NJT_LOG_DEBUG, hc_peer->peer.connection->log, 0,
                           "unsupported operation %u.\n", input->operation);

            return NJT_ERROR;
    }

    return rc;
}


static njt_int_t
njt_health_check_http_module_http_match_body(njt_health_check_http_match_body_t *body,
                                      njt_health_check_peer_t *hc_peer) {
    njt_int_t                       n;
    njt_str_t                       result;
    njt_health_check_http_parse_t   *hp;
    njt_buf_t                       *content;
    njt_uint_t                      size;
    njt_chain_t                     *node;
    njt_connection_t                *c = hc_peer->peer.connection;


    /* recreate the body buffer*/
    content = njt_create_temp_buf(hc_peer->pool, njt_pagesize);
    if (content == NULL) {
        /*log*/
        njt_log_error(NJT_LOG_ERR, c->log, 0,
                       "content buffer allocation error for health check");
        return NJT_ERROR;
    }

    hp = hc_peer->parser;
    if (hc_peer->recv_chain == NULL) {

        size = hc_peer->recv_buf->last - hp->body_start;
        content->last = njt_snprintf(content->last, size, "%s", hp->body_start);

    } else {

        node = hc_peer->recv_chain;
        size = node->buf->last - hp->body_start;
        content->last = njt_snprintf(content->last, size, "%s", hp->body_start);

        if (node->buf->end == node->buf->last) {
            node = node->next;
            size = njt_pagesize - size;
            content->last = njt_snprintf(content->last, size, "%s", node->buf->pos);
        }

    }

    result.data = content->start;
    result.len = content->last - content->start;

    n = njt_regex_exec(body->regex, &result, NULL, 0);

    if (body->operation == njt_health_check_http_match_REG_MATCH) {

        if (n == NJT_REGEX_NO_MATCHED) {
            return NJT_ERROR;
        } else {
            return NJT_OK;
        }

    } else if (body->operation == njt_health_check_http_match_NOT_REG_MATCH) {

        if (n == NJT_REGEX_NO_MATCHED) {
            return NJT_OK;
        } else {
            return NJT_ERROR;
        }
    } else {
        return NJT_ERROR;
    }

    return NJT_OK;
}

static void njt_health_check_http_module_http_update_status(njt_health_check_peer_t *hc_peer, njt_int_t status) {
    njt_int_t                       rc;
    njt_health_check_http_parse_t   *hp;
    njt_health_check_http_match_t                *match;
    njt_uint_t                      i, result;
    njt_health_check_http_match_code_t           *code, *codes;
    njt_health_check_http_match_header_t         *header, *headers;
    njt_health_check_http_module_conf_ctx_t *cf_ctx;
    njt_health_check_conf_t         *hhccf;
    njt_peer_connection_t           *c;

    hp = hc_peer->parser;
    hhccf = hc_peer->hhccf;
    cf_ctx = hhccf->ctx;
    match = cf_ctx->match;
    c = &hc_peer->peer;

    /*By default, we change the code is in range 200 or 300*/
    rc = NJT_ERROR;
    if (hp == NULL) {
        njt_log_error(NJT_LOG_WARN, c->log, 0, "hp parse is null error.");
        njt_health_check_http_module_free_peer_resource(hc_peer, rc);

        return ;
    }

    if (match == NULL) {
        if (hp->code >= 200 && hp->code <= 399) {
            rc = NJT_OK;
        }else{
            njt_log_error(NJT_LOG_WARN, c->log, 0, "match is null and hp->code is %d error", hp->code);
        }
    
        goto set_status;
    }

    /*an empty match*/
    if (!match->conditions) {
        goto set_status;
    }

    /*check the status*/

    /*step1 check code*/
    if (match->status.codes.nelts == 0) {
        goto headers;
    }

    result = 0;
    codes = match->status.codes.elts;
    for (i = 0; i < match->status.codes.nelts; i++) {
        code = codes + i;
        if (code->single) {
            if (hp->code == code->code) {
                result = 1;
                break;
            }
        } else {
            if (hp->code >= code->code && hp->code <= code->last_code) {
                result = 1;
                break;
            }
        }
    }

    if (match->status.not_operation) {
        result = !result;
    }

    rc = result ? NJT_OK : NJT_ERROR;

    if (rc == NJT_ERROR) {
        njt_log_error(NJT_LOG_WARN, c->log, 0, "match status error retcode:%d", hp->code);
        goto set_status;
    }

    headers:
    /*step2 check header*/
    headers = match->headers.elts;
    for (i = 0; i < match->headers.nelts; i++) {
        header = headers + i;

        rc = njt_health_check_http_module_http_match_header(header, hc_peer);
        if (rc == NJT_ERROR) {
            njt_log_error(NJT_LOG_WARN, c->log, 0, "match header %V error.",
                           &header->value);
            goto set_status;
        }

    }

    /*step3 check body*/
    if (match->body.regex) {
        rc = njt_health_check_http_module_http_match_body(&match->body, hc_peer);
        /*regular expression match of the body*/
        if (rc == NJT_ERROR) {
            njt_log_error(NJT_LOG_WARN, c->log, 0, "match body %V error.",
                           &match->body.value);
            goto set_status;
        }
    }

set_status:
    njt_health_check_http_module_free_peer_resource(hc_peer, rc);

    return ;
}

static void
njt_health_check_http_module_dummy_handler(njt_event_t *ev) {
    njt_log_error(NJT_LOG_DEBUG, ev->log, 0,
                   "http health check dummy handler");
}

static njt_int_t njt_health_check_http_module_http_write_handler(njt_event_t *wev) {
    njt_connection_t                    *c;
    njt_health_check_peer_t             *hc_peer;
    ssize_t                             n, size;
    njt_health_check_http_module_conf_ctx_t    *cf_ctx;
    njt_health_check_conf_t             *hhccf;
    njt_http_upstream_srv_conf_t        *uscf;

    c = wev->data;
    hc_peer = c->data;
    hhccf = hc_peer->hhccf;
    cf_ctx = hhccf->ctx;

    njt_log_error(NJT_LOG_DEBUG, c->log, 0, "http check send.");

    if (hc_peer->send_buf == NULL) {
        hc_peer->send_buf = njt_create_temp_buf(hc_peer->pool, njt_pagesize);
        if (hc_peer->send_buf == NULL) {
            /*log the send buf allocation failure*/
            njt_log_error(NJT_LOG_ERR, c->log, 0,
                           "malloc failure of the send buffer for health check.");
            return NJT_ERROR;
        }

        uscf = njt_health_check_find_http_upstream_by_name(njet_master_cycle, &hhccf->upstream_name);
        if (uscf == NULL) {
            njt_log_debug0(NJT_LOG_DEBUG_HTTP, c->log, 0, "no stream upstream data");
            return NJT_ERROR;
        }

        /*Fill in the buff*/
        hc_peer->send_buf->last = njt_snprintf(hc_peer->send_buf->last,
                                               hc_peer->send_buf->end - hc_peer->send_buf->last, "GET %V HTTP/1.1" CRLF,
                                               &cf_ctx->uri);
        hc_peer->send_buf->last = njt_snprintf(hc_peer->send_buf->last,
                                               hc_peer->send_buf->end - hc_peer->send_buf->last,
                                               "Connection: close" CRLF);
        hc_peer->send_buf->last = njt_snprintf(hc_peer->send_buf->last,
                                               hc_peer->send_buf->end - hc_peer->send_buf->last, "Host: %V" CRLF,
                                               &hc_peer->server);
        hc_peer->send_buf->last = njt_snprintf(hc_peer->send_buf->last,
                                               hc_peer->send_buf->end - hc_peer->send_buf->last,
                                               "User-Agent: njet (health-check)" CRLF);
        hc_peer->send_buf->last = njt_snprintf(hc_peer->send_buf->last,
                                               hc_peer->send_buf->end - hc_peer->send_buf->last, CRLF);
    }

    size = hc_peer->send_buf->last - hc_peer->send_buf->pos;

    n = c->send(c, hc_peer->send_buf->pos,
                hc_peer->send_buf->last - hc_peer->send_buf->pos);

    if (n == NJT_ERROR) {
        njt_log_error(NJT_LOG_ERR, c->log, 0, "hc http module send error");
        return NJT_ERROR;
    }

    if (n > 0) {
        hc_peer->send_buf->pos += n;
        if (n == size) {
            wev->handler = njt_health_check_http_module_dummy_handler;

            if (njt_handle_write_event(wev, 0) != NJT_OK) {
                /*LOG the failure*/
                njt_log_error(NJT_LOG_ERR, c->log, 0,
                               "write event handle error for health check");
                return NJT_ERROR;
            }
            return NJT_DONE;
        }
    }

    return NJT_AGAIN;
}




