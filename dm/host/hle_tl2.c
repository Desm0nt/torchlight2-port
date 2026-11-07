// [TL2] Torchlight II (Switch, Panic Button; SDK 8.2.0 sdk build 3ae34e68 = D3's): nn:: entry points its main imports that the D3 build
// does not serve under these overloads. Everything else comes from the D3 set (hle_d3.c: account / nfp / ldn / offline network) and the
// DOOM / D2R HLE. Found statically: main imports that would go to the native sdk and open a service session the host does not have.
#ifdef DM_GAME_TL2
#include "hle.h"
#include <string.h>
#include <stdbool.h>

// ---- audio: the SDK 8 AudioOut overloads without a buffer event (FMOD's output polls GetReleasedAudioOutBuffer); hle_audio.c binds no
// event when it is null
Result dm_audio_open_out(void *g, void *ev, const char *name, const void *param);
HLE_FORCE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPKcRKNS0_17AudioOutParameterE", "[TL2] audio out by name, no event -> hle_audio.c",
          Result, h_tl2_OpenAudioOut, void *out, const char *name, const void *param) { return dm_audio_open_out(out, NULL, name, param); }
HLE_FORCE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", "[TL2] default audio out, no event -> hle_audio.c",
          Result, h_tl2_OpenDefaultAudioOut, void *out, const void *param) { return dm_audio_open_out(out, NULL, NULL, param); }

// ---- hid: the single-state touch read (SDK 8 template TouchScreenState<16>, 0x290 bytes), the same touches as GetTouchScreenStates
int touch_fill(void *out, uint64_t now_ns);
HLE_FORCE("_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE", "[TL2] touch screen state: the front end's touches (hle_touch.c)",
          void, h_tl2_GetTouch, void *out) { memset(out, 0, 0x290); touch_fill(out, guest_ns()); }

// ---- settings: the operator== overload with the enum first (main 0x1bbdd70 at boot; the other order is in hle_doom.c), Horizon table
HLE_FORCE("_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE", "[TL2] settings: Language enum -> code (Horizon table), compare", bool, h_tl2_LangEq,
          const int *lang, const char *code) {
    static const char *const k[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
    if (!code || !lang || *lang < 0 || *lang >= (int)(sizeof k / sizeof k[0])) return false;
    return strncmp(code, k[*lang], 8) == 0;
}

// ---- account: the host's one local user exists
HLE_FORCE("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", "[TL2] the host's one local user exists", Result, h_tl2_UserExists, uint8_t *out, const void *uid) {
    (void)uid; *out = 1; return RES_OK;
}

// ---- fs: save data by program id (main+0x1335e4 at boot asks about its own save; others, read-only: none here). Same as hle_xc2.c.
#define TL2_PROGRAM_ID 0x010090400D366000ull
HLE_FORCE("_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE", "[TL2] fsp-srv; only this game's own save data exists",
          int, h_tl2_SaveExisting, uint64_t app, const void *uid) {
    (void)uid; LOG("fs::IsSaveDataExisting program %016llx -> %s", (unsigned long long)app, app == TL2_PROGRAM_ID ? "yes (this game)" : "no");
    return app == TL2_PROGRAM_ID;
}
HLE_FORCE("_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE", "[TL2] fsp-srv; another program's save data: none",
          Result, h_tl2_MountSaveRO, const char *name, uint64_t app, const void *uid) {
    (void)uid; LOG("fs::MountSaveDataReadOnly %s program %016llx: none on the host", name ? name : "?", (unsigned long long)app);
    return 0x7d402u;   // fs ResultTargetNotFound
}

// ---- ec: the shop applet (offline); the oe exit request section is in hle_d3.c
HLE_FORCE("_ZN2nn2ec30ShowShopApplicationInformationENS_13ApplicationIdE", "[TL2] shop applet: none (offline)", void, h_tl2_ShopApp, uint64_t app) {
    LOG("ec::ShowShopApplicationInformation %016llx (no shop, skipped)", (unsigned long long)app);
}

// ---- the rest of TL2's imports that nxnative (MIT, https://github.com/seedds/nxnative: dm/host/hle_offline.c, hle_audio.c, hle_account.c)
// serves and this tree did not: TL2's LAN co-op (ldn past Initialize, which hle_d3.c fails), nifm's SDK 8 overload, the socket overloads the
// DOOM set lacks (hle_net.c has the int / SockAddr ones), the AudioIn getters (microphone, no device) and AsyncContext's move
HLE_FORCE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", "[TL2] local wireless state event: bound, never fires", void, h_tl2_ldnAttach, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "[TL2] local wireless scan (cut): 0 found", Result, h_tl2_ldnScan,
          void *out, int *n, int max, const void *f, int ch) { (void)out; (void)max; (void)f; (void)ch; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", "[TL2] local wireless (cut)",
          Result, h_tl2_ldnCreate, const void *a, const void *b, const void *c, const void *d, int n, const void *e) { (void)a; (void)b; (void)c; (void)d; (void)n; (void)e; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", "[TL2] local wireless (cut)",
          Result, h_tl2_ldnConnect, const void *a, const void *b, const void *c, const void *d, int v, int o) { (void)a; (void)b; (void)c; (void)d; (void)v; (void)o; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", "[TL2] local wireless (cut)", Result, h_tl2_ldnNetInfo, void *o) { (void)o; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", "[TL2] local wireless (cut)", Result, h_tl2_ldnSecPrm, void *o) { (void)o; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn19GetDisconnectReasonEv", "[TL2] local wireless (cut): 0", int, h_tl2_ldnDisc, void) { return 0; }
HLE_FORCE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", "[TL2] local wireless (cut)", Result, h_tl2_ldnPolicy, int p) { (void)p; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", "[TL2] local wireless (cut)", Result, h_tl2_ldnReject, uint32_t a) { (void)a; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", "[TL2] local wireless (cut)", Result, h_tl2_ldnNetCfg, void *o) { (void)o; return RES_FAIL; }
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEPNS_6socket6InAddrES3_S3_S3_S3_", "[TL2] offline: no IP configuration", Result, h_tl2_nifmIpCfg,
          uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) {
    uint32_t *p[5] = {a, b, c, d, e}; for (int i = 0; i < 5; i++) if (p[i]) *p[i] = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", "[TL2] resolver error text", const char *, h_tl2_GaiStr, int e) { (void)e; return "network unavailable"; }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", "[TL2] offline (cut)", long, h_tl2_SendTo,
          int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return -1; }
HLE_FORCE("_ZN2nn6socket8ShutdownEiNS0_14ShutdownMethodE", "[TL2] offline (cut)", int, h_tl2_Shutdown, int s, int h) { (void)s; (void)h; return -1; }
HLE_FORCE("_ZN2nn5audio20GetAudioInSampleRateEPKNS0_7AudioInE", "[TL2] recording sample rate", int, h_tl2_aiRate, const void *g) { (void)g; return 48000; }
HLE_FORCE("_ZN2nn5audio22GetAudioInChannelCountEPKNS0_7AudioInE", "[TL2] recording channel count", int, h_tl2_aiCh, const void *g) { (void)g; return 2; }
HLE_FORCE("_ZN2nn5audio22GetAudioInSampleFormatEPKNS0_7AudioInE", "[TL2] recording sample format: PCM16", int, h_tl2_aiFmt, const void *g) { (void)g; return 2; }
HLE_FORCE("_ZN2nn7account12AsyncContextaSEOS1_", "[TL2] async context move assignment; the object holds no host state", void *, h_tl2_AcMove, void *self, void *other) { (void)other; return self; }
#endif
