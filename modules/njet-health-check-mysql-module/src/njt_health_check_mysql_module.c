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
#include "njt_health_check_mysql_module.h"

#define NJT_SMYSQL_HC_CHECK_ERROR               0
#define NJT_SMYSQL_HC_CHECK_OK                  1
#define NJT_SMYSQL_HC_CHECK_FREE_RESOUCE        2


static void *njt_health_check_mysql_module_create_ctx(void *hhccf_info, void *api_data_info);
static void njt_health_check_mysql_module_start_check(void *hc_peer_info, interal_update_handler udpate_handler);
static njt_int_t njt_health_check_mysql_module_init_process(njt_cycle_t *cycle);
static void njt_health_check_mysql_module_start(njt_health_check_peer_t *hc_peer);
static void njt_health_check_mysql_module_status_handler(njt_health_check_peer_t *init_hc_peer,
        njt_int_t event, njt_event_t *ev);
static njt_int_t 
njt_health_check_mysql_module_context_set(njt_health_check_api_data_t *api_data, njt_health_check_conf_t *hhccf,
        njt_health_check_mysql_module_conf_ctx_t  *mysql_ctx);
static void njt_health_check_mysql_module_close_connection(njt_connection_t *c);

extern njt_cycle_t *njet_master_cycle;

static njt_http_module_t njt_health_check_mysql_module_ctx = {
        NULL,                                   /* preconfiguration */
        NULL,          /* postconfiguration */

        NULL,                                   /* create main configuration */
        NULL,                                  /* init main configuration */

        NULL,                                  /* create server configuration */
        NULL,                                  /* merge server configuration */

        NULL,                                   /* create location configuration */
        NULL                                    /* merge location configuration */
};

njt_module_t njt_health_check_mysql_module = {
        NJT_MODULE_V1,
        &njt_health_check_mysql_module_ctx,      /* module context */
        NULL,                                   /* module directives */
        NJT_HTTP_MODULE,                        /* module type */
        NULL,                                   /* init master */
        NULL,                                   /* init module */
        njt_health_check_mysql_module_init_process,                                   /* init process */
        NULL,                                   /* init thread */
        NULL,                                   /* exit thread */
        NULL,                                   /* exit process */
        NULL,                                   /* exit master */
        NJT_MODULE_V1_PADDING
};


static njt_int_t njt_health_check_mysql_module_init_process(njt_cycle_t *cycle){
    njt_health_check_reg_info_t             h;

    njt_str_t  server_type = njt_string("mysql");
    njt_memzero(&h, sizeof(njt_health_check_reg_info_t));

    h.server_type = &server_type;
    h.create_ctx = njt_health_check_mysql_module_create_ctx;
    h.start_check = njt_health_check_mysql_module_start_check;

    njt_health_check_reg_handler(&h);

    return NJT_OK;
}



static void njt_health_check_mysql_module_start_check(void *hc_peer_info, interal_update_handler udpate_handler){
    njt_health_check_peer_t         *hc_peer = hc_peer_info;
    njt_health_check_tcp_module_state_data_t         *sd;

    //create mysql data
    sd = njt_pcalloc(hc_peer->pool, sizeof(njt_health_check_tcp_module_state_data_t));
    if(sd == NULL){
        njt_log_error(NJT_LOG_WARN, njt_cycle->log, 0,
            "health check mysql data malloc error:%V", hc_peer->peer.name);

        udpate_handler(hc_peer, NJT_ERROR);

        return;
    }

    hc_peer->data = sd;

    njt_health_check_mysql_module_start(hc_peer);

    return;
}


static void *njt_health_check_mysql_module_create_ctx(void *hhccf_info, void *api_data_info){
    njt_health_check_conf_t                     *hhccf = hhccf_info;
    njt_health_check_mysql_module_conf_ctx_t    *mysql_ctx;
    njt_health_check_api_data_t                 *api_data = api_data_info;
    njt_int_t                                   rc;

    if (hhccf == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "health check alloc mysql_ctx error ");
        return NULL;
    }

    mysql_ctx = njt_pcalloc(hhccf->pool, sizeof(njt_health_check_mysql_module_conf_ctx_t));
    if (mysql_ctx == NULL) {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "health check alloc mysql_ctx error ");
        return NULL;
    }

    rc = njt_health_check_mysql_module_context_set(api_data, hhccf, mysql_ctx);
    if (rc != HC_SUCCESS) {
        return NULL;
    }

    return mysql_ctx;
}

static void njt_health_check_mysql_module_free_peer_resource(njt_health_check_peer_t *hc_peer, njt_int_t status) {
    njt_health_check_tcp_module_state_data_t         *sd = hc_peer->data;
    njt_connection_t                *c = sd->c;

    if(c != NULL){
        njt_health_check_mysql_module_close_connection(c);
    }

    hc_peer->update_handler(hc_peer, status);

    return;
}

static njt_int_t 
njt_health_check_mysql_module_context_set(njt_health_check_api_data_t *api_data, njt_health_check_conf_t *hhccf,
        njt_health_check_mysql_module_conf_ctx_t  *mysql_ctx) {
    njt_str_t                           *val = NULL;

    if (api_data->hc_data->is_sql_set) {
        if(api_data->hc_data->sql->is_select_set && api_data->hc_data->sql->select.len > 0){
            val = &api_data->hc_data->sql->select;
            mysql_ctx->select.data = njt_pcalloc(hhccf->pool, val->len + 1);
            if(NULL == mysql_ctx->select.data) {
                njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, 
                    "mysql select malloc error, select:%V", val);
                return HC_SERVER_ERROR;
            }

            mysql_ctx->select.len = val->len;
            njt_memcpy(mysql_ctx->select.data, val->data, val->len);
        }

        mysql_ctx->use_ssl = 1;      //default use ssl
        if(api_data->hc_data->sql->is_useSsl_set){
            mysql_ctx->use_ssl = api_data->hc_data->sql->useSsl;
        }

        if(api_data->hc_data->sql->is_user_set && api_data->hc_data->sql->user.len > 0){
            val = &api_data->hc_data->sql->user;
            mysql_ctx->username.data = njt_pcalloc(hhccf->pool, val->len + 1);
            if(NULL == mysql_ctx->username.data) {
                njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, 
                    "mysql user malloc error, user:%V", val);
                return HC_SERVER_ERROR;
            }

            mysql_ctx->username.len = val->len;
            njt_memcpy(mysql_ctx->username.data, val->data, val->len);
        }

        if(api_data->hc_data->sql->is_password_set && api_data->hc_data->sql->password.len > 0){
            val = &api_data->hc_data->sql->password;
            mysql_ctx->password.data = njt_pcalloc(hhccf->pool, val->len + 1);
            if(NULL == mysql_ctx->password.data) {
                njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, 
                    "mysql password malloc error, password:%V", val);
                return HC_SERVER_ERROR;
            }

            mysql_ctx->password.len = val->len;
            njt_memcpy(mysql_ctx->password.data, val->data, val->len);
        }

        if(!api_data->hc_data->sql->is_db_set || api_data->hc_data->sql->db.len < 1){
            njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, 
                    "mysql db_name must be set and not empty", val);
            return HC_SMYSQL_DB_NOT_SET_ERROR;
        }

        if(api_data->hc_data->sql->is_db_set && api_data->hc_data->sql->db.len > 0){
            val = &api_data->hc_data->sql->db;
            mysql_ctx->db_name.data = njt_pcalloc(hhccf->pool, val->len + 1);
            if(NULL == mysql_ctx->db_name.data) {
                njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, 
                    "mysql db_name malloc error, db_name:%V", val);
                return HC_SERVER_ERROR;
            }

            mysql_ctx->db_name.len = val->len;
            njt_memcpy(mysql_ctx->db_name.data, val->data, val->len);
        }
    }

    return HC_SUCCESS;
}


static void njt_health_check_mysql_module_close_connection(njt_connection_t *c)
{
    if (c->fd == (njt_socket_t) -1) {
        njt_log_error(NJT_LOG_ALERT, c->log, 0, "connection already closed");
        return;
    }

    if (c->read->timer_set) {
        njt_del_timer(c->read);
    }

    if (c->write->timer_set) {
        njt_del_timer(c->write);
    }

    if (!c->shared) {
        if (njt_del_conn) {
            njt_del_conn(c, NJT_CLOSE_EVENT);

        } else {
            if (c->read->active || c->read->disabled) {
                njt_del_event(c->read, NJT_READ_EVENT, NJT_CLOSE_EVENT);
            }

            if (c->write->active || c->write->disabled) {
                njt_del_event(c->write, NJT_WRITE_EVENT, NJT_CLOSE_EVENT);
            }
        }
    }

    if (c->read->posted) {
        njt_delete_posted_event(c->read);
    }

    if (c->write->posted) {
        njt_delete_posted_event(c->write);
    }

    c->read->closed = 1;
    c->write->closed = 1;

    njt_reusable_connection(c, 0);

    njt_free_connection(c);

    c->fd = (njt_socket_t) -1;
}


static void
njt_health_check_mysql_module_next_event(int new_st, int status, njt_health_check_tcp_module_state_data_t *sd)
{
    if (status & MYSQL_WAIT_READ){
        //remove write event
        if(sd->c->write->active){
            njt_del_event(sd->c->write, NJT_WRITE_EVENT, 0);
        }
        
        if (njt_add_event(sd->c->read, NJT_READ_EVENT, NJT_CLEAR_EVENT) != NJT_OK) {
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "hc mysql add read event fail, fd:%d", sd->c->fd);
        }
    }
    if (status & MYSQL_WAIT_WRITE){
        if(sd->c->read->active){
            njt_del_event(sd->c->read, NJT_READ_EVENT, 0);
        }
        if (njt_add_event(sd->c->write, NJT_WRITE_EVENT, NJT_CLEAR_EVENT) != NJT_OK) {
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "hc mysql add write event, fd:%d", sd->c->fd);
        }
    }

    if (status & MYSQL_WAIT_TIMEOUT)
    {
        //free connection close task
        njt_log_error(NJT_LOG_INFO, njt_cycle->log, 0,
                "hc mysql timeout, fd:%d", sd->c->fd);
    }

    sd->ST = new_st;
}


#define NEXT_IMMEDIATE(sd_, new_st) do { sd_->ST= new_st; goto again; } while (0)


static void njt_health_check_mysql_module_check_read_event_handler(njt_event_t *ev){
    njt_health_check_peer_t         *hc_peer = NULL;
    njt_health_check_tcp_module_state_data_t         *sd = NULL;
    njt_connection_t                *c = NULL;

    c = ev->data;
    hc_peer = c->data;
    sd = hc_peer->data;

    if(ev->timedout){
        /*log the case and update the peer status.*/
        njt_log_error(NJT_LOG_ERR, c->log, 0,
                "mysql read action for health check timeout");

        ev->timedout = 0;

        sd->ST = 40;
        sd->check_status_ok = NJT_SMYSQL_HC_CHECK_ERROR;
        njt_health_check_mysql_module_status_handler(NULL, MYSQL_WAIT_READ, ev);

        return;
    }

    if (hc_peer->hhccf->disable) {
        //close
        sd->ST = 40;
        sd->check_status_ok = NJT_SMYSQL_HC_CHECK_FREE_RESOUCE;
        njt_health_check_mysql_module_status_handler(NULL, MYSQL_WAIT_READ, ev);

        return;
    }

    njt_health_check_mysql_module_status_handler(NULL, MYSQL_WAIT_READ, ev);
}

static void njt_health_check_mysql_module_check_write_event_handler(njt_event_t *ev){
    njt_health_check_peer_t         *hc_peer = NULL;
    njt_health_check_tcp_module_state_data_t         *sd = NULL;
    njt_connection_t                *c = NULL;

    c = ev->data;
    hc_peer = c->data;
    sd = hc_peer->data;

    if(ev->timedout){
        /*log the case and update the peer status.*/
        njt_log_error(NJT_LOG_ERR, c->log, 0,
                "mysql write action for health check timeout");

        ev->timedout = 0;

        sd->ST = 40;
        sd->check_status_ok = NJT_SMYSQL_HC_CHECK_ERROR;
        njt_health_check_mysql_module_status_handler(NULL, MYSQL_WAIT_WRITE, ev);
        return;
    }

    if (hc_peer->hhccf->disable) {
        //close
        sd->ST = 40;
        sd->check_status_ok = NJT_SMYSQL_HC_CHECK_FREE_RESOUCE;
        njt_health_check_mysql_module_status_handler(NULL, MYSQL_WAIT_WRITE, ev);

        return;
    }

    njt_health_check_mysql_module_status_handler(NULL, MYSQL_WAIT_WRITE, ev);
}



static void njt_health_check_mysql_module_status_handler(njt_health_check_peer_t *init_hc_peer,
        njt_int_t event, njt_event_t *ev){
    njt_health_check_peer_t      *hc_peer = NULL;
    njt_health_check_conf_t      *hhccf = NULL;
    njt_health_check_tcp_module_state_data_t             *sd = NULL;
    njt_health_check_mysql_module_conf_ctx_t  *mysql_ctx = NULL;
    int                                 status;
    njt_connection_t                    *c = NULL;
    njt_socket_t                        fd;
    njt_event_t                         *rev, *wev;
    // size_t                              len;
    u_char                              host[NJT_SOCKADDR_STRLEN+1];
    // njt_str_t                           tmp_str;
    char                                *user = NULL;
    char                                *password = NULL;
    char                                *db_name = NULL;
    njt_int_t                           port = 3306; //default

    if(init_hc_peer != NULL){
        hc_peer = init_hc_peer;
    }else{
        //here has connection data
        c = ev->data;
        hc_peer = c->data;
    }

    hhccf = hc_peer->hhccf;
    mysql_ctx = hhccf->ctx;
    sd = hc_peer->data;

again:
    switch(sd->ST)
    {
    case 0:
        if(mysql_ctx->db_name.len > 0){
            db_name = (char *)mysql_ctx->db_name.data;     //will be end with 0
        }

        if(mysql_ctx->username.len > 0){
            user = (char *)mysql_ctx->username.data;     //will be end with 0
        }

        if(mysql_ctx->password.len > 0){
            password = (char *)mysql_ctx->password.data;     //will be end with 0
        }

        //get host and port from peer sockaddr
        port = njt_inet_get_port(hc_peer->peer.sockaddr);
        njt_memzero(host, NJT_SOCKADDR_STRLEN + 1);
        // len = njt_sock_ntop(hc_peer->peer.sockaddr, hc_peer->peer.socklen, host, NJT_SOCKADDR_STRLEN, 0);
        njt_sock_ntop(hc_peer->peer.sockaddr, hc_peer->peer.socklen, host, NJT_SOCKADDR_STRLEN, 0);
        // tmp_str.data = host;
        // tmp_str.len = len;
        // njt_log_error(NJT_LOG_ERR, c->log, 0,
        //         "======================host:%V   port:%d", &tmp_str, port);

        if(hhccf->port > 0){
            port = hhccf->port;     //self port
        }

        /* Initial state, start making the connection. */
        if(mysql_ctx->use_ssl){
            status = mysql_real_connect_start(&sd->ret, &sd->mysql, (char *)host, user, password, db_name, port, NULL, CLIENT_SSL);
        }else{
            status = mysql_real_connect_start(&sd->ret, &sd->mysql, (char *)host, user, password, db_name, port, NULL, 0);
        }

        fd = mysql_get_socket(&sd->mysql);

        c = njt_get_connection(fd, njt_cycle->log);
        if (c == NULL) {
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "hc mysql get connection fail, fd:%d", fd);
            NEXT_IMMEDIATE(sd, 40);
        }

        c->log->connection = c->number;
        c->type = SOCK_STREAM;
        c->data = hc_peer;
        rev = c->read;
        wev = c->write;

        sd->c = c;

        rev->log = njt_cycle->log;
        wev->log = njt_cycle->log;
        rev->handler = njt_health_check_mysql_module_check_read_event_handler;
        // wev.cancelable = 1;
        wev->handler = njt_health_check_mysql_module_check_write_event_handler;

        //timeout
        // if (hhccf->timeout) {
        //     njt_add_timer(rev, hhccf->timeout);
        //     njt_add_timer(wev, hhccf->timeout);
        // }

        // wev.cancelable = 1;

        if (status)
            /* Wait for connect to complete. */
            njt_health_check_mysql_module_next_event(1, status, sd);
        else
            NEXT_IMMEDIATE(sd, 9);
        
        break;

    case 1:
        status = mysql_real_connect_cont(&sd->ret, &sd->mysql, event);
        if (status)
            njt_health_check_mysql_module_next_event(1, status, sd);
        else
            NEXT_IMMEDIATE(sd, 9);
        break;

    case 9:
        if (!sd->ret){
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "hc mysql has error ret is 0, error:%s", mysql_error(&sd->mysql));
            NEXT_IMMEDIATE(sd, 40);
        }

        NEXT_IMMEDIATE(sd, 10);
        break;

    case 10:
        status = mysql_real_query_start(&sd->err, &sd->mysql, (char *)mysql_ctx->select.data,
                                    mysql_ctx->select.len);
        if (status)
            njt_health_check_mysql_module_next_event(11, status, sd);
        else
            NEXT_IMMEDIATE(sd, 20);
        break;

    case 11:
        status = mysql_real_query_cont(&sd->err, &sd->mysql, event);
        if (status)
            njt_health_check_mysql_module_next_event(11, status, sd);
        else
            NEXT_IMMEDIATE(sd, 20);
        break;

    case 20:
        if (sd->err)
        {
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "hc mysql 20 has error:%s", mysql_error(&sd->mysql));
            NEXT_IMMEDIATE(sd, 40);
        }
        else
        {
            sd->result = mysql_use_result(&sd->mysql);
            if (!sd->result){
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "hc mysql use result has error:%s", mysql_error(&sd->mysql));
                NEXT_IMMEDIATE(sd, 40);
            }

            NEXT_IMMEDIATE(sd, 30);
        }
        break;

    case 30:
        status = mysql_fetch_row_start(&sd->row, sd->result);
        if (status)
            njt_health_check_mysql_module_next_event(31, status, sd);
        else
            NEXT_IMMEDIATE(sd, 39);
        break;

    case 31:
        status = mysql_fetch_row_cont(&sd->row, sd->result, event);
        if (status)
            njt_health_check_mysql_module_next_event(31, status, sd);
        else
            NEXT_IMMEDIATE(sd, 39);
        break;

    case 39:
        if (sd->row)
        {
            /* Got a row. */
            unsigned int i;
            for (i= 0; i < mysql_num_fields(sd->result); i++){
                // njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                //         "======================row:%s", sd->row[i]);
            }

            NEXT_IMMEDIATE(sd, 30);
        }
        else
        {
            if (mysql_errno(&sd->mysql))
            {
                /* An error occurred. */
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                        "hc mysql get row  error:%s", mysql_error(&sd->mysql));
            }
            else
            {
                /* EOF. */
                // njt_atomic_fetch_add(&count, 1);
                // if(count == test_count){
                    // njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    //     "======================get row  EOF, success ");
                // }
            }

            sd->check_status_ok = NJT_SMYSQL_HC_CHECK_OK;

            mysql_free_result(sd->result);

            //todo free resource
            NEXT_IMMEDIATE(sd, 40);
        }
        break;

    case 40:
        status = mysql_close_start(&sd->mysql);
        if (status)
            njt_health_check_mysql_module_next_event(41, status, sd);
        else
            NEXT_IMMEDIATE(sd, 50);
        break;

    case 41:
        status = mysql_close_cont(&sd->mysql, event);
        if (status)
        njt_health_check_mysql_module_next_event(41, status, sd);
        else
        NEXT_IMMEDIATE(sd, 50);
        break;

    case 50:
        /* We are done! */
        if(sd->check_status_ok == NJT_SMYSQL_HC_CHECK_FREE_RESOUCE){
            if(sd->c != NULL){
                njt_health_check_mysql_module_close_connection(sd->c);
            }
        }else if(sd->check_status_ok == NJT_SMYSQL_HC_CHECK_OK){
            // count++;
            // if(count == 500){
            //     njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0, "===================500 ok");
            //     count = 0;
            // }
            njt_health_check_mysql_module_free_peer_resource(hc_peer, NJT_OK);
        }else{
            njt_health_check_mysql_module_free_peer_resource(hc_peer, NJT_ERROR);
        }

        break;

    default:
        //todo error code
        break;
    }
}


static void njt_health_check_mysql_module_start(njt_health_check_peer_t *hc_peer){
    njt_health_check_conf_t             *hhccf = hc_peer->hhccf;
    njt_health_check_tcp_module_state_data_t             *sd = hc_peer->data;
    njt_health_check_mysql_module_conf_ctx_t  *mysql_ctx = hhccf->ctx;

    //init mysql context
    if(NULL == mysql_init(&sd->mysql)){
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "mysql init error for health check.");
        
        njt_destroy_pool(hc_peer->pool);
        return;
    }

    mysql_options(&sd->mysql, MYSQL_OPT_NONBLOCK, 0);

    if(mysql_ctx->use_ssl){
        unsigned int enable_ssl = 1; 
        mysql_options(&sd->mysql, MYSQL_OPT_SSL_ENFORCE, &enable_ssl);
        unsigned int ssl_mode = 0;
        mysql_options(&sd->mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &ssl_mode);
    }

    sd->ST = 0;

    njt_health_check_mysql_module_status_handler(hc_peer, 0, NULL);
}
