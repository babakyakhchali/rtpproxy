/* Auto-generated by genfincode_stat.sh - DO NOT EDIT! */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#define RTPP_FINCODE
#include "rtpp_types.h"
#include "rtpp_debug.h"
#include "rtpp_pearson_perfect.h"
#include "rtpp_pearson_perfect_fin.h"
static void rtpp_pearson_perfect_hash_fin(void *pub) {
    fprintf(stderr, "Method rtpp_pearson_perfect@%p::hash (rtpp_pearson_perfect_hash) is invoked after destruction\x0a", pub);
    RTPP_AUTOTRAP();
}
static const struct rtpp_pearson_perfect_smethods rtpp_pearson_perfect_smethods_fin = {
    .hash = (rtpp_pearson_perfect_hash_t)&rtpp_pearson_perfect_hash_fin,
};
void rtpp_pearson_perfect_fin(struct rtpp_pearson_perfect *pub) {
    RTPP_DBG_ASSERT(pub->smethods->hash != (rtpp_pearson_perfect_hash_t)NULL);
    RTPP_DBG_ASSERT(pub->smethods != &rtpp_pearson_perfect_smethods_fin &&
      pub->smethods != NULL);
    pub->smethods = &rtpp_pearson_perfect_smethods_fin;
}
#if defined(RTPP_FINTEST)
#include <assert.h>
#include <stddef.h>
#include "rtpp_mallocs.h"
#include "rtpp_refcnt.h"
#include "rtpp_linker_set.h"
#define CALL_TFIN(pub, fn) ((void (*)(typeof(pub)))((pub)->smethods->fn))(pub)

void
rtpp_pearson_perfect_fintest()
{
    int naborts_s;

    struct {
        struct rtpp_pearson_perfect pub;
    } *tp;

    naborts_s = _naborts;
    tp = rtpp_rzmalloc(sizeof(*tp), offsetof(typeof(*tp), pub.rcnt));
    assert(tp != NULL);
    assert(tp->pub.rcnt != NULL);
    static const struct rtpp_pearson_perfect_smethods dummy = {
        .hash = (rtpp_pearson_perfect_hash_t)((void *)0x1),
    };
    tp->pub.smethods = &dummy;
    CALL_SMETHOD(tp->pub.rcnt, attach, (rtpp_refcnt_dtor_t)&rtpp_pearson_perfect_fin,
      &tp->pub);
    RTPP_OBJ_DECREF(&(tp->pub));
    CALL_TFIN(&tp->pub, hash);
    assert((_naborts - naborts_s) == 1);
}
const static void *_rtpp_pearson_perfect_ftp = (void *)&rtpp_pearson_perfect_fintest;
DATA_SET(rtpp_fintests, _rtpp_pearson_perfect_ftp);
#endif /* RTPP_FINTEST */