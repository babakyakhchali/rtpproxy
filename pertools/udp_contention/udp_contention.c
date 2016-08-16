#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <machine/cpufunc.h>

#include "rtpp_network.h"

#define TEST_KIND_ALL         0
#define TEST_KIND_UNCONNECTED 1
#define TEST_KIND_CONNECTED   2
#define TEST_KIND_HALFCONN    3
#define TEST_KIND_MAX         TEST_KIND_CONNECTED

struct tconf {
    int nthreads_max;
    int nthreads_min;
    int paylen_min;
    int paylen_max;
    struct addrinfo *dstaddrs;
    int ndstaddrs;
    const char *dstaddr;
    int dstnetpref;
    int test_kind;
    uint64_t magic;
};

static int
genrandomdest(struct tconf *cfp, struct sockaddr *sap)
{
    long rnum;
    uint16_t rport;

    do {
        rport = (uint16_t)(random());
    } while (rport < 1000);
    if (cfp->dstaddrs == NULL) {
        struct in_addr raddr;
        struct sockaddr_in *s_in;

        if (inet_aton(cfp->dstaddr, &raddr) == 0) {
            return (-1);
        }
        rnum = random() >> cfp->dstnetpref;
        raddr.s_addr |= htonl(rnum);
        s_in = satosin(sap);
        s_in->sin_addr = raddr;
        s_in->sin_port = htons(rport);
        s_in->sin_family = AF_INET;
        return (0);
    } else {
        struct addrinfo *i_res;

        rnum = random() % cfp->ndstaddrs;

        for (i_res = cfp->dstaddrs; i_res != NULL; i_res = i_res->ai_next) {
            if (rnum != 0) {
                rnum--;
                continue;
            }
            memcpy(sap, i_res->ai_addr, i_res->ai_addrlen);
            return (0);
        }
    }
    abort();
}

struct pktdata {
    uint64_t magic;
    uint64_t send_ts;
    int idx;
};

union pkt {
    unsigned char d[256];
    struct pktdata pd;
};

struct destination
{
    int sin;
    int sout;
    int sconnected;
    struct sockaddr_storage daddr;
    int buflen;
    union pkt buf;
};

struct workset
{
    pthread_t tid;
    int nreps;
    int ndest;
    double stime;
    double etime;
    uint64_t send_nerrs;
    uint64_t send_nshrts;
    struct destination dests[0]; /* <- keep this the last member! */
};

struct recvset
{
    pthread_t tid;
    int ndest;
    uint64_t **nrecvd;
    uint64_t nrecvd_total;
    uint64_t npolls;
    uint64_t rtt_total;
    int done;
    uint64_t magic;
    struct pollfd pollset[0];
};

static void
genrandombuf(struct destination *dp, int minlen, int maxlen)
{
    unsigned int difflen;
    int i;

    assert(minlen <= maxlen && maxlen <= sizeof(dp->buf));
    difflen = maxlen - minlen;
    if (difflen > 0) {
        dp->buflen = minlen + (random() % (difflen + 1));
    } else {
        dp->buflen = minlen;
    }
    for (i = 0; i < dp->buflen; i++) {
        dp->buf.d[i] = (unsigned char)random();
    }
}

static int
socket_ctor(int domain)
{
    int s, flags;

    s = socket(domain, SOCK_DGRAM, 0);
    if (s == -1) {
        return (-1);
    }
    flags = fcntl(s, F_GETFL);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    return (s);
}

static struct workset *
generate_workset(int setsize, struct tconf *cfp)
{
    struct workset *wp;
    struct destination *dp;
    size_t msize;
    int i;

    msize = sizeof(struct workset) + (setsize * sizeof(struct destination));
    wp = malloc(msize);
    if (wp == NULL) {
        return (NULL);
    }
    memset(wp, '\0', msize);
    wp->ndest = setsize;
    for (i = 0; i < setsize; i++) {
        dp = &(wp->dests[i]);
        genrandomdest(cfp, sstosa(&dp->daddr));
        dp->sout = dp->sin = socket_ctor(sstosa(&dp->daddr)->sa_family);
        if (dp->sin == -1) {
            goto e1;
        }
        genrandombuf(dp, cfp->paylen_min, cfp->paylen_max);
        dp->buf.pd.magic = cfp->magic;
        dp->buf.pd.idx = i;
    }
    return (wp);
e1:
    for (i = i - 1; i >= 0; i--) {
        close(wp->dests[i].sin);
    }
    free(wp);
    return (NULL);
}

#if !defined(sstosin)
/* This should go, once we make it protocol-agnostic */
#define sstosin(ss)      ((struct sockaddr_in *)(ss))
#endif

static int
connect_workset(struct workset *wp, int test_type)
{
    int i, r, reuse;
    int rval;
    socklen_t llen;
    struct destination *dp;
    struct sockaddr_storage la;
    struct sockaddr_in *lip;

    rval = 0;
    for (i = 0; i < wp->ndest; i++) {
        dp = &(wp->dests[i]);
        if (dp->sconnected == 0) {
            if (test_type == TEST_KIND_HALFCONN) {
                dp->sout = socket_ctor(sstosa(&dp->daddr)->sa_family);
                if (dp->sout == -1) {
                    rval -= 1;
                    dp->sout = dp->sin;
                    continue;
                }

                reuse = 1;
                r = setsockopt(dp->sin, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
                if (r == -1) {
                    rval -= 1;
                    continue;
                }

                lip = sstosin(&la);
                lip->sin_addr.s_addr = INADDR_ANY;
                lip->sin_port = htons(0);
                llen = sizeof(struct sockaddr_in);
                r = bind(dp->sin, sstosa(&la), llen);
                if (r == -1) {
                    rval -= 1;
                    continue;
                }
                llen = sizeof(la);
                r = getsockname(dp->sin, sstosa(&la), &llen);
                if (r == -1) {
                    rval -= 1;
                    continue;
                }

                reuse = 1;
                r = setsockopt(dp->sout, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
                if (r == -1) {
                    rval -= 1;
                    continue;
                }
                lip = sstosin(&la);
#if 0
                r = local4remote(sstosa(&dp->daddr), &lat);
                if (r == -1) {
                    rval -= 1;
                    continue;
                }
                lip->sin_addr.s_addr = sstosin(&lat)->sin_addr.s_addr;
#endif
                llen = sizeof(struct sockaddr_in);
                r = bind(dp->sout, sstosa(&la), llen);
                if (r == -1) {
                    rval -= 1;
                    continue;
                }
            }
            if (connect(dp->sout, sstosa(&dp->daddr), SS_LEN(&dp->daddr)) != 0) {
                rval -= 1;
                continue;
            }
            if (test_type == TEST_KIND_HALFCONN) {
#if 0
                llen = sizeof(la);
                r = getsockname(dp->sout, sstosa(&la), &llen);
                if (r == -1) {
                    rval -= 1;
                    continue;
                }
                reuse = 1;
                r = setsockopt(dp->sin, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
                if (r == -1) {
                    rval -= 1;
                    continue;
                }
                lip = sstosin(&la);
                lip->sin_addr.s_addr = INADDR_ANY;
                llen = sizeof(struct sockaddr_in);
                r = bind(dp->sin, sstosa(&la), llen);
                if (r == -1) {
                    rval -= 1;
                    continue;
                }
#endif
                r = shutdown(dp->sout, SHUT_RD);
                if (r == -1) {
                    rval -= 1;
                    continue;
                }
            }
            dp->sconnected = 1;
        }
    }
    return (rval);
}

#if defined(CLOCK_UPTIME_PRECISE)
#define RTPP_CLOCK CLOCK_UPTIME_PRECISE
#else
# if defined(CLOCK_MONOTONIC_RAW)
#define RTPP_CLOCK CLOCK_MONOTONIC_RAW
# else
#define RTPP_CLOCK CLOCK_MONOTONIC
#endif
#endif

static double timespec2dtime(time_t, long);

double
getdtime(void)
{
    struct timespec tp;

    if (clock_gettime(RTPP_CLOCK, &tp) == -1)
        return (-1);

    return timespec2dtime(tp.tv_sec, tp.tv_nsec);
}

static double
timespec2dtime(time_t tv_sec, long tv_nsec)
{

    return (double)tv_sec + (double)tv_nsec / 1000000000.0;
}

static void
process_workset(struct workset *wp)
{
    int i, j, r;
    struct destination *dp;

    wp->stime = getdtime();
    for (i = 0; i < wp->nreps; i++) {
        for (j = 0; j < wp->ndest; j++) {
            dp = &(wp->dests[j]);
            dp->buf.pd.send_ts = rdtsc();
            if (dp->sconnected == 0) {
                r = sendto(dp->sout, dp->buf.d, dp->buflen, 0,
                  sstosa(&dp->daddr), SS_LEN(&dp->daddr));
            } else {
                r = send(dp->sout, dp->buf.d, dp->buflen, 0);
            }
            if (r <= 0) {
                wp->send_nerrs += 1;
            } else if (r < dp->buflen) {
                wp->send_nshrts += 1;
            }
        }
    }
    wp->etime = getdtime();
}

static void
process_recvset(struct recvset  *rp)
{
    int nready, i, rval;
    struct pollfd *pdp;
    union pkt buf;
    struct sockaddr_storage raddr;
    socklen_t fromlen;
    uint64_t rtime, rtt;

    for (;;) {
        nready = poll(rp->pollset, rp->ndest, 100);
        rp->npolls++;
        if (rp->done != 0 && nready == 0) {
            break;
        }
        if (nready <= 0) {
            continue;
        }
        for (i = 0; i < rp->ndest && nready > 0; i++) {
            pdp = &rp->pollset[i];
            if ((pdp->revents & POLLIN) == 0) {
                continue;
            }
            fromlen = sizeof(raddr);
            rval = recvfrom(pdp->fd, buf.d, sizeof(buf.d), 0, sstosa(&raddr),
              &fromlen);
            rtime = rdtsc();
            if (rval > 0) {
                if (buf.pd.magic != rp->magic) {
                    continue;
                }
                rtt = rtime - buf.pd.send_ts;
                rp->nrecvd[i]++;
                rp->nrecvd_total++;
                rp->rtt_total += rtt;
            }
            nready -= 1;
        }
    }
}


static void
release_workset(struct workset *wp)
{
    int i;
    struct destination *dp;

    for (i = 0; i < wp->ndest; i++) {
        dp = &(wp->dests[i]);
        close(dp->sin);
        if (dp->sout != dp->sin) {
            close(dp->sout);
        }
    }
    free(wp);
}

static void
release_recvset(struct recvset  *rp)
{

    free(rp->nrecvd);
    free(rp);
}

struct recvset *
generate_recvset(struct workset *wp, struct tconf *cfp)
{
    struct recvset *rp;
    int msize, i;
    struct pollfd *pdp;

    msize = sizeof(struct recvset) + (sizeof(struct pollfd) * wp->ndest);
    rp = malloc(msize);
    if (rp == NULL) {
        return (NULL);
    }
    memset(rp, '\0', msize);
    msize = sizeof(uint64_t) * wp->ndest;
    rp->nrecvd = malloc(msize);
    if (rp->nrecvd == NULL) {
        free(rp);
        return (NULL);
    }
    memset(rp->nrecvd, '\0', msize);
    for (i = 0; i < wp->ndest; i++) {
        pdp = &rp->pollset[i];
        pdp->fd = wp->dests[i].sin;
        pdp->events = POLLIN;
    }
    rp->ndest = wp->ndest;
    rp->magic = cfp->magic;
    return (rp);
}

struct tstats {
    double total_pps;
    double total_poll_rate;
    double ploss_ratio;
    double send_nerrs_ratio;
    double send_nshrts_ratio;
};

static void
run_test(int nthreads, int test_type, struct tconf *cfp, struct tstats *tsp)
{
    int nreps = 10 * 100;
    int npkts = 4000;
    struct workset *wsp[32];
    struct recvset *rsp[32];
    int i;
    double pps, tduration, poll_rate;
    uint64_t nrecvd_total, nsent_total, nsent_succ_total, rtt_total;
    uint64_t send_nerrs_total, send_nshrts_total;

    for (i = 0; i < nthreads; i++) {
        wsp[i] = generate_workset(npkts, cfp);
        assert(wsp[i] != NULL);
        wsp[i]->nreps = nreps;
        if (test_type == TEST_KIND_CONNECTED || test_type == TEST_KIND_HALFCONN) {
            if (connect_workset(wsp[i], test_type) != 0) {
                fprintf(stderr, "connect_workset() failed\n");
                abort();
            }
        }
        rsp[i] = generate_recvset(wsp[i], cfp);
    }
    for (i = 0; i < nthreads; i++) {
        pthread_create(&wsp[i]->tid, NULL, (void *(*)(void *))process_workset, wsp[i]);
        pthread_create(&rsp[i]->tid, NULL, (void *(*)(void *))process_recvset, rsp[i]);
    }
    nrecvd_total = nsent_total = send_nerrs_total = send_nshrts_total = 0;
    for (i = 0; i < nthreads; i++) {
        pthread_join(wsp[i]->tid, NULL);
        rsp[i]->done = 1;
        pthread_join(rsp[i]->tid, NULL);
        nsent_total += wsp[i]->nreps * wsp[i]->ndest;
        tduration = wsp[i]->etime - wsp[i]->stime;
        send_nerrs_total += wsp[i]->send_nerrs;
        send_nshrts_total += wsp[i]->send_nshrts;
        pps = (wsp[i]->nreps * wsp[i]->ndest) - wsp[i]->send_nerrs;
        pps /= tduration;
        tsp->total_pps += pps;
        nrecvd_total += rsp[i]->nrecvd_total;
        rtt_total += rsp[i]->rtt_total;
        poll_rate = ((double)rsp[i]->npolls) / tduration;
        tsp->total_poll_rate += poll_rate / (double)nthreads;
        release_workset(wsp[i]);
        release_recvset(rsp[i]);
    }
    nsent_succ_total = nsent_total -= send_nerrs_total;
    fprintf(stderr, "nsent_total=%ju, nsent_succ_total=%ju, nrecvd_total=%ju\n",
      (uintmax_t)nsent_total, (uintmax_t)nsent_succ_total,
      (uintmax_t)nrecvd_total);
    tsp->ploss_ratio = (double)(nsent_succ_total - nrecvd_total) /
      (double)(nsent_succ_total);
    tsp->send_nerrs_ratio = (double)(send_nerrs_total) /
      (double)(nsent_total);
     tsp->send_nshrts_ratio = (double)(send_nshrts_total) /
      (double)(nsent_total);
    return;
}

static void
usage(void)
{

    exit(1);
}

static void
print_test_stats(int nthreads, int test_kind, struct tstats *tp)
{

    printf("nthreads = %d, connected = %d: total PPS = %f, "
      "loss %f%%, poll %f\n", nthreads, test_kind, tp->total_pps,
      tp->ploss_ratio * 100, tp->total_poll_rate);
    if (tp->send_nerrs_ratio != 0.0 || tp->send_nshrts_ratio != 0.0) {
        printf("  send channel issues: error = %f%%, short send %f%%\n",
          tp->send_nerrs_ratio * 100.0, tp->send_nshrts_ratio * 100.0);
    }
}

int
main(int argc, char **argv)
{
    struct tconf cfg;
    int i, j, ch, dstishost;
    struct tstats tstats;
    char *cp;

    memset(&cfg, '\0', sizeof(struct tconf));
    dstishost = 0;
    cfg.nthreads_max = 10;
    cfg.nthreads_min = 1;
    cfg.dstaddr = "170.178.193.146";
    cfg.dstnetpref = 32;
    cfg.magic = ((uint64_t)random() << 32) | (uint64_t)random();
    cfg.paylen_min = 30;
    cfg.paylen_max = 170;

    while ((ch = getopt(argc, argv, "m:M:k:p:P:h")) != -1) {
        switch (ch) {
        case 'm':
            cfg.nthreads_min = atoi(optarg);
            break;

        case 'M':
            cfg.nthreads_max = atoi(optarg);
            break;

        case 'k':
            cfg.test_kind = atoi(optarg);
            break;

        case 'p':
            cfg.paylen_min = atoi(optarg);
            if (cfg.paylen_min < sizeof(struct pktdata)) {
                usage();
            }
            break;

        case 'P':
            cfg.paylen_max = atoi(optarg);
            break;

        case 'h':
            dstishost = 1;
            break;

        case '?':
        default:
            usage();
        }
    }
    if (cfg.paylen_max < cfg.paylen_min) {
        usage();
    }
    argc -= optind;
    argv += optind;

    if (argc != 1) {
        usage();
    }
    if (dstishost == 0) {
        cfg.dstaddr = argv[0];
        cp = strrchr(cfg.dstaddr, '/');
        if (cp != NULL) {
            cp[0] = '\0';
            cfg.dstnetpref = atoi(cp + 1);
            if (cfg.dstnetpref < 1 || cfg.dstnetpref > 32) {
                usage();
            }
        }
    } else {
        struct addrinfo hints, *i_res;

        memset(&hints, 0, sizeof(hints));
        hints.ai_flags = 0;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;     /* UDP */
        i = getaddrinfo(argv[0], "5060", &hints, &cfg.dstaddrs);
        if (i != 0) {
            fprintf(stderr, "%s: %s\n", argv[0], gai_strerror(i));
            exit(1);
        }
        for (i_res = cfg.dstaddrs; i_res != NULL; i_res = i_res->ai_next) {
            cfg.ndstaddrs++;
        }
        if (cfg.ndstaddrs == 0) {
            fprintf(stderr, "getaddrinfo() returned no error but list is "
              "empty!\n");
            abort();
        }
    }

    srandomdev();
    for (i = cfg.nthreads_min; i <= cfg.nthreads_max; i++) {
        if (cfg.test_kind != TEST_KIND_ALL) {
            memset(&tstats, '\0', sizeof(struct tstats));
            run_test(i, cfg.test_kind, &cfg, &tstats);
            print_test_stats(i, cfg.test_kind, &tstats);
            continue;
        }
        for (j = TEST_KIND_ALL + 1; j <= TEST_KIND_MAX; j++) {
            memset(&tstats, '\0', sizeof(struct tstats));
            run_test(i, j, &cfg, &tstats);
            print_test_stats(i, j, &tstats);
        }
    }
    exit(0);
}
