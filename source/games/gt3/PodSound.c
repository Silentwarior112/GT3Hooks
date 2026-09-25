#include "Pod.h"

#include "core/Target.h"
#include "core/ps2/Memory.h"
#include "core/Log.h"
#include "GameConfig.h"

/*
    Toyota Pod - its voice.

    Lupo 2 plays one pod_* effect each time the Pod's emotion changes (its
    dispatcher, L2 0x001A5318, fires on the step's one-shot). Retail has the
    whole sound engine, but none of the Pod's material:

      * NAMES. Game code refers to effects by slot in a table of 40 names
        (SE_NAME_TABLE); at boot each name is looked up in sound/se.inf, whose
        entry says which bank and which programs play it. Lupo 2's table has 51
        - the same 40 plus 11 pod_* names. Retail's cannot grow in place (an
        .eh_frame CIE follows it), so the plugin keeps a 51-entry copy and
        re-points the two places that build its address, and raises the one
        loop bound that knows its length.
      * THE BANK. Lupo 2 loads sound/gt3pod.ins into a fourth bank slot. Retail
        builds three bank objects and loads three banks; the plugin has the
        static constructor build a fourth and loads the file into it, just
        before rn_01.es, which is where Lupo 2 does it.
      * THE PLAY. From the per-frame hook, on the emotion one-shot.

    Everything but the play happens at boot, from init() and from inside the
    game's own sound init, and none of it may run later: loading is blocking
    disc I/O, and the file layer retries a missing file forever.

    Every call in the boot half is integers only, so plain C is ABI-safe. The
    positional play takes four floats and goes through PodThunk.S.

    Nothing here is gated on the files being present at build time. If
    sound/gt3pod.ins is missing the bank is skipped and logged, and the Pod is
    silent; if se.inf lacks the pod_* names they are logged and skipped.
*/

#if POD_SOUND

/* ----------------------------------------------------------------- the game */

/* void BankLoad(bank*, const char* path, int mode) - integers only. */
static void (*SE_BankLoad)(void* bank, const char* path, int mode) =
    (void*)ADDR_SE_bankLoader;

/*
    int file_resolve(const char* path, u32 out[4], void* volume) - integers
    only. Returns -1 when the file does not exist; the game's own loader
    (0x002418D0) calls it in a loop until it stops returning -1.
*/
static int (*File_Resolve)(const char* path, unsigned int* out, void* vol) =
    (void*)ADDR_file_resolve;

#if POD_SOUND == 2
/*
    The voice primitives - all integers only, so plain C is ABI-safe. Every
    one that takes the owner byte validates it with interrupts disabled: it
    acts only if that byte still names a voice whose owner field points back
    at it, so a voice that has ended or been stolen is never touched.
*/
static void* (*SE_Acquire)(int slot, int variant) = (void*)ADDR_SE_acquire;
static int   (*SE_NoteToPitch)(int note8, int fine) = (void*)ADDR_SE_noteToPitch;
static void  (*SE_KeyOn)(signed char* owner, void* req) = (void*)ADDR_SE_keyOn;
static void  (*SE_VoiceSetVolume)(signed char* owner, const unsigned int* lr) =
    (void*)ADDR_SE_voiceSetVolume;
static void  (*SE_ChannelStop)(void* ch, int zeroFirst) = (void*)ADDR_SE_channelStop;

/* PodThunk.S - the float leaves, with the floats handed across as memory. */
extern void Pod_SinCos(const float* ang, float* out);
#else
/* void raceSE(int slot) - centred, at the race SE level; no floats. */
static void (*SE_PlayRace)(int slot) = (void*)ADDR_SE_playRace;
#endif

#define W(a) (*(volatile unsigned int*)(a))

/* ------------------------------------------------------------ the SE table */

typedef struct { const char* name; int seIndex; } SeRec;

#define SE_POD_FIRST 40
#define SE_POD_COUNT 11
#define SE_TOTAL     (SE_NAME_COUNT + SE_POD_COUNT)     /* 51, as in Lupo 2 */

/*
    Slots 40..50 in Lupo 2's order, which is alphabetical. kowai2 is
    registered but no emotion plays it.
*/
static const char* const s_podNames[SE_POD_COUNT] = {
    "pod_genki",  "pod_iya",   "pod_kowai1",  "pod_kowai2",
    "pod_kyoro",  "pod_naku",  "pod_nemui",   "pod_odoroku",
    "pod_okoru",  "pod_sabishii", "pod_wink"
};

/*
    Emotion -> slot, from Lupo 2's dispatcher table (L2 0x00342DD0). Neutral
    is silent.
*/
static const signed char s_emoSlot[11] = {
    -1,     /*  0 neutral  */
    50,     /*  1 wink     */
    40,     /*  2 genki    */
    44,     /*  3 kyoro    */
    48,     /*  4 okoru    */
    47,     /*  5 odoroku  */
    41,     /*  6 iya      */
    49,     /*  7 sabishii */
    42,     /*  8 kowai    */
    45,     /*  9 naku     */
    46      /* 10 nemui    */
};

/* Writable: the game's registration loop fills seIndex. */
static SeRec g_seTable[SE_TOTAL] __attribute__((aligned(16)));

/*
    Bank 3's header goes here rather than into the game's 0x800-byte header
    arena, which is sized for three banks, is never bounds-checked, and ends
    exactly where the bank objects begin.
*/
#define POD_HDR_BYTES 0x800
static unsigned char g_podHdr[POD_HDR_BYTES] __attribute__((aligned(64)));

static int          g_patched;      /* the boot patches went in             */
static int          g_bankLoaded;   /* gt3pod.ins was found and loaded      */
static int          g_ready;        /* 0 unchecked, 1 ready, -1 off         */
static unsigned int g_slotOk;       /* bit (slot - SE_POD_FIRST)            */

/* ------------------------------------------------------------ boot: bank 3 */

static void Pod_LoadPodBank(void)
{
    static const char path[] = "sound/gt3pod.ins";
    unsigned int out[16];
    unsigned int savePtr, saveLeft, spu0;

    if (File_Resolve(path, out, (void*)SE_VOLUME) < 0) {
        LOG("[gt3hooks] snd: %s not found - Pod sound effects off\n", path);
        return;
    }

    savePtr  = W(SE_HDR_PTR);
    saveLeft = W(SE_HDR_LEFT);
    spu0     = W(SE_SPU_PTR);
    W(SE_HDR_PTR)  = (unsigned int)g_podHdr;
    W(SE_HDR_LEFT) = POD_HDR_BYTES;

    SE_BankLoad((void*)SE_BANK3, path, 0);      /* mode 0, as Lupo 2 */

    if ((int)W(SE_HDR_LEFT) < 0)
        LOG("[gt3hooks] snd: gt3pod.ins header overran its %X-byte buffer by %X "
            "- plugin memory after it is damaged\n",
            POD_HDR_BYTES, -(int)W(SE_HDR_LEFT));
    LOG("[gt3hooks] snd: gt3pod.ins -> bank 3, header %X bytes, SPU %05X..%05X\n",
        POD_HDR_BYTES - (int)W(SE_HDR_LEFT), spu0, W(SE_SPU_PTR));

    W(SE_HDR_PTR)  = savePtr;       /* rn_01.es carries on exactly as retail */
    W(SE_HDR_LEFT) = saveLeft;
    g_bankLoaded = 1;
}

/*
    Replaces the `jal BankLoad` that loads sound/rn_01.es (SE_RN01_LOAD_JAL),
    inside the game's boot-time sound init. The delay slot has already set
    a2, so this receives rn_01's exact arguments, and the caller reads no
    result. Lupo 2's order: the Pod bank, then rn_01.
*/
void Pod_Rn01Tramp(void* bank, const char* path, int mode)
{
    Pod_LoadPodBank();
    SE_BankLoad(bank, path, mode);

    if (W(SE_SPU_PTR) > SE_SPU_RACE_BASE)
        LOG("[gt3hooks] snd: SE banks end at SPU %05X, past %05X where music and "
            "race-car samples start - expect garbled sound\n",
            W(SE_SPU_PTR), SE_SPU_RACE_BASE);
    else
        LOG("[gt3hooks] snd: SE banks end at SPU %05X (limit %05X)\n",
            W(SE_SPU_PTR), SE_SPU_RACE_BASE);
}

/* ------------------------------------------------------------- boot: init */

static int Pod_SndCheck(unsigned int addr, unsigned int want)
{
    if (W(addr) == want)
        return 1;
    LOG("[gt3hooks] snd: %08X holds %08X, expected %08X - "
        "Pod sound NOT installed\n", addr, W(addr), want);
    return 0;
}

/* Replace the immediate of a lui (hi half, carry-adjusted) or addiu (lo). */
static unsigned int Pod_Hi(unsigned int word, unsigned int addr)
{
    return (word & 0xFFFF0000u) | (((addr + 0x8000u) >> 16) & 0xFFFFu);
}

static unsigned int Pod_Lo(unsigned int word, unsigned int addr)
{
    return (word & 0xFFFF0000u) | (addr & 0xFFFFu);
}

/*
    Called from init(), which runs before the static constructors (__main) and
    long before the sound init (main -> 0x00102D98 -> 0x00102C00 -> 0x0022C900
    -> 0x0022C3E8) - both of which the patches below must precede.
*/
void Pod_InstallSound(void)
{
    const SeRec* retail = (const SeRec*)SE_NAME_TABLE;
    unsigned int T = (unsigned int)g_seTable;
    int i;

    g_patched = g_bankLoaded = g_ready = 0;
    g_slotOk = 0;

    /* All or nothing: every word is checked before any is written. */
    if (!Pod_SndCheck(SE_REG_LUI,     SE_REG_LUI_WORD)     ||
        !Pod_SndCheck(SE_REG_ADDIU,   SE_REG_ADDIU_WORD)   ||
        !Pod_SndCheck(SE_GET_LUI,     SE_GET_LUI_WORD)     ||
        !Pod_SndCheck(SE_GET_ADDIU,   SE_GET_ADDIU_WORD)   ||
        !Pod_SndCheck(SE_COUNT_SLTI,  SE_COUNT_SLTI_WORD)  ||
        !Pod_SndCheck(SE_BANK_CTOR_N, SE_BANK_CTOR_N_WORD) ||
        !Pod_SndCheck(SE_BANK_EH_N,   SE_BANK_EH_N_WORD)   ||
        !Pod_SndCheck(SE_BANK_DTOR_END, SE_BANK_DTOR_END_WORD) ||
        !Pod_SndCheck(SE_RN01_LOAD_JAL, SE_RN01_LOAD_JAL_WORD))
        return;

    for (i = 0; i < SE_NAME_COUNT; i++)
        g_seTable[i] = retail[i];
    /*
        -1, not retail's 0: the registration loop leaves an unmatched name's
        index alone, and 0 is a real se.inf entry - an unmatched name would
        otherwise play entry 0 instead of nothing.
    */
    for (i = 0; i < SE_POD_COUNT; i++) {
        g_seTable[SE_POD_FIRST + i].name    = s_podNames[i];
        g_seTable[SE_POD_FIRST + i].seIndex = -1;
    }

    /* The table: both places that build its address. */
    PATCH_INT(SE_REG_LUI,   Pod_Hi(SE_REG_LUI_WORD,   T));
    PATCH_INT(SE_REG_ADDIU, Pod_Lo(SE_REG_ADDIU_WORD, T));
    PATCH_INT(SE_GET_LUI,   Pod_Hi(SE_GET_LUI_WORD,   T));
    PATCH_INT(SE_GET_ADDIU, Pod_Lo(SE_GET_ADDIU_WORD, T));
    /* Its length: slti $v0,$s2,0x28 -> 0x33. */
    PATCH_INT(SE_COUNT_SLTI, (SE_COUNT_SLTI_WORD & 0xFFFF0000u) | SE_TOTAL);

    /* Four bank objects instead of three - the loader makes a virtual call
       through the object, so bank 3 must be constructed, not just zeroed. */
    PATCH_INT(SE_BANK_CTOR_N,   (SE_BANK_CTOR_N_WORD & 0xFFFF0000u) | 3);
    PATCH_INT(SE_BANK_EH_N,     (SE_BANK_EH_N_WORD   & 0xFFFF0000u) | 3);
    PATCH_INT(SE_BANK_DTOR_END, (SE_BANK_DTOR_END_WORD & 0xFFFF0000u) | 0x80);

    MAKE_JAL(SE_RN01_LOAD_JAL, (void*)Pod_Rn01Tramp);

    g_patched = 1;
    LOG("[gt3hooks] snd: SE table -> %08X, %d names; 4 banks; Pod bank loads "
        "before rn_01; play mode %d\n", T, SE_TOTAL, POD_SOUND);
}

/* ------------------------------------------------------------ first race */

/*
    Once, the first time a Pod is seen: by now the sound init has run, so the
    registration loop has filled seIndex. Each pod_* slot is enabled only if
    se.inf named it AND pointed it at bank 3 - a slot that resolved nowhere,
    or to another bank, is logged and left silent.
*/
void Pod_SoundCheck(void)
{
    unsigned char* blob;
    unsigned int count;
    int ready = 0;
    int s;

    if (g_ready)
        return;
    g_ready = -1;

    if (!g_patched)
        return;                     /* already logged at boot */
    if (!g_bankLoaded || !W(SE_BANK3 + 4)) {
        LOG("[gt3hooks] snd: bank 3 not loaded - Pod silent\n");
        return;
    }
    blob = (unsigned char*)W(SE_INF_OBJ);
    if (!blob) {
        LOG("[gt3hooks] snd: no se.inf loaded - Pod silent\n");
        return;
    }
    count = *(unsigned int*)(blob + 0xC);

    for (s = SE_POD_FIRST; s < SE_TOTAL; s++) {
        int idx = g_seTable[s].seIndex;
        unsigned int packed;

        if (idx < 0 || (unsigned int)idx >= count) {
            LOG("[gt3hooks] snd: slot %d %s is NOT in se.inf - silent\n",
                s, g_seTable[s].name);
            continue;
        }
        /* The same word getSEDescriptor(s) returns. */
        packed = *(unsigned int*)(blob + 0x14 + idx * 8);
        if ((packed >> 24) != 3) {
            LOG("[gt3hooks] snd: slot %d %s -> se.inf #%d names bank %d, not 3 "
                "- silent\n", s, g_seTable[s].name, idx, packed >> 24);
            continue;
        }
#if POD_DIAG
        LOG("[gt3hooks] snd: slot %d %s -> se.inf #%d, bank 3, prog %d/%d\n",
            s, g_seTable[s].name, idx, (packed >> 12) & 0xFFF, packed & 0xFFF);
#endif
        g_slotOk |= 1u << (s - SE_POD_FIRST);
        ready++;
    }
    g_ready = 1;
    LOG("[gt3hooks] snd: %d of %d Pod sounds ready\n", ready, SE_POD_COUNT);
}

/* ------------------------------------------------------------------ play */

/* Which slot an emotion plays, or -1: neutral, or not ready / not in se.inf. */
static int Pod_EmotionSlot(int emo)
{
    int slot;

    if (emo <= 0 || emo > 10)
        return -1;
    slot = s_emoSlot[emo];
    if (slot < 0)
        return -1;
    Pod_SoundCheck();
    if (g_ready != 1 || !(g_slotOk & (1u << (slot - SE_POD_FIRST))))
        return -1;
    return slot;
}

#if POD_SOUND == 2
/*
    POSITIONAL: Lupo 2's Pod voice, rebuilt.

    Lupo 2 gives each Pod one tracked voice (its dispatcher, 0x001A5318, and a
    channel class retail does not have). A new emotion releases the voice that
    is still sounding and keys on the new one; and every frame the voice's
    level and pan are set again from where the Pod now is, so it fades and
    pans as the Pod moves, and stops when its level reaches zero.

    Retail has every piece, just not assembled that way. Its car-SE player
    (0x001A6ED8) computes exactly Lupo 2's key-on levels but keys on with no
    owner, so the voice cannot be reached again. So the player is mirrored
    here field for field, keying on with an owner byte - which is what makes
    the release and the per-frame level possible, through the same
    primitives retail's own tracked sounds use.

    Floats are computed here and only integers cross into the game, except
    through Pod_SinCos.
*/

/* The key-on request, as 0x001A6ED8 builds it on its stack. */
typedef struct {
    short          l;       /* +0x00 Q14 left level                          */
    short          r;       /* +0x02 Q14 right level                         */
    unsigned short pitch;   /* +0x04 note2pitch(desc[+0x10] << 8, desc[+8])  */
    unsigned short pad6;
    unsigned int   addr;    /* +0x08 desc[+0x00] - the sample's SPU address  */
    unsigned int   d14;     /* +0x0C desc[+0x14]                             */
    unsigned short dA;      /* +0x10 desc[+0x0A]                             */
    unsigned char  prio;    /* +0x12                                         */
    unsigned char  dE;      /* +0x13 desc[+0x0E]                             */
    unsigned int   pad[3];
} SeKeyOn;

/* Retail's Q14 clamp, applied twice by the player: [-0x4000, 0x3FFF]. */
static int Pod_ClampQ14(int v)
{
    if (v > 0x3FFF)
        return 0x3FFF;
    if (v < -0x4000)
        return -0x4000;
    return v;
}

/* One channel's level: int(gain * 16384), clamped, times the tone's volume. */
static int Pod_Level(float gain, const unsigned char* desc)
{
    int v = Pod_ClampQ14((int)(gain * 16384.0f));
    return Pod_ClampQ14((v * *(const short*)(desc + 6)) >> 14);
}

/*
    This frame's volume and the two pan gains, from the pan state retail
    refreshed for this car earlier in the frame (0x001A6C38). 0 if there is
    nothing to hear: race sound muted, or the state not computed yet.

        vol = min(32/d, 1) x trim x race SE on x race fade      (Lupo 2's g x
                                                                  st+0x37C x ...)
        angle = clamp((x/d + 1) x pi/4, 0, pi/2)
        L = +-cos(angle), R = +-sin(angle), each negated by its flag
*/
static int Pod_VoiceGains(void* cm, float* vol, float* gl, float* gr)
{
    unsigned char* so = (unsigned char*)cm + CARMODEL_SNDSTATE_OFF;
    float d, g, ang, cs[2];

    if (W(SE_RACE_MUTE))
        return 0;                   /* the pan state is stale while muted */
    d = *(float*)(so + 0x10);
    if (!(d > 0.0f))
        return 0;

    g = 32.0f / d;
    if (g > 1.0f)
        g = 1.0f;
    *vol = g * (POD_SOUND_VOLUME / 100.0f) *
           *(volatile float*)SE_RACE_LEVEL * *(volatile float*)SE_RACE_FADE;

    ang = (*(float*)(so + 4) + 1.0f) * 0.78539816f;
    if (!(ang > 0.0f))
        ang = 0.0f;
    else if (ang > 1.57079633f)
        ang = 1.57079633f;
    Pod_SinCos(&ang, cs);           /* cs[0] = cos, cs[1] = sin */

    *gl = so[0] ? -cs[0] : cs[0];
    *gr = so[1] ? -cs[1] : cs[1];
    return 1;
}

static void Pod_VoiceStart(PodVoice* v, int slot, void* cm)
{
    const unsigned char* desc;
    SeKeyOn req __attribute__((aligned(16)));
    float vol, gl, gr, a;
    int i;

    if (v->voice >= 0)
        SE_ChannelStop(v, 0);       /* release the one still sounding */

    if (!Pod_VoiceGains(cm, &vol, &gl, &gr) || vol == 0.0f)
        return;

    desc = (const unsigned char*)SE_Acquire(slot, 0);
    if (!desc)
        return;

    for (i = 0; i < (int)(sizeof(req) / 4); i++)
        ((unsigned int*)&req)[i] = 0;

    /* priority: 0 at full level, falling to 255 as it fades - 0x001A6F30 */
    a = vol < 0.0f ? -vol : vol;
    if (a >= 1.0f) {
        req.prio = 0;
    } else {
        i = (int)((1.0f - a) * 255.0f);
        req.prio = (unsigned char)(i > 255 ? 255 : i);
    }
    req.l     = (short)Pod_Level(vol * gl, desc);
    req.r     = (short)Pod_Level(vol * gr, desc);
    req.pitch = (unsigned short)SE_NoteToPitch(desc[0x10] << 8,
                                               *(const unsigned short*)(desc + 8));
    req.addr  = *(const unsigned int*)(desc + 0x00);
    req.d14   = *(const unsigned int*)(desc + 0x14);
    req.dA    = *(const unsigned short*)(desc + 0x0A);
    req.dE    = desc[0x0E];

    v->desc   = (void*)desc;
    v->lastLR = ((unsigned int)(unsigned short)req.r << 16) |
                (unsigned short)req.l;
    SE_KeyOn(&v->voice, &req);      /* writes the voice index to v->voice */

#if POD_DIAG
    LOG("[gt3hooks] snd: play slot %d %s: voice %d, L %d R %d, prio %d\n",
        slot, g_seTable[slot].name, v->voice, req.l, req.r, req.prio);
#endif
}

/* Every frame while the voice sounds: its level and pan follow the Pod. */
static void Pod_VoiceFollow(PodVoice* v, void* cm)
{
    float vol, gl, gr;
    unsigned int lr;
    int l, r;

    if (v->voice < 0 || !v->desc)
        return;                     /* ended, stolen, or never started */
    if (!Pod_VoiceGains(cm, &vol, &gl, &gr))
        return;                     /* muted: leave it be, as retail does */

    l = Pod_Level(vol * gl, (const unsigned char*)v->desc);
    r = Pod_Level(vol * gr, (const unsigned char*)v->desc);
    if (l == 0 && r == 0) {
        SE_ChannelStop(v, 0);       /* faded out - Lupo 2 stops it here too */
        return;
    }
    lr = ((unsigned int)(unsigned short)r << 16) | (unsigned short)l;
    if (lr != v->lastLR) {
        v->lastLR = lr;
        SE_VoiceSetVolume(&v->voice, &v->lastLR);
    }
}
#endif  /* POD_SOUND == 2 */

void Pod_VoiceInit(PodVoice* v)
{
    v->desc   = 0;
    v->voice  = -1;
    v->lastLR = 0;
}

void Pod_VoiceStop(PodVoice* v)
{
#if POD_SOUND == 2
    if (g_patched && v->voice >= 0)
        SE_ChannelStop(v, 0);
#else
    (void)v;
#endif
}

/*
    Every frame, for every Pod, with the state entered this step (0 if none).
*/
void Pod_VoiceUpdate(PodVoice* v, int newEmo, void* cm)
{
    int slot = newEmo ? Pod_EmotionSlot(newEmo) : -1;

#if POD_SOUND == 2
    if (slot >= 0)
        Pod_VoiceStart(v, slot, cm);
    else
        Pod_VoiceFollow(v, cm);
#else
    (void)v;
    (void)cm;
    if (slot >= 0) {
        SE_PlayRace(slot);          /* checks the race mute itself */
#if POD_DIAG
        LOG("[gt3hooks] snd: play slot %d %s (centred)\n",
            slot, g_seTable[slot].name);
#endif
    }
#endif
}

#else   /* !POD_SOUND */

void Pod_InstallSound(void) { }
void Pod_SoundCheck(void) { }
void Pod_VoiceInit(PodVoice* v) { v->desc = 0; v->voice = -1; v->lastLR = 0; }
void Pod_VoiceStop(PodVoice* v) { (void)v; }
void Pod_VoiceUpdate(PodVoice* v, int newEmo, void* cm)
{
    (void)v;
    (void)newEmo;
    (void)cm;
}

#endif

#if POD_DIAG
/*
    Diagnostic only, and read-only: log each change of the GAME's own race
    sound-effect controls, so that a dip in volume heard in play can be put
    next to what the game did at that moment (and next to the "snd: play" line
    of any Pod sound). The plugin never writes any of these:

        level  gp-0x5810  the race SE option, 1.0 or 0.0
        fade   gp-0x580C  full at race start; CameraManager drives it from
                          its own in-race SE level (0x0020C654: raised and
                          lowered from the pad, saved to the options) and
                          switches it between that level and full (0x0020B5CC)
        mute   gp-0x5808  set by the race (0x0017BBB8)

    A ramping fade changes every frame, so it is logged only when it has
    moved by 0.05 or reached 0 or 1.
*/
void Pod_SoundWatch(void)
{
    static int lastLevel = -1;
    static int lastFade  = -1;
    static int lastMute  = -1;
    int level = (int)(*(volatile float*)SE_RACE_LEVEL * 1000.0f);
    int fade  = (int)(*(volatile float*)SE_RACE_FADE * 1000.0f);
    int mute  = (int)*(volatile unsigned int*)SE_RACE_MUTE;
    int moved = fade - lastFade;

    if (moved < 0)
        moved = -moved;
    if (level != lastLevel || mute != lastMute || moved >= 50 ||
        (fade != lastFade && (fade == 0 || fade == 1000))) {
        LOG("[gt3hooks] snd: game race SE level %d, fade %d (x1000), mute %d\n",
            level, fade, mute);
        lastLevel = level;
        lastFade  = fade;
        lastMute  = mute;
    }
}
#endif
