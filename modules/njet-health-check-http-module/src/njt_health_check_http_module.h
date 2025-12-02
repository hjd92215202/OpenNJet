#ifndef NJT_HEALTH_CHECK_HTTP_MODULE_H_
#define NJT_HEALTH_CHECK_HTTP_MODULE_H_

#include <njt_config.h>
#include <njt_core.h>
#include <njt_http.h>
#include <njt_health_check_common.h>


typedef struct njt_health_check_http_match_code_s {
    /*range such as 100-200 or a single one such as 188*/
    njt_flag_t  single;
    njt_uint_t  code;
    njt_uint_t  last_code;
} njt_health_check_http_match_code_t;


typedef struct njt_health_check_http_match_status_s {
    njt_flag_t    not_operation;
    /*array of the njt_http_status_t */
    njt_array_t   codes;
} njt_health_check_http_match_status_t;



typedef struct njt_health_check_http_match_header_s {
    njt_str_t      key;
    njt_str_t      value;
    njt_regex_t    *regex;
    njt_uint_t     operation;
} njt_health_check_http_match_header_t;


typedef struct njt_health_check_http_match_body_s {
    njt_uint_t           operation;
    njt_regex_t          *regex;
    njt_str_t            value;
} njt_health_check_http_match_body_t;


typedef struct njt_health_check_http_match_s {
    njt_flag_t                defined;
    njt_flag_t                conditions;
    njt_str_t                 name;
    njt_health_check_http_match_status_t   status;
    /*array of njt_health_check_http_match_header_t*/
    njt_array_t               headers;
    njt_health_check_http_match_body_t     body;
} njt_health_check_http_match_t;

typedef struct njt_health_check_http_module_conf_ctx_s {
    njt_health_check_checker_t *checker;
    njt_health_check_http_match_t *match;
    njt_str_t uri;
    njt_flag_t only_check_port;
    njt_str_t body;
    njt_str_t status;
    njt_array_t headers;
    // njt_http_upstream_srv_conf_t *upstream;         //remove because dynupstream maybe has free, will be core if use
} njt_health_check_http_module_conf_ctx_t;


/*Structure used for holding http parser internal info*/
struct njt_health_check_http_parse_s {
    njt_uint_t state;
    njt_uint_t code;
    njt_flag_t done;
    njt_flag_t body_used;
    njt_uint_t stage;
    u_char *status_text;
    u_char *status_text_end;
    njt_uint_t count;
    njt_flag_t chunked;
    off_t content_length_n;
    njt_array_t headers;
    u_char *header_name_start;
    u_char *header_name_end;
    u_char *header_start;
    u_char *header_end;
    u_char *body_start;

    njt_int_t (*process)(njt_health_check_peer_t *hc_peer);

    njt_msec_t start;
};

typedef struct njt_health_check_http_parse_s njt_health_check_http_parse_t;

#endif
