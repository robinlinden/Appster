#include "appster.h"
#include "appster_struct.h"

#include "log.h"
#include "evbuffer.h"
#include "shema.h"
#include "channel.h"
#include "http_parser.h"

#include <stdlib.h>
#include <ctype.h>
#include <uv.h>
#include <libdill.h>

typedef struct error_cb_s {
    as_route_cb_t cb;
    void* user_data;
} error_cb_t;

typedef struct context_s {
    struct connection_s* con;
    uv_write_t* write;
    hashmap_t* headers,* send_headers;
    evbuffer_t* body,* send_body;
    value_t** vars;
    shema_t* sh;
    channel_t channel;
    int handle;
    char* str;
    struct {
        uint32_t parse_error:1;
        uint32_t parsed_arguments:1;
        uint32_t parsed_field:1;
        uint32_t should_keepalive:1;
    } flag;
#define appster con->tcp->loop->data
} context_t;

typedef struct connection_s {
    http_parser_t parser[1];
    vector_t contexts;
    uv_stream_t* tcp;
} connection_t;

typedef union addr_u {
    sa_family_t af;
    struct sockaddr sa[1];
    struct sockaddr_in sin[1];
    struct sockaddr_in6 sin6[1];
} addr_t;

__thread context_t* __active_ctx = NULL;

#define __AP_PREAMPLE \
    context_t* ctx; \
    appster_t* a; \
    ctx = parser_get_context(p); \
    if (ctx->flag.parse_error) { \
        return 0; \
    } \
    a = ctx->appster; \
    (void) a

#define __AP_DATA_CB http_parser_t* p, const char *at, size_t len
#define __AP_EVENT_CB http_parser_t* p

/* misc */
inline void to_lower(char* str);
static int hm_cb_free(const void* key, void* value, void* context);
static int hm_cb_sh_free(const void* key, void* value, void* context);
static int basic_error(void* data);
static void send_reply(context_t* ctx, int status);
static int add_header(const void* key, void* value, void* context);
coroutine void execute_context();
/* Connection and messages */
static void bind_listener(uv_loop_t* loop, const addr_t* ad, int backlog);
static void run_loop(void* loop);
static void on_new_connection(uv_stream_t *tcp, int status);
static void alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
static void read_cb(uv_stream_t* c, ssize_t nread, const uv_buf_t* buf);
static void write_cb(uv_write_t* req, int status);
static void free_context(context_t* ctx);
static void free_connection(uv_handle_t* handle);
/* Incoming message parsing functions */
static int on_parse_error(context_t* ctx);
static int on_message_begin(__AP_EVENT_CB);
static int on_inc_url(__AP_DATA_CB);
static int on_inc_header_field(__AP_DATA_CB);
static int on_inc_header_value(__AP_DATA_CB);
static int on_inc_headers_complete(__AP_EVENT_CB);
static int on_inc_body(__AP_DATA_CB);
static int on_inc_message_complete(__AP_EVENT_CB);
static int complete_header(__AP_EVENT_CB);
/* Casts and getters */
static context_t* parser_get_context(http_parser_t* p);
/* Redis funcs */
#ifndef DISABLE_REDIS
void redis_remote_initialize_for_this_thread(uv_loop_t* loop);
void redis_remote_cleanup_for_this_thread();
void free_redis_remote(void* value);
#endif

static http_parser_settings incoming = {
    on_message_begin,
    on_inc_url,
    NULL,           // on_status
    on_inc_header_field,
    on_inc_header_value,
    on_inc_headers_complete,
    on_inc_body,
    on_inc_message_complete,
    NULL,           // on_chunk
    NULL,           // on_chunk_complete
};

appster_t* as_alloc(unsigned threads) {
    __log_set_file(stdout);

    int err;
    uv_loop_t* loop;
    appster_t* rc;

    rc = calloc(1, sizeof(appster_t));

    vector_setup(rc->loops, threads, sizeof(uv_loop_t*));

    for (unsigned i = 1; i < threads; i++) {
        loop = malloc(sizeof(uv_loop_t));
        err = uv_loop_init(loop);
        if (err != 0) {
            ELOG("Failed to initialize uv loop %s", uv_strerror(err));
            goto fail;
        }
        vector_push_back(rc->loops, &loop);
    }

    vector_setup(rc->redises, 10, sizeof(void*));
    rc->general_error_cb = malloc((sizeof(error_cb_t)));
    rc->general_error_cb->cb = basic_error;
    rc->general_error_cb->user_data = NULL;
    rc->routes = hm_alloc(10, NULL, NULL);
    rc->error_cbs = hm_alloc(10, NULL, NULL);
    return rc;

fail:
    as_free(rc);
    return NULL;
}
void as_free(appster_t* a) {
    if (!a)
        return;

    VECTOR_FOR_EACH(a->loops, loop) {
        uv_loop_close(ITERATOR_GET_AS(uv_loop_t*, &loop));
        free(ITERATOR_GET_AS(uv_loop_t*, &loop));
    }

#ifndef DISABLE_REDIS
    VECTOR_FOR_EACH(a->redises, redis) {
        free_redis_remote(ITERATOR_GET_AS(void*, &redis));
    }
#endif

    vector_destroy(a->loops);
    hm_foreach(a->routes, hm_cb_free, NULL);
    hm_free(a->routes);
    hm_foreach(a->error_cbs, hm_cb_free, (void*) 1);
    hm_free(a->error_cbs);
    free(a->general_error_cb);
    free(a);
}
int as_add_route(appster_t* a, const char* path, as_route_cb_t cb, appster_shema_entry_t* shema, void* user_data) {
    static appster_shema_entry_t empty_shema[] = { { NULL } };
    shema_t* sh;

    lassert(a);
    lassert(path);
    lassert(strlen(path));
    lassert(path[0] == '/');
    lassert(cb);

    if (!shema) {
        shema = empty_shema;
    }

    sh = sh_alloc(path, shema, cb, user_data);
    if (!sh) {
        ELOG("Failed to create shema for '%s' from supplied information", path);
        return -1;
    }

    hm_put(a->routes, sh_get_path(sh), sh);
    return 0;
}
int as_add_route_error(appster_t* a, const char* path, as_route_cb_t cb, void* user_data) {
    error_cb_t* err;

    lassert(a);
    lassert(cb);

    if (!path || !strlen(path)) {
        a->general_error_cb->cb = cb;
        a->general_error_cb->user_data = user_data;
    } else {
        err = malloc(sizeof(error_cb_t));
        err->cb = cb;
        err->user_data = user_data;
        free(hm_put(a->error_cbs, strdup(path), err));
    }

    return 0;
}
int as_bind(appster_t* a, const char* addr, uint16_t port, int backlog) {
    addr_t ad;

    lassert(a);

    // TODO set nodelay flag

    if (uv_ip4_addr(addr, port, ad.sin) != 0 &&
            uv_ip6_addr(addr, port, ad.sin6) != 0) {
        ELOG("Failed to parse ip address: %s", addr);
        return -1;
    }

    if (!vector_size(a->loops)) {
        bind_listener(uv_default_loop(), &ad, backlog);
    } else  {
        VECTOR_FOR_EACH(a->loops, loop) {
            bind_listener(ITERATOR_GET_AS(uv_loop_t*, &loop), &ad, backlog);
        }
    }

    return 0;
}
int as_loop(appster_t* a) {
    int err;
    vector_t threads;
    uv_thread_t id;

    lassert(a);

    if (!vector_size(a->loops)) {
        uv_default_loop()->data = a;
        run_loop(uv_default_loop());
    } else {
        vector_setup(threads, vector_size(a->loops), sizeof(uv_thread_t));

        VECTOR_FOR_EACH(a->loops, loop) {
            ITERATOR_GET_AS(uv_loop_t*, &loop)->data = a;
            err = uv_thread_create(&id, run_loop, ITERATOR_GET_AS(uv_loop_t*, &loop));
            if (err != 0) {
                FLOG("Failed to create thread %s", uv_strerror(err));
            }
            vector_push_back(threads, &id);
        }

        VECTOR_FOR_EACH(threads, thread) {
            id = ITERATOR_GET_AS(uv_thread_t, &thread);
            err = uv_thread_join(&id);
            if (err != 0) {
                ELOG("Failed to join thread %s", uv_strerror(err));
            }
        }

        vector_destroy(threads);
    }

    return err;
}
int as_arg_flag(uint32_t idx) {
    lassert(__active_ctx && __active_ctx->sh);
    return sh_arg_flag(__active_ctx->sh, __active_ctx->vars, idx);
}
uint64_t as_arg_integer(uint32_t idx) {
    lassert(__active_ctx && __active_ctx->sh);
    return sh_arg_integer(__active_ctx->sh, __active_ctx->vars, idx);
}
double as_arg_number(uint32_t idx) {
    lassert(__active_ctx && __active_ctx->sh);
    return sh_arg_number(__active_ctx->sh, __active_ctx->vars, idx);
}
const char* as_arg_string(uint32_t idx) {
    lassert(__active_ctx && __active_ctx->sh);
    return sh_arg_string(__active_ctx->sh, __active_ctx->vars, idx);
}
uint32_t as_arg_string_length(uint32_t idx) {
    lassert(__active_ctx && __active_ctx->sh);
    return sh_arg_string_length(__active_ctx->sh, __active_ctx->vars, idx);
}
uint32_t as_arg_list_length(uint32_t idx) {
    lassert(__active_ctx && __active_ctx->sh);
    return sh_arg_list_length(__active_ctx->sh, __active_ctx->vars, idx);
}
uint64_t as_arg_list_integer(uint32_t idx, uint32_t list_idx) {
    lassert(__active_ctx && __active_ctx->sh);
    return sh_arg_list_integer(__active_ctx->sh, __active_ctx->vars, idx, list_idx);
}
double as_arg_list_number(uint32_t idx, uint32_t list_idx) {
    lassert(__active_ctx && __active_ctx->sh);
    return sh_arg_list_number(__active_ctx->sh, __active_ctx->vars, idx, list_idx);
}
const char* as_arg_list_string(uint32_t idx, uint32_t list_idx) {
    lassert(__active_ctx && __active_ctx->sh);
    return sh_arg_list_string(__active_ctx->sh, __active_ctx->vars, idx, list_idx);
}
uint32_t as_arg_list_string_length(uint32_t idx, uint32_t list_idx) {
    lassert(__active_ctx && __active_ctx->sh);
    return sh_arg_list_string_length(__active_ctx->sh, __active_ctx->vars, idx, list_idx);
}

void to_lower(char* str) {
    for(int i = 0; str[i]; i++){
      str[i] = tolower(str[i]);
    }
}
int hm_cb_free(const void* key, void* value, void* context) {
    if (context)
        free((void*)key);
    free(value);
    return 1;
}
int hm_cb_sh_free(const void* key, void* value, void* context) {
    sh_free(value);
    return 1;
}
int basic_error(void* data) {
    return 500;
}
void send_reply(context_t* ctx, int status) {
    evbuffer_t* buf;
    int len;

    buf = evbuffer_new();

    evbuffer_add_printf(buf,
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Length: %zu\r\n",
                        status, http_status_str(status),
                        ctx->send_body
                            ? evbuffer_get_length(ctx->send_body)
                            : (size_t) 0
                        );

    // remove content length header if present
    free(hm_remove(ctx->send_headers, "content-length"));

    // send headers if present
    hm_foreach(ctx->send_headers, add_header, ctx->send_body);
    hm_foreach(ctx->send_headers, hm_cb_free, 0);
    hm_free(ctx->send_headers);
    ctx->send_headers = NULL;

    evbuffer_add(buf, "\r\n", 2);
    if (ctx->send_body) {
        evbuffer_add_buffer(buf, ctx->send_body);
        evbuffer_free(ctx->send_body);
    }

    ctx->write = malloc(sizeof(uv_write_t));
    ctx->write->data = ctx;
    ctx->send_body = buf;

    len = MIN(65536, evbuffer_get_length(buf));

    ctx->str = malloc(len);
    uv_buf_t buffer = {
        ctx->str,
        evbuffer_remove(buf, ctx->str, len)
    };

    uv_write(ctx->write, ctx->con->tcp, &buffer, 1, write_cb);
}
int add_header(const void* key, void* value, void* context) {
    evbuffer_add_printf(context, "%s: %s\r\n", (const char*)key, (char*)value);
    return 1;
}
void execute_context() {
    // This code is executed in coroutine. It's best to keep the stack as low
    // as possible.

    int status, handle;
    context_t* ctx;
    appster_t* a;

    ctx = __active_ctx;
    a = ctx->appster;

    if (ctx->flag.parse_error) {
        error_cb_t* cb = hm_get(a->error_cbs, sh_get_path(ctx->sh));

        if (cb && cb->cb) {
            status = cb->cb(cb->user_data);
        } else {
            status = a->general_error_cb->cb(a->general_error_cb->user_data);
        }
    } else {
        status = sh_call_cb(ctx->sh);
    }

    __active_ctx = NULL;

    if (status > 0) {
        send_reply(ctx, status);
    } else {
        uv_close((uv_handle_t*) ctx->con->tcp, free_connection);
    }
}
void bind_listener(uv_loop_t* loop, const addr_t* ad, int backlog) {
    int err, fd, one = 1;
    uv_tcp_t* tcp = malloc(sizeof(uv_tcp_t));

    err = uv_tcp_init(loop, tcp);
    if (err != 0) {
        FLOG("Failed to initialize tcp socket %s", uv_strerror(err));
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    err = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    if (err != 0) {
        FLOG("Failed to set SO_REUSEPORT %s", strerror(errno));
    }

    err = uv_tcp_open(tcp, fd);
    if (err != 0) {
        FLOG("Failed to open tcp handle %s", uv_strerror(err));
    }

    err = uv_tcp_bind(tcp, ad->sa, 0);
    if (err != 0) {
        FLOG("Tcp bind failed %s", uv_strerror(err));
    }

    err = uv_listen((uv_stream_t*) tcp, backlog, on_new_connection);
    if (err != 0) {
        FLOG("Tcp listen failed %s", uv_strerror(err));
    }
}
void run_loop(void* loop) {
    int err;

    DLOG("Running event loop");

#ifndef DISABLE_REDIS
    redis_remote_initialize_for_this_thread(loop);
#endif

    err = uv_run(loop, UV_RUN_DEFAULT);
    if (err != 0) {
        ELOG("Failed to run uv loop %s", uv_strerror(err));
    } else {
        ELOG("Run complete");
    }

#ifndef DISABLE_REDIS
    redis_remote_cleanup_for_this_thread();
#endif
}
void on_new_connection(uv_stream_t *tcp, int status) {
    int err;

    if (status < 0) {
        ELOG("New connection error %s", uv_strerror(status));
        return;
    }

    uv_tcp_t* c = calloc(1, sizeof(uv_tcp_t));
    err = uv_tcp_init(tcp->loop, c);

    if (err != 0) {
        goto fail;
    }

    err = uv_accept(tcp, (uv_stream_t*) c);

    if (err != 0) {
        goto fail;
    }

    uv_read_start((uv_stream_t*) c, alloc_cb, read_cb);

    connection_t* con = calloc(1, sizeof(connection_t));

    http_parser_init(con->parser, HTTP_REQUEST);
    vector_setup(con->contexts, 5, sizeof(context_t*));

    c->data = con;
    con->tcp = (uv_stream_t*) c;
    con->parser->data = con;

fail:
    if (err) {
        ELOG("Failed to accept on tcp socket %s", uv_strerror(err));
        uv_close((uv_handle_t*) c, NULL);
    }
}
void alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}
void read_cb (uv_stream_t* c, ssize_t nread, const uv_buf_t* buf) {
    connection_t* con = c->data;

    if (nread < 0) {
        if (nread == UV__EOF) {
            uv_close((uv_handle_t*) c, free_connection);
        }
    } else if (nread > 0) {
        if (nread != http_parser_execute(con->parser, &incoming, buf->base, buf->len)) {
            uv_close((uv_handle_t*) c, free_connection);
        }
    }
}
void write_cb(uv_write_t* req, int status) {
    context_t* ctx;

    ctx = req->data;
    if (status) {
        ELOG("uv_write error: %s\n", uv_strerror(errno));
        uv_close((uv_handle_t*) req->handle, free_connection);
        return;
    }

    if (evbuffer_get_length(ctx->send_body)) {
        uv_buf_t buffer = {
            ctx->str,
            evbuffer_remove(ctx->send_body, ctx->str, 65536)
        };

        uv_write(ctx->write, ctx->con->tcp, &buffer, 1, write_cb);
    } else {
        // TODO what if the reply has been sent and the server receives body?
        if (!ctx->flag.should_keepalive) {
            uv_close((uv_handle_t*) req->handle, free_connection);
        } else {
            uv_read_start((uv_stream_t*)ctx->con->tcp, alloc_cb, read_cb);
            vector_pop_front(ctx->con->contexts);
            free_context(ctx);
        }
    }
}
void free_context(context_t* ctx) {
    if (!ctx)
        return;

    hm_foreach(ctx->headers, hm_cb_free, (void*) 1);
    hm_foreach(ctx->send_headers, hm_cb_free, 0);
    hm_free(ctx->headers);
    hm_free(ctx->send_headers);
    sh_free_values(ctx->sh, ctx->vars);
    evbuffer_free(ctx->body);
    evbuffer_free(ctx->send_body);
    free(ctx->str);
    free(ctx->write);
    if (ctx->handle != -1) {
        hclose(ctx->handle);
    }

    free(ctx);
}
void free_connection(uv_handle_t* handle) {
    connection_t* con;

    if (!handle || !handle->data)
        return;

    con = handle->data;

    VECTOR_FOR_EACH(con->contexts, msg) {
        free_context(ITERATOR_GET_AS(context_t*, &msg));
    }

    vector_destroy(con->contexts);
    free(con);
}
int on_parse_error(context_t* ctx) {
    hm_foreach(ctx->headers, hm_cb_free, (void*) 1);
    hm_free(ctx->headers);
    sh_free_values(ctx->sh, ctx->vars);
    evbuffer_free(ctx->body);
    free(ctx->str);
    free(ctx->write);
    if (ctx->handle != -1) {
        hclose(ctx->handle);
    }

    ctx->headers = NULL;
    ctx->body = NULL;
    ctx->vars = NULL;
    ctx->str = NULL;
    ctx->flag.parse_error = 1;
    ctx->handle = -1;
    return 0;
}
int on_message_begin(__AP_EVENT_CB) {
    connection_t* con;
    context_t* ctx;

    con = p->data;
    ctx = calloc(1, sizeof(context_t));
    ctx->headers = hm_alloc(10, NULL, NULL);
    ctx->body = evbuffer_new();
    ctx->con = con;
    ctx->handle = -1;

    vector_push_back(con->contexts, &ctx);
    return 0;
}
int on_inc_url(__AP_DATA_CB) {
    __AP_PREAMPLE;

    evbuffer_add(ctx->body, at, len);
    return 0;
}
int on_inc_header_field(__AP_DATA_CB) {
    __AP_PREAMPLE;

    if (!ctx->flag.parsed_arguments) { // parse the uri arguments
        char buf[8192], * it,* s;
        int nread;

        if (evbuffer_get_length(ctx->body) >= 8192) {
            // this is a protocol error so close the connection
            return 1;
        }

        nread = evbuffer_remove(ctx->body, buf, 8192);
        lassert(nread >= 0);

        buf[nread] = 0;

        it = strtok_r(buf, "?", &s); // path
        ctx->sh = hm_get(a->routes, it);

        if (!ctx->sh) {
            ELOG("Missing shema for %s", it);
            return on_parse_error(ctx);
        }

        it = strtok_r(NULL, "", &s); // args

        ctx->vars = sh_parse(ctx->sh, it);
        if (!ctx->vars) {
            ELOG("Failed to parse args");
            return on_parse_error(ctx);
        }

        ctx->flag.parsed_arguments = 1;
    }

    if (ctx->flag.parsed_field) {
        if (complete_header(p))  {
            // this is a protocol error so close the connection
            return 1;
        }
        ctx->flag.parsed_field = 0; // start parsing new field
    }

    evbuffer_add(ctx->body, at, len);
    return 0;
}
int on_inc_header_value(__AP_DATA_CB) {
    __AP_PREAMPLE;

    if (!ctx->flag.parsed_field) { // parse this value's field
        int len;

        if (!evbuffer_get_length(ctx->body)) {
            // this is a protocol error so close the connection
            return 1;
        }

        ctx->flag.parsed_field = 1; // signal that the field has been parsed

        len = evbuffer_get_length(ctx->body) + 1;

        ctx->str = malloc(len);
        evbuffer_remove(ctx->body, ctx->str, len);
        ctx->str[len - 1] = 0;
    }

    evbuffer_add(ctx->body, at, len);
    return 0;
}
int on_inc_headers_complete(__AP_EVENT_CB) {
    context_t* ctx;

    ctx = parser_get_context(p);

    if (complete_header(p))  {
        // this is a protocol error so close the connection
        return 1;
    }

    if (http_body_is_final(p)) {
        evbuffer_free(ctx->body);
        ctx->body = NULL;
    }

    if (http_should_keep_alive(p) && !ctx->flag.parse_error) {
        ctx->flag.should_keepalive = 1;
    }

    // stop the connection
    uv_read_stop((uv_stream_t*)ctx->con->tcp);

    __active_ctx = ctx;

    // execute the concurr callback
    if (ctx->handle == -1) {
        ctx->handle = go(execute_context());
    }
    return 0;
}
int on_inc_body(__AP_DATA_CB) {
    context_t* ctx;

    ctx = parser_get_context(p);

    if (ctx->flag.parse_error) {
        // drop the download
        return 1;
    }

    if (!ctx->body) {
        // Should never happen tho
        return 1;
    }

    return 0;
}
int on_inc_message_complete(__AP_EVENT_CB) {

    return 0;
}
int complete_header(__AP_EVENT_CB) {
    char* value;
    int len;
    void* prev;

    __AP_PREAMPLE;

    if (!evbuffer_get_length(ctx->body)) {
        // this is a protocol error so close the connection
        return 1;
    }

    len = evbuffer_get_length(ctx->body) + 1;

    value = malloc(len);
    evbuffer_remove(ctx->body, value, len);
    value[len - 1] = 0;

    to_lower(ctx->str);
    prev = hm_put(ctx->headers, ctx->str, value);
    if (prev) {
        free(prev);
        free(ctx->str);
    }
    ctx->str = NULL;

    return 0;
}
context_t* parser_get_context(http_parser_t* p) {
    connection_t* con;

    con = p->data;
    return VECTOR_GET_AS(context_t*, con->contexts, 0);
}
