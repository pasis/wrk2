// Copyright (C) 2012 - Will Glozer.  All rights reserved.

#include <assert.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>

#include "wrk.h"
#include "script.h"
#include "main.h"
#include "hdr_histogram.h"
#include "stats.h"

// Max recordable latency of 1 day
#define MAX_LATENCY 24L * 60 * 60 * 1000000

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

enum {
    PHASE_INIT = 0,
    PHASE_WARMUP,
    PHASE_NORMAL,
};

static struct config {
    uint64_t threads;
    uint64_t connections;
    uint64_t duration;
    uint64_t timeout;
    uint64_t pipeline;
    uint64_t rate;
    uint64_t delay_ms;
    uint64_t warmup_timeout;
    bool     latency;
    bool     u_latency;
    bool     dynamic;
    bool     record_all_responses;
    bool     warmup;
    char    *host;
    char    *script;
    char    *local_ip;
    SSL_CTX *ctx;
} cfg;

static struct {
    stats *requests;
    pthread_mutex_t mutex;
} statistics;

static struct sock sock = {
    .connect  = sock_connect,
    .close    = sock_close,
    .read     = sock_read,
    .write    = sock_write,
    .readable = sock_readable
};

static struct http_parser_settings parser_settings = {
    .on_message_complete = response_complete
};

static volatile sig_atomic_t stop = 0;

// XXX This is a hack not to pass parameter to the script module.
char *g_local_ip = NULL;

int g_ready_threads = 0;
static volatile sig_atomic_t g_is_ready = 0;

static void handler(int sig) {
    stop = 1;
}

static void usage() {
    printf("Usage: wrk <options> <url>                            \n"
           "  Options:                                            \n"
           "    -c, --connections <N>  Connections to keep open   \n"
           "    -i, --local_ip    <S>  Bind to the specified local IP(s)\n"
           "                           It can be a comma separated list\n"
           "    -d, --duration    <T>  Duration of test           \n"
           "    -t, --threads     <N>  Number of threads to use   \n"
           "                                                      \n"
           "    -s, --script      <S>  Load Lua script file       \n"
           "    -H, --header      <H>  Add header to request      \n"
           "    -L  --latency          Print latency statistics   \n"
           "    -U  --u_latency        Print uncorrected latency statistics\n"
           "        --timeout     <T>  Socket/request timeout     \n"
           "    -B, --batch_latency    Measure latency of whole   \n"
           "                           batches of pipelined ops   \n"
           "                           (as opposed to each op)    \n"
           "    -v, --version          Print version details      \n"
           "    -R, --rate        <T>  work rate (throughput)     \n"
           "                           in requests/sec (total)    \n"
           "                           [Required Parameter]       \n"
           "    -W  --warmup           Enable warmup phase        \n"
           "                           In warmup phase connections are establised,\n"
           "                           but no requests are sent   \n"
           "                                                      \n"
           "                                                      \n"
           "  Numeric arguments may include a SI unit (1k, 1M, 1G)\n"
           "  Time arguments may include a time unit (2s, 2m, 2h)\n");
}

static size_t csv_nr(const char *s) {
    const char *p = s;
    size_t nr = 0;

    if (s == NULL) {
        return 0;
    }
    while ((p = strchr(p, ',')) != NULL) {
        ++p;
        ++nr;
    }
    return nr + 1;
}

int main(int argc, char **argv) {
    char *url, **headers = zmalloc(argc * sizeof(char *));
    struct http_parser_url parts = {};

    if (parse_args(&cfg, &url, &parts, headers, argc, argv)) {
        usage();
        exit(1);
    }

    char *schema  = copy_url_part(url, &parts, UF_SCHEMA);
    char *host    = copy_url_part(url, &parts, UF_HOST);
    char *port    = copy_url_part(url, &parts, UF_PORT);
    char *service = port ? port : schema;

    if (!strncmp("https", schema, 5)) {
        if ((cfg.ctx = ssl_init()) == NULL) {
            fprintf(stderr, "unable to initialize SSL\n");
            ERR_print_errors_fp(stderr);
            exit(1);
        }
        sock.connect  = ssl_connect;
        sock.close    = ssl_close;
        sock.read     = ssl_read;
        sock.write    = ssl_write;
        sock.readable = ssl_readable;
    }
	
    cfg.host = host;

    // Split comma separated IPs list into the array.
    char *local_ip_tokens = cfg.local_ip != NULL ? strdup(cfg.local_ip) : NULL;
    size_t local_ip_nr = csv_nr(local_ip_tokens);
    char **local_ip_arr = local_ip_nr > 0 ? malloc(local_ip_nr * sizeof(char *)) : NULL;
    if (local_ip_tokens != NULL) {
        char *saveptr = NULL;
        char *token = strtok_r(local_ip_tokens, ",", &saveptr);
        size_t i = 0;

        assert(local_ip_arr != NULL);
        while (token != NULL) {
            if (*token != '\0') {
                assert(i < local_ip_nr);
                local_ip_arr[i++] = token;
            }
            token = strtok_r(NULL, ",", &saveptr);
        }
        // We skip empty items, so 'i' may be lower then 'local_ip_nr'
        local_ip_nr = i;
        if (local_ip_nr > 0)
            g_local_ip = local_ip_arr[0];
    }

    signal(SIGPIPE, SIG_IGN);

    pthread_mutex_init(&statistics.mutex, NULL);
    statistics.requests = stats_alloc(10);
    thread *threads = zcalloc(cfg.threads * sizeof(thread));


    hdr_init(1, MAX_LATENCY, 3, &(statistics.requests->histogram));

    lua_State *L = script_create(cfg.script, url, headers);
    if (!script_resolve(L, host, service)) {
        char *msg = strerror(errno);
        fprintf(stderr, "unable to connect to %s:%s %s\n", host, service, msg);
        exit(1);
    }
    
    uint64_t connections = cfg.connections / cfg.threads;
    double throughput    = (double)cfg.rate / cfg.threads;
    uint64_t stop_at     = time_us() + (cfg.duration * 1000000);

    for (uint64_t i = 0; i < cfg.threads; i++) {
        thread *t = &threads[i];
        // TODO Review whether we can reduce number of events per thread
        t->loop        = aeCreateEventLoop(10 + cfg.connections * 3);
        t->connections = connections;
        t->throughput = throughput;
        t->stop_at     = stop_at;

        if (local_ip_nr > 0)
            t->local_ip = local_ip_arr[i % local_ip_nr];

        t->L = script_create(cfg.script, url, headers);
        script_init(L, t, argc - optind, &argv[optind]);

        if (i == 0) {
            cfg.pipeline = script_verify_request(t->L);
            cfg.dynamic = !script_is_static(t->L);
            if (script_want_response(t->L)) {
                parser_settings.on_header_field = header_field;
                parser_settings.on_header_value = header_value;
                parser_settings.on_body         = response_body;
            }
        }

        if (!t->loop || pthread_create(&t->thread, NULL, &thread_main, t)) {
            char *msg = strerror(errno);
            fprintf(stderr, "unable to create thread %"PRIu64": %s\n", i, msg);
            exit(2);
        }
    }

    struct sigaction sa = {
        .sa_handler = handler,
        .sa_flags   = 0,
    };
    sigfillset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    char *time = format_time_s(cfg.duration);
    printf("Running %s test @ %s\n", time, url);
    printf("  %"PRIu64" threads and %"PRIu64" connections\n",
            cfg.threads, cfg.connections);

    uint64_t start    = time_us();
    uint64_t complete = 0;
    uint64_t bytes    = 0;
    errors errors     = { 0 };

    struct hdr_histogram* latency_histogram;
    hdr_init(1, MAX_LATENCY, 3, &latency_histogram);
    struct hdr_histogram* u_latency_histogram;
    hdr_init(1, MAX_LATENCY, 3, &u_latency_histogram);

    uint64_t phase_normal_start_min = 0;

    for (uint64_t i = 0; i < cfg.threads; i++) {
        thread *t = &threads[i];
        pthread_join(t->thread, NULL);
        // Find timestamp of the first transition to the NORMAL phase.
        // With WARMUP period enabled we use it to measure real benchmarking runtime.
        if (!phase_normal_start_min || (t->phase_normal_start && t->phase_normal_start < phase_normal_start_min)) {
            phase_normal_start_min = t->phase_normal_start;
        }
    }

    if (phase_normal_start_min != 0) {
        // Measure runtime starting from the first transition to NORMAL phase.
        start = phase_normal_start_min;
    }
    uint64_t runtime_us = time_us() - start;

    for (uint64_t i = 0; i < cfg.threads; i++) {
        thread *t = &threads[i];
        complete += t->complete;
        bytes    += t->bytes;

        errors.connect += t->errors.connect;
        errors.read    += t->errors.read;
        errors.write   += t->errors.write;
        errors.timeout += t->errors.timeout;
        errors.status  += t->errors.status;
        errors.established += t->errors.established;
        errors.reconnect += t->errors.reconnect;

        hdr_add(latency_histogram, t->latency_histogram);
        hdr_add(u_latency_histogram, t->u_latency_histogram);
    }

    long double runtime_s   = runtime_us / 1000000.0;
    long double req_per_s   = complete   / runtime_s;
    long double bytes_per_s = bytes      / runtime_s;

    stats *latency_stats = stats_alloc(10);
    latency_stats->min = hdr_min(latency_histogram);
    latency_stats->max = hdr_max(latency_histogram);
    latency_stats->histogram = latency_histogram;

    print_stats_header();
    print_stats("Latency", latency_stats, format_time_us);
    print_stats("Req/Sec", statistics.requests, format_metric);

    if (cfg.latency) {
        print_hdr_latency(latency_histogram,
                "Recorded Latency");
        printf("----------------------------------------------------------\n");
    }

    if (cfg.u_latency) {
        printf("\n");
        print_hdr_latency(u_latency_histogram,
                "Uncorrected Latency (measured without taking delayed starts into account)");
        printf("----------------------------------------------------------\n");
    }

    char *runtime_msg = format_time_us(runtime_us);

    printf("  %"PRIu64" requests in %s, %sB read\n",
            complete, runtime_msg, format_binary(bytes));
    if (errors.connect || errors.read || errors.write || errors.timeout || errors.reconnect) {
        printf("  Socket errors: connect %d, read %d, write %d, timeout %d, reconnect %d\n",
               errors.connect, errors.read, errors.write, errors.timeout, errors.reconnect);
    }

    if (errors.status) {
        printf("  Non-2xx or 3xx responses: %d\n", errors.status);
    }

    printf("Established connections: %u\n", errors.established);
    printf("Requests/sec: %9.2Lf\n", req_per_s);
    printf("Transfer/sec: %10sB\n", format_binary(bytes_per_s));

    if (script_has_done(L)) {
        script_summary(L, runtime_us, complete, bytes);
        script_errors(L, &errors);
        script_done(L, latency_stats, statistics.requests);
    }

    free(local_ip_tokens);
    free(local_ip_arr);

    return 0;
}

static void phase_move(thread *thread, int phase) {
    if (thread->phase == PHASE_WARMUP && phase == PHASE_NORMAL) {
        connection *c  = thread->cs;

        printf("Warmup phase is ended (thread=%p, duration=%"PRIu64"sec).\n",
               thread, (time_us() - thread->start) / 1000000UL);

        for (uint64_t i = 0; i < thread->connections; i++, c++) {
            if (c->is_connected) {
                aeCreateFileEvent(thread->loop, c->fd, AE_READABLE, socket_readable, c);
                aeCreateFileEvent(thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);
            }
        }
        aeCreateTimeEvent(thread->loop, CALIBRATE_DELAY_MS, calibrate, thread, NULL);
        thread->start = time_us();
        thread->phase_normal_start = thread->start;
    }

    thread->phase = phase;
}

void *thread_main(void *arg) {
    thread *thread = arg;
    aeEventLoop *loop = thread->loop;

    thread->cs = zcalloc(thread->connections * sizeof(connection));
    tinymt64_init(&thread->rand, time_us());
    hdr_init(1, MAX_LATENCY, 3, &thread->latency_histogram);
    hdr_init(1, MAX_LATENCY, 3, &thread->u_latency_histogram);

    char *request = NULL;
    size_t length = 0;

    if (!cfg.dynamic) {
        script_request(thread->L, &request, &length);
    }

    double throughput = (thread->throughput / 1000000.0) / thread->connections;

    connection *c = thread->cs;

    for (uint64_t i = 0; i < thread->connections; i++, c++) {
        c->thread     = thread;
        c->ssl        = cfg.ctx ? SSL_new(cfg.ctx) : NULL;
        c->request    = request;
        c->length     = length;
        c->throughput = throughput;
        c->catch_up_throughput = throughput * 2;
        c->complete   = 0;
        c->caught_up  = true;
        // Stagger connects 5 msec apart within thread:
        aeCreateTimeEvent(loop, i * 5, delayed_initial_connect, c, NULL);
    }

    aeCreateTimeEvent(loop, STOP_CHECK_INTERNAL_MS, check_stop, thread, NULL);
    if (cfg.warmup) {
        uint64_t warmup_timeout = cfg.warmup_timeout;
        if (!warmup_timeout) {
            // Default timeout is 600sec for 350K connections
            warmup_timeout = cfg.connections * 600000UL / 350000UL;
            if (warmup_timeout < 1000) {
                // Don't make too short timeout, not to be affected by timer resolution
                warmup_timeout = 1000;
            }
        }
        aeCreateTimeEvent(loop, warmup_timeout, warmup_timed_out, thread, NULL);
    }

    thread->start = time_us();
    thread->phase = cfg.warmup ? PHASE_WARMUP : PHASE_NORMAL;
    aeMain(loop);

    aeDeleteEventLoop(loop);
    zfree(thread->cs);

    return NULL;
}

static const char *af_name(sa_family_t family)
{
    switch (family) {
    case AF_INET:
        return "AF_INET";
    case AF_INET6:
        return "AF_INET6";
    default:
        return "Unknown";
    }
}

void bind_socket(int fd, sa_family_t family, const char *bind_addr)
{
    void *dst;
    int rc;
    socklen_t addrlen;
    char *addr = strdup(bind_addr);

    union {
        struct sockaddr_in sa4;
        struct sockaddr_in6 sa6;
    } u_sa;

    if (g_local_ip == NULL)
        return;

    assert(addr != NULL);

    memset(&u_sa, 0, sizeof u_sa);
    if (family == AF_INET) {
        u_sa.sa4.sin_family = AF_INET;
        dst = (void*)&u_sa.sa4.sin_addr;
        addrlen = sizeof(u_sa.sa4);
    } else {
        char *ifname;

        u_sa.sa6.sin6_family = AF_INET6;
        dst = (void*)&u_sa.sa6.sin6_addr;
        addrlen = sizeof(u_sa.sa6);
        ifname = strchr(addr, '%');
        if (ifname != NULL) {
            *ifname = '\0';
            ++ifname;
            u_sa.sa6.sin6_scope_id = if_nametoindex(ifname);
        }
    }
    rc = inet_pton(family, addr, dst);
    if (rc != 1) {
        fprintf(stderr, "address '%s' is invalid for address family %s\n",
                addr, af_name(family));
        exit(1);
    }
    rc = bind(fd, (struct sockaddr*)&u_sa, addrlen);
    if (rc != 0) {
        fprintf(stderr, "warning: couldn't bind socket to address '%s', "
                "benchmark results may be invalid\n", addr);
    }

    free(addr);
}

static int connect_socket(thread *thread, connection *c) {
    struct addrinfo *addr = thread->addr;
    struct aeEventLoop *loop = thread->loop;
    int fd, flags;

    c->is_connected = false;

    fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (fd < 0) {
        char *msg = strerror(errno);
        fprintf(stderr, "unable to create socket (errno=%d): %s\n", errno, msg);
        exit(1);
    }

    if (thread->local_ip != NULL)
        bind_socket(fd, addr->ai_family, thread->local_ip);

    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (connect(fd, addr->ai_addr, addr->ai_addrlen) == -1) {
        if (errno != EINPROGRESS) goto error;
    }

    flags = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags));

    c->latest_connect = time_us();

    flags = AE_READABLE | AE_WRITABLE;
    c->connect_mask = flags;
    if (aeCreateFileEvent(loop, fd, flags, socket_connected, c) == AE_OK) {
        c->parser.data = c;
        c->fd = fd;
        return fd;
    }

  error:
    thread->errors.connect++;
    close(fd);
    return -1;
}

static int reconnect_socket(thread *thread, connection *c) {
    aeDeleteFileEvent(thread->loop, c->fd, AE_WRITABLE | AE_READABLE);
    sock.close(c);
    close(c->fd);
    thread->errors.reconnect++;
    return connect_socket(thread, c);
}

static int delayed_initial_connect(aeEventLoop *loop, long long id, void *data) {
    connection* c = data;
    c->thread_start = time_us();
    connect_socket(c->thread, c);
    return AE_NOMORE;
}

static int calibrate(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;

    long double mean = hdr_mean(thread->latency_histogram);
    long double latency = hdr_value_at_percentile(
            thread->latency_histogram, 90.0) / 1000.0L;
    long double interval = MAX(latency * 2, 10);

    if (mean == 0) return CALIBRATE_DELAY_MS;

    thread->mean     = (uint64_t) mean;
    hdr_reset(thread->latency_histogram);
    hdr_reset(thread->u_latency_histogram);

    thread->start    = time_us();
    thread->interval = interval;
    thread->requests = 0;

    printf("  Thread calibration: mean lat.: %.3fms, rate sampling interval: %dms\n",
            (thread->mean)/1000.0,
            thread->interval);

    aeCreateTimeEvent(loop, thread->interval, sample_rate, thread, NULL);

    return AE_NOMORE;
}

static void close_all_conn(thread *thread) {
    connection *c = thread->cs;

    for (uint64_t i = 0; i < thread->connections; i++, c++) {
        aeDeleteFileEvent(thread->loop, c->fd, AE_WRITABLE | AE_READABLE);
        sock.close(c);
        close(c->fd);
    }
}

static int check_stop(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;
    uint64_t now   = time_us();

    if (stop || now >= thread->stop_at) {
        close_all_conn(thread);
        aeStop(loop);
    }

    return STOP_CHECK_INTERNAL_MS;
}

static int warmup_timed_out(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;

    // It is safe to transit to NORMAL if we're already in NORMAL phase
    phase_move(thread, PHASE_NORMAL);

    return AE_NOMORE;
}

static int inter_thread_sync(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;

    if (g_is_ready) {
        phase_move(thread, PHASE_NORMAL);
    }

    return thread->phase == PHASE_NORMAL ? AE_NOMORE : THREAD_SYNC_INTERVAL_MS;
}

static int sample_rate(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;

    uint64_t elapsed_ms = (time_us() - thread->start) / 1000;
    uint64_t requests = (thread->requests / (double) elapsed_ms) * 1000;

    pthread_mutex_lock(&statistics.mutex);
    stats_record(statistics.requests, requests);
    pthread_mutex_unlock(&statistics.mutex);

    thread->requests = 0;
    thread->start    = time_us();

    return thread->interval;
}

static int header_field(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    if (c->state == VALUE) {
        *c->headers.cursor++ = '\0';
        c->state = FIELD;
    }
    buffer_append(&c->headers, at, len);
    return 0;
}

static int header_value(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    if (c->state == FIELD) {
        *c->headers.cursor++ = '\0';
        c->state = VALUE;
    }
    buffer_append(&c->headers, at, len);
    return 0;
}

static int response_body(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    buffer_append(&c->body, at, len);
    return 0;
}

static uint64_t usec_to_next_send(connection *c) {
    uint64_t now = time_us();

    uint64_t next_start_time = c->thread_start + (c->complete / c->throughput);

    bool send_now = true;

    if (next_start_time > now) {
        // We are on pace. Indicate caught_up and don't send now.
        c->caught_up = true;
        send_now = false;
    } else {
        // We are behind
        if (c->caught_up) {
            // This is the first fall-behind since we were last caught up
            c->caught_up = false;
            c->catch_up_start_time = now;
            c->complete_at_catch_up_start = c->complete;
        }

        // Figure out if it's time to send, per catch up throughput:
        uint64_t complete_since_catch_up_start =
                c->complete - c->complete_at_catch_up_start;

        next_start_time = c->catch_up_start_time +
                (complete_since_catch_up_start / c->catch_up_throughput);

        if (next_start_time > now) {
            // Not yet time to send, even at catch-up throughout:
            send_now = false;
        }
    }

    if (send_now) {
        c->latest_should_send_time = now;
        c->latest_expected_start = next_start_time;
    }

    return send_now ? 0 : (next_start_time - now);
}

static int delay_request(aeEventLoop *loop, long long id, void *data) {
    connection* c = data;
    uint64_t time_usec_to_wait = usec_to_next_send(c);
    if (time_usec_to_wait) {
        return round((time_usec_to_wait / 1000.0L) + 0.5); /* don't send, wait */
    }
    aeCreateFileEvent(c->thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);
    return AE_NOMORE;
}

static int response_complete(http_parser *parser) {
    connection *c = parser->data;
    thread *thread = c->thread;
    uint64_t now = time_us();
    int status = parser->status_code;

    thread->complete++;
    thread->requests++;

    if (status > 399) {
        thread->errors.status++;
    }

    if (c->headers.buffer) {
        *c->headers.cursor++ = '\0';
        script_response(thread->L, status, &c->headers, &c->body);
        c->state = FIELD;
    }

    if (now >= thread->stop_at) {
        aeStop(thread->loop);
        goto done;
    }

    // Count all responses (including pipelined ones:)
    c->complete++;

    // Note that expected start time is computed based on the completed
    // response count seen at the beginning of the last request batch sent.
    // A single request batch send may contain multiple requests, and
    // result in multiple responses. If we incorrectly calculated expect
    // start time based on the completion count of these individual pipelined
    // requests we can easily end up "gifting" them time and seeing
    // negative latencies.
    uint64_t expected_latency_start = c->thread_start +
            (c->complete_at_last_batch_start / c->throughput);

    int64_t expected_latency_timing = now - expected_latency_start;

    if (expected_latency_timing < 0) {
        printf("\n\n ---------- \n\n");
        printf("We are about to crash and die (recoridng a negative #)");
        printf("This wil never ever ever happen...");
        printf("But when it does. The following information will help in debugging");
        printf("response_complete:\n");
        printf("  expected_latency_timing = %"PRId64"\n", expected_latency_timing);
        printf("  now = %"PRIu64"\n", now);
        printf("  expected_latency_start = %"PRIu64"\n", expected_latency_start);
        printf("  c->thread_start = %"PRIu64"\n", c->thread_start);
        printf("  c->complete = %"PRIu64"\n", c->complete);
        printf("  throughput = %g\n", c->throughput);
        printf("  latest_should_send_time = %"PRIu64"\n", c->latest_should_send_time);
        printf("  latest_expected_start = %"PRIu64"\n", c->latest_expected_start);
        printf("  latest_connect = %"PRIu64"\n", c->latest_connect);
        printf("  latest_write = %"PRIu64"\n", c->latest_write);

        expected_latency_start = c->thread_start +
                ((c->complete ) / c->throughput);
        printf("  next expected_latency_start = %"PRIu64"\n", expected_latency_start);
    }

    c->latest_should_send_time = 0;
    c->latest_expected_start = 0;

    if (--c->pending == 0) {
        c->has_pending = false;
        aeCreateFileEvent(thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);
    }

    // Record if needed, either last in batch or all, depending in cfg:
    if (cfg.record_all_responses || !c->has_pending) {
        hdr_record_value(thread->latency_histogram, expected_latency_timing);

        uint64_t actual_latency_timing = now - c->actual_latency_start;
        hdr_record_value(thread->u_latency_histogram, actual_latency_timing);
    }


    if (!http_should_keep_alive(parser)) {
        reconnect_socket(thread, c);
        goto done;
    }

    http_parser_init(parser, HTTP_RESPONSE);

  done:
    return 0;
}

static void socket_connected(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    int retry_flags = 0;
    int add_flags = 0;
    int del_flags = 0;
    int rc;

    switch (sock.connect(c, cfg.host, &retry_flags)) {
        case OK:    break;
        case ERROR: goto error;
        case RETRY:
            // Remove non-reqeusted events not to consume 100% of CPU because of
            // polling TLS socket during TLS handshake phase.
            if ((retry_flags & E_WANT_READ) && !(c->connect_mask & AE_READABLE))
                add_flags |= AE_READABLE;
            if (!(retry_flags & E_WANT_READ) && (c->connect_mask & AE_READABLE))
                del_flags |= AE_READABLE;
            if ((retry_flags & E_WANT_WRITE) && !(c->connect_mask & AE_WRITABLE))
                add_flags |= AE_WRITABLE;
            if (!(retry_flags & E_WANT_WRITE) && (c->connect_mask & AE_WRITABLE))
                del_flags |= AE_WRITABLE;
            assert((add_flags & del_flags) == 0);
            if (del_flags != 0) {
                aeDeleteFileEvent(loop, c->fd, del_flags);
                c->connect_mask &= ~del_flags;
            }
            if (add_flags != 0) {
                rc = aeCreateFileEvent(loop, c->fd, add_flags, socket_connected, c);
                assert(rc == AE_OK);
                c->connect_mask |= add_flags;
            }
            return;
    }

    if (c->is_connected) {
        return;
    }

    http_parser_init(&c->parser, HTTP_RESPONSE);
    c->written = 0;
    c->thread->errors.established++;
    c->is_connected = true;

    // Create file events only in NORMAL phase. We create the events for connected
    // sockets when move from WARMUP to NORMAL phase.
    if (c->thread->phase == PHASE_NORMAL) {
        aeCreateFileEvent(c->thread->loop, fd, AE_READABLE, socket_readable, c);
        aeCreateFileEvent(c->thread->loop, fd, AE_WRITABLE, socket_writeable, c);
    }

    if (cfg.warmup && c->thread->errors.established == c->thread->connections) {
        // Create a timed event to periodically check whether all threads are finished
        // with handshakes. Without a synchronization can get high concurrency between
        // TLS handshakes and requests.
        aeCreateTimeEvent(c->thread->loop, THREAD_SYNC_INTERVAL_MS, inter_thread_sync, c->thread, NULL);
        int counter = __sync_add_and_fetch(&g_ready_threads, 1);
        if (counter == cfg.threads) {
            g_is_ready = 1;
        }
    }

    return;

  error:
    c->thread->errors.connect++;
    reconnect_socket(c->thread, c);

}

static void socket_writeable(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    thread *thread = c->thread;

    if (!c->written) {
        uint64_t time_usec_to_wait = usec_to_next_send(c);
        if (time_usec_to_wait) {
            int msec_to_wait = round((time_usec_to_wait / 1000.0L) + 0.5);

            // Not yet time to send. Delay:
            aeDeleteFileEvent(loop, fd, AE_WRITABLE);
            aeCreateTimeEvent(
                    thread->loop, msec_to_wait, delay_request, c, NULL);
            return;
        }
        c->latest_write = time_us();
    }

    if (!c->written && cfg.dynamic) {
        script_request(thread->L, &c->request, &c->length);
    }

    char  *buf = c->request + c->written;
    size_t len = c->length  - c->written;
    size_t n;

    if (!c->written) {
        c->start = time_us();
        if (!c->has_pending) {
            c->actual_latency_start = c->start;
            c->complete_at_last_batch_start = c->complete;
            c->has_pending = true;
        }
        c->pending = cfg.pipeline;
    }

    switch (sock.write(c, buf, len, &n)) {
        case OK:    break;
        case ERROR: goto error;
        case RETRY: return;
    }

    c->written += n;
    if (c->written == c->length) {
        c->written = 0;
        aeDeleteFileEvent(loop, fd, AE_WRITABLE);
    }

    return;

  error:
    thread->errors.write++;
    reconnect_socket(thread, c);
}


static void socket_readable(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    size_t n;

    do {
        switch (sock.read(c, &n)) {
            case OK:    break;
            case ERROR: goto error;
            case RETRY: return;
        }

        if (http_parser_execute(&c->parser, &parser_settings, c->buf, n) != n) goto error;
        c->thread->bytes += n;
    } while (n == RECVBUF && sock.readable(c) > 0);

    return;

  error:
    c->thread->errors.read++;
    reconnect_socket(c->thread, c);
}

static uint64_t time_us() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (t.tv_sec * 1000000) + t.tv_usec;
}

static char *copy_url_part(char *url, struct http_parser_url *parts, enum http_parser_url_fields field) {
    char *part = NULL;

    if (parts->field_set & (1 << field)) {
        uint16_t off = parts->field_data[field].off;
        uint16_t len = parts->field_data[field].len;
        part = zcalloc(len + 1 * sizeof(char));
        memcpy(part, &url[off], len);
    }

    return part;
}

static struct option longopts[] = {
    { "connections",    required_argument, NULL, 'c' },
    { "local_ip",       required_argument, NULL, 'i' },
    { "duration",       required_argument, NULL, 'd' },
    { "threads",        required_argument, NULL, 't' },
    { "script",         required_argument, NULL, 's' },
    { "header",         required_argument, NULL, 'H' },
    { "latency",        no_argument,       NULL, 'L' },
    { "u_latency",      no_argument,       NULL, 'U' },
    { "batch_latency",  no_argument,       NULL, 'B' },
    { "timeout",        required_argument, NULL, 'T' },
    { "help",           no_argument,       NULL, 'h' },
    { "version",        no_argument,       NULL, 'v' },
    { "rate",           required_argument, NULL, 'R' },
    { "warmup",         no_argument,       NULL, 'W' },
    { NULL,             0,                 NULL,  0  }
};

static int parse_args(struct config *cfg, char **url, struct http_parser_url *parts, char **headers, int argc, char **argv) {
    char c, **header = headers;

    memset(cfg, 0, sizeof(struct config));
    cfg->threads     = 2;
    cfg->connections = 10;
    cfg->duration    = 10;
    cfg->timeout     = SOCKET_TIMEOUT_MS;
    cfg->rate        = 0;
    cfg->record_all_responses = true;
    cfg->warmup      = false;
    cfg->warmup_timeout = 0;

    while ((c = getopt_long(argc, argv, "t:c:i:d:s:H:T:R:LUBrWv?", longopts, NULL)) != -1) {
        switch (c) {
            case 't':
                if (scan_metric(optarg, &cfg->threads)) return -1;
                break;
            case 'c':
                if (scan_metric(optarg, &cfg->connections)) return -1;
                break;
            case 'i':
                cfg->local_ip = optarg;
                break;
            case 'd':
                if (scan_time(optarg, &cfg->duration)) return -1;
                break;
            case 's':
                cfg->script = optarg;
                break;
            case 'H':
                *header++ = optarg;
                break;
            case 'L':
                cfg->latency = true;
                break;
            case 'B':
                cfg->record_all_responses = false;
                break;
            case 'U':
                cfg->latency = true;
                cfg->u_latency = true;
                break;
            case 'T':
                if (scan_time(optarg, &cfg->timeout)) return -1;
                cfg->timeout *= 1000;
                break;
            case 'R':
                if (scan_metric(optarg, &cfg->rate)) return -1;
                break;
            case 'v':
                printf("wrk %s [%s] ", VERSION, aeGetApiName());
                printf("Copyright (C) 2012 Will Glozer\n");
                break;
            case 'W':
                cfg->warmup = true;
                break;
            case 'h':
            case '?':
            case ':':
            default:
                return -1;
        }
    }

    if (optind == argc || !cfg->threads || !cfg->duration) return -1;

    if (!script_parse_url(argv[optind], parts)) {
        fprintf(stderr, "invalid URL: %s\n", argv[optind]);
        return -1;
    }

    if (!cfg->connections || cfg->connections < cfg->threads) {
        fprintf(stderr, "number of connections must be >= threads\n");
        return -1;
    }

    if (cfg->rate == 0) {
        fprintf(stderr,
                "Throughput MUST be specified with the --rate or -R option\n");
        return -1;
    }

    *url    = argv[optind];
    *header = NULL;

    return 0;
}

static void print_stats_header() {
    printf("  Thread Stats%6s%11s%8s%12s\n", "Avg", "Stdev", "Max", "+/- Stdev");
}

static void print_units(long double n, char *(*fmt)(long double), int width) {
    char *msg = fmt(n);
    int len = strlen(msg), pad = 2;

    if (isalpha(msg[len-1])) pad--;
    if (isalpha(msg[len-2])) pad--;
    width -= pad;

    printf("%*.*s%.*s", width, width, msg, pad, "  ");

    free(msg);
}

static void print_stats(char *name, stats *stats, char *(*fmt)(long double)) {
    uint64_t max = stats->max;
    long double mean  = stats_summarize(stats);
    long double stdev = stats_stdev(stats, mean);

    printf("    %-10s", name);
    print_units(mean,  fmt, 8);
    print_units(stdev, fmt, 10);
    print_units(max,   fmt, 9);
    printf("%8.2Lf%%\n", stats_within_stdev(stats, mean, stdev, 1));
}

static void print_hdr_latency(struct hdr_histogram* histogram, const char* description) {
    long double percentiles[] = { 50.0, 75.0, 90.0, 99.0, 99.9, 99.99, 99.999, 100.0 };
    printf("  Latency Distribution (HdrHistogram - %s)\n", description);
    for (size_t i = 0; i < sizeof(percentiles) / sizeof(long double); i++) {
        long double p = percentiles[i];
        int64_t n = hdr_value_at_percentile(histogram, p);
        printf("%7.3Lf%%", p);
        print_units(n, format_time_us, 10);
        printf("\n");
    }
    printf("\n%s\n", "  Detailed Percentile spectrum:");
    hdr_percentiles_print(histogram, stdout, 5, 1000.0, CLASSIC);
}
