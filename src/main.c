#include <stdio.h>
#include "appster.h"

int exec_route(void* data) {
    redis_reply_t rp;

    rp = as_redis("SET hello %s", as_arg_string(0));
    as_free_redis_reply(&rp);

    rp = as_redis("GET hello");
    printf("hello %.*s\n", rp.len, rp.str);
    fflush(stdout);

    as_free_redis_reply(&rp);
    return 200;
}

int main() {
    appster_shema_entry_t shema[] = {
        {"hello", 0, AVT_STRING, 1},
        {NULL}
    };

    appster_t* a = as_alloc(1);

    as_add_route(a, "/", exec_route, shema, NULL);
    as_add_redis(a, "127.0.0.1", 6379);

    as_bind(a, "0.0.0.0", 8080, 2048);
    as_loop(a); // after this point, 'a' should not be tampered with!
    as_free(a);
    return 0;
}
