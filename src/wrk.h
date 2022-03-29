#ifndef WRK_H
#define WRK_H

#include "config.h"
#include <pthread.h>
#include <inttypes.h>
#include <sys/types.h>
#include <netdb.h>
#include <sys/socket.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <lua.h>

#include "stats.h"
#include "ae.h"
#include "http_parser.h"
#include "hdr_histogram.h"

#define VERSION  "4.0.0"
#define RECVBUF  8192
#define SAMPLES  100000000

#define SOCKET_TIMEOUT_MS   2000
#define CALIBRATE_DELAY_MS  10000
#define TIMEOUT_INTERVAL_MS 2000
#define STOP_CHECK_INTERNAL_MS 2000
#define THREAD_SYNC_INTERVAL_MS 1000

typedef struct {
    double throughput;
    uint64_t sent;
    bool caught_up;
    uint64_t catch_up_start_time;
    uint64_t complete_at_catch_up_start;
    double catch_up_throughput;
    uint64_t thread_start;
} rate_handler_t;

typedef struct {
    pthread_t thread;
    aeEventLoop *loop;
    struct addrinfo *addr;
    uint64_t connections;
    uint64_t phase_normal_start;
    int phase;
    int interval;
    uint64_t stop_at;
    uint64_t complete;
    uint64_t requests;
    uint64_t bytes;
    uint64_t start;
    uint64_t mean;
    struct hdr_histogram *latency_histogram;
    struct hdr_histogram *u_latency_histogram;
    tinymt64_t rand;
    lua_State *L;
    errors errors;
    struct connection *cs;
    char *local_ip;
    rate_handler_t rate_handler;
} thread;

typedef struct {
    char  *buffer;
    size_t length;
    char  *cursor;
} buffer;

typedef struct connection {
    thread *thread;
    http_parser parser;
    enum {
        FIELD, VALUE
    } state;
    int fd;
    int connect_mask;
    SSL *ssl;
    rate_handler_t rate_handler;
    uint64_t complete_at_last_batch_start;
    uint64_t start;
    char *request;
    size_t length;
    size_t written;
    uint64_t pending;
    buffer headers;
    buffer body;
    char buf[RECVBUF];
    uint64_t actual_latency_start;
    bool is_connected;
    bool has_pending;
    // Internal tracking numbers (used purely for debugging):
    uint64_t latest_should_send_time;
    uint64_t latest_expected_start;
    uint64_t latest_connect;
    uint64_t latest_write;
} connection;

extern char *g_local_ip;

void bind_socket(int fd, sa_family_t family, const char *addr);

#endif /* WRK_H */
