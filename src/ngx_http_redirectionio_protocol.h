#ifndef redirectionio_protocol_h
#define redirectionio_protocol_h

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

typedef struct {
    ngx_str_t   project_key;
    ngx_str_t   uri;
    ngx_str_t   host;
    ngx_str_t   rule_id;
    ngx_int_t   status;
    ngx_str_t   location;
    ngx_str_t   user_agent;
    ngx_str_t   referer;
} ngx_http_redirectionio_log_t;

void ngx_http_redirectionio_protocol_send_match(ngx_connection_t *c, ngx_http_request_t *r, ngx_str_t *project_key);
void ngx_http_redirectionio_protocol_send_log(ngx_connection_t *c, ngx_http_redirectionio_log_t *log);
ngx_http_redirectionio_log_t* ngx_http_redirectionio_protocol_create_log(ngx_http_request_t *r, ngx_str_t *project_key, ngx_str_t *rule_id);
void ngx_http_redirectionio_protocol_free_log(ngx_http_redirectionio_log_t *log);

#endif
