static void njt_smysql_free_peer_resource(njt_stream_health_check_peer_t *hc_peer) {
    njt_connection_t                *c = hc_peer->sd.c;
    njt_health_check_conf_t  *hhccf = hc_peer->hhccf;

    if(hhccf->ref_count>0){
        hhccf->ref_count--;
    }
        
    if(c != NULL){
        njt_smysql_hc_close_connection(c);
    }

    // njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
    //     "====smysql free peer check, peerid:%d ref_count=%d", hc_peer->peer_id, hhccf->ref_count);

    if (hc_peer->pool) {
        njt_destroy_pool(hc_peer->pool);
    }
    return;
}

static njt_int_t 
njt_smysql_context_set(njt_hc_api_data_t *api_data, njt_health_check_conf_t *hhccf) {
    njt_smysql_health_check_conf_ctx_t  *smysql_ctx = NULL; 
    njt_str_t                           *val = NULL;

    /* smysql health check context */
    smysql_ctx = hhccf->ctx;

    if (api_data->hc_data->is_sql_set) {
        if(api_data->hc_data->sql->is_select_set && api_data->hc_data->sql->select.len > 0){
            val = &api_data->hc_data->sql->select;
            smysql_ctx->select.data = njt_pcalloc(hhccf->pool, val->len + 1);
            if(NULL == smysql_ctx->select.data) {
                njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, 
                    "smysql select malloc error, select:%V", val);
                return HC_SERVER_ERROR;
            }

            smysql_ctx->select.len = val->len;
            njt_memcpy(smysql_ctx->select.data, val->data, val->len);
        }

        smysql_ctx->use_ssl = 1;      //default use ssl
        if(api_data->hc_data->sql->is_useSsl_set){
            smysql_ctx->use_ssl = api_data->hc_data->sql->useSsl;
        }

        if(api_data->hc_data->sql->is_user_set && api_data->hc_data->sql->user.len > 0){
            val = &api_data->hc_data->sql->user;
            smysql_ctx->username.data = njt_pcalloc(hhccf->pool, val->len + 1);
            if(NULL == smysql_ctx->username.data) {
                njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, 
                    "smysql user malloc error, user:%V", val);
                return HC_SERVER_ERROR;
            }

            smysql_ctx->username.len = val->len;
            njt_memcpy(smysql_ctx->username.data, val->data, val->len);
        }

        if(api_data->hc_data->sql->is_password_set && api_data->hc_data->sql->password.len > 0){
            val = &api_data->hc_data->sql->password;
            smysql_ctx->password.data = njt_pcalloc(hhccf->pool, val->len + 1);
            if(NULL == smysql_ctx->password.data) {
                njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, 
                    "smysql password malloc error, password:%V", val);
                return HC_SERVER_ERROR;
            }

            smysql_ctx->password.len = val->len;
            njt_memcpy(smysql_ctx->password.data, val->data, val->len);
        }

        if(!api_data->hc_data->sql->is_db_set || api_data->hc_data->sql->db.len < 1){
            njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, 
                    "smysql db_name must be set and not empty", val);
            return HC_SMYSQL_DB_NOT_SET_ERROR;
        }

        if(api_data->hc_data->sql->is_db_set && api_data->hc_data->sql->db.len > 0){
            val = &api_data->hc_data->sql->db;
            smysql_ctx->db_name.data = njt_pcalloc(hhccf->pool, val->len + 1);
            if(NULL == smysql_ctx->db_name.data) {
                njt_log_error(NJT_LOG_EMERG, hhccf->log, 0, 
                    "smysql db_name malloc error, db_name:%V", val);
                return HC_SERVER_ERROR;
            }

            smysql_ctx->db_name.len = val->len;
            njt_memcpy(smysql_ctx->db_name.data, val->data, val->len);
        }
    }

    return HC_SUCCESS;
}


static void njt_smysql_hc_close_connection(njt_connection_t *c)
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
njt_smysql_hc_next_event(int new_st, int status, njt_smysql_state_data_t *sd)
{
    if (status & MYSQL_WAIT_READ){
        //remove write event
        if(sd->c->write->active){
            njt_del_event(sd->c->write, NJT_WRITE_EVENT, 0);
        }
        
        if (njt_add_event(sd->c->read, NJT_READ_EVENT, NJT_CLEAR_EVENT) != NJT_OK) {
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "hc smysql add read event fail, fd:%d", sd->c->fd);
        }
    }
    if (status & MYSQL_WAIT_WRITE){
        if(sd->c->read->active){
            njt_del_event(sd->c->read, NJT_READ_EVENT, 0);
        }
        if (njt_add_event(sd->c->write, NJT_WRITE_EVENT, NJT_CLEAR_EVENT) != NJT_OK) {
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "hc smysql add write event, fd:%d", sd->c->fd);
        }
    }

    if (status & MYSQL_WAIT_TIMEOUT)
    {
        //free connection close task
        njt_log_error(NJT_LOG_INFO, njt_cycle->log, 0,
                "hc smysql timeout, fd:%d", sd->c->fd);
    }

    sd->ST = new_st;
}


#define NEXT_IMMEDIATE(sd_, new_st) do { sd_->ST= new_st; goto again; } while (0)


static void njt_smysql_hc_check_read_event_handler(njt_event_t *ev){
    njt_stream_health_check_peer_t      *hc_peer = NULL;
    njt_connection_t                    *c = NULL;

    c = ev->data;
    hc_peer = c->data;

    if(ev->timedout){
        /*log the case and update the peer status.*/
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                "mysql read action for health check timeout");

        ev->timedout = 0;

        hc_peer->sd.ST = 40;
        hc_peer->sd.check_status_ok = NJT_SMYSQL_HC_CHECK_ERROR;
        njt_smysql_hc_status_handler(NULL, MYSQL_WAIT_READ, ev);

        return;
    }

    if (hc_peer->hhccf->disable) {
        //close
        hc_peer->sd.ST = 40;
        hc_peer->sd.check_status_ok = NJT_SMYSQL_HC_CHECK_FREE_RESOUCE;
        njt_smysql_hc_status_handler(NULL, MYSQL_WAIT_READ, ev);

        return;
    }

    njt_smysql_hc_status_handler(NULL, MYSQL_WAIT_READ, ev);
}

static void njt_smysql_hc_check_write_event_handler(njt_event_t *ev){
    njt_stream_health_check_peer_t      *hc_peer = NULL;
    njt_connection_t                    *c = NULL;

    c = ev->data;
    hc_peer = c->data;

    if(ev->timedout){
        /*log the case and update the peer status.*/
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                "mysql write action for health check timeout");

        ev->timedout = 0;

        hc_peer->sd.ST = 40;
        hc_peer->sd.check_status_ok = NJT_SMYSQL_HC_CHECK_ERROR;
        njt_smysql_hc_status_handler(NULL, MYSQL_WAIT_WRITE, ev);
        return;
    }

    if (hc_peer->hhccf->disable) {
        //close
        hc_peer->sd.ST = 40;
        hc_peer->sd.check_status_ok = NJT_SMYSQL_HC_CHECK_FREE_RESOUCE;
        njt_smysql_hc_status_handler(NULL, MYSQL_WAIT_WRITE, ev);

        return;
    }

    njt_smysql_hc_status_handler(NULL, MYSQL_WAIT_WRITE, ev);
}



static void njt_smysql_hc_status_handler(njt_stream_health_check_peer_t *init_hc_peer,
        njt_int_t event, njt_event_t *ev){
    njt_stream_health_check_peer_t      *hc_peer = NULL;
    njt_health_check_conf_t      *hhccf = NULL;
    njt_smysql_state_data_t             *sd = NULL;
    njt_smysql_health_check_conf_ctx_t  *smysql_ctx = NULL;
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
    smysql_ctx = hhccf->ctx;
    sd = &hc_peer->sd;

again:
    switch(sd->ST)
    {
    case 0:
        if(smysql_ctx->db_name.len > 0){
            db_name = (char *)smysql_ctx->db_name.data;     //will be end with 0
        }

        if(smysql_ctx->username.len > 0){
            user = (char *)smysql_ctx->username.data;     //will be end with 0
        }

        if(smysql_ctx->password.len > 0){
            password = (char *)smysql_ctx->password.data;     //will be end with 0
        }

        //get host and port from peer sockaddr
        port = njt_inet_get_port(hc_peer->peer.sockaddr);
        njt_memzero(host, NJT_SOCKADDR_STRLEN + 1);
        // len = njt_sock_ntop(hc_peer->peer.sockaddr, hc_peer->peer.socklen, host, NJT_SOCKADDR_STRLEN, 0);
        njt_sock_ntop(hc_peer->peer.sockaddr, hc_peer->peer.socklen, host, NJT_SOCKADDR_STRLEN, 0);
        // tmp_str.data = host;
        // tmp_str.len = len;
        // njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
        //         "======================host:%V   port:%d", &tmp_str, port);

        if(hhccf->port > 0){
            port = hhccf->port;     //self port
        }

        /* Initial state, start making the connection. */
        if(smysql_ctx->use_ssl){
            status = mysql_real_connect_start(&sd->ret, &sd->mysql, (char *)host, user, password, db_name, port, NULL, CLIENT_SSL);
        }else{
            status = mysql_real_connect_start(&sd->ret, &sd->mysql, (char *)host, user, password, db_name, port, NULL, 0);
        }

        hhccf->ref_count++;

        fd = mysql_get_socket(&sd->mysql);

        c = njt_get_connection(fd, njt_cycle->log);
        if (c == NULL) {
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "hc smysql get connection fail, fd:%d", fd);
            NEXT_IMMEDIATE(sd, 40);
        }
        c->type = SOCK_STREAM;
        c->data = hc_peer;
        rev = c->read;
        wev = c->write;

        sd->c = c;

        rev->log = njt_cycle->log;
        wev->log = njt_cycle->log;
        rev->handler = njt_smysql_hc_check_read_event_handler;
        // wev.cancelable = 1;
        wev->handler = njt_smysql_hc_check_write_event_handler;

        //timeout
        // if (hhccf->timeout) {
        //     njt_add_timer(rev, hhccf->timeout);
        //     njt_add_timer(wev, hhccf->timeout);
        // }

        // wev.cancelable = 1;

        if (status)
            /* Wait for connect to complete. */
            njt_smysql_hc_next_event(1, status, sd);
        else
            NEXT_IMMEDIATE(sd, 9);
        
        break;

    case 1:
        status = mysql_real_connect_cont(&sd->ret, &sd->mysql, event);
        if (status)
            njt_smysql_hc_next_event(1, status, sd);
        else
            NEXT_IMMEDIATE(sd, 9);
        break;

    case 9:
        if (!sd->ret){
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "hc smysql has error ret is 0, error:%s", mysql_error(&sd->mysql));
            NEXT_IMMEDIATE(sd, 40);
        }

        NEXT_IMMEDIATE(sd, 10);
        break;

    case 10:
        status = mysql_real_query_start(&sd->err, &sd->mysql, (char *)smysql_ctx->select.data,
                                    smysql_ctx->select.len);
        if (status)
            njt_smysql_hc_next_event(11, status, sd);
        else
            NEXT_IMMEDIATE(sd, 20);
        break;

    case 11:
        status = mysql_real_query_cont(&sd->err, &sd->mysql, event);
        if (status)
            njt_smysql_hc_next_event(11, status, sd);
        else
            NEXT_IMMEDIATE(sd, 20);
        break;

    case 20:
        if (sd->err)
        {
            njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "hc smysql 20 has error:%s", mysql_error(&sd->mysql));
            NEXT_IMMEDIATE(sd, 40);
        }
        else
        {
            sd->result = mysql_use_result(&sd->mysql);
            if (!sd->result){
                njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "hc smysql use result has error:%s", mysql_error(&sd->mysql));
                NEXT_IMMEDIATE(sd, 40);
            }

            NEXT_IMMEDIATE(sd, 30);
        }
        break;

    case 30:
        status = mysql_fetch_row_start(&sd->row, sd->result);
        if (status)
            njt_smysql_hc_next_event(31, status, sd);
        else
            NEXT_IMMEDIATE(sd, 39);
        break;

    case 31:
        status = mysql_fetch_row_cont(&sd->row, sd->result, event);
        if (status)
            njt_smysql_hc_next_event(31, status, sd);
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
                        "hc smysql get row  error:%s", mysql_error(&sd->mysql));
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
            njt_smysql_hc_next_event(41, status, sd);
        else
            NEXT_IMMEDIATE(sd, 50);
        break;

    case 41:
        status = mysql_close_cont(&sd->mysql, event);
        if (status)
        njt_smysql_hc_next_event(41, status, sd);
        else
        NEXT_IMMEDIATE(sd, 50);
        break;

    case 50:
        /* We are done! */
        if(sd->check_status_ok == NJT_SMYSQL_HC_CHECK_FREE_RESOUCE){
            njt_smysql_free_peer_resource(hc_peer);
        }else if(sd->check_status_ok == NJT_SMYSQL_HC_CHECK_OK){
            // count++;
            // if(count == 500){
            //     njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0, "===================500 ok");
            //     count = 0;
            // }
            njt_stream_health_check_common_update(hc_peer, NJT_OK);
        }else{
            njt_stream_health_check_common_update(hc_peer, NJT_ERROR);
        }

        break;

    default:
        //todo error code
        break;
    }
}



void njt_smysql_hc_start(njt_stream_health_check_peer_t *hc_peer){
    njt_health_check_conf_t      *hhccf = hc_peer->hhccf;
    njt_smysql_state_data_t             *sd = &hc_peer->sd;
    njt_smysql_health_check_conf_ctx_t  *smysql_ctx = hhccf->ctx;

    //init mysql context
    if(NULL == mysql_init(&sd->mysql)){
        njt_log_error(NJT_LOG_ERR, njt_cycle->log, 0,
                    "mysql init error for health check.");
        
        njt_destroy_pool(hc_peer->pool);
        return;
    }

    mysql_options(&sd->mysql, MYSQL_OPT_NONBLOCK, 0);

    if(smysql_ctx->use_ssl){
        unsigned int enable_ssl = 1; 
        mysql_options(&sd->mysql, MYSQL_OPT_SSL_ENFORCE, &enable_ssl);
        unsigned int ssl_mode = 0;
        mysql_options(&sd->mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &ssl_mode);
    }

    sd->ST = 0;

    njt_smysql_hc_status_handler(hc_peer, 0, NULL);
}
