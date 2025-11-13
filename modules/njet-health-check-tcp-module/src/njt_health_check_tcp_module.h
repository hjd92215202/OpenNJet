#ifndef NJT_HEALTH_CHECK_TCP_MODULE_H_
#define NJT_HEALTH_CHECK_TCP_MODULE_H_

#include <njt_config.h>
#include <njt_core.h>
#include <njt_http.h>
#include <njt_health_check_common.h>



typedef struct njt_health_check_tcp_match_s {
    njt_str_t                  send;      /* content need to send */
    njt_str_t                  expect;    /* expect string or binary */
} njt_health_check_tcp_match_t;

typedef struct njt_health_check_tcp_module_conf_ctx_s {
    njt_health_check_checker_t       *checker;     /* type operations */
    njt_health_check_tcp_match_t               *match;       /* stream match rule */

    njt_str_t                         send;
    njt_str_t                         expect;

} njt_health_check_tcp_module_conf_ctx_t;


#endif