//
// Created by frank on 17-5-24.
//

#include <ctype.h>
#include "http_parser.h"
#include "request.h"

#define CB_0(func, user) \
do {    \
    if (func != NULL) { \
        func(user); \
    }   \
} while(0)  \

#define CB_1(func, user, data) \
do {    \
    if (func != NULL) { \
        func(user, data); \
    }   \
} while(0)  \

#define CB_2(func, user, data1, data2) \
do {    \
    if (func != NULL) { \
        func(user, data1, data2); \
    }   \
} while(0)  \

const char *method_strs[] = {
        "GET",
        "HEAD",
        "POST",
        "OPTIONS",
        "DELETE",
        "TRACE",
        "CONNECT",
};

const char *status_code_out_str[] = {
        "200 OK",
        "400 Bad Request",
        "403 Forbidden",
        "404 Not Found",
        "408 Request Timeout",
        "411 Length Required",
        "413 Payload Too Large",
        "414 URI Too Long",
        "431 Request Header Fields Too Large",
        "500 Internal Server Error",
        "501 Not Implemented",
};

enum {
    start_ = 0,

    method_,
    space_before_uri_,
    uri_,
    space_before_version_,
    version_H_,
    version_HT_,
    version_HTT_,
    version_HTTP_,
    version_HTTP_slash_,
    version_HTTP_slash_1_,
    version_HTTP_slash_1_dot_,
    space_after_version_,
    line_almost_done_,
    line_done_,

    header_start_,
    name_,
    space_before_value_,
    value_,
    space_after_value_,
    header_almost_done_,
    all_headers_almost_done,
    all_done_,

    error_,
};

static int parse_request(http_parser *ps, buffer *in, mem_pool *pool);
static int parse_request_line(http_parser *ps, buffer *in);
static int parse_uri(http_parser *ps, mem_pool *pool);

static int parse_response(http_parser *ps, buffer *in);
static int parse_response_line(http_parser *ps, buffer *in);

static int parse_headers(http_parser *ps, buffer *in);

int parser_execute(http_parser *ps, buffer *in, mem_pool *pool)
{
    if (ps->type == HTTP_PARSE_REQUEST) {
        return parse_request(ps, in, pool);
    }
    else {
        return parse_response(ps, in);
    }
}

static int parse_request(http_parser *ps, buffer *in, mem_pool *pool)
{
    assert(ps->state != error_);

    if (ps->state < line_done_) {
        switch (parse_request_line(ps, in)) {
            case FCY_AGAIN:
                assert(ps->state < line_done_);
                return FCY_AGAIN;
            case FCY_OK:
                assert(ps->state == line_done_);
                ps->state = header_start_;
                break;
            default:
                assert(ps->state == error_);
                return FCY_ERROR;
        }
    }

    if (ps->state < all_done_) {
        switch (parse_headers(ps, in)) {
            case FCY_AGAIN:
                assert(ps->state < all_done_);
                return FCY_AGAIN;
            case FCY_OK:
                assert(ps->state == all_done_);
                break;
            default:
                assert(ps->state == error_);
                return FCY_ERROR;
        }
    }

    assert(ps->state == all_done_);
    return parse_uri(ps, pool);
}

static int parse_request_line(http_parser *ps, buffer *in)
{
    char        c, *p;
    unsigned    state = ps->state;

    for (p = buffer_read(in);
         !buffer_empty(in);
         p = buffer_seek_start(in, 1)) {

        c = *p;
        switch (state) {
            case start_:
                /* method_start_ */
                switch(c) {
                    case 'G': ps->method = HTTP_METHOD_GET; break;
                    case 'H': ps->method = HTTP_METHOD_HEAD; break;
                    case 'P': ps->method = HTTP_METHOD_POST; break;
                    case 'O': ps->method = HTTP_METHOD_OPTIONS; break;
                    case 'D': ps->method = HTTP_METHOD_DELETE; break;
                    case 'T': ps->method = HTTP_METHOD_TRACE; break;
                    case 'C': ps->method = HTTP_METHOD_CONNECT; break;
                    default:
                        goto error;
                    }
                    ps->index = 1;
                    state = method_;
                    break;

            case method_:
            {
                const char *matcher = method_strs[ps->method];

                if (matcher[ps->index] != '\0') {
                    if (matcher[ps->index] == c) {
                        ++ps->index;
                        break;
                    }
                    else {
                        goto error;
                    }
                }

                state = space_before_uri_;
                break;
            }

            case space_before_uri_:
                if (c == ' ') {
                    break;
                }
                if (c == '/') {
                    ps->uri_start = p;
                    state = uri_;
                    break;
                }
                goto error;

            case uri_:
                if (c == ' ') {
                    ps->uri_end = p;
                    state = space_before_version_;
                    break;
                }
                if (!iscntrl(c)) {
                    break;
                }
                goto error;

            case space_before_version_:
                if (c == ' ') {
                    break;
                }
                if ((c | 0x20) == 'h') {
                    state = version_H_;
                    break;
                }
                goto error;

            case version_H_:
                if ((c | 0x20) == 't') {
                    state = version_HT_;
                    break;
                }
                goto error;

            case version_HT_:
                if ((c | 0x20) == 't') {
                    state = version_HTT_;
                    break;
                }
                goto error;

            case version_HTT_:
                if ((c | 0x20) == 'p') {
                    state = version_HTTP_;
                    break;
                }
                goto error;

            case version_HTTP_:
                if (c == '/') {
                    state = version_HTTP_slash_;
                    break;
                }
                goto error;

            case version_HTTP_slash_:
                if (c == '1') {
                    state = version_HTTP_slash_1_;
                    break;
                }

            case version_HTTP_slash_1_:
                if (c == '.') {
                    state = version_HTTP_slash_1_dot_;
                    break;
                }
                goto error;

            case version_HTTP_slash_1_dot_:
                if (c == '0') {
                    ps->version = HTTP_V10;
                    state = space_after_version_;
                    break;
                }
                if (c == '1') {
                    ps->version = HTTP_V11;
                    state = space_after_version_;
                    break;
                }
                goto error;

            case space_after_version_:
                if (c == ' ') {
                    break;
                }
                if (c == '\r') {
                    state = line_almost_done_;
                    break;
                }
                goto error;

            case line_almost_done_:
                if (c == '\n') {
                    buffer_seek_start(in, 1);
                    state = line_done_;
                    goto done;
                }
                goto error;

            default:
                assert(0);
        }
    }

    done:
    ps->state = state;
    return (state == line_done_ ? FCY_OK : FCY_AGAIN);

    error:
    ps->state = error_;
    return FCY_ERROR;
}

static int parse_headers(http_parser *ps, buffer *in)
{
    char        c, *p;
    unsigned    state = ps->state;

    for (p = buffer_read(in);
         !buffer_empty(in);
         p = buffer_seek_start(in, 1)) {

        c = *p;
        switch (state) {
            case header_start_:
                if (c == '\r') {
                    state = all_headers_almost_done;
                    break;
                }
                if (isalpha(c) || c == '-') {
                    state = name_;
                    ps->last_header_name_start = p;
                    break;
                }
                goto error;

            case name_:
                if (isalpha(c) || c == '-') {
                    break;
                }
                if (c == ':') {
                    state = space_before_value_;
                    break;
                }
                goto error;

            case space_before_value_:
                if (c == ' ') {
                    break;
                }
                if (!iscntrl(c)) {
                    ps->last_header_value_start = p;
                    state = value_;
                    break;
                }
                goto error;

            case value_:
                if (c == '\r' || c == ' ') {

                    CB_2(ps->header_cb, ps->user,
                         ps->last_header_name_start,
                         ps->last_header_value_start);

                    state = (c == '\r' ? header_almost_done_ : space_before_value_);
                    break;
                }
                if (!iscntrl(c)) {
                    break;
                }
                goto error;

            case space_after_value_:
                if (c == ' ') {
                    break;
                }
                if (c == '\r') {
                    state = header_almost_done_;
                    break;
                }
                goto error;

            case header_almost_done_:
                if (c == '\n') {
                    state = header_start_;
                    break;
                }
                goto error;

            case all_headers_almost_done:
                if (c == '\n') {
                    buffer_seek_start(in, 1);
                    state = all_done_;
                    goto done;
                }

            default:
                assert(0);
        }
    }

    done:
    ps->state = state;
    if (state == all_done_) {
        return FCY_OK;
    }
    else {
        return FCY_AGAIN;
    }

    error:
    ps->state = error_;
    return FCY_ERROR;
}

static int parse_uri(http_parser *ps, mem_pool *pool)
{
    int     hex1, hex2;
    char    c, *p, *u, *host_uri = NULL, *last_dot = NULL;
    enum { /* local */
        start_ = 0,
        after_slash_,
        quote_,
        args_,
    } state = start_;
    u = ps->uri_start;

    /* 注意，此时uri已经读完了，不需要考虑FCY_AGAIN的情况 */
    for (p = ps->uri_start; p != ps->uri_end; ++p) {

        c = *p;
        switch (state) {
            case start_:
                if (c == '/') {
                    u = host_uri = pcalloc(pool, ps->uri_end - ps->uri_start + 32);
                    if (host_uri == NULL) {
                        goto error;
                    }

                    strcpy(u, root);
                    u += strlen(root);
                    *u++ = '/';

                    state = after_slash_;
                    break;
                }
                goto error;

            case after_slash_:
                switch (c) {
                    case '/':
                        *u++ = '/';
                        last_dot = NULL;
                        break;
                    case '#':
                        goto done;
                    case '?':
                        // 暂时不用
                        // r->has_args = 1;
                        *u++ = '?';
                        state = args_;
                        break;
                    case '%':
                        state = quote_;
                        break;
                    case '.':
                        last_dot = u;
                        *u++ = '.';
                        break;
                    default:
                        *u++ = c;
                        break;
                }
                break;

            case quote_:
                if (isdigit(c)) {
                    hex1 = c - '0';
                }
                else {
                    hex1 = (c | 0x20);
                    if (hex1 >= 'a' && hex1 <= 'f') {
                        hex1 = hex1 - 'a' + 10;
                    }
                    else {
                        goto error;
                    }
                }

                c = *++p;

                if (isdigit(c)) {
                    hex2 = c - '0';
                }
                else {
                    hex2 = (c | 0x20);
                    if (hex2 >= 'a' && hex2 <= 'f') {
                        hex2 = hex2 - 'a' + 10;
                    }
                    else {
                        goto error;
                    }
                }

                *u++ = (char)((hex1 << 4) + hex2);
                state = after_slash_;
                break;

            case args_:
                switch (c) {
                    case '#':
                        goto done;
                    case '/':
                        goto error;
                    default:
                        *u++ = c;
                        break;
                }
                break;

            default:
                assert(0);
        }
    }

    done:
    if (last_dot != NULL) {
        CB_2(ps->uri_cb, ps->user, host_uri, last_dot + 1);
    }
    else {
        CB_2(ps->uri_cb, ps->user, host_uri, u);
    }
    return FCY_OK;

    error:
    ps->state = error_;
    return FCY_ERROR;
}

static int parse_response(http_parser *ps, buffer *in)
{
    assert(ps->state != error_);

    if (ps->state < line_done_) {
        switch (parse_response_line(ps, in)) {
            case FCY_AGAIN:
                assert(ps->state < line_done_);
                return FCY_AGAIN;
            case FCY_OK:
                assert(ps->state == line_done_);
                ps->state = header_start_;
                break;
            default:
                assert(ps->state == error_);
                return FCY_ERROR;
        }
    }

    if (ps->state < all_done_) {
        switch (parse_headers(ps, in)) {
            case FCY_AGAIN:
                assert(ps->state < all_done_);
                return FCY_AGAIN;
            case FCY_OK:
                assert(ps->state == all_done_);
                return FCY_OK;
            default:
                assert(ps->state == error_);
                return FCY_ERROR;
        }
    }

    /* never reach here */
    return FCY_ERROR;
}

static int parse_response_line(http_parser *ps, buffer *in)
{
    char        c, *p;
    unsigned    state = ps->state;

    for (p = buffer_read(in);
         !buffer_empty(in);
         p = buffer_seek_start(in, 1)) {

        c = *p;
        switch(state) {
            case start_:
                if (c == '\r') {
                    state = line_almost_done_;
                }
                break;

            case line_almost_done_:
                if (c == '\n') {
                    buffer_seek_start(in, 1);
                    state = line_done_;
                    goto done;
                }
                /* fall through */
            default:
                goto error;
        }
    }

    done:
    ps->state = state;
    return (state == line_done_ ? FCY_OK : FCY_AGAIN);

    error:
    ps->state = error_;
    return FCY_ERROR;
}