/* Load generator for kvnet. Each thread opens one TCP connection and sends
 * SET/GET requests one at a time, recording the round trip of each request.
 * Usage: kvbench <port> <connections> <requests-per-connection> */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct worker {
    int port;
    int id;
    int requests;
    double *lat_us;
    int errors;
};

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* Reads until a full "\r\n"-terminated reply arrives. Returns 0 on success. */
static int read_reply(int fd, char *buf, size_t cap) {
    size_t len = 0;
    while (len + 1 < cap) {
        ssize_t n = recv(fd, buf + len, cap - 1 - len, 0);
        if (n <= 0) return -1;
        len += (size_t)n;
        buf[len] = '\0';
        if (len >= 2 && buf[len - 2] == '\r' && buf[len - 1] == '\n') return 0;
    }
    return -1;
}

static void *run(void *arg) {
    struct worker *w = arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)w->port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        w->errors = w->requests;
        return NULL;
    }
    char req[128], rep[256];
    for (int i = 0; i < w->requests; i++) {
        int n;
        if (i % 2 == 0)
            n = snprintf(req, sizeof req, "SET k%d_%d v%d\r\n", w->id, i % 64, i);
        else
            n = snprintf(req, sizeof req, "GET k%d_%d\r\n", w->id, (i - 1) % 64);
        double t0 = now_us();
        if (send(fd, req, (size_t)n, 0) != n || read_reply(fd, rep, sizeof rep) != 0) {
            w->errors++;
            break;
        }
        w->lat_us[i] = now_us() - t0;
        if (i % 2 == 1 && rep[0] != '$') w->errors++; /* a GET after a SET must hit */
    }
    close(fd);
    return NULL;
}

static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <port> <connections> <requests-per-connection>\n", argv[0]);
        return 2;
    }
    int port = atoi(argv[1]), conns = atoi(argv[2]), reqs = atoi(argv[3]);
    if (port <= 0 || conns <= 0 || reqs <= 0) return 2;

    pthread_t *th = calloc((size_t)conns, sizeof *th);
    struct worker *ws = calloc((size_t)conns, sizeof *ws);
    for (int i = 0; i < conns; i++) {
        ws[i].port = port;
        ws[i].id = i;
        ws[i].requests = reqs;
        ws[i].lat_us = calloc((size_t)reqs, sizeof(double));
    }
    double t0 = now_us();
    for (int i = 0; i < conns; i++) pthread_create(&th[i], NULL, run, &ws[i]);
    for (int i = 0; i < conns; i++) pthread_join(th[i], NULL);
    double secs = (now_us() - t0) / 1e6;

    size_t total = 0;
    int errors = 0;
    double *all = malloc((size_t)conns * (size_t)reqs * sizeof(double));
    for (int i = 0; i < conns; i++) {
        errors += ws[i].errors;
        for (int j = 0; j < reqs; j++)
            if (ws[i].lat_us[j] > 0) all[total++] = ws[i].lat_us[j];
    }
    qsort(all, total, sizeof(double), cmp);
    printf("connections=%d requests=%zu errors=%d elapsed=%.2fs throughput=%.0f req/s\n",
           conns, total, errors, secs, total / secs);
    if (total) {
        printf("latency_us p50=%.0f p99=%.0f max=%.0f\n", all[total / 2],
               all[(size_t)(total * 0.99)], all[total - 1]);
    }
    return errors ? 1 : 0;
}
