//
// Created by frank on 17-2-16.
//

#define _GNU_SOURCE
#include <sys/socket.h>

#include <assert.h>
#include <strings.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include "timer.h"
#include "conn_pool.h"
#include "request.h"

#define CONN_MAX            128
#define EVENT_MAX           128
#define REQUEST_TIMEOUT     5000
#define SERV_PORT           9877

static int init_http();
static int add_accept_event();

static void sig_handler(int signo);
static void accept_handler(event *ev);
static void read_handler(event *ev);
static void process_request_handler(event *ev);
static void write_headers_handler(event *ev);
static void write_body_handler(event *ev);
static void finalize_request_handler(event *ev);
static void empty_handler(event *ev);

static int  tcp_listen();
static void close_connection(connection *conn);

int main()
{
    timer_msec  timeout;
    int         n_ev;

    if (init_http() == FCY_ERROR) {
        err_quit("init_http error");
    }

    if (add_accept_event() == FCY_ERROR) {
        err_quit("add_accept_event error");
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sig_handler);

    printf("port %d\n", SERV_PORT);

    /* 事件循环 */
    while (1) {

        timeout = timer_recent();

        n_ev = event_process(timeout);

        if (n_ev == FCY_ERROR) {
            break;
        }

        timer_process();
    }

    err_msg("\nserver quit normally");
}

static int init_http()
{
    mem_pool    *pool;

    pool = mem_pool_create(MEM_POOL_DEFAULT_SIZE);
    if (pool == NULL){
        return FCY_ERROR;
    }

    if (conn_pool_init(pool, CONN_MAX) == -1) {
        mem_pool_destroy(pool);
        return FCY_ERROR;
    }

    if (event_init(pool, EVENT_MAX) == -1) {
        mem_pool_destroy(pool);
        return FCY_ERROR;
    }

    timer_init();

    return FCY_OK;
}

static int add_accept_event()
{
    int         listenfd;
    connection  *conn;

    listenfd = tcp_listen();

    conn = conn_pool_get();
    if (conn == NULL) {
        return FCY_ERROR;
    }

    conn->fd = listenfd;
    conn->read->handler = accept_handler;

    if (event_add(conn->read) == FCY_ERROR) {
        return FCY_ERROR;
    }

    return FCY_OK;
}

static void sig_handler(int signo)
{
}

static void accept_handler(event *ev)
{
    int             connfd;
    int             f_flag;
    struct sockaddr addr;
    socklen_t       len;
    connection      *conn;
    int             err;

    conn = conn_pool_get();
    if (conn == NULL) {
        err_msg("%s error at line %d: not enough free connections", __FUNCTION__, __LINE__);
        return;
    }

    len = sizeof(addr);

    inter:
    connfd = accept4(ev->conn->fd, &addr, &len, SOCK_NONBLOCK);
    if (connfd == -1) {
        switch (errno) {
            case EINTR:
                goto inter;
            case EAGAIN:
                return;
            default:
                err_sys("accept4 error");
        }
    }

    err_msg("new connection(%p) %d",conn, connfd);

    f_flag = fcntl(connfd, F_GETFL, 0);
    if (f_flag == -1) {
        err_sys("fcntl error");
    }

    err = fcntl(connfd, F_SETFL, f_flag | O_NONBLOCK);
    if (err == -1) {
        err_sys("fcntl_error");
    }

    conn->fd = connfd;
    conn->read->handler = read_handler;
    conn->write->handler = empty_handler;

    err = event_conn_add(conn);
    if (err == -1) {
        err_quit("event_conn_add error");
    }

    timer_add(conn->read, REQUEST_TIMEOUT);
}

static void read_handler(event *ev)
{
    ssize_t     n;
    connection  *conn;
    request     *rqst;
    buffer      *header_in;
    int         fd;
    int         err;

    conn = ev->conn;
    rqst = conn->app;
    fd = conn->fd;

    /* 处理超时 */
    if (ev->timeout) {

        if (rqst) {
            request_destroy(rqst);
        }

        close_connection(conn);
        return;
    }

    /* 若是第一次调用则需要创建request */
    if (rqst == NULL) {
        rqst = conn->app = request_create(conn);
        if (rqst == NULL) {
            err_quit("request_create error");
        }
    }

    header_in = rqst->header_in;

    /* read buffer满 */
    if (buffer_full(header_in)) {
        if (rqst->parse_state < line_done_) {
            rqst->status_code = HTTP_R_URI_TOO_LONG;
        }
        else {
            rqst->status_code = HTTP_R_REQUEST_HEADER_FIELD_TOO_LARGE;
        }
        goto error;
    }

    assert(buffer_empty(rqst->header_out));

    /* 读http请求 */
    eintr:
    n = read(fd, header_in->data_end, header_in->end - header_in->data_end);

    switch (n) {
        case -1:
            if (errno == EINTR) {
                goto eintr;
            }
            if (errno == EAGAIN) {
                if (!ev->timer_set) {
                    timer_add(ev, REQUEST_TIMEOUT);
                }
                return;
            }
            if (errno != ECONNRESET) {
                err_sys("read error");
            }

            /* fall through */
        case 0:
            /* 对端在没有发送完整请求的情况下关闭连接 */
            request_destroy(rqst);
            close_connection(conn);
            return;

        default:
            break;
    }

    buffer_seek_end(header_in, (int)n);

    assert(rqst->parse_state != error_);

    /* 解析请求行 */
    if (rqst->parse_state < line_done_) {
        err = parse_request_line(rqst);
        if (err == FCY_AGAIN) {
            read_handler(ev);
            return;
        }
        if (err == FCY_ERROR) {
            rqst->status_code = HTTP_R_BAD_REQUEST;
            goto error;
        }
    }

    /* 解析请求头 */
    err = parse_request_headers(rqst);
    if (err == FCY_AGAIN) {
        read_handler(ev);
        return;
    }
    if (err == FCY_ERROR) {
        rqst->status_code = HTTP_R_BAD_REQUEST;
        goto error;
    }

    /* 解析完毕 */
    if (ev->timer_set) {
        timer_del(ev);
    }

    ev->handler = process_request_handler;
    process_request_handler(ev);
    return;

    error:
    conn->keep_alive = 0;
    ev->handler = empty_handler;
    conn->write->handler = write_headers_handler;
    write_headers_handler(conn->write);
    return;
}

static void process_request_handler(event *ev)
{
    connection  *conn;
    request     *rqst;
    int         err;

    conn = ev->conn;
    rqst = conn->app;

    /* 目前只处理静态内容 */
    assert(rqst->line->uri_static);

    err = process_request_header(rqst);
    if (err == FCY_OK) {
        err = process_request_static(rqst);
    }

    if (err == FCY_ERROR) {
        conn->keep_alive = 0;
    }

    ev->handler = empty_handler;
    conn->write->handler = write_headers_handler;
    write_headers_handler(conn->write);
    return;
}

static void write_headers_handler(event *ev)
{
    connection  *conn;
    request     *rqst;
    buffer      *header_out;
    const char  *status_str;
    ssize_t     n;

    conn = ev->conn;
    rqst = conn->app;
    header_out = rqst->header_out;
    status_str = status_code_out_str[rqst->status_code];

    if (buffer_empty(header_out)) {
        /* 根据status code写header_out */
        buffer_write(header_out, "HTTP/1.1 ", 9);
        buffer_write(header_out, status_str, strlen(status_str));
        buffer_write(header_out, "\r\n"
                "Server: Fancy\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: ", 58);

        n = sprintf((char*)header_out->data_end, "%ld\r\n", rqst->sbuf.st_size);
        buffer_seek_end(header_out, (int)n);

        if (conn->keep_alive) {
            buffer_write(header_out, "Connection: keep-alive\r\n\r\n", 26);
        }
        else {
            buffer_write(header_out, "Connection: close\r\n\r\n", 21);
        }
    }

    while (!buffer_empty(header_out)) {
        inter:
        n = write(conn->fd, header_out->data_start, buffer_size(header_out));
        if (n == -1) {
            switch (errno) {
                case EINTR:
                    goto inter;
                case EAGAIN:
                    return;
                case EPIPE:
                case ECONNRESET:
                    request_destroy(rqst);
                    close_connection(conn);
                    return;
                default:
                    err_sys("write error");
            }
        }
        buffer_seek_start(header_out, (int) n);
    }

    if (rqst->send_fd > 0) {
        ev->handler = write_body_handler;
    }
    else {
        ev->handler = finalize_request_handler;
    }

    ev->handler(ev);
}

static void write_body_handler(event *ev)
{
    connection  *conn;
    request     *rqst;
    struct stat *sbuf;
    ssize_t     n;

    conn = ev->conn;
    rqst = conn->app;
    sbuf = &rqst->sbuf;

    while(sbuf->st_size > 0) {
        inter:
        n = sendfile(conn->fd, rqst->send_fd, NULL, (size_t) sbuf->st_size);
        if (n == -1) {
            switch (errno) {
                case EINTR:
                    goto inter;
                case EAGAIN:
                    return;
                case EPIPE:
                case ECONNRESET:
                    request_destroy(rqst);
                    close_connection(conn);
                    return;
                default:
                    err_sys("sendfile error");
            }
        }
        sbuf->st_size -= n;
    }

    ev->handler = finalize_request_handler;
    finalize_request_handler(ev);
}

static void finalize_request_handler(event *ev)
{
    connection  *conn;
    request     *rqst;

    conn = ev->conn;
    rqst = conn->app;

    /*
    if (rqst->parse_state != error_) {
        request_print(rqst);
    }
   */

    request_destroy(rqst);

    if (!conn->keep_alive) {
        close_connection(conn);
        return;
    }

    ev->handler = empty_handler;
    conn->read->handler = read_handler;
    read_handler(conn->read);
    return;
}

static void empty_handler(event *ev)
{
}

static int tcp_listen()
{
    int                 listenfd;
    struct sockaddr_in  servaddr;
    socklen_t           addrlen;
    int                 bind_ret, listen_ret;
    const int           sockopt;
    int                 ret;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) {
        err_sys("socket error");
    }

    ret = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(sockopt));
    if (ret == -1) {
        err_sys("setsockopt error");
    }

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family         = AF_INET;
    servaddr.sin_addr.s_addr    = htonl(INADDR_ANY);
    servaddr.sin_port           = htons(SERV_PORT);

    addrlen = sizeof(servaddr);
    bind_ret = bind(listenfd, (struct sockaddr*)&servaddr, addrlen);
    if (bind_ret == -1) {
        err_sys("bind error");
    }

    listen_ret = listen(listenfd, 1024);
    if (listen_ret == -1) {
        err_sys("listen error");
    }

    return listenfd;
}

static void close_connection(connection *conn)
{
    int fd = conn->fd;

    if (event_conn_del(conn) == -1) {
        err_quit("event_conn_del error");
    }

    if (conn->read->timer_set) {
        timer_del(conn->read);
    }

    if (close(fd) == -1) {
        err_sys("close error");
    }

    conn_pool_free(conn);

    err_msg("close connection(%p) %d\n",conn, fd);
}