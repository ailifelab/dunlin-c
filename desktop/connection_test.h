#ifndef DUNLIN_CONNECTION_TEST_H
#define DUNLIN_CONNECTION_TEST_H
#include "internal.h"
int dc_connection_test_schema(dc_store *);
/* Current saved provider's exact configuration only. Caller owns the array. */
struct json_object *dc_connection_tests(dc_store *,struct json_object *providers);
#endif
