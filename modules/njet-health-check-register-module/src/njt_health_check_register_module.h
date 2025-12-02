
/*
 * Copyright (C) 2021-2023 TMLake(Beijing) Technology Co., Ltd.
 */

#ifndef NJT_HEALTH_CHECK_REGISTER_H_
#define NJT_HEALTH_CHECK_REGISTER_H_

#include <njt_config.h>
#include <njt_core.h>
#include <njt_http.h>
#include <njt_stream.h>
#include <njet.h>
#include <njt_event.h>
#include <njt_json_api.h>
#include <njt_json_util.h>



typedef void (*interal_update_handler)(void *hc_peer_info, njt_int_t status);
typedef void (*start_check_handler)(void *hc_peer_info, interal_update_handler udpate_handler);
typedef void *(*create_ctx_handler)(void *hhccf_info, void *api_data_info);

struct njt_health_check_reg_info_s {
   njt_str_t               *server_type;
   create_ctx_handler      create_ctx;
   start_check_handler     start_check;
};

typedef struct njt_health_check_reg_info_s njt_health_check_reg_info_t;

njt_int_t njt_health_check_reg_handler(njt_health_check_reg_info_t *reg_info);
njt_health_check_reg_info_t *
njt_health_check_register_find_handler(njt_str_t *server_type);




#endif //NJT_HEALTH_CHECK_REGISTER_H_