/*
 * Copyright (c) 2006-2020 Sippy Software, Inc., http://www.sippysoft.com
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

#if defined(LINUX_XXX) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config_pp.h"

#include "rtpp_cfg.h"
#include "rtpp_ssrc.h"
#include "rtpa_stats.h"
#include "rtpp_log.h"
#include "rtpp_mallocs.h"
#include "rtpp_types.h"
#include "rtpp_list.h"
#include "rtpp_log_obj.h"
#include "rtpp_acct_pipe.h"
#include "rtpp_acct.h"
#include "rtpp_acct_rtcp.h"
#include "rtpp_pcount.h"
#include "rtpp_time.h"
#include "rtpp_pcnts_strm.h"
#include "rtpp_stream.h"
#include "rtpp_pipe.h"
#include "rtpp_session.h"
#include "advanced/packet_observer.h"
#include "advanced/po_manager.h"
#define MODULE_IF_CODE
#include "rtpp_module.h"
#include "rtpp_module_acct.h"
#include "rtpp_module_wthr.h"
#include "rtpp_module_cplane.h"
#include "rtpp_module_if.h"
#include "rtpp_modman.h"
#include "rtpp_module_if_fin.h"
#include "rtpp_queue.h"
#include "rtpp_refcnt.h"
#include "rtpp_wi.h"
#include "rtpp_wi_apis.h"
#include "rtpp_wi_sgnl.h"
#include "rtpp_command_sub.h"
#include "commands/rpcpv1_ul.h"
#ifdef RTPP_CHECK_LEAKS
#include "rtpp_memdeb_internal.h"
#endif

struct rtpp_module_if_priv {
    struct rtpp_module_if pub;
    void *dmp;
    struct rtpp_minfo *mip;
    struct rtpp_module_priv *mpvt;
    struct rtpp_log *log;
    struct rtpp_modids ids;
    /* Privary version of the module's memdeb_p, store it here */
    /* just in case module screws it up                        */
    void *memdeb_p;
    char *mpath;
    int started;
};

static void rtpp_mif_dtor(struct rtpp_module_if_priv *);
static void rtpp_mif_run_acct(void *);
static int rtpp_mif_load(struct rtpp_module_if *, const struct rtpp_cfg *, struct rtpp_log *);
static int rtpp_mif_start(struct rtpp_module_if *, const struct rtpp_cfg *);
static void rtpp_mif_do_acct(struct rtpp_module_if *, struct rtpp_acct *);
static void rtpp_mif_do_acct_rtcp(struct rtpp_module_if *, struct rtpp_acct_rtcp *);
static int rtpp_mif_get_mconf(struct rtpp_module_if *, struct rtpp_module_conf **);
static int rtpp_mif_ul_subc_handle(const struct after_success_h_args *,
  const struct rtpp_subc_ctx *);
static int rtpp_mif_construct(struct rtpp_module_if *self, const struct rtpp_cfg *);
static void rtpp_mif_kaput(struct rtpp_module_if *self);

static const char *do_acct_aname = "do_acct";
static const char *do_acct_rtcp_aname = "do_acct_rtcp";

struct rtpp_module_if *
rtpp_module_if_ctor(const char *mpath)
{
    struct rtpp_module_if_priv *pvt;

    pvt = rtpp_rzmalloc(sizeof(struct rtpp_module_if_priv), PVT_RCOFFS(pvt));
    if (pvt == NULL) {
        goto e0;
    }
    pvt->mpath = strdup(mpath);
    if (pvt->mpath == NULL) {
        goto e1;
    }
    pvt->pub.load = &rtpp_mif_load;
    pvt->pub.construct = &rtpp_mif_construct;
    pvt->pub.do_acct = &rtpp_mif_do_acct;
    pvt->pub.do_acct_rtcp = &rtpp_mif_do_acct_rtcp;
    pvt->pub.start = &rtpp_mif_start;
    pvt->pub.get_mconf = &rtpp_mif_get_mconf;
    pvt->pub.ul_subc_handle = &rtpp_mif_ul_subc_handle;
    pvt->pub.kaput = &rtpp_mif_kaput;
    CALL_SMETHOD(pvt->pub.rcnt, attach, (rtpp_refcnt_dtor_t)&rtpp_mif_dtor,
      pvt);
    return ((&pvt->pub));

e1:
    RTPP_OBJ_DECREF(&(pvt->pub));
    free(pvt);
e0:
    return (NULL);
}

static int
packet_is_rtcp(struct po_mgr_pkt_ctx *pktx)
{

    if (pktx->strmp->pipe_type != PIPE_RTCP)
        return (0);
    return (1);
}

static void
acct_rtcp_enqueue(void *arg, const struct po_mgr_pkt_ctx *pktx)
{
    struct rtpp_module_if_priv *pvt;
    struct rtpp_acct_rtcp *rarp;

    pvt = (struct rtpp_module_if_priv *)arg;
    rarp = rtpp_acct_rtcp_ctor(pktx->sessp->call_id, pktx->pktp);
    if (rarp == NULL) {
        return;
    }
    rtpp_mif_do_acct_rtcp(&(pvt->pub), rarp);
}

static int
rtpp_mif_load(struct rtpp_module_if *self, const struct rtpp_cfg *cfsp, struct rtpp_log *log)
{
    struct rtpp_module_if_priv *pvt;
    const char *derr;

    PUB2PVT(self, pvt);
    pvt->dmp = dlopen(pvt->mpath, RTLD_NOW);
    if (pvt->dmp == NULL) {
        derr = dlerror();
        if (strstr(derr, pvt->mpath) == NULL) {
            RTPP_LOG(log, RTPP_LOG_ERR, "can't dlopen(%s): %s", pvt->mpath, derr);
        } else {
            RTPP_LOG(log, RTPP_LOG_ERR, "can't dlopen() module: %s", derr);
        }
        goto e1;
    }
    pvt->mip = dlsym(pvt->dmp, "rtpp_module");
    if (pvt->mip == NULL) {
        derr = dlerror();
        if (strstr(derr, pvt->mpath) == NULL) {
            RTPP_LOG(log, RTPP_LOG_ERR, "can't find 'rtpp_module' symbol in the %s"
              ": %s", pvt->mpath, derr);
        } else {
            RTPP_LOG(log, RTPP_LOG_ERR, "can't find 'rtpp_module' symbol: %s",
              derr);
        }
        goto e2;
    }
    if (!MI_VER_CHCK(pvt->mip)) {
        RTPP_LOG(log, RTPP_LOG_ERR, "incompatible API version in the %s, "
          "consider recompiling the module", pvt->mpath);
        goto e2;
    }

#if RTPP_CHECK_LEAKS
    if (pvt->mip->memdeb_p == NULL) {
        RTPP_LOG(log, RTPP_LOG_ERR, "memdeb pointer is NULL in the %s, "
          "trying to load non-debug module?", pvt->mpath);
        goto e2;
    }
    pvt->mip->_malloc = &rtpp_memdeb_malloc;
    pvt->mip->_zmalloc = &rtpp_zmalloc_memdeb;
    pvt->mip->_rzmalloc = &rtpp_rzmalloc_memdeb;
    pvt->mip->_free = &rtpp_memdeb_free;
    pvt->mip->_realloc = &rtpp_memdeb_realloc;
    pvt->mip->_strdup = &rtpp_memdeb_strdup;
    pvt->mip->_asprintf = &rtpp_memdeb_asprintf;
    pvt->mip->_vasprintf = &rtpp_memdeb_vasprintf;
    pvt->memdeb_p = rtpp_memdeb_init(false);
    if (pvt->memdeb_p == NULL) {
        goto e2;
    }
    rtpp_memdeb_setlog(pvt->memdeb_p, log);
    rtpp_memdeb_setname(pvt->memdeb_p, pvt->mip->descr.name);
    /* We make a copy, so that the module cannot screw us up */
    *pvt->mip->memdeb_p = pvt->memdeb_p;
#else
    if (pvt->mip->memdeb_p != NULL) {
        RTPP_LOG(log, RTPP_LOG_ERR, "memdeb pointer is not NULL in the %s, "
          "trying to load debug module?", pvt->mpath);
        goto e2;
    }
    pvt->mip->_malloc = &malloc;
    pvt->mip->_zmalloc = &rtpp_zmalloc;
    pvt->mip->_rzmalloc = &rtpp_rzmalloc;
    pvt->mip->_free = &free;
    pvt->mip->_realloc = &realloc;
    pvt->mip->_strdup = &strdup;
    pvt->mip->_asprintf = &asprintf;
    pvt->mip->_vasprintf = &vasprintf;
#endif
    pvt->mip->wthr.sigterm = rtpp_wi_malloc_sgnl(SIGTERM, NULL, 0);
    if (pvt->mip->wthr.sigterm == NULL) {
        goto e3;
    }
    pvt->mip->wthr.mod_q = rtpp_queue_init(RTPQ_SMALL_CB_LEN, "rtpp_module_if(%s)",
      pvt->mip->descr.name);
    if (pvt->mip->wthr.mod_q == NULL) {
        goto e4;
    }
    RTPP_OBJ_INCREF(log);
    pvt->mip->log = log;
    if (pvt->mip->aapi != NULL) {
        if (pvt->mip->aapi->on_session_end.func != NULL &&
          pvt->mip->aapi->on_session_end.argsize != rtpp_acct_OSIZE()) {
            RTPP_LOG(log, RTPP_LOG_ERR, "incompatible API version in the %s, "
              "consider recompiling the module", pvt->mpath);
            goto e5;
        }
        if (pvt->mip->aapi->on_rtcp_rcvd.func != NULL &&
          pvt->mip->aapi->on_rtcp_rcvd.argsize != rtpp_acct_rtcp_OSIZE()) {
            RTPP_LOG(log, RTPP_LOG_ERR, "incompatible API version in the %s, "
              "consider recompiling the module", pvt->mpath);
            goto e5;
        }
        self->has.do_acct = (pvt->mip->aapi->on_session_end.func != NULL);
    }
    self->has.ul_subc_h = (pvt->mip->capi != NULL &&
      pvt->mip->capi->ul_subc_handle != NULL);
    pvt->ids.instance_id = CALL_METHOD(cfsp->modules_cf, get_next_id,
      pvt->mip->descr.module_id);
    pvt->mip->ids = self->ids = &pvt->ids;
    pvt->mip->module_rcnt = self->rcnt;
    self->descr = &(pvt->mip->descr);

    return (0);
e5:
    RTPP_OBJ_DECREF(pvt->mip->log);
    rtpp_queue_destroy(pvt->mip->wthr.mod_q);
    pvt->mip->wthr.mod_q = NULL;
#if RTPP_CHECK_LEAKS
    if (rtpp_memdeb_dumpstats(pvt->memdeb_p, 1) != 0) {
        RTPP_LOG(log, RTPP_LOG_ERR, "module '%s' leaked memory in the failed "
          "constructor", pvt->mip->descr.name);
    }
#endif
e4:
    CALL_METHOD(pvt->mip->wthr.sigterm, dtor);
    pvt->mip->wthr.sigterm = NULL;
e3:
#if RTPP_CHECK_LEAKS
    rtpp_memdeb_dtor(pvt->memdeb_p);
#endif
e2:
    dlclose(pvt->dmp);
    pvt->mip = NULL;
e1:
    return (-1);
}

static void
rtpp_mif_dtor(struct rtpp_module_if_priv *pvt)
{

    if (pvt->dmp != NULL && pvt->mip != NULL) {
        if (pvt->started != 0) {
            /* First, stop the worker thread */
            rtpp_queue_put_item(pvt->mip->wthr.sigterm, pvt->mip->wthr.mod_q);
        } else if (pvt->mip->wthr.sigterm != NULL) {
            CALL_METHOD(pvt->mip->wthr.sigterm, dtor);
        }
    }
}

static void
rtpp_mif_kaput(struct rtpp_module_if *self)
{
    struct rtpp_module_if_priv *pvt;

    PUB2PVT(self, pvt);

    if (pvt->dmp != NULL) {
        if (pvt->started != 0) {
            /* First, wait for worker thread to terminate */
            pthread_join(pvt->mip->wthr.thread_id, NULL);
        }
        rtpp_module_if_fin(&(pvt->pub));
        if (pvt->mip != NULL) {
            if (pvt->mip->wthr.mod_q != NULL)
                rtpp_queue_destroy(pvt->mip->wthr.mod_q);
            /* Then run module destructor (if any) */
            if (pvt->mip->proc.dtor != NULL && pvt->mpvt != NULL) {
                pvt->mip->proc.dtor(pvt->mpvt);
            }
            RTPP_OBJ_DECREF(pvt->mip->log);

#if RTPP_CHECK_LEAKS
            /* Check if module leaked any mem */
            if (rtpp_memdeb_dumpstats(pvt->memdeb_p, 1) != 0) {
                RTPP_LOG(pvt->mip->log, RTPP_LOG_ERR, "module '%s' leaked memory after "
                  "destruction", pvt->mip->descr.name);
            }
            rtpp_memdeb_dtor(pvt->memdeb_p);
#endif
            /* Unload and free everything */
            dlclose(pvt->dmp);
        }
    }
    free(pvt->mpath);
    free(pvt);
}

static void
rtpp_mif_run_acct(void *argp)
{
    struct rtpp_module_if_priv *pvt;
    struct rtpp_wi *wi;
    int signum;
    const char *aname;
    const struct rtpp_acct_handlers *aap;

    pvt = (struct rtpp_module_if_priv *)argp;
    aap = pvt->mip->aapi;
    for (;;) {
        wi = rtpp_queue_get_item(pvt->mip->wthr.mod_q, 0);
        if (rtpp_wi_get_type(wi) == RTPP_WI_TYPE_SGNL) {
            signum = rtpp_wi_sgnl_get_signum(wi);
            CALL_METHOD(wi, dtor);
            if (signum == SIGTERM) {
                break;
            }
            continue;
        }
        aname = rtpp_wi_apis_getname(wi);
        if (aname == do_acct_aname) {
            struct rtpp_acct *rap;

            rtpp_wi_apis_getnamearg(wi, (void **)&rap, sizeof(rap));
            if (aap->on_session_end.func != NULL)
                aap->on_session_end.func(pvt->mpvt, rap);
            RTPP_OBJ_DECREF(rap);
        }
        if (aname == do_acct_rtcp_aname) {
            struct rtpp_acct_rtcp *rapr;

            rtpp_wi_apis_getnamearg(wi, (void **)&rapr, sizeof(rapr));
            if (aap->on_rtcp_rcvd.func != NULL)
                aap->on_rtcp_rcvd.func(pvt->mpvt, rapr);
            RTPP_OBJ_DECREF(rapr);
        }
        CALL_METHOD(wi, dtor);
    }
}

static void
rtpp_mif_do_acct(struct rtpp_module_if *self, struct rtpp_acct *acct)
{
    struct rtpp_module_if_priv *pvt;
    struct rtpp_wi *wi;

    PUB2PVT(self, pvt);
    wi = rtpp_wi_malloc_apis(do_acct_aname, &acct, sizeof(acct));
    if (wi == NULL) {
        RTPP_LOG(pvt->mip->log, RTPP_LOG_ERR, "module '%s': cannot allocate "
          "memory", pvt->mip->descr.name);
        return;
    }
    RTPP_OBJ_INCREF(acct);
    rtpp_queue_put_item(wi, pvt->mip->wthr.mod_q);
}

static void
rtpp_mif_do_acct_rtcp(struct rtpp_module_if *self, struct rtpp_acct_rtcp *acct)
{
    struct rtpp_module_if_priv *pvt;
    struct rtpp_wi *wi;

    PUB2PVT(self, pvt);
    wi = rtpp_wi_malloc_apis(do_acct_rtcp_aname, &acct, sizeof(acct));
    if (wi == NULL) {
        RTPP_LOG(pvt->mip->log, RTPP_LOG_ERR, "module '%s': cannot allocate "
          "memory", pvt->mip->descr.name);
        RTPP_OBJ_DECREF(acct);
        return;
    }
    rtpp_queue_put_item(wi, pvt->mip->wthr.mod_q);
}

#define PTH_CB(x) ((void *(*)(void *))(x))

static int
rtpp_mif_construct(struct rtpp_module_if *self, const struct rtpp_cfg *cfsp)
{
    struct rtpp_module_if_priv *pvt;

    PUB2PVT(self, pvt);
    if (pvt->mip->proc.ctor != NULL) {
        pvt->mpvt = pvt->mip->proc.ctor(cfsp);
        if (pvt->mpvt == NULL) {
            RTPP_LOG(pvt->mip->log, RTPP_LOG_ERR, "module '%s' failed to initialize",
              pvt->mip->descr.name);
            return (-1);
        }
    }
    if (pvt->mip->proc.config != NULL) {
        if (pvt->mip->proc.config(pvt->mpvt) != 0) {
            RTPP_LOG(pvt->mip->log, RTPP_LOG_ERR, "%p->config() method has failed: %s",
              self, pvt->mip->descr.name);
            if (pvt->mip->proc.dtor != NULL) {
                pvt->mip->proc.dtor(pvt->mpvt);
            }
            return (-1);
        }
    }
    return (0);
}

static int
rtpp_mif_start(struct rtpp_module_if *self, const struct rtpp_cfg *cfsp)
{
    struct rtpp_module_if_priv *pvt;

    PUB2PVT(self, pvt);
    if (pvt->mip->aapi == NULL && pvt->mip->wapi == NULL)
        return (0);
    if (pvt->mip->aapi != NULL) {
        if (pvt->mip->aapi->on_rtcp_rcvd.func != NULL) {
            struct packet_observer_if acct_rtcp_poi;

            acct_rtcp_poi.taste = packet_is_rtcp;
            acct_rtcp_poi.enqueue = acct_rtcp_enqueue;
            acct_rtcp_poi.arg = pvt;
            if (CALL_METHOD(cfsp->observers, reg, &acct_rtcp_poi) < 0)
                return (-1);
        }
        if (pthread_create(&pvt->mip->wthr.thread_id, NULL,
          PTH_CB(&rtpp_mif_run_acct), pvt) != 0) {
            return (-1);
        }
    } else {
        pvt->mip->wthr.mpvt = pvt->mpvt;
        if (pthread_create(&pvt->mip->wthr.thread_id, NULL,
          PTH_CB(pvt->mip->wapi->main_thread), &pvt->mip->wthr) != 0) {
            return (-1);
        }
    }
    pvt->started = 1;
    return (0);
}

static int
rtpp_mif_get_mconf(struct rtpp_module_if *self, struct rtpp_module_conf **mcpp)
{
    struct rtpp_module_if_priv *pvt;
    struct rtpp_module_conf *rval;

    PUB2PVT(self, pvt);
    if (pvt->mip->proc.get_mconf == NULL) {
        *mcpp = NULL;
        return (0);
    }
    rval = pvt->mip->proc.get_mconf();
    if (rval == NULL) {
        return (-1);
    }
    *mcpp = rval;
    return (0);
}

static int
rtpp_mif_ul_subc_handle(const struct after_success_h_args *ashap,
  const struct rtpp_subc_ctx *ctxp)
{
    struct rtpp_module_if_priv *pvt;
    struct rtpp_module_if *self;

    self = ashap->stat;
    PUB2PVT(self, pvt);
    return (pvt->mip->capi->ul_subc_handle(pvt->mpvt, ctxp));
}
