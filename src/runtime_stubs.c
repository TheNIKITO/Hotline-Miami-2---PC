/* Stubs for PS4 system functions that Hotline Miami 2 imports and the runtime
 * does not implement yet. Online services report "unavailable", dialogs
 * finish immediately, NGS2 audio is silent. */
#include "runtime.h"
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#define SCE_OK 0
#define ERR_UNAVAILABLE ((int32_t)0x80550001)
#define ERR_INVALID_ARG ((int32_t)0x80020016)
#define DIALOG_STATUS_FINISHED 3

extern const char *runtime_symbol(const char *nid);
uintptr_t runtime_pngdec_resolve(const char *nid);
uintptr_t runtime_ngs2_resolve(const char *nid);

static ABI int32_t k_pthread_getthreadid(void) {
    static atomic_int next = 100;
    static _Thread_local int id = 0;
    if (!id) id = atomic_fetch_add(&next, 1);
    return id;
}
static ABI int32_t k_pthread_getschedparam(void *thread, int32_t *policy, int32_t *param) {
    (void)thread;
    if (!policy || !param) return ERR_INVALID_ARG;
    *policy = 1;
    *param = 700;
    return SCE_OK;
}
static ABI int32_t k_pthread_setschedparam(void *thread, int32_t policy, const int32_t *param) {
    (void)thread; (void)policy; (void)param;
    return SCE_OK;
}

static ABI int32_t stub_ok(void) { return SCE_OK; }
static ABI int32_t stub_unavailable(void) { return ERR_UNAVAILABLE; }
static ABI int32_t stub_dialog_finished(void) { return DIALOG_STATUS_FINISHED; }

static uint64_t fake_system, fake_rack, fake_voice;

static ABI int32_t ngs2_system_create(const void *opt, const void *alloc, uint64_t *out) {
    (void)opt; (void)alloc;
    if (!out) return ERR_INVALID_ARG;
    *out = (uint64_t)(uintptr_t)&fake_system;
    return SCE_OK;
}
static ABI int32_t ngs2_rack_create(uint64_t sys, uint32_t id, const void *opt,
                                    const void *alloc, uint64_t *out) {
    (void)sys; (void)id; (void)opt; (void)alloc;
    if (!out) return ERR_INVALID_ARG;
    *out = (uint64_t)(uintptr_t)&fake_rack;
    return SCE_OK;
}
static ABI int32_t ngs2_rack_get_voice(uint64_t rack, uint32_t id, uint64_t *out) {
    (void)rack; (void)id;
    if (!out) return ERR_INVALID_ARG;
    *out = (uint64_t)(uintptr_t)&fake_voice;
    return SCE_OK;
}
static ABI int32_t ngs2_voice_state_flags(uint64_t voice, uint32_t *flags) {
    (void)voice;
    if (flags) *flags = 0;
    return SCE_OK;
}

static const RuntimeExport exports[] = {
    {"scePthreadGetthreadid", k_pthread_getthreadid},
    {"scePthreadGetschedparam", k_pthread_getschedparam},
    {"scePthreadSetschedparam", k_pthread_setschedparam},
    {"sceNetCtlTerm", stub_ok},
    {"sceErrorDialogInitialize", stub_ok},
    {"sceErrorDialogOpen", stub_ok},
    {"sceErrorDialogTerminate", stub_ok},
    {"sceErrorDialogUpdateStatus", stub_dialog_finished},
    {"sceNgs2SystemCreateWithAllocator", ngs2_system_create},
    {"sceNgs2SystemDestroy", stub_ok},
    {"sceNgs2RackCreateWithAllocator", ngs2_rack_create},
    {"sceNgs2RackDestroy", stub_ok},
    {"sceNgs2RackGetVoiceHandle", ngs2_rack_get_voice},
    {"sceNgs2VoiceControl", stub_ok},
    {"sceNgs2VoiceGetStateFlags", ngs2_voice_state_flags},
    {"sceNgs2ParseWaveformData", stub_unavailable},
    {"sceNpScoreGetFriendsRanking", stub_unavailable},
    {"sceNpScoreGetRankingByNpId", stub_unavailable},
    {"sceNpTusCreateNpTitleCtx", stub_unavailable},
    {"sceNpTusCreateRequest", stub_unavailable},
    {"sceNpTusSetData", stub_unavailable},
    {"sceNpTusSetDataVUser", stub_unavailable},
    {"sceNpTusGetData", stub_unavailable},
    {"sceNpTusGetDataVUser", stub_unavailable},
    {"sceNpTusSetMultiSlotVariable", stub_unavailable},
    {"sceNpTusGetMultiSlotVariable", stub_unavailable},
    {"sceNpTusGetMultiSlotVariableVUser", stub_unavailable},
    {"sceNpTusAddAndGetVariable", stub_unavailable},
    {"sceNpTusAddAndGetVariableVUser", stub_unavailable},
    {"sceNpTusTryAndSetVariable", stub_unavailable},
    {"sceNpTusTryAndSetVariableVUser", stub_unavailable},
    {"sceNgs2_unk_i0VnXM", stub_ok},
    {"sceNpManager_unk_GpLQDN", stub_unavailable},
};

uintptr_t runtime_stubs_resolve(const char *nid) {
    uintptr_t r = runtime_ngs2_resolve(nid);
    if (!r) r = runtime_pngdec_resolve(nid);
    if (!r) r = RUNTIME_LOOKUP(exports, nid);
    if (r) return r;
    const char *s = runtime_symbol(nid);
    if (s && !strncmp(s, "unk_ok_", 7)) return (uintptr_t)stub_ok;
    if (s && !strncmp(s, "unk_err_", 8)) return (uintptr_t)stub_unavailable;
    return 0;
}
