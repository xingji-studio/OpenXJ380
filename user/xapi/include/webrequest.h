#pragma once

#include "stdint.h"
#include "xposix/stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

char *xapi_httpGet(const char *url, uint16_t port, bool tls);

#ifdef __cplusplus
}
#endif
