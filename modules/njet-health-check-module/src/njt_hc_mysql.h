
typedef struct njt_smysql_state_data_s{
    int                   ST;                                    /* State machine current state */
    MYSQL                 mysql;
    MYSQL_RES             *result;
    MYSQL                 *ret;
    int                   err;
    MYSQL_ROW             row;

    njt_flag_t            check_status_ok;    //1: ok     0: not ok

    njt_connection_t      *c;
}njt_smysql_state_data_t;


//mysql
typedef struct njt_smysql_health_check_conf_ctx_s {
    njt_health_checker_t             *checker;     /* type operations */
    njt_str_t                         select;
    njt_flag_t                        use_ssl;
    njt_str_t                         username;
    njt_str_t                         password;
    njt_str_t                         db_name;

    njt_stream_upstream_srv_conf_t   *upstream;    /* stream upstream server conf */

} njt_smysql_health_check_conf_ctx_t;



static void njt_smysql_hc_status_handler(njt_stream_health_check_peer_t *init_hc_peer,
        njt_int_t event, njt_event_t *ev);
// static char *njt_http_health_check_conf(njt_conf_t *cf, njt_command_t *cmd, void *conf);
static njt_int_t 
njt_smysql_context_set(njt_hc_api_data_t *api_data, njt_health_check_conf_t *hhccf);
static void njt_smysql_hc_close_connection(njt_connection_t *c);