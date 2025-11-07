
/*
 * Copyright (C) 2021-2023 TMLake(Beijing) Technology Co., Ltd.
 */

#ifndef NJT_HEALTH_CHECK_REGISTER_H_
#define NJT_HEALTH_CHECK_REGISTER_H_

#include <njt_config.h>
#include <njt_core.h>
#include <njt_http.h>

typedef njt_int_t (*config_check_handler)(void *data);
typedef void (*start_check_handler)(void *peer_info);

struct njt_health_check_reg_info_s {
   njt_str_t               *server_type;
   config_check_handler    config_check;
   start_check_handler     start_check;
};

typedef struct njt_health_check_reg_info_s njt_health_check_reg_info_t;


njt_int_t njt_health_check_reg_handler(njt_health_check_reg_info_t *reg_info);

#endif //NJT_HEALTH_CHECK_REGISTER_H_