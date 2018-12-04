#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <dlfcn.h>

#include <ngx_http_pool.h>
#include <ngx_http_redirectionio_module.h>

#define RIO_MIN_CONNECTIONS 0
#define RIO_KEEP_CONNECTIONS 10
#define RIO_MAX_CONNECTIONS 10
#define RIO_TIMEOUT 100

/**
 * List of values for boolean
 */
static ngx_conf_enum_t  ngx_http_redirectionio_enable_state[] = {
    { ngx_string("off"), NGX_HTTP_REDIRECTIONIO_OFF },
    { ngx_string("on"), NGX_HTTP_REDIRECTIONIO_ON },
    { ngx_null_string, 0 }
};

static void *ngx_http_redirectionio_create_conf(ngx_conf_t *cf);
static char *ngx_http_redirectionio_merge_conf(ngx_conf_t *cf, void *parent, void *child);
static char *ngx_http_redirectionio_set_url(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_int_t ngx_http_redirectionio_init_worker(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_redirectionio_postconfiguration(ngx_conf_t *cf);

static ngx_int_t ngx_http_redirectionio_create_ctx_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_redirectionio_redirect_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_redirectionio_log_handler(ngx_http_request_t *r);

ngx_int_t ngx_http_redirectionio_get_connection(ngx_peer_connection_t *pc, void *data);

static void ngx_http_redirectionio_read_handler(ngx_event_t *rev);

static void ngx_http_redirectionio_write_match_rule_handler(ngx_event_t *wev);
static void ngx_http_redirectionio_dummy_handler(ngx_event_t *wev);
static void ngx_http_redirectionio_read_event_dummy_handler(ngx_event_t *wev);

static void ngx_http_redirectionio_read_match_rule_handler(ngx_event_t *rev, cJSON *json);
static void ngx_http_redirectionio_read_dummy_handler(ngx_event_t *rev, cJSON *json);

static void ngx_http_redirectionio_json_cleanup(void *data);

static ngx_int_t ngx_http_redirectionio_pool_construct(void **resource, void *params);
static ngx_int_t ngx_http_redirectionio_pool_destruct(void *resource, void *params);
static ngx_int_t ngx_http_redirectionio_pool_available(ngx_reslist_t *reslist, void *resource, void *data, ngx_int_t deferred);
static ngx_int_t ngx_http_redirectionio_pool_available_log_handler(ngx_reslist_t *reslist, void *resource, void *data, ngx_int_t deferred);

static void ngx_http_redirectionio_release_resource(ngx_reslist_t *reslist, ngx_http_redirectionio_resource_t *resource, ngx_uint_t in_error);

static ngx_http_output_header_filter_pt ngx_http_next_header_filter;
static ngx_int_t ngx_http_redirectionio_match_on_response_status_header_filter(ngx_http_request_t *r);

/**
 * Commands definitions
 */
static ngx_command_t ngx_http_redirectionio_commands[] = {
    {
        ngx_string("redirectionio"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_SIF_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_enum_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_redirectionio_conf_t, enable),
        ngx_http_redirectionio_enable_state
    },
    {
        ngx_string("redirectionio_project_key"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_SIF_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_redirectionio_conf_t, project_key),
        NULL
    },
    {
        ngx_string("redirectionio_logs"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_SIF_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_enum_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_redirectionio_conf_t, enable_logs),
        ngx_http_redirectionio_enable_state
    },
    {
        ngx_string("redirectionio_pass"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_SIF_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_TAKE1,
        ngx_http_redirectionio_set_url,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_redirectionio_conf_t, pass),
        NULL
    },
    ngx_null_command /* command termination */
};

/* The module context. */
static ngx_http_module_t ngx_http_redirectionio_module_ctx = {
    NULL, /* preconfiguration */
    ngx_http_redirectionio_postconfiguration, /* postconfiguration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    ngx_http_redirectionio_create_conf, /* create location configuration */
    ngx_http_redirectionio_merge_conf /* merge location configuration */
};

/* Module definition. */
ngx_module_t ngx_http_redirectionio_module = {
    NGX_MODULE_V1,
    &ngx_http_redirectionio_module_ctx, /* module context */
    ngx_http_redirectionio_commands, /* module directives */
    NGX_HTTP_MODULE, /* module type */
    NULL, /* init master */
    NULL, /* init module */
    ngx_http_redirectionio_init_worker, /* init process */
    NULL, /* init thread */
    NULL, /* exit thread */
    NULL, /* exit process */
    NULL, /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t ngx_http_redirectionio_init_worker(ngx_cycle_t *cycle) {
    // @TODO Init connections here
    return NGX_OK;
}

static ngx_int_t ngx_http_redirectionio_postconfiguration(ngx_conf_t *cf) {
    ngx_http_core_main_conf_t           *cmcf;
    ngx_http_handler_pt                 *create_ctx_handler;
    ngx_http_handler_pt                 *redirect_handler;
    ngx_http_handler_pt                 *log_handler;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    // Log handler -> log phase
    log_handler = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);

    if (log_handler == NULL) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->cycle->log, 0, "redirectionio: init(): error pushing log handler");
        return NGX_ERROR;
    }

    *log_handler = ngx_http_redirectionio_log_handler;

#ifdef NGX_HTTP_PRECONTENT_PHASE
    redirect_handler = ngx_array_push(&cmcf->phases[NGX_HTTP_PRECONTENT_PHASE].handlers);
#else
    redirect_handler = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
#endif

    if (redirect_handler == NULL) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->cycle->log, 0, "redirectionio: init(): error pushing redirect handler");
        return NGX_ERROR;
    }

    *redirect_handler = ngx_http_redirectionio_redirect_handler;

    // Create context handler -> pre access phase
    create_ctx_handler = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);

    if (create_ctx_handler == NULL) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->cycle->log, 0, "redirectionio: init(): error pushing ctx handler");
        return NGX_ERROR;
    }

    *create_ctx_handler = ngx_http_redirectionio_create_ctx_handler;

    // Filters
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_redirectionio_match_on_response_status_header_filter;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->cycle->log, 0, "redirectionio: init(): return OK");

    return NGX_OK;
}

static ngx_int_t ngx_http_redirectionio_create_ctx_handler(ngx_http_request_t *r) {
    ngx_http_redirectionio_ctx_t    *ctx;
    ngx_http_redirectionio_conf_t   *conf;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_redirectionio_module);

    if (conf->enable == NGX_HTTP_REDIRECTIONIO_OFF) {
        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_redirectionio_module);

    if (ctx == NULL) {
        ctx = (ngx_http_redirectionio_ctx_t *) ngx_pcalloc(r->pool, sizeof(ngx_http_redirectionio_ctx_t));

        if (ctx == NULL) {
            return NGX_DECLINED;
        }

        ctx->status = 0;
        ctx->connection_error = 0;
        ctx->wait_for_connection = 0;
        ctx->wait_for_match = 0;
        ctx->match_on_response_status = 0;
        ctx->is_redirected = 0;

        ngx_http_set_ctx(r, ctx, ngx_http_redirectionio_module);
    }

    return NGX_DECLINED;
}
/**
 * RedirectionIO Middleware
 *
 * Call at every request
 */
static ngx_int_t ngx_http_redirectionio_redirect_handler(ngx_http_request_t *r) {
    ngx_http_redirectionio_conf_t   *conf;
    ngx_http_redirectionio_ctx_t    *ctx;
    ngx_int_t                       status;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_redirectionio_module);

    if (conf->enable == NGX_HTTP_REDIRECTIONIO_OFF) {
        // Call next handler
        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_redirectionio_module);

    if (ctx == NULL) {
        return NGX_DECLINED;
    }

    if (ctx->connection_error) {
        return NGX_DECLINED;
    }

    if (ctx->resource == NULL) {
        if (ctx->wait_for_connection) {
            return NGX_AGAIN;
        }

        status = ngx_reslist_acquire(conf->connection_pool, ngx_http_redirectionio_pool_available, r);

        if (status == NGX_AGAIN) {
            ctx->wait_for_connection = 1;

            return status;
        }

        if (status != NGX_OK) {
            return NGX_DECLINED;
        }
    }

    if (ctx->connection_error) {
        ngx_http_redirectionio_release_resource(conf->connection_pool, ctx->resource, 1);

        return NGX_DECLINED;
    }

    if (ctx->matched_rule_id.data == NULL) {
        if (ctx->wait_for_match) {
            return NGX_AGAIN;
        }

        ctx->wait_for_match = 1;
        ngx_http_redirectionio_write_match_rule_handler(ctx->resource->peer.connection->write);

        return NGX_AGAIN;
    }

    ngx_http_redirectionio_release_resource(conf->connection_pool, ctx->resource, 0);

    if (ctx->status == 0 || ctx->match_on_response_status != 0) {
        return NGX_DECLINED;
    }

    if (ctx->status != 410) {
        // Set target
        r->headers_out.location = ngx_list_push(&r->headers_out.headers);

        if (r->headers_out.location == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        r->headers_out.location->hash = 1;
        ngx_str_set(&r->headers_out.location->key, "Location");
        r->headers_out.location->value.len = ctx->target.len;
        r->headers_out.location->value.data = ctx->target.data;
    }

    r->headers_out.status = ctx->status;
    ctx->is_redirected = 1;

    return ctx->status;
}

static ngx_int_t ngx_http_redirectionio_log_handler(ngx_http_request_t *r) {
    ngx_http_redirectionio_conf_t   *conf;
    ngx_http_redirectionio_ctx_t    *ctx;
    ngx_http_redirectionio_log_t    *log;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_redirectionio_module);

    if (conf->enable == NGX_HTTP_REDIRECTIONIO_OFF) {
        return NGX_DECLINED;
    }

    if (conf->enable_logs == NGX_HTTP_REDIRECTIONIO_OFF) {
        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_redirectionio_module);

    if (ctx == NULL) {
        return NGX_DECLINED;
    }

    log = ngx_http_redirectionio_protocol_create_log(r, &conf->project_key, &ctx->matched_rule_id);

    if (log == NULL) {
        return NGX_DECLINED;
    }

    ngx_reslist_acquire(conf->connection_pool, ngx_http_redirectionio_pool_available_log_handler, log);

    return NGX_DECLINED;
}

/* Create configuration object */
static void *ngx_http_redirectionio_create_conf(ngx_conf_t *cf) {
    ngx_http_redirectionio_conf_t   *conf;

    conf = (ngx_http_redirectionio_conf_t *) ngx_pcalloc(cf->pool, sizeof(ngx_http_redirectionio_conf_t));

    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    conf->enable = NGX_CONF_UNSET_UINT;
    conf->enable_logs = NGX_CONF_UNSET_UINT;

    return conf;
}

static char *ngx_http_redirectionio_merge_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_http_redirectionio_conf_t       *prev = parent;
    ngx_http_redirectionio_conf_t       *conf = child;

    ngx_conf_merge_uint_value(conf->enable_logs, prev->enable_logs, NGX_HTTP_REDIRECTIONIO_ON);
    ngx_conf_merge_str_value(conf->project_key, prev->project_key, "");

    if (conf->pass.url.data == NULL) {
        if (prev->pass.url.data) {
            conf->pass = prev->pass;
            // Limit number of connection pool
            conf->connection_pool = prev->connection_pool;
        } else {
            // Should create new connection pool
            conf->pass.url = (ngx_str_t)ngx_string("127.0.0.1:10301");

            if (ngx_parse_url(cf->pool, &conf->pass) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            if(ngx_reslist_create(
                &conf->connection_pool,
                cf->pool,
                RIO_MIN_CONNECTIONS,
                RIO_KEEP_CONNECTIONS,
                RIO_MAX_CONNECTIONS,
                RIO_TIMEOUT,
                conf,
                ngx_http_redirectionio_pool_construct,
                ngx_http_redirectionio_pool_destruct
            ) != NGX_OK) {
                ngx_log_error(NGX_LOG_ERR, cf->log, 0, "[redirectionio] cannot create connection pool for redirectionio, disabling module");

                conf->enable = NGX_HTTP_REDIRECTIONIO_OFF;
            }
        }
    } else {
        if(ngx_reslist_create(
            &conf->connection_pool,
            cf->pool,
            RIO_MIN_CONNECTIONS,
            RIO_KEEP_CONNECTIONS,
            RIO_MAX_CONNECTIONS,
            RIO_TIMEOUT,
            conf,
            ngx_http_redirectionio_pool_construct,
            ngx_http_redirectionio_pool_destruct
        ) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, cf->log, 0, "[redirectionio] cannot create connection pool for redirectionio, disabling module");

            conf->enable = NGX_HTTP_REDIRECTIONIO_OFF;
        }
    }

    if (conf->project_key.len > 0) {
        ngx_conf_merge_uint_value(conf->enable, prev->enable, NGX_HTTP_REDIRECTIONIO_ON);
    } else {
        ngx_conf_merge_uint_value(conf->enable, prev->enable, NGX_HTTP_REDIRECTIONIO_OFF);
    }


    return NGX_CONF_OK;
}

static char *ngx_http_redirectionio_set_url(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    char  *p = conf;

    ngx_url_t *field;
    ngx_str_t *value;
    ngx_conf_post_t  *post;

    field = (ngx_url_t *) (p + cmd->offset);

    if (field->url.data) {
        return "is duplicate";
    }

    value = cf->args->elts;

    field->url = value[1];

    if (ngx_parse_url(cf->pool, field) != NGX_OK) {
        return field->err;
    }

    if (cmd->post) {
        post = cmd->post;
        return post->post_handler(cf, post, field);
    }

    return NGX_CONF_OK;
}


ngx_int_t ngx_http_redirectionio_get_connection(ngx_peer_connection_t *pc, void *data) {
    return NGX_OK;
}

static void ngx_http_redirectionio_write_match_rule_handler(ngx_event_t *wev) {
    ngx_http_redirectionio_ctx_t    *ctx;
    ngx_connection_t                *c;
    ngx_http_request_t              *r;
    ngx_http_redirectionio_conf_t   *conf;

    c = wev->data;
    r = c->data;
    ctx = ngx_http_get_module_ctx(r, ngx_http_redirectionio_module);
    conf = ngx_http_get_module_loc_conf(r, ngx_http_redirectionio_module);

    ngx_add_timer(c->read, RIO_TIMEOUT);
    ctx->read_handler = ngx_http_redirectionio_read_match_rule_handler;

    ngx_http_redirectionio_protocol_send_match(c, r, &conf->project_key);
}

static void ngx_http_redirectionio_dummy_handler(ngx_event_t *wev) {
    return;
}

static void ngx_http_redirectionio_read_event_dummy_handler(ngx_event_t *rev) {
    return;
}

static void ngx_http_redirectionio_read_match_rule_handler(ngx_event_t *rev, cJSON *json) {
    ngx_http_redirectionio_ctx_t    *ctx;
    ngx_http_request_t              *r;
    ngx_connection_t                *c;

    c = rev->data;
    r = c->data;
    ctx = ngx_http_get_module_ctx(r, ngx_http_redirectionio_module);
    ctx->read_handler = ngx_http_redirectionio_read_dummy_handler;

    if (json == NULL) {
        ctx->matched_rule_id.data = (u_char *)"";
        ctx->matched_rule_id.len = 0;
        ctx->status = 0;

        ngx_http_core_run_phases(r);

        return;
    }

    cJSON *status = cJSON_GetObjectItem(json, "status_code");
    cJSON *match_on_response_status = cJSON_GetObjectItem(json, "match_on_response_status");
    cJSON *location = cJSON_GetObjectItem(json, "location");
    cJSON *matched_rule = cJSON_GetObjectItem(json, "matched_rule");
    cJSON *rule_id = NULL;

    if (matched_rule != NULL && matched_rule->type != cJSON_NULL) {
        rule_id = cJSON_GetObjectItem(matched_rule, "id");
    }

    if (matched_rule == NULL || matched_rule->type == cJSON_NULL) {
        ctx->matched_rule_id.data = (u_char *)"";
        ctx->matched_rule_id.len = 0;
        ctx->status = 0;
        ctx->match_on_response_status = 0;

        ngx_http_core_run_phases(r);

        return;
    }

    ctx->matched_rule_id.data = (u_char *)rule_id->valuestring;
    ctx->matched_rule_id.len = strlen(rule_id->valuestring);
    ctx->target.data = (u_char *)location->valuestring;
    ctx->target.len = strlen(location->valuestring);
    ctx->status = status->valueint;
    ctx->match_on_response_status = 0;

    if (match_on_response_status != NULL && match_on_response_status->type != cJSON_NULL) {
        ctx->match_on_response_status = match_on_response_status->valueint;
    }

    ngx_http_core_run_phases(r);
}

static void ngx_http_redirectionio_read_handler(ngx_event_t *rev) {
    ngx_connection_t                *c;
    ngx_http_request_t              *r;
    ngx_http_redirectionio_ctx_t    *ctx;
    u_char                          *buffer;
    u_char                          read;
    size_t                          len = 0;
    ngx_uint_t                      max_size = 8192;
    ssize_t                         readed;
    ngx_pool_cleanup_t              *cln;

    c = rev->data;
    r = c->data;
    ctx = ngx_http_get_module_ctx(r, ngx_http_redirectionio_module);
    ctx->resource->peer.connection->read->handler = ngx_http_redirectionio_read_event_dummy_handler;
    buffer = (u_char *) ngx_pcalloc(c->pool, max_size);

    if (rev->timedout) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "[redirectionio] connection timeout while reading, skipping module for this request");

        ctx->connection_error = 1;
        ctx->read_handler(rev, NULL);

        return;
    }

    if (rev->timer_set) {
        ngx_del_timer(rev);
    }

    for (;;) {
        readed = ngx_recv(c, &read, 1);

        if (readed == -1) { /* Error */
            ctx->connection_error = 1;
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "[redirectionio] connection error while reading, skipping module for this request");
            ctx->read_handler(rev, NULL);

            return;
        }

        if (readed == 0) { /* EOF */
            ctx->connection_error = 1;
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "[redirectionio] connection terminated while reading, skipping module for this request");
            ctx->read_handler(rev, NULL);

            return;
        }

        if (len > max_size) { /* Too big */
            ctx->connection_error = 1;
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "[redirectionio] message too big while reading, skipping module for this request");
            ctx->read_handler(rev, NULL);

            return;
        }

        if (read == '\0') { /* Message readed, push it to the current handler */
            if (len == 0) {
                continue;
            }

            *buffer = '\0';
            cJSON *json = cJSON_Parse((char *)(buffer - len));

            cln = ngx_pool_cleanup_add(r->pool, 0);
            cln->handler = ngx_http_redirectionio_json_cleanup;
            cln->data = json;

            ctx->read_handler(rev, json);

            return;
        }

        len++;
        *buffer = read;
        buffer++;
    }
}

static void ngx_http_redirectionio_json_cleanup(void *data) {
    cJSON_Delete((cJSON *)data);
}

static void ngx_http_redirectionio_read_dummy_handler(ngx_event_t *rev, cJSON *json) {
    return;
}

static ngx_int_t ngx_http_redirectionio_pool_construct(void **rp, void *params) {
    ngx_pool_t                          *pool;
    ngx_http_redirectionio_resource_t   *resource;
    ngx_int_t                           rc;
    ngx_http_redirectionio_conf_t       *conf = (ngx_http_redirectionio_conf_t *)params;

    pool = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, ngx_cycle->log);

    if (pool == NULL) {
        return NGX_ERROR;
    }

    resource = ngx_pcalloc(pool, sizeof(ngx_http_redirectionio_resource_t));

    if (resource == NULL) {
        return NGX_ERROR;
    }

    resource->pool = pool;
    resource->peer.sockaddr = (struct sockaddr *)&conf->pass.sockaddr;
    resource->peer.socklen = conf->pass.socklen;
    resource->peer.name = &conf->pass.url;
    resource->peer.get = ngx_http_redirectionio_get_connection;
    resource->peer.log = pool->log;
    resource->peer.log_error = NGX_ERROR_ERR;

    rc = ngx_event_connect_peer(&resource->peer);

    if (rc == NGX_ERROR || rc == NGX_BUSY || rc == NGX_DECLINED) {
        if (resource->peer.connection) {
            ngx_close_connection(resource->peer.connection);
        }

        return NGX_ERROR;
    }

    int tcp_nodelay = 1;

    if (setsockopt(resource->peer.connection->fd, IPPROTO_TCP, TCP_NODELAY, (const void *) &tcp_nodelay, sizeof(int)) == -1) {
        ngx_log_error(NGX_LOG_ALERT, pool->log, ngx_socket_errno,  "setsockopt(TCP_NODELAY) %V failed, ignored", &resource->peer.connection->addr_text);
    }

    resource->peer.connection->pool = pool;
    resource->peer.connection->read->handler = ngx_http_redirectionio_dummy_handler;
    resource->peer.connection->write->handler = ngx_http_redirectionio_dummy_handler;

    *rp = resource;

    return NGX_OK;
}

static ngx_int_t ngx_http_redirectionio_pool_destruct(void *rp, void *params) {
    ngx_http_redirectionio_resource_t   *resource = (ngx_http_redirectionio_resource_t *)rp;

    ngx_close_connection(resource->peer.connection);
    ngx_destroy_pool(resource->pool);

    return NGX_OK;
}

static ngx_int_t ngx_http_redirectionio_pool_available(ngx_reslist_t *reslist, void *resource, void *data, ngx_int_t deferred) {
    ngx_http_redirectionio_ctx_t    *ctx;
    ngx_http_request_t              *r = (ngx_http_request_t *)data;

    ctx = ngx_http_get_module_ctx(r, ngx_http_redirectionio_module);

    if (ctx == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "[redirectionio] no context, skipping module for this request");

        if (deferred) {
            ngx_http_core_run_phases(r);
        }

        return NGX_ERROR;
    }

    if (resource == NULL) {
        ctx->connection_error = 1;
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "[redirectionio] cannot acquire connection, retrieving resource from pool timed out, skipping module for this request");

        if (deferred) {
            ngx_http_core_run_phases(r);
        }

        return NGX_ERROR;
    }

    ctx->resource = (ngx_http_redirectionio_resource_t *)resource;
    ctx->resource->peer.connection->data = r;
    ctx->resource->peer.connection->read->handler = ngx_http_redirectionio_read_handler;

    if (deferred) {
        ngx_http_core_run_phases(r);
    }

    return NGX_OK;
}

static ngx_int_t ngx_http_redirectionio_pool_available_log_handler(ngx_reslist_t *reslist, void *resource, void *data, ngx_int_t deferred) {
    ngx_http_redirectionio_log_t    *log = (ngx_http_redirectionio_log_t *)data;
    ngx_peer_connection_t           *peer = (ngx_peer_connection_t *)resource;

    if (peer == NULL) {
        ngx_http_redirectionio_protocol_free_log(log);

        return NGX_ERROR;
    }

    ngx_http_redirectionio_protocol_send_log(peer->connection, log);
    ngx_http_redirectionio_protocol_free_log(log);
    ngx_http_redirectionio_release_resource(reslist, resource, 0);

    return NGX_OK;
}

static void ngx_http_redirectionio_release_resource(ngx_reslist_t *reslist, ngx_http_redirectionio_resource_t *resource, ngx_uint_t in_error) {
    if (in_error) {
        ngx_reslist_invalidate(reslist, resource);

        return;
    }

    resource->usage++;

    if (resource->usage > NGX_HTTP_REDIRECTIONIO_RESOURCE_MAX_USAGE) {
        ngx_reslist_invalidate(reslist, resource);

        return;
    }

    ngx_reslist_release(reslist, resource);
}

static ngx_int_t ngx_http_redirectionio_match_on_response_status_header_filter(ngx_http_request_t *r) {
    ngx_http_redirectionio_ctx_t    *ctx;
    ngx_http_redirectionio_conf_t   *conf;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_redirectionio_module);

    if (conf->enable == NGX_HTTP_REDIRECTIONIO_OFF) {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_redirectionio_module);

    // Skip if no need to redirect
    if (ctx == NULL || ctx->status == 0 || ctx->match_on_response_status == 0 || ctx->is_redirected) {
        return ngx_http_next_header_filter(r);
    }

    if (r->headers_out.status != ctx->match_on_response_status) {
        return ngx_http_next_header_filter(r);
    }

    if (ctx->status != 410) {
        // Set target
        r->headers_out.location = ngx_list_push(&r->headers_out.headers);

        if (r->headers_out.location == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        r->headers_out.location->hash = 1;
        ngx_str_set(&r->headers_out.location->key, "Location");
        r->headers_out.location->value.len = ctx->target.len;
        r->headers_out.location->value.data = ctx->target.data;
    }

    r->headers_out.status = ctx->status;
    // Avoid loop if we redirect on the same status as we match
    ctx->is_redirected = 1;

    return ngx_http_special_response_handler(r, ctx->status);
}
