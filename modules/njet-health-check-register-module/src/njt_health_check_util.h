/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) 2021-2023  TMLake(Beijing) Technology Co., Ltd.
 */

#ifndef NJT_COMMON_HEALTH_CHECK_H
#define NJT_COMMON_HEALTH_CHECK_H

#include <njt_config.h>
#include <njt_core.h>
#include <njet.h>
#include <njt_event.h>
#include <njt_json_api.h>
#include <njt_json_util.h>
#include <njt_http.h>
#include <njt_health_check_common.h>



#if (NJT_OPENSSL)

njt_int_t njt_health_check_json_parse_ssl_protocols(njt_str_t value, njt_uint_t *np);

njt_int_t njt_health_check_set_ssl(njt_health_check_conf_t *hhccf, njt_health_check_ssl_conf_t *hcscf);

#endif

njt_stream_upstream_srv_conf_t *njt_health_check_find_stream_upstream_by_name(njt_cycle_t *cycle, njt_str_t *name);
njt_http_upstream_srv_conf_t* njt_health_check_find_http_upstream_by_name(njt_cycle_t *cycle,njt_str_t *name);


#endif //NJET_MAIN_NJT_COMMON_HEALTH_CHECK_H
