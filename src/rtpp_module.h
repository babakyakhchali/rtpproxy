/*
 * Copyright (c) 2016-2018 Sippy Software, Inc., http://www.sippysoft.com
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

#ifndef _RTPP_MODULE_H
#define _RTPP_MODULE_H

#define MODULE_API_REVISION 11

#include "rtpp_codeptr.h"

struct rtpp_cfg;
struct rtpp_module_priv;
struct rtpp_module_conf;
struct rtpp_acct_handlers;
struct rtpp_cplane_handlers;
struct rtpp_wthr_handlers;

#if !defined(MODULE_IF_CODE)
#include <sys/types.h>
#include "rtpp_types.h"
#endif

DEFINE_RAW_METHOD(rtpp_module_ctor, struct rtpp_module_priv *,
  const struct rtpp_cfg *);
DEFINE_RAW_METHOD(rtpp_module_get_mconf, struct rtpp_module_conf *, void);
DEFINE_METHOD(rtpp_module_priv, rtpp_module_config, int);
DEFINE_METHOD(rtpp_module_priv, rtpp_module_dtor, void);

#include <stdarg.h>

#if defined(RTPP_CHECK_LEAKS)
DEFINE_RAW_METHOD(rtpp_module_malloc, void *, size_t,  void *, HERETYPE);
DEFINE_RAW_METHOD(rtpp_module_zmalloc, void *, size_t,  void *, HERETYPE);
DEFINE_RAW_METHOD(rtpp_module_rzmalloc, void *, size_t, size_t, void *, HERETYPE);
DEFINE_RAW_METHOD(rtpp_module_free, void, void *, void *, HERETYPE);
DEFINE_RAW_METHOD(rtpp_module_realloc, void *, void *, size_t,   void *,
  HERETYPE);
DEFINE_RAW_METHOD(rtpp_module_strdup, char *, const char *,  void *,
  HERETYPE);
DEFINE_RAW_METHOD(rtpp_module_asprintf, int, char **, const char *,
   void *, HERETYPE, ...) __attribute__ ((format (printf, 2, 5)));;
DEFINE_RAW_METHOD(rtpp_module_vasprintf, int, char **, const char *,
   void *, HERETYPE, va_list);;
#else
DEFINE_RAW_METHOD(rtpp_module_malloc, void *, size_t);
DEFINE_RAW_METHOD(rtpp_module_zmalloc, void *, size_t);
DEFINE_RAW_METHOD(rtpp_module_rzmalloc, void *, size_t, size_t);
DEFINE_RAW_METHOD(rtpp_module_free, void, void *);
DEFINE_RAW_METHOD(rtpp_module_realloc, void *, void *, size_t);
DEFINE_RAW_METHOD(rtpp_module_strdup, char *, const char *);
DEFINE_RAW_METHOD(rtpp_module_asprintf, int, char **, const char *, ...)
  __attribute__ ((format (printf, 2, 3)));;
DEFINE_RAW_METHOD(rtpp_module_vasprintf, int, char **, const char *, va_list);
#endif

#if !defined(MODULE_IF_CODE)

#if defined(RTPP_CHECK_LEAKS)
#define _MMDEB *(rtpp_module.memdeb_p)

#define mod_malloc(n) rtpp_module._malloc((n), _MMDEB, \
  HEREVAL)
#define mod_zmalloc(n) rtpp_module._zmalloc((n), _MMDEB, \
  HEREVAL)
#define mod_rzmalloc(n, m) rtpp_module._rzmalloc((n), (m), _MMDEB, \
  HEREVAL)
#define mod_free(p) rtpp_module._free((p), _MMDEB, \
  HEREVAL)
#define mod_realloc(p,n) rtpp_module._realloc((p), (n), _MMDEB, \
  HEREVAL)
#define mod_strdup(p) rtpp_module._strdup((p), _MMDEB, \
  HEREVAL)
#define mod_asprintf(pp, fmt, args...) rtpp_module._asprintf((pp), (fmt), \
  _MMDEB, HEREVAL, ## args)
#define mod_vasprintf(pp, fmt, vl) rtpp_module._vasprintf((pp), (fmt), \
  _MMDEB, HEREVAL, (vl))
#else
#define mod_malloc(n) rtpp_module._malloc((n))
#define mod_zmalloc(n) rtpp_module._zmalloc((n))
#define mod_rzmalloc(n, m) rtpp_module._rzmalloc((n), m)
#define mod_free(p) rtpp_module._free((p))
#define mod_realloc(p,n) rtpp_module._realloc((p), (n))
#define mod_strdup(p) rtpp_module._strdup((p))
#define mod_asprintf(pp, fmt, args...) rtpp_module._asprintf((pp), (fmt), ## args)
#define mod_vasprintf(pp, fmt, vl) rtpp_module._vasprintf((pp), (fmt), (vl))
#endif
#endif /* !MODULE_IF_CODE */

#define mod_log(args...) CALL_METHOD(rtpp_module.log, genwrite, __FUNCTION__, \
  __LINE__, ## args)
#define mod_elog(args...) CALL_METHOD(rtpp_module.log, errwrite, __FUNCTION__, \
  __LINE__, ## args)

struct api_version {
    int rev;
    size_t mi_size;
    const char *build;
};

struct rtpp_mdescr {
    struct api_version ver;
    const char *name;
    const char *author;
    const char *copyright;
    const char *maintainer;
    unsigned int module_id;
};

struct rtpp_mhandlers {
    rtpp_module_ctor_t ctor;
    rtpp_module_dtor_t dtor;
    rtpp_module_get_mconf_t get_mconf;
    rtpp_module_config_t config;
};

struct rtpp_wthrdata {
    struct rtpp_wi *sigterm;
    pthread_t thread_id;
    struct rtpp_queue *mod_q;
    struct rtpp_module_priv *mpvt;
};

struct rtpp_modids {
    unsigned int instance_id;
    unsigned int module_idx;
};

struct rtpp_minfo {
    /* Upper half, filled by the module */
    struct rtpp_mdescr descr;
    struct rtpp_mhandlers proc;
    const struct rtpp_acct_handlers *aapi;
    const struct rtpp_cplane_handlers *capi;
    const struct rtpp_wthr_handlers *wapi;
    /* Lower half, filled by the core */
    const struct rtpp_modids *ids;
    rtpp_module_malloc_t _malloc;
    rtpp_module_zmalloc_t _zmalloc;
    rtpp_module_rzmalloc_t _rzmalloc;
    rtpp_module_free_t _free;
    rtpp_module_realloc_t _realloc;
    rtpp_module_strdup_t _strdup;
    rtpp_module_asprintf_t _asprintf;
    rtpp_module_vasprintf_t _vasprintf;
    void **memdeb_p;
    struct rtpp_log *log;
    struct rtpp_wthrdata wthr;
};

extern struct rtpp_minfo rtpp_module;

#define MI_VER_INIT() { \
    .rev = MODULE_API_REVISION, \
    .mi_size = sizeof(rtpp_module), \
    .build = RTPP_SW_VERSION}
#define MI_VER_CHCK(sptr) ( \
  (sptr)->descr.ver.rev == MODULE_API_REVISION && \
  (sptr)->descr.ver.mi_size == sizeof(struct rtpp_minfo) && \
  strcmp((sptr)->descr.ver.build, RTPP_SW_VERSION) == 0)

#endif /* _RTPP_MODULE_H */
