// [TL2] Torchlight II (Switch, Panic Button; SDK 8.2.0 sdk build 3ae34e68 = D3's): nn:: entry points its main imports that the D3 build
// does not serve under these overloads. Everything else comes from the D3 set (hle_d3.c: account / nfp / ldn / offline network) and the
// DOOM / D2R HLE. Found statically: main imports that would go to the native sdk and open a service session the host does not have.
#ifdef DM_GAME_TL2
#include "hle.h"
#include <string.h>

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

// ---- account: the host's one local user exists
HLE_FORCE("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", "[TL2] the host's one local user exists", Result, h_tl2_UserExists, uint8_t *out, const void *uid) {
    (void)uid; *out = 1; return RES_OK;
}
#endif
