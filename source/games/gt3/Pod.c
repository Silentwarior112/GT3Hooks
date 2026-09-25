#include "Pod.h"

#include "core/Target.h"
#include "core/ps2/Memory.h"
#include "core/Log.h"
#include "GameConfig.h"

/*
    Toyota Pod - on retail GT3.

    The Pod is an emotion machine (Lupo 2 0x001E1238..0x001E1D60). Eleven states
    drive everything it does:

      * THE FACE. Palette-animated records in the face texture, selected by the
        emotion state; Model::animate advances them and Model::reset applies
        them. The meshes that show them are chosen by the model program's
        SWITCH on id 10, which retail's CarModel query never answers - so the
        Pod's CarModels get a vtable whose query does (Pod_GetSwitch).
      * THE TAIL. SWITCH id 7 picks between a side-to-side "look" arm, whose
        morph weight follows the mood, and an up-down "bob" arm, whose weight
        follows an oscillator; SWITCH id 8 picks a colour category, cross-faded
        over the cool-down after a change. The weights go to the game through
        setBlend. Nothing moves unless an emotion drives it.
      * THE SQUAT. Suspension physics, independent of the emotions: hold the side
        brake at a standstill and the springs let go one corner at a time.
      * THE SOUND. One pod_* effect on each change of state, on the one-shot
        newEmo below. PodSound.c.

    FLOAT BOUNDARIES. The game (GCC 2.9x EABI) numbers integer and float
    arguments independently; this compiler (GCC 14) numbers argument slots by
    position. They disagree on any call that mixes the two, so every call into
    the game that takes or returns a float goes through PodThunk.S, which
    explains the rule. Nothing in this file passes a float across a function
    boundary; integer-only calls, like the switch query, agree and are safe.
*/

/* ----------------------------------------------------------------- the game */

extern void Pod_ModelAnimate(void* model, int select, unsigned int dtBits,
                             void* fn);
extern void Pod_SetBlendBits(unsigned int wBits);
extern void Pod_SuspWrap(void);

/*
    Model::reset(model), US 0x00224B48 - despite the name, the APPLY pass: it
    walks the textures calling Tex1_applyAnim (0x00249110), which writes each
    dirty record's palette into its target CLUT. Model::animate only advances.
*/
static void (*Model_reset)(void* model) = (void*)ADDR_Model_reset;

/* Per-record reset, US 0x00248FF8. $a0 = the record; integers only. */
static void (*AnimRecord_reset)(void* rec) = (void*)ADDR_AnimRecord_reset;

/* CarModel::setModel(cm, model, relocate), US 0x0021EA90 - integers only. */
static int (*CarModel_setModel)(void* cm, void* model, int reloc) =
    (void*)ADDR_CarModel_setModel;

/* Retail's switch query. int f(void* obj, int id) - integers only, so C-safe. */
static int (*CarModel_getSwitch)(void* obj, int id) =
    (void*)ADDR_CarModel_getSwitch;

/*
    THE FRAME RATE. Everything the Pod counts, it counts in frames or physics
    steps, and Lupo 2 (0x001E1238..0x001E1D60) counts at 60 Hz. The PAL build
    runs at 50 (GAME_FRAME_HZ, target header). PD's own 50 Hz build of the same
    code - GT Concept 2002 Tokyo-Geneva, SCES-50858 - supplies every 50 Hz value
    below: read from its bytes, twin by twin (docs/eu50/EU50_SPEC.md, rows
    P1..P24). They are its literals, not arithmetic: PD rounded some counts its
    own way (34 -> 29, 13/37 -> 11/31, 25 -> 21).
*/
#if GAME_FRAME_HZ == 60                     /* Lupo 2's values                  */
#define POD_FRAME_DT_BITS 0x3C888888u       /* 1/60: Lupo 2 CarModel::animate    */
#define POD_DUR           180               /* default hold (g_emoDur)           */
#define POD_RESET_TIMER    60               /* start-up grace                    */
#define POD_COOL_CAT       36               /* cool-down leaving a category      */
#define POD_COOL_NEU       24               /*   leaving neutral; the fade split */
#define POD_COOL_CUT       25               /* returning to neutral: cool < this */
#define POD_TRI_A          13               /* triangle: rising while n < A      */
#define POD_TRI_B          37               /*   falling while n < B             */
#define POD_TRI_H          24               /*   H - n                           */
#define POD_TRI_P          48               /*   period                          */
#define POD_MOOD_MAX       30               /* mood clamp                        */
#define POD_HOLD_TRIG     120               /* side by side, slipstream          */
#define POD_HOLD_CRASH    210               /* car-to-car edge hits              */
#define POD_MOOD_DIV     60.0f              /* 2 x POD_MOOD_MAX                  */
#define POD_TRI_DIV      24.0f              /* 2 x the triangle's reach          */
#define POD_FADEIN_DIV   24.0f              /* POD_COOL_NEU                      */
#define POD_FADEOUT_DIV  12.0f              /* POD_COOL_CAT - POD_COOL_NEU       */
#define POD_SQUAT_MAX      34               /* squat steps to "sat down"         */
#define POD_SQUAT_STEP      7               /* steps between corners             */
#elif GAME_FRAME_HZ == 50                   /* GT Concept SCES-50858's values    */
#define POD_FRAME_DT_BITS 0x3CA3D70Au       /* 0.02: GC 0x00220D50, and EU's own */
#define POD_DUR           150               /* GC table 0x002BB8B8               */
#define POD_RESET_TIMER    50               /* GC 0x001DB79C                     */
#define POD_COOL_CAT       30               /* GC 0x001DB8B0                     */
#define POD_COOL_NEU       20               /* GC 0x001DB8B4                     */
#define POD_COOL_CUT       21               /* GC 0x001DBB3C                     */
#define POD_TRI_A          11               /* GC 0x001DB954                     */
#define POD_TRI_B          31               /* GC 0x001DB950                     */
#define POD_TRI_H          20               /* GC 0x001DB968                     */
#define POD_TRI_P          40               /* GC 0x001DB95C, 0x001DBC3C         */
#define POD_MOOD_MAX       25               /* GC 0x001DBB84, 0x001DBBAC         */
#define POD_HOLD_TRIG     100               /* GC 0x001DBA54                     */
#define POD_HOLD_CRASH    175               /* GC 0x001DBF9C/CC/E8               */
#define POD_MOOD_DIV     50.0f              /* GC 0x001DBD70                     */
#define POD_TRI_DIV      20.0f              /* GC 0x001DBDA0                     */
#define POD_FADEIN_DIV   20.0f              /* GC 0x001DBCA0                     */
#define POD_FADEOUT_DIV  10.0f              /* GC 0x001DBCEC                     */
#define POD_SQUAT_MAX      29               /* GC 0x001DC158                     */
#define POD_SQUAT_STEP      6               /* GC 0x001DC218..0x001DC220         */
#else
#error "Pod.c has values for 60 and 50 Hz only (GAME_FRAME_HZ, target header)"
#endif
#define F32_0_0           0x00000000u

static unsigned int Pod_F2U(float f)
{
    union { float f; unsigned int u; } c;
    c.f = f;
    return c.u;
}

/* ------------------------------------------------------------- the emotions */

/*
    Lupo 2's three tables, 0x002DE920 / 0x002DE930 / 0x002DE940:

      duration  default hold for a request of -1, in steps (POD_DUR = 3 s)
      attribute +1 / -1: mood drifts up / down (the tail leans one way or the
                other); 2: the oscillator runs (the tail bobs); 0: neither
      category  the face category, answered to SWITCH id 8
*/
static const unsigned char g_emoDur[11]  = {
    0, POD_DUR, 0, POD_DUR, POD_DUR, POD_DUR, POD_DUR, POD_DUR, POD_DUR, POD_DUR, 0
};
static const signed char   g_emoAttr[11] = { 0,  2,  1,  2,  1,  1, -1, -1, -1, -1, -1 };
static const unsigned char g_emoCat[11]  = { 0,  4,  4,  4,  1,  1,  1,  2,  2,  2,  3 };

#if POD_DIAG
static const char* const g_emoName[11] = {
    "neutral", "wink", "genki", "kyoro", "okoru", "odoroku",
    "iya", "sabishii", "kowai", "naku", "nemui"
};
#endif

/* ------------------------------------------------------------ per-car state */

/*
    Retail has none of Lupo 2's Pod fields - its CarModel is 0x1C bytes smaller
    and its car carries no emotion or squat state - so they live here, one entry
    per CarModel. The draw side finds an entry by CarModel, the physics side by
    Car*; RaceCarModel::update supplies both.

    The emotion fields mirror Lupo 2's car+0x5F1..+0x5F6 one for one.
*/
#define POD_SELECT_NONE (-99)
#define POD_ENTRIES     16

typedef struct {
    void* cm;               /* CarModel*: the key                             */
    void* car;              /* Car*, from RaceCarModel+0x6AC                   */
    void* model;            /* cm+0x24 when this entry was last classified     */
    int   isPod;
    int   applied;          /* the select last handed to the model             */
    int   squat;            /* 0..POD_SQUAT_MAX                               */

    unsigned char emo;      /* prev << 4 | cur                                */
    unsigned char timer;    /* steps left; 0 idle, 0xFF locked                */
    signed char   cool;     /* category cool-down / cross-fade                */
    unsigned char phase;    /* oscillator, 0..POD_TRI_P-1                     */
    signed char   mood;     /* -POD_MOOD_MAX..POD_MOOD_MAX                    */
    unsigned char newEmo;   /* state entered since the voice last looked, else 0 */

    int demoTick;
    int demoState;

    PodVoice voice;         /* the Pod's one sounding voice - PodSound.c      */

    unsigned int seen;      /* g_podSeq at its last update: oldest is reused  */
} PodEntry;

static PodEntry g_pod[POD_ENTRIES];
static unsigned int g_podSeq;

static PodEntry* Pod_FindByCm(void* cm)
{
    int i;
    for (i = 0; i < POD_ENTRIES; i++)
        if (g_pod[i].cm == cm)
            return &g_pod[i];
    return 0;
}

#if POD_SQUAT || POD_EMOTION_SOURCE == 0
/*
    The physics side's lookup - the squat, and crash events. Pod_SetCar keeps
    each Car* in at most one entry, so the first match is the only one.
*/
static PodEntry* Pod_FindByCar(void* car)
{
    int i;
    for (i = 0; i < POD_ENTRIES; i++)
        if (g_pod[i].car == car && g_pod[i].cm)
            return &g_pod[i];
    return 0;
}
#endif

/*
    Entries are never told when a race ends, and the game places cars at the
    same addresses race after race (race + 0xC8 + slot * 0x16DC). So an entry
    left from an earlier race can hold the Car* a live Pod now has, and a
    first-match lookup would find the dead one. Whenever an entry takes a
    Car*, any other entry holding it lets go.
*/
static void Pod_SetCar(PodEntry* e, void* car)
{
    int i;

    if (e->car == car)
        return;
    for (i = 0; i < POD_ENTRIES; i++)
        if (&g_pod[i] != e && g_pod[i].car == car)
            g_pod[i].car = 0;
    e->car = car;
}

static void Pod_ClearEntry(PodEntry* e)
{
    e->cm        = 0;
    e->car       = 0;
    e->model     = 0;
    e->isPod     = 0;
    e->applied   = POD_SELECT_NONE;
    e->squat     = 0;
    e->emo       = 0;
    e->timer     = 0;
    e->cool      = 0;
    e->phase     = 0;
    e->mood      = 0;
    e->newEmo    = 0;
    e->demoTick  = 0;
    e->demoState = 0;
    Pod_VoiceInit(&e->voice);
    e->seen      = g_podSeq;
}

/*
    A free entry, or else the one updated longest ago. A race has at most 8
    cars, updated every frame, so with 16 entries the oldest always belongs to
    a CarModel that no longer exists - race after race, the table recycles
    instead of filling up. Nothing of the old CarModel is touched: its memory
    may already be something else. Its voice is let go through our own owner
    byte, which the voice system validates.
*/
static PodEntry* Pod_Claim(void* cm)
{
    PodEntry* e = 0;
    int i;

    for (i = 0; i < POD_ENTRIES; i++) {
        if (!g_pod[i].cm) {
            e = &g_pod[i];
            break;
        }
        if (!e || (int)(g_pod[i].seen - e->seen) < 0)
            e = &g_pod[i];
    }
    if (e->cm)
        Pod_VoiceStop(&e->voice);
    Pod_ClearEntry(e);
    e->cm = cm;
    return e;
}

/* ------------------------------------------------------- the emotion machine */

/* Lupo 2 0x001E1470: a triangle over one period, zero at 0 and half way -
   at 60 Hz 0..47, range -12..12, zero at 0 and 24. */
static int Pod_Tri(int n)
{
    return n < POD_TRI_A ? n : (n < POD_TRI_B ? POD_TRI_H - n : n - POD_TRI_P);
}

/*
    Lupo 2 0x001E12A0, run on race-car reset. The timer (one second) is a
    start-up grace period in which a weak (duration 0) request is refused.
*/
static void Pod_EmotionReset(PodEntry* e)
{
    e->newEmo = 0;
    e->timer  = POD_RESET_TIMER;
    e->emo    = 0;
    e->cool   = 0;
    e->phase  = 0;
    e->mood   = 0;
}

/*
    setAnimState, Lupo 2 0x001E1308.

        dur  -1   the state's default duration
              0   weak: holds only while nothing else is running
            255   locked: nothing but a reset clears it; also the only request
                  that is honoured during a cool-down
            else  that many steps

    A change of face category starts a cool-down - POD_COOL_NEU steps, or
    POD_COOL_CAT leaving a non-neutral category (24 / 36 at 60 Hz) - during
    which SWITCH id 8 cross-fades the categories and every other request is
    refused, including the return to neutral.
*/
static void Pod_SetEmotion(PodEntry* e, int s, int dur)
{
    int cur;

    if (s < 0 || s > 10)
        return;
    if (e->cool > 0 && dur != 255)
        return;
    if (e->timer == 0xFF)
        return;
    if (dur == 0 && e->timer != 0)
        return;

    cur = e->emo & 0xF;
    if (cur != s) {
        e->newEmo = (unsigned char)s;
        e->emo    = (unsigned char)((cur << 4) | s);
    }
    e->timer = (dur == -1) ? g_emoDur[s] : (unsigned char)dur;
    if (g_emoCat[s] != g_emoCat[cur])
        e->cool = (signed char)(g_emoCat[cur] ? POD_COOL_CAT : POD_COOL_NEU);
}

#if POD_EMOTION_SOURCE == 0
/*
    THE GAME'S OWN TRIGGERS - all of Lupo 2's that retail can reach.

    Two sources, as in Lupo 2:
      * the per-step condition chain (L2 0x001E1520), below;
      * crash events (L2 0x001DE730), in Pod_OnCarEvent further down.
    The one trigger left out is Lupo 2's POD race rule (L2 0x001F7BAC: lap
    without stopping in the stop area -> sabishii, locked). Retail has no such
    race mode, rule, stop area or message; there is nothing honest to map it to.
*/

/* int f(void* rel, int me, int* otherOut) - integers only. Target header. */
static int (*Battle_Level)(void* rel, int me, int* otherOut) =
    (void*)ADDR_BattleLevel;

/*
    Condition 4: Lupo 2's battle level for this car is 5 - its body overlaps
    another car's front to back, on the same lap. Retail keeps the relation
    matrix up to date every race step and still carries Lupo 2's query, unused;
    it is a pure reader.

    The race object comes from car+4, so first prove this car really is car
    `slot` of that race - by arithmetic, before any read through it.
*/
static int Pod_SideBySide(unsigned char* car)
{
    unsigned int race = *(unsigned int*)(car + CAR_RACE_OFF);
    unsigned int me   = car[CAR_SLOT_OFF];
    int n;
    int other;

    if (!race || (race & 3) || me >= 8)
        return 0;
    if ((unsigned int)car != race + RACE_CAR0_OFF + me * RACE_CAR_STRIDE)
        return 0;
    if (*(unsigned int*)(race + RACE_REL_OFF) != race)
        return 0;                   /* relation block not set up yet */
    n = *(int*)(race + RACE_NCARS_OFF);
    if (n < 2 || n > 8 || (int)me >= n)
        return 0;
    return Battle_Level((void*)(race + RACE_REL_OFF), (int)me, &other) == 5;
}

/*
    Condition 6, Lupo 2 0x001E14A0: every wheel stands on off-track ground -
    surface ids 2, 3, 4 and 6 in the per-wheel road-contact records. The mask
    equals the game's class table for ids 0..15; above that the game's
    unbounded table read hits unrelated bytes, so the mask is the honest form.
*/
static int Pod_AllWheelsOffTrack(const unsigned char* car)
{
    int i;

    for (i = 0; i < CAR_CONTACT_COUNT; i++) {
        unsigned int id =
            car[CAR_CONTACT_OFF + i * CAR_CONTACT_STRIDE + CONTACT_SURFACE_OFF];
        if (id >= 16 || !((OFFTRACK_CLASS_MASK >> id) & 1))
            return 0;
    }
    return 1;
}

/*
    Lupo 2 0x001E1520's chain, first match wins, in its order. Lupo 2 asks for
    the battle level up front; it is a pure query, so asking only when it is
    reached is the same thing.
*/
static void Pod_EmotionInputs(PodEntry* e, unsigned char* car)
{
    unsigned char finished = car[CAR_FINISHED_OFF];
    signed char   rank     = (signed char)car[CAR_RANK_OFF];
    unsigned char message  = car[CAR_MESSAGE_OFF];
    unsigned char flags    = car[CAR_FLAGS_OFF];
    float         draft    = *(float*)(car + CAR_GAUGE_OFF);

    if (finished)
        Pod_SetEmotion(e, rank == 1 ? 2 : 7, 0);    /* 1st: genki, else sabishii */
    else if (message == 1)
        Pod_SetEmotion(e, 6, 0);                    /* WRONG WAY: iya            */
    else if (flags & 0x10)
        Pod_SetEmotion(e, 3, 0);                    /* pit lane: kyoro           */
    else if (Pod_SideBySide(car))
        Pod_SetEmotion(e, 1, POD_HOLD_TRIG);        /* side by side: wink        */
    else if (draft == 1.0f)
        Pod_SetEmotion(e, 2, POD_HOLD_TRIG);        /* full slipstream: genki    */
    else if (Pod_AllWheelsOffTrack(car))
        Pod_SetEmotion(e, 10, 0);                   /* all wheels off: nemui     */
    else if (e->timer == 0)
        Pod_SetEmotion(e, 0, -1);
}
#endif

#if POD_EMOTION_SOURCE == 1
/*
    The demo: walk through states 1..10, each held for POD_DEMO_HOLD steps
    and then left to fall back to neutral, one every POD_DEMO_PERIOD steps. The
    gap is longer than the cool-down, so no request is refused. GameConfig.h
    gives both in 60 Hz steps; at 50 Hz they are scaled by PD's 5/6 (the demo
    is the plugin's own, so no PD build carries it).
*/
#define POD_DEMO_PERIOD_STEPS (POD_DEMO_PERIOD * GAME_FRAME_HZ / 60)
#define POD_DEMO_HOLD_STEPS   (POD_DEMO_HOLD * GAME_FRAME_HZ / 60)

static void Pod_EmotionDemo(PodEntry* e)
{
    if (++e->demoTick < POD_DEMO_PERIOD_STEPS) {
        if (e->timer == 0)
            Pod_SetEmotion(e, 0, -1);
        return;
    }
    e->demoTick  = 0;
    e->demoState = e->demoState % 10 + 1;
    Pod_SetEmotion(e, e->demoState, POD_DEMO_HOLD_STEPS);
}
#endif

/*
    The per-step updater, Lupo 2 0x001E1520, below its condition chain:

      * the timer counts down, and returns the Pod to neutral when it runs out;
      * the cool-down counts down, and is cut short leaving neutral;
      * the MOOD drifts toward +-POD_MOOD_MAX while the state's attribute is +1 or
        -1, and back toward 0 otherwise - but only at the oscillator's rest
        points, so a lean never interrupts a bob part-way;
      * the OSCILLATOR runs while the mood is 0 and either the state's
        attribute is 2 or it is part-way through a cycle.
*/
static void Pod_TickEmotion(PodEntry* e, unsigned char* car)
{
    int t;
    int cur;
    int v;

    /*
        newEmo is NOT cleared here, as Lupo 2's updater clears car+0x5F6: an
        event can set a state between two frames (Pod_OnCarEvent), and its
        one-shot has to survive until the voice has seen it. The hook clears
        it after the voice update instead.
    */

#if POD_EMOTION_SOURCE == 0
    Pod_EmotionInputs(e, car);
#elif POD_EMOTION_SOURCE == 1
    (void)car;
    Pod_EmotionDemo(e);
#else
    (void)car;
#endif

    t = e->timer;
    if (t != 0 && t != 0xFF) {
        e->timer = (unsigned char)(--t);
        if (t == 0)
            Pod_SetEmotion(e, 0, -1);
    }

    cur = e->emo & 0xF;
    if (e->cool > 0) {
        e->cool--;
        if (g_emoCat[cur] == 0 && e->cool < POD_COOL_CUT)
            e->cool = 0;
    }

    v = (g_emoAttr[cur] == 1 || g_emoAttr[cur] == -1) ? g_emoAttr[cur] : 0;
    if (Pod_Tri(e->phase) == 0) {
        if (v > 0) {
            if (e->mood < POD_MOOD_MAX)
                e->mood++;
        } else if (v < 0) {
            if (e->mood > -POD_MOOD_MAX)
                e->mood--;
        } else if (e->mood > 0) {
            e->mood--;
        } else if (e->mood < 0) {
            e->mood++;
        }
    }

    if (e->mood == 0 && (g_emoAttr[cur] == 2 || Pod_Tri(e->phase) != 0))
        e->phase = (unsigned char)((e->phase + 1) % POD_TRI_P);
}

/* --------------------------------------------------------- the switch query */

/*
    One vtable, shared by every Pod CarModel: retail's CarModel::Callback table
    with slot 2's function replaced.

    Swapping the vptr of a Pod's CarModel rather than patching the shared table
    means no other car ever runs this code - retail's query stays retail's for
    everything else, and nothing here has to recognise a Pod at draw time. The
    copy keeps every other slot, the destructor included.
*/
static unsigned int g_podVtable[CARMODEL_CB_VTABLE_WORDS];

/*
    Retail's query for ids 0..5, Lupo 2's (0x0022D9E8, with the render outputs
    of 0x001E17D8 / 0x001E18A0) for 6..10:

        id  6  setBlend(0); arm 0          the Pod's cm+0x24 weight is 0
        id  7  THE TAIL'S MOTION
               mood != 0: setBlend(0.5 - mood/60); arm 0   - the side-to-side
               mood == 0: setBlend(0.5 + tri/24);  arm 1   - the up-down bob
        id  8  THE COLOUR CATEGORY
               cool <= 24: setBlend((24 - cool)/24); arm = category(cur)
               cool >  24: setBlend((cool - 24)/12); arm = category(prev)
        (at 60 Hz; the POD_* frame-rate constants above give the 50 Hz ones)
        id  9  arm = current state
        id 10  arm = select                THE FACE

    Every float is computed here and handed to setBlend as raw bits.
    Called by the game's SWITCH handler (US 0x002255A0) as int f(void*, int).
*/
int Pod_GetSwitch(void* obj, int id)
{
    PodEntry* e;
    int cur;
    int prev;
    int ret;

    if (id < 6 || id > 10)
        return CarModel_getSwitch(obj, id);

    e = Pod_FindByCm(obj);
    if (!e || !e->isPod)
        return CarModel_getSwitch(obj, id);

    cur  = e->emo & 0xF;
    prev = (e->emo >> 4) & 0xF;

    switch (id) {
    case 6:
        Pod_SetBlendBits(F32_0_0);
        ret = 0;
        break;

    case 7:
        if (e->mood != 0) {
            Pod_SetBlendBits(Pod_F2U(0.5f - (float)e->mood / POD_MOOD_DIV));
            ret = 0;
        } else {
            Pod_SetBlendBits(Pod_F2U(0.5f + (float)Pod_Tri(e->phase) / POD_TRI_DIV));
            ret = 1;
        }
        break;

    case 8:
        if (e->cool <= POD_COOL_NEU) {
            Pod_SetBlendBits(Pod_F2U((float)(POD_COOL_NEU - e->cool) / POD_FADEIN_DIV));
            ret = g_emoCat[cur];
        } else {
            Pod_SetBlendBits(Pod_F2U((float)(e->cool - POD_COOL_NEU) / POD_FADEOUT_DIV));
            ret = g_emoCat[prev];
        }
        break;

    case 9:
        ret = cur;
        break;

    default:            /* 10 */
        ret = (e->applied >= 0 && e->applied <= 10) ? e->applied : cur;
        break;
    }
    return ret;
}

/*
    Put a CarModel on the Pod vtable, or take it off again.

    Only a vptr that is exactly retail's is replaced. Anything else means the
    callback object is not what this was verified against, and the face will
    not appear - so it is logged rather than guessed at.
*/
static void Pod_SetVtable(void* cm, int pod)
{
    unsigned int* vptr = (unsigned int*)cm;

    if (pod) {
        if (*vptr == CARMODEL_CB_VTABLE) {
            *vptr = (unsigned int)g_podVtable;
            LOG("[gt3hooks] pod: CarModel %08X now answers switch ids 6..10\n",
                (unsigned int)cm);
        } else if (*vptr != (unsigned int)g_podVtable) {
            LOG("[gt3hooks] pod: CarModel %08X has vptr %08X, expected %08X - "
                "switch override NOT installed\n",
                (unsigned int)cm, *vptr, CARMODEL_CB_VTABLE);
        }
    } else if (*vptr == (unsigned int)g_podVtable) {
        *vptr = CARMODEL_CB_VTABLE;
    }
}

/* ------------------------------------------------------------ the face data */

/*
    Is this the Pod's model?

    Retail never animates a car model, so an ordinary car carries no palette
    animation at all. The Pod's face texture carries 27 records under ids 1..10.
    Requiring both ends of that range is specific enough, and needs nothing from
    the paramDB - the special-kind byte that marks a Pod in Lupo 2 (POD_PORT_SPEC
    step R1) does not exist in retail yet.
*/
static int Pod_IsPodModel(void* model)
{
    unsigned short texCount;
    void** texArray;
    int i;
    int j;
    int has1  = 0;
    int has10 = 0;

    if (!model)
        return 0;

    texCount = *(unsigned short*)((char*)model + 0x16);
    texArray = *(void***)((char*)model + 0x2C);
    if (!texArray)
        return 0;

    for (i = 0; i < (int)texCount; i++) {
        void* blk;
        char* recs;
        int count;

        if (!texArray[i])
            continue;
        blk = *(void**)((char*)texArray[i] + 0x24);
        if (!blk)
            continue;
        count = (int)*(unsigned short*)blk;
        recs  = *(char**)((char*)blk + 4);
        if (!recs)
            continue;
        for (j = 0; j < count; j++) {
            int id = (int)*(unsigned short*)(recs + j * 0x20);
            if (id == 1)
                has1 = 1;
            if (id == 10)
                has10 = 1;
        }
    }
    return has1 && has10;
}

/*
    Lupo 2's rebind, reproduced: on a change of select, reset only the records
    whose id is the new select, and mark them dirty so the next apply writes
    their first frame. Lupo 2 0x002321D8 -> 0x00264720 per texture.

    Retail's Model_rebind (US 0x002248D0) resets and APPLIES every record of
    every id instead, stamping the other emotions' first-frame palettes into
    their CLUTs as well - not what Lupo 2 does, so it is not used here.
*/
static void Pod_RebindSelect(void* model, int sel)
{
    unsigned short texCount = *(unsigned short*)((char*)model + 0x16);
    void** texArray = *(void***)((char*)model + 0x2C);
    int i;
    int j;

    if (!texArray)
        return;

    for (i = 0; i < (int)texCount; i++) {
        void* blk;
        char* recs;
        int count;

        if (!texArray[i])
            continue;
        blk = *(void**)((char*)texArray[i] + 0x24);
        if (!blk)
            continue;
        count = (int)*(unsigned short*)blk;
        recs  = *(char**)((char*)blk + 4);
        if (!recs)
            continue;
        for (j = 0; j < count; j++) {
            char* rec = recs + j * 0x20;
            if (sel >= 0 && (int)*(unsigned short*)rec != sel)
                continue;
            AnimRecord_reset(rec);
            rec[0x1F] = 1;
        }
    }
}

/* ---------------------------------------------------------------- the squat */

/*
    Called by Pod_SuspWrap for every wheel of every car, every physics step.
    Returns 1 when this wheel should carry no spring for this one force call.

    Lupo 2, reversed from 0x001E1C18 / 0x001E1C88 / 0x001E1D00:

        wanted   isPod && side brake >= 1.0 && planar speed <= 1/3.6 m/s
        counter  +1 while wanted, up to POD_SQUAT_MAX ("sat down", 34 at
                 60 Hz); -1 otherwise
        wheel i  off once counter > POD_SQUAT_STEP*r (7*r at 60 Hz), where
                 r = (i + slot) & 3, reversed
                 when (slot & 7) >= 4 - so the four corners drop one after
                 another rather than all at once

    Lupo 2 ticks the counter once per car per step before the dynamics run; the
    suspension loop visits wheel 0 first, so ticking on wheel 0 is the same
    ordering.

    The floats read here are fields of the car, compared inside this function.
    None of them crosses a call.
*/
/*
    1 km/h in m/s: Lupo 2's own bits (0x3E8E38E3, L2 [0x0035B6A8]; GT Concept
    the same), compared as Lupo 2 does - sqrt.s of the squared planar speed
    against it (L2 0x001E1C60). Comparing the square against a squared literal
    instead is not the same test: at two float values right at 1 km/h the two
    disagree, whichever way the literal is rounded (PDIFF_pod.py, test T7b).
    The bits go through a volatile so GCC cannot fold or round them.
*/
#if POD_SQUAT
static float Pod_Kmh1(void)
{
    volatile union { unsigned int u; float f; } k;
    k.u = 0x3E8E38E3u;
    return k.f;
}

static float Pod_Sqrt(float x)
{
    float r;
    __asm__ ("sqrt.s %0, %1" : "=f"(r) : "f"(x));
    return r;
}
#endif

#if POD_SQUAT && POD_DIAG
static int g_squatLogged = 0;
#endif

int Pod_SquatWheelOff(unsigned char* car, int i)
{
#if POD_SQUAT
    PodEntry* e = Pod_FindByCar(car);
    int s;
    int r;

    if (!e || !e->isPod)
        return 0;

    if (i == 0) {
        float brake = *(float*)(car + CAR_SIDEBRAKE_OFF);
        float vx    = *(float*)(car + CAR_VEL_X_OFF);
        float vz    = *(float*)(car + CAR_VEL_Z_OFF);

        if (!(brake < 1.0f) && !(Pod_Sqrt(vx * vx + vz * vz) > Pod_Kmh1())) {
            if (e->squat < POD_SQUAT_MAX)
                e->squat++;
        } else if (e->squat) {
            e->squat--;
        }

#if POD_DIAG
        if (!g_squatLogged && e->squat == 1) {
            g_squatLogged = 1;
            LOG("[gt3hooks] pod/squat: car %08X starting to sit\n",
                (unsigned int)car);
        }
#endif
    }

    if (!e->squat)
        return 0;

    s = *(unsigned char*)(car + CAR_SLOT_OFF);
    r = (i + s) & 3;
    if ((s & 7) >= 4)
        r = 3 - r;
    return e->squat > POD_SQUAT_STEP * r;
#else
    (void)car;
    (void)i;
    return 0;
#endif
}

/* ------------------------------------------------------- a new race's cars */

/*
    Replaces the game's setModel call for race cars (RACECAR_SETMODEL_SITE),
    reached once per car per race, just after the race entry - CarModel
    included - has been built afresh.

    Everything the plugin knows about the CarModel at `cm` is now wrong,
    whatever the addresses say: the constructor put the retail vptr back
    (undoing the switch override), the model is a new copy whose animation
    records have never been bound, and Lupo 2 resets the emotions for a new
    race too (0x001E12A0). So forget the entry; the next frame claims it again
    and classifies the car from scratch, exactly as in the first race.

    Nothing of the object is touched here - the game is still setting it up.
*/
int Pod_SetModelHook(void* cm, void* model, int reloc)
{
    PodEntry* e = Pod_FindByCm(cm);

    if (e) {
        Pod_VoiceStop(&e->voice);
        Pod_ClearEntry(e);
#if POD_DIAG
        LOG("[gt3hooks] pod: CarModel %08X given model %08X - classified afresh "
            "next frame\n", (unsigned int)cm, (unsigned int)model);
#endif
    }
    return CarModel_setModel(cm, model, reloc);
}

/* ------------------------------------------------------------ crash events */

/*
    Called by Pod_CrashThunk (PodThunk.S) for every crash event the game raises
    - a wall, another car or a loose course object - for every car, inside the
    race step, up to twice per car per step, and again every step a contact
    lasts. Lupo 2 put three Pod calls at the head of the same notifier; this is
    them.

    type 0, a wall                          -> naku           (L2 0x001E1A00)
    type 1, another car, by `kind`:            (L2 0x001E1A48)
        own corner 1, 2 (one end's face)    -> odoroku
        own corner 5, 6 (the other end's)   -> kowai
        own corner 0, 3, 4, 7 (flanks)      -> okoru
        own edge struck, 8..10 (first end)  -> odoroku, POD_HOLD_CRASH steps
        own edge struck, 11 or 15 (sides)   -> okoru,   POD_HOLD_CRASH steps
        own edge struck, 12..14 (other end) -> kowai,   POD_HOLD_CRASH steps
    type 2, a loose course object           -> odoroku        (L2 0x001E1B80)

    Which end is the front is inferred, not proven; the table is Lupo 2's by
    number, so it does not depend on it. Pod_SetEmotion's cool-down, weak and
    lock rules are Lupo 2's own, so repeated events behave as they did there.

    Only integers cross: the float strength stays in the thunk. No logging -
    this runs inside the physics step.
*/
#if POD_EMOTION_SOURCE == 0
static void Pod_CrashEmotion(PodEntry* e, int type, int kind)
{
    if (type == 0) {
        Pod_SetEmotion(e, 9, -1);
    } else if (type == 1) {
        if ((unsigned int)(kind - 1) < 2u)
            Pod_SetEmotion(e, 5, -1);
        else if ((unsigned int)(kind - 5) < 2u)
            Pod_SetEmotion(e, 8, -1);
        else if (kind < 8)
            Pod_SetEmotion(e, 4, -1);
        else if (kind < 11)
            Pod_SetEmotion(e, 5, POD_HOLD_CRASH);
        else if (kind == 11 || kind == 15)
            Pod_SetEmotion(e, 4, POD_HOLD_CRASH);
        else
            Pod_SetEmotion(e, 8, POD_HOLD_CRASH);
    } else if (type == 2) {
        Pod_SetEmotion(e, 5, -1);
    }
}
#endif

void Pod_OnCarEvent(void* car, int type, int kind)
{
#if POD_EMOTION_SOURCE == 0
    PodEntry* e = Pod_FindByCar(car);

    if (e && e->isPod)
        Pod_CrashEmotion(e, type, kind);
#else
    (void)car;
    (void)type;
    (void)kind;
#endif
}

#if POD_EMOTION_SOURCE == 0
/*
    Route the notifier's five call sites through Pod_CrashThunk. Each is
    checked to still be `jal notify` before it is replaced; the delay slots
    are left alone and run exactly as before.
*/
static void Pod_InstallCrashHooks(void)
{
    static const unsigned int sites[5] = {
        CRASH_SITE_OBJECT, CRASH_SITE_CAR_STRUCK, CRASH_SITE_CAR_STRIKER,
        CRASH_SITE_WALL_CORNER, CRASH_SITE_WALL_EDGE
    };
    int i;
    int n = 0;

    for (i = 0; i < 5; i++) {
        unsigned int w = *(volatile unsigned int*)sites[i];

        if (w == CRASH_NOTIFY_JAL_WORD) {
            MAKE_JAL(sites[i], (void*)Pod_CrashThunk);
            n++;
        } else {
            LOG("[gt3hooks] pod: crash site %08X holds %08X, expected %08X - "
                "not hooked\n", sites[i], w, CRASH_NOTIFY_JAL_WORD);
        }
    }
    LOG("[gt3hooks] pod: %d of 5 crash-event sites hooked\n", n);
}
#endif

/* ----------------------------------------------------------------- the hook */

/*
    Called from Pod_UpdateTailThunk at the end of every RaceCarModel::update.
*/
void Pod_OnCarModelUpdated(void* cm, void* car)
{
    PodEntry* e;
    void* model;
    int select;

    if (!cm)
        return;

#if POD_DIAG
    Pod_SoundWatch();               /* read-only: the game's race SE controls */
#endif

    e = Pod_FindByCm(cm);
    if (!e)
        e = Pod_Claim(cm);
    Pod_SetCar(e, car);
    e->seen = ++g_podSeq;

    /*
        Classify on first sight, and again whenever the CarModel is handed a
        different model. A new race's CarModel normally arrives through
        Pod_SetModelHook, which forgets the entry; as a second line, a Pod whose
        CarModel carries the retail vptr again has been rebuilt behind our back
        (the constructor writes it), so it is classified afresh as well.
    */
    model = *(void**)((char*)cm + CARMODEL_MODEL_OFF);
    if (model != e->model ||
        (e->isPod && *(unsigned int*)cm == CARMODEL_CB_VTABLE)) {
        Pod_VoiceStop(&e->voice);   /* a Pod's voice does not outlive it */
        e->model   = model;
        e->isPod   = Pod_IsPodModel(model);
        e->applied = POD_SELECT_NONE;
        e->squat   = 0;
        Pod_EmotionReset(e);
#if POD_EMOTION_SOURCE == 2
        if (e->isPod)
            Pod_SetEmotion(e, POD_FORCE_SELECT, 255);
#endif
        Pod_SetVtable(cm, e->isPod);
        if (e->isPod)
            Pod_SoundCheck();       /* once: logs what se.inf gave the pod_* names */
#if POD_DIAG
        LOG("[gt3hooks] pod: cm=%08X car=%08X model=%08X -> %s\n",
            (unsigned int)cm, (unsigned int)car, (unsigned int)model,
            e->isPod ? "POD" : "not a Pod");
#endif
    }

    /*
        Only the Pod is animated. Retail never animates a car model at all.
    */
    if (!e->isPod || !model)
        return;

    Pod_TickEmotion(e, (unsigned char*)car);

    /* Lupo 2 voices each change of state on the step it happens, and keeps
       the voice's level and pan following the Pod while it sounds. */
    Pod_VoiceUpdate(&e->voice, e->newEmo, cm);
    e->newEmo = 0;

    select = e->emo & 0xF;
    if (e->applied != select) {
        /*
            Logged here rather than off newEmo: Lupo 2's one-shot uses 0 for "no
            change", and neutral is state 0, so a return to neutral never shows
            there.
        */
#if POD_DIAG
        LOG("[gt3hooks] pod/emo: %s -> %s\n",
            (e->applied >= 0 && e->applied <= 10) ? g_emoName[e->applied] : "-",
            g_emoName[select]);
#endif
        Pod_RebindSelect(model, select);
        e->applied = select;
    }

    Pod_ModelAnimate(model, select, POD_FRAME_DT_BITS,
                     (void*)ADDR_Model_animate);
    Model_reset(model);
}

void Pod_InstallHooks(void)
{
    const unsigned int* retail = (const unsigned int*)CARMODEL_CB_VTABLE;
    int i;

    g_podSeq = 0;                   /* .bss is not cleared for the plugin */
    for (i = 0; i < POD_ENTRIES; i++)
        Pod_ClearEntry(&g_pod[i]);

    for (i = 0; i < CARMODEL_CB_VTABLE_WORDS; i++)
        g_podVtable[i] = retail[i];
    g_podVtable[CARMODEL_CB_SWITCH_SLOT] = (unsigned int)Pod_GetSwitch;

    MAKE_JAL(ADDR_RaceCarModel_update_tail, (void*)Pod_UpdateTailThunk);

    if (*(volatile unsigned int*)RACECAR_SETMODEL_SITE == RACECAR_SETMODEL_WORD) {
        MAKE_JAL(RACECAR_SETMODEL_SITE, (void*)Pod_SetModelHook);
    } else {
        LOG("[gt3hooks] pod: setModel site %08X holds %08X, expected %08X - "
            "a Pod may not be recognised after the first race\n",
            RACECAR_SETMODEL_SITE, *(volatile unsigned int*)RACECAR_SETMODEL_SITE,
            RACECAR_SETMODEL_WORD);
    }

#if POD_SQUAT
    if (*(volatile unsigned int*)ADDR_SquatSeam == SQUAT_SEAM_WORD) {
        MAKE_JAL(ADDR_SquatSeam, (void*)Pod_SuspWrap);
        LOG("[gt3hooks] pod: squat hooked at %08X\n", ADDR_SquatSeam);
    } else {
        LOG("[gt3hooks] pod: squat seam %08X holds %08X, expected %08X - "
            "squat NOT installed\n",
            ADDR_SquatSeam, *(volatile unsigned int*)ADDR_SquatSeam,
            SQUAT_SEAM_WORD);
    }
#endif

#if POD_EMOTION_SOURCE == 0
    Pod_InstallCrashHooks();
#endif

    Pod_InstallSound();

    LOG("[gt3hooks] pod: RaceCarModel::update tail hooked at %08X, emotion source %d\n",
        ADDR_RaceCarModel_update_tail, POD_EMOTION_SOURCE);
}
