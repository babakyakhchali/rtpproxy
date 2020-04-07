/*
 * Copyright (c) 2019 Razvan Crainea <razvan@opensips.org>
 * Copyright (c) 2019 Maxim Sobolev <sobomax@sippysoft.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "config_pp.h"

#include "rtpp_log.h"
#include "rtpp_mallocs.h"
#include "rtpp_types.h"
#include "rtpp_refcnt.h"
#include "rtpp_wi.h"
#include "rtpp_wi_sgnl.h"
#include "rtpp_wi_data.h"
#include "rtpp_queue.h"
#include "rtpp_stream.h"
#include "rtpp_pipe.h"
#include "rtp.h"
#include "rtp_packet.h"
#include "rtpp_log_obj.h"
#include "rtpp_session.h"
#include "rtpp_notify.h"
#include "rtpp_command_private.h"
#include "rtpp_timeout_data.h"
#include "rtpp_util.h"

#include "advanced/packet_observer.h"
#include "advanced/po_manager.h"
#include "../modules/catch_dtmf/rtpp_catch_dtmf.h"

struct rtpp_catch_dtmf_pvt {
    struct rtpp_catch_dtmf pub;
    struct rtpp_wi *sigterm;
    struct rtpp_queue *q;
    pthread_t worker;
    struct rtpp_log *log;
    struct rtpp_notify *notifier;
};

struct catch_dtmf_einfo {
    int pending;
    char digit;
    uint32_t ts;
    uint16_t duration;
};

#define EINFO_HST_DPTH 4

struct catch_dtmf_edata {
    struct rtpp_refcnt *rcnt;
    struct catch_dtmf_einfo hst[EINFO_HST_DPTH];
    int hst_next;
    enum rtpp_stream_side side;
};

struct catch_dtmf_stream_cfg {
    atomic_int pt;
    struct catch_dtmf_edata *edata;
    const struct rtpp_timeout_data *rtdp;
};

#define PUB2PVT(pubp, pvtp) \
    (pvtp) = (typeof(pvtp))((char *)(pubp) - offsetof(typeof(*(pvtp)), pub))

static void
rtpp_catch_dtmf_edata_dtor(void *p)
{
    free(p);
}

static struct catch_dtmf_edata *
rtpp_catch_dtmf_edata_ctor(enum rtpp_stream_side side)
{
    struct catch_dtmf_edata *edata;
    struct rtpp_refcnt *rcnt;
    int i;

    edata = rtpp_rzmalloc(sizeof(*edata), &rcnt);
    if (edata == NULL) {
        goto e0;
    }
    edata->rcnt = rcnt;
    for (i = 0; i < EINFO_HST_DPTH; i++) {
        edata->hst[i].digit = -1;
    }
    edata->side = side;
    CALL_SMETHOD(edata->rcnt, attach, rtpp_catch_dtmf_edata_dtor, edata);
    return edata;
e0:
    return (NULL);
}

static void
rtpp_catch_dtmf_dtor(struct rtpp_catch_dtmf_pvt *pvt)
{

    rtpp_queue_put_item(pvt->sigterm, pvt->q);
    pthread_join(pvt->worker, NULL);
    rtpp_queue_destroy(pvt->q);
    CALL_METHOD(pvt->sigterm, dtor);
    CALL_SMETHOD(pvt->log->rcnt, decref);
    free(pvt);
}

struct wipkt {
    const struct rtp_packet *pkt;
    struct catch_dtmf_edata *edata;
    const struct rtpp_timeout_data *rtdp;
};

struct rtp_dtmf_event {
    unsigned int event:8;        /* event_id - digit */
#if BYTE_ORDER == BIG_ENDIAN
    unsigned int end:1;            /* indicates the end of the event */
    unsigned int res:1;            /* reserved - should be 0 */
    unsigned int volume:6;        /* volume */
#else
    unsigned int volume:6;        /* volume */
    unsigned int res:1;            /* reserved - should be 0 */
    unsigned int end:1;            /* indicates the end of the event */
#endif
    unsigned int duration:16;    /* duration */
} __attribute__((__packed__));

#define RTPP_MAX_NOTIFY_BUF 512
static const char *notyfy_type = "DTMF";

static void
rtpp_catch_dtmf_worker(void *arg)
{
    struct rtpp_catch_dtmf_pvt *pvt;
    struct rtpp_wi *wi;
    struct wipkt *wip;
    char buf[RTPP_MAX_NOTIFY_BUF];
    const char dtmf_events[] = "0123456789*#ABCD ";
    struct catch_dtmf_einfo *eip, ei;
    int i;

    pvt = (struct rtpp_catch_dtmf_pvt *)arg;
    for (;;) {
        wi = rtpp_queue_get_item(pvt->q, 0);
        if (wi == pvt->sigterm) {
            break;
        }
        wip = rtpp_wi_data_get_ptr(wi, sizeof(*wip), sizeof(*wip));

        struct rtp_dtmf_event *dtmf =
            (struct rtp_dtmf_event *)(wip->pkt->data.buf + sizeof(rtp_hdr_t));
        if (dtmf->event > sizeof(dtmf_events) - 1) {
            RTPP_LOG(pvt->log, RTPP_LOG_DBUG, "Unhandled DTMF event %u", dtmf->event);
            goto skip;
        }
        ei.digit = dtmf_events[dtmf->event];
        ei.ts = ntohl(wip->pkt->data.header.ts);
        ei.duration = ntohs(dtmf->duration);
        eip = NULL;
        for (i = 1; i <= EINFO_HST_DPTH; i++) {
            int j = wip->edata->hst_next - i;
            if (j < 0)
                j = EINFO_HST_DPTH + j;
            if (wip->edata->hst[j].ts == ei.ts && wip->edata->hst[j].digit != -1) {
                eip = &wip->edata->hst[j];
                break;
            }
        }

        if (eip == NULL) {
            /* this is a new event */
            eip = &wip->edata->hst[wip->edata->hst_next];
            eip->ts = ei.ts;
            eip->pending = 1;
            eip->digit = ei.digit;
            eip->duration = ei.duration;
            wip->edata->hst_next += 1;
            if (wip->edata->hst_next == EINFO_HST_DPTH)
                wip->edata->hst_next = 0;
            goto skip;
        }
        if (!eip->pending) {
            if (!dtmf->end && eip->duration <= ei.duration)
                RTPP_LOG(pvt->log, RTPP_LOG_WARN, "Received DTMF for %c without "
                        "start %d", ei.digit, eip->pending);
            goto skip;
        }

        if (ei.digit != eip->digit) {
            RTPP_LOG(pvt->log, RTPP_LOG_WARN, "Received DTMF for %c "
                    "while processing %c", ei.digit, eip->digit);
            goto skip;
        }
        if (eip->duration < ei.duration)
            eip->duration = ei.duration;

        if (!dtmf->end)
            goto skip;
        /* we received the end of the DTMF */
        /* all good - send the notification */
        eip->pending = 0;
        snprintf(buf, RTPP_MAX_NOTIFY_BUF, "%s %c %u %u %d",
          wip->rtdp->notify_tag, ei.digit, dtmf->volume, eip->duration,
          (wip->edata->side == RTPP_SSIDE_CALLER) ? 0 : 1);
        CALL_METHOD(pvt->notifier, schedule, wip->rtdp->notify_target, buf,
          notyfy_type);

skip:
        CALL_SMETHOD(wip->edata->rcnt, decref);
        CALL_SMETHOD(wip->rtdp->rcnt, decref);
        CALL_SMETHOD(wip->pkt->rcnt, decref);
        CALL_METHOD(wi, dtor);
    }
}

static int
rtpp_catch_dtmf_handle_command(struct rtpp_catch_dtmf *pub, const struct rtpp_subc_ctx *ctxp)
{
    struct rtpp_catch_dtmf_pvt *pvt;
    struct catch_dtmf_stream_cfg *rtps_c;
    void *rtps_c_prev;
    int len;
    int new_pt = 101;
    int old_pt = -1;
    char *dtmf_tag;

    rtps_c_prev = NULL;

    PUB2PVT(pub, pvt);
    if (ctxp->sessp->timeout_data == NULL) {
        RTPP_LOG(pvt->log, RTPP_LOG_ERR, "notification is not enabled (sp=%p)",
          ctxp->sessp);
        return (-1);
    }

    rtps_c = atomic_load(&(ctxp->strmp->catch_dtmf_data));
    if (rtps_c == NULL) {

        if (ctxp->subc_args->c < 2) {
            RTPP_LOG(pvt->log, RTPP_LOG_DBUG, "no tag specified (sp=%p)",
              ctxp->sessp);
            return (-1);
        }

        dtmf_tag = ctxp->subc_args->v[1];
        len = url_unquote((uint8_t *)dtmf_tag, strlen(dtmf_tag));
        if (len == -1) {
            RTPP_LOG(pvt->log, RTPP_LOG_ERR, "syntax error: invalid URL "
              "encoding");
            return (-1);
        }
        dtmf_tag[len] = '\0';

        if (ctxp->subc_args->c > 2) {
            if (atoi_saferange(ctxp->subc_args->v[2], &new_pt, 0, 127)) {
                RTPP_LOG(pvt->log, RTPP_LOG_ERR, "syntax error: invalid "
                  "payload type: %s", ctxp->subc_args->v[2]);
                return (-1);
            }
        }

        rtps_c = rtpp_zmalloc(sizeof(*rtps_c));
        if (rtps_c == NULL) {
            return (-1);
        }
        rtps_c->rtdp = rtpp_timeout_data_ctor(
                ctxp->sessp->timeout_data->notify_target, dtmf_tag);
        atomic_init(&(rtps_c->pt), new_pt);
        rtps_c->edata = rtpp_catch_dtmf_edata_ctor(ctxp->strmp->side);
        if (!rtps_c->edata) {
            RTPP_LOG(pvt->log, RTPP_LOG_ERR, "cannot create edata (sp=%p)",
              ctxp->strmp);
            free(rtps_c);
            return (-1);
        }
        if (!atomic_compare_exchange_strong(&(ctxp->strmp->catch_dtmf_data),
          &rtps_c_prev, rtps_c)) {
            CALL_SMETHOD(rtps_c->edata->rcnt, decref);
            free(rtps_c);
            rtps_c = (typeof(rtps_c))rtps_c_prev;
            old_pt = atomic_exchange(&(rtps_c->pt), new_pt);
            return (-1);
        }
    }
    if (old_pt != -1)
        RTPP_LOG(pvt->log, RTPP_LOG_DBUG, "sp=%p, pt=%d->%d",
          ctxp->strmp, old_pt, new_pt);
    return (0);
}

static int
rtp_packet_is_dtmf(struct po_mgr_pkt_ctx *pktx)
{
    struct catch_dtmf_stream_cfg *rtps_c;

    if (pktx->strmp->pipe_type != PIPE_RTP)
        return (0);
    rtps_c = atomic_load(&(pktx->strmp->catch_dtmf_data));
    if (rtps_c == NULL)
        return (0);
    if (atomic_load(&(rtps_c->pt)) != pktx->pktp->data.header.pt)
        return (0);
    pktx->auxp = rtps_c;

    return (1);
}

static void
rtpp_catch_dtmf_enqueue(void *arg, const struct po_mgr_pkt_ctx *pktx)
{
    struct rtpp_catch_dtmf_pvt *pvt;
    struct rtpp_wi *wi;
    struct wipkt *wip;
    struct catch_dtmf_stream_cfg *rtps_c;

    pvt = (struct rtpp_catch_dtmf_pvt *)arg;
    rtps_c = (struct catch_dtmf_stream_cfg *)pktx->auxp;
    /* we duplicate the tag to make sure it does not vanish */
    wi = rtpp_wi_malloc_udata((void **)&wip, sizeof(struct wipkt));
    if (wi == NULL)
        return;
    CALL_SMETHOD(pktx->pktp->rcnt, incref);
    /* we need to duplicate the tag and state */
    wip->edata = rtps_c->edata;
    CALL_SMETHOD(rtps_c->edata->rcnt, incref);
    wip->pkt = pktx->pktp;
    CALL_SMETHOD(rtps_c->rtdp->rcnt, incref);
    wip->rtdp = rtps_c->rtdp;
    rtpp_queue_put_item(wi, pvt->q);
}

struct rtpp_catch_dtmf *
rtpp_catch_dtmf_ctor(struct rtpp_log *log, struct po_manager *pomp,
  struct rtpp_notify *rnp)
{
    struct rtpp_catch_dtmf_pvt *pvt;
    struct rtpp_refcnt *rcnt;
    struct packet_observer_if dtmf_poi;

    pvt = rtpp_rzmalloc(sizeof(*pvt), &rcnt);
    if (pvt == NULL)
        goto e0;
    pvt->pub.rcnt = rcnt;
    pvt->pub.handle_command = rtpp_catch_dtmf_handle_command;
    pvt->sigterm = rtpp_wi_malloc_sgnl(SIGTERM, NULL, 0);
    if (pvt->sigterm == NULL)
        goto e1;
    pvt->q = rtpp_queue_init(1, "rtpp_catch_dtmf(%p)", pvt);
    pvt->notifier = rnp;
    if (pvt->q == NULL)
        goto e2;
    /*
     * Assign log here so it's usable by the worker, but incref later so that
     * we don't have to worry about cleaning it out on error.
     */
    pvt->log = log;
    if (pthread_create(&pvt->worker, NULL, (void *(*)(void *))rtpp_catch_dtmf_worker, pvt) != 0)
        goto e3;
    memset(&dtmf_poi, '\0', sizeof(dtmf_poi));
    dtmf_poi.taste = rtp_packet_is_dtmf;
    dtmf_poi.enqueue = rtpp_catch_dtmf_enqueue;
    dtmf_poi.arg = pvt;
    if (CALL_METHOD(pomp, reg, &dtmf_poi) < 0)
        goto e4;
    CALL_SMETHOD(pvt->pub.rcnt, attach, (rtpp_refcnt_dtor_t)&rtpp_catch_dtmf_dtor,
      pvt);
    CALL_SMETHOD(pvt->log->rcnt, incref);
    return (&(pvt->pub));
e4:
    rtpp_queue_put_item(pvt->sigterm, pvt->q);
    pthread_join(pvt->worker, NULL);
e3:
    rtpp_queue_destroy(pvt->q);
e2:
    CALL_METHOD(pvt->sigterm, dtor);
e1:
    CALL_SMETHOD(pvt->pub.rcnt, decref);
    free(pvt);
e0:
    return (NULL);
}

void
catch_dtmf_data_free(void *p)
{
    struct catch_dtmf_stream_cfg *rtps_c;

    rtps_c = (struct catch_dtmf_stream_cfg *)p;
    CALL_SMETHOD(rtps_c->rtdp->rcnt, decref);
    CALL_SMETHOD(rtps_c->edata->rcnt, decref);
    free(rtps_c);
}