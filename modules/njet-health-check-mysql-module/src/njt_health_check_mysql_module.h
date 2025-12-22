#ifndef __NJT_HEALTH_CHECK_MYSQL_MODULE_H__
#define __NJT_HEALTH_CHECK_MYSQL_MODULE_H__

#include <mysql.h>
#include <mysqld_error.h>
#include <njt_config.h>
#include <njt_core.h>
#include <njt_http.h>
#include <njt_health_check_common.h>

typedef struct njt_health_check_tcp_module_state_data_s{
    int                   ST;                                    /* State machine current state */
    MYSQL                 mysql;
    MYSQL_RES             *result;
    MYSQL                 *ret;
    int                   err;
    MYSQL_ROW             row;

    njt_flag_t            check_status_ok;    //1: ok     0: not ok

    njt_connection_t      *c;
}njt_health_check_tcp_module_state_data_t;


//mysql
typedef struct njt_health_check_mysql_module_conf_ctx_s {
    njt_str_t                         select;
    njt_flag_t                        use_ssl;
    njt_str_t                         username;
    njt_str_t                         password;
    njt_str_t                         db_name;
} njt_health_check_mysql_module_conf_ctx_t;


#endif