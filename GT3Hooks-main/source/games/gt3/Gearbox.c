#include "Drive.h"

#include "core/Log.h"
#include "GameConfig.h"

/*
    Lupo 2's gearbox types on retail GT3.

    A gearbox type (0 stock, 1 CVT, 2 clutchless stepped, 3 electric single
    speed) travels GEAR row -> car spec +0xE1 -> Transmission +0x12, exactly
    Lupo 2's path, through two bytes retail never uses. Lupo 2 then consults it
    at 33 places in its physics through four predicates; every one of those
    decisions is reproduced here, at the retail twin of the code it sits in.
    Nine Lupo 2 functions have no retail twin and are rebuilt below.

    For a type-0 car every hook hands straight back to the stock code (in most
    places without reaching C at all), so a stock car drives exactly as before.

    Drive.c installs the hooks; docs/gearbox_dualnote.md has the Lupo 2
    original (L2 ...) and the retail site behind every function here.
*/

#define K_1KMH      0x3E8E38E3u     /*  1/3.6, Lupo 2's bits               */
#define K_M1KMH     0xBE8E38E3u     /* -1/3.6                              */
#define K_2PI_60    0x3DD6774Eu     /*  2 pi / 60                          */
#define K_2PI       0x40C90FDAu

static int g_gbxOn = 0;             /* Drive.c installed every gearbox hook */

/* The CVT controller's rates, computed at run time as Lupo 2's 0x001DF9C8
   does - (float)byte * 0.01f - so they round as the EE rounds. */
static float g_R40, g_R15, g_R150a, g_R150b;

/* ---------------------------------------------------------------- the game */

static u8*  (*CarDataBase_getBlock)(void* db, int table) = (void*)ADDR_CarDataBase_getBlock;
static void (*ApplyGear)(u8* eq, u8* spec, const u8* row) = (void*)ADDR_applyGear;
static void (*TransCtor)(u8* spec, u8* T, void* eng, int a3) = (void*)ADDR_TransCtor;
static void (*TopSpeed)(u8* m10, int a1) = (void*)ADDR_TopSpeed;
static void (*PowerCurve)(void* engine, u8* buf, int a1) = (void*)ADDR_PowerCurve;
static void (*RpmClutch)(void* car) = (void*)ADDR_RpmClutch;
static void (*ShiftHandler)(void* car) = (void*)ADDR_ShiftHandler;
static int  (*GearRequest)(void* car, s8* out) = (void*)ADDR_GearRequest;
static void (*SetGear)(u8* blk, int gear) = (void*)ADDR_SetGear;
static void (*EngineState)(void* car) = (void*)ADDR_EngineState;
static void (*AutoClutch)(void* car, u8* in, s8* out, int mode) = (void*)ADDR_AutoClutch;
static void (*AiClutch)(void* car, void* a1) = (void*)ADDR_AiClutch;
static void (*AiDriveA)(void* ai) = (void*)ADDR_AiDriveA;
static void (*AiDriveB)(void* ai) = (void*)ADDR_AiDriveB;
static void (*EngineSound)(void* car) = (void*)ADDR_EngineSound;

/* ------------------------------------------------------ the type per GEAR row */

/*
    Lupo 2's typed GEAR rows (paramdb_bs.db table 17, byte +0x30), for a
    stock-width retail table that has no room for the byte. A Lupo 2 car
    transplanted with its own GEAR rows keeps its row ids, and so its gearbox.
    Add a line here to give any other gearbox a type.
*/
static const struct { u64 id; u8 type; } g_gbxRows[] = {
    { 0x9A14A0D09C9FC739ULL, 1 },   /* 2 gears, final 5.777                  */
    { 0xFDAF6ACCF479AD10ULL, 1 },   /*   the same box, second car            */
    { 0x52C97F9F940BEB54ULL, 2 },   /* 5 gears, final 4.312                  */
    { 0x52C99C83A40A4A14ULL, 2 },   /* 7 gears, final 3.545                  */
    { 0x52C99C83A40A4A77ULL, 2 },   /*   its fully customisable row          */
    { 0x5733B01093994955ULL, 2 },   /* 5 gears, final 3.333                  */
    { 0x5733B01693994958ULL, 2 },
    { 0x6F32731F64374553ULL, 2 },   /* 7 gears, customisable                 */
    { 0xD6D8A338F5B7A653ULL, 2 },   /* 6 gears, final 3.940                  */
    { 0xD6D8A338F5B7D656ULL, 2 },
    { 0xE54D86D4F54A3D3BULL, 2 },   /* 5 gears, final 2.820                  */
    { 0xE55086D4F54A3D3EULL, 2 },
    { 0xEC6AC1A6FC8967DAULL, 2 },   /* 5 gears, final 2.823                  */
    { 0xEC6AC1A7148967DDULL, 2 },
    { 0xF6DD4F2CF548AD1DULL, 2 },   /* 6 gears, final 3.540                  */
    { 0xF6E04F2CF548AD20ULL, 2 },
    { 0, 0 }
};

static int g_gbxWidthLogged = 0;

static int GB_RowType(u8* eq, const u8* row)
{
    const u8* blk;
    u32 es, n, i;
    u64 id;

    if (!row)
        return 0;
    /* Looked up every time: the game frees and reloads the paramDB buffer. */
    blk = CarDataBase_getBlock(eq + CAREQ_DB_OFF, PARAMDB_TBL_GEAR);
    if (!blk)
        return 0;
    es = AT16(blk, DATABLOCK_ELEMSIZE_OFF);
    n  = AT16(blk, DATABLOCK_NUM_OFF);
    if (!g_gbxWidthLogged) {
        g_gbxWidthLogged = 1;
        LOG("[gt3hooks] gbx: GEAR table %u rows x 0x%X - types from %s\n", n, es,
            es > GEAR_ROW_TYPE_OFF ? "row +0x30" : "the plugin's list of row ids");
    }
    if (row < blk + DATABLOCK_ROWS_OFF || row >= blk + DATABLOCK_ROWS_OFF + n * es)
        return 0;                                   /* not a GEAR row */
    if (es > GEAR_ROW_TYPE_OFF)
        return row[GEAR_ROW_TYPE_OFF];              /* widened table: Lupo 2's layout */

    id = *(const u64*)row;                          /* stock 0x30-byte rows */
    for (i = 0; g_gbxRows[i].id; i++)
        if (g_gbxRows[i].id == id)
            return g_gbxRows[i].type;
    return 0;
}

/*
    Replaces applyGear's only call (APPLYGEAR_SITE): the original, then Lupo
    2's copy of the type into the spec (L2 0x00241BC0/0x00241BC8).
*/
void GB_ApplyGear(u8* eq, u8* spec, const u8* row)
{
    int t;

    ApplyGear(eq, spec, row);
    t = (GBX_FORCE_TYPE >= 0) ? GBX_FORCE_TYPE : GB_RowType(eq, row);
    spec[SPEC_GEARBOX_TYPE_OFF] = (u8)((t >= 0 && t <= 3) ? t : 0);
}

/* ------------------------------------------------------------ per-car state */

int GB_Type(const void* car)
{
    return g_gbxOn ? AT8(TRANS(car), TRANS_TYPE_OFF) : 0;
}

/*
    Lupo 2's one gearbox value with no retail field: the ratio factor of the
    CVT and the clutchless box (L2 blk+0x3A4). One per race slot; a new car in
    the slot starts at 1.0, which every Lupo 2 reader tolerates - each read
    follows a write in the same state sequence.
*/
static struct { void* car; float f; } g_factor[8];

static float* GB_Factor(void* car)
{
    int s = AT8(car, CAR_SLOT_OFF) & 7;

    if (g_factor[s].car != car) {
        g_factor[s].car = car;
        g_factor[s].f   = 1.0f;
    }
    return &g_factor[s].f;
}

/* ------------------------------------------------ the Transmission, at setup */

/*
    Wraps the Transmission constructor's calls: afterwards gears 2 and up of a
    type-1/2 box become ratios relative to 1st (L2 0x001E3900..0x001E396C),
    which is what the type-aware readers below expect.
*/
void GB_CtorWrap(u8* spec, u8* T, void* engine, int a3)
{
    int t, n, k;

    TransCtor(spec, T, engine, a3);
    t = AT8(T, TRANS_TYPE_OFF);
    n = AT8(T, TRANS_COUNT_OFF);
    if ((t == 1 || t == 2) && n >= 2)
        for (k = 2; k <= n; k++)
            ATF(T, TRANS_RATIO_OFF + 4 * k) =
                (float)ATS16(spec, SPEC_RATIO_OFF + 2 * k) /
                (float)ATS16(spec, SPEC_RATIO_OFF + 2);
}

/* L2 0x001DF910: hold engine speed at the CVT's target rpm, by ratio. */
static void GB_CvtLimit(u8* T, float* w, float* fac)
{
    float lim = (float)AT16(T, TRANS_CVTRPM_OFF) * KF(K_2PI_60);
    float top;

    if (*w < lim) {
        *fac = 1.0f;
        return;
    }
    *fac = lim / *w;
    top  = ATF(T, TRANS_RATIO_OFF + 4 * AT8(T, TRANS_COUNT_OFF));
    if (*fac < top)
        *fac = top;
    *w = *fac * *w;
}

/*
    Replaces the top-speed estimate's call in the parameter setup
    (TOPSPEED_SITE). Type 3: a fixed 50 m/s. Type 1: the CVT's target rpm (L2
    0x001DBC00), then the stock loop with the CVT's limiter and no gear steps
    (L2 0x001DB940). The AI drives by this estimate.
*/
static void GB_TopSpeedCvt(u8* m10, int a1)
{
    u8* T = m10 + (CAR_TRANS_OFF - CAR_PARAM_OFF);
    void* engine = m10 + PARAM_ENGINE_OFF;
    int n, step, i;

    n = (int)(ATF(m10, PARAM_TOPSPEED_OFF) * KF(0x40666666));
    if (n <= 0)
        return;
    step = n / 500 + 1;
    for (i = 0; i < n; i += step) {
        float v   = (float)i * KF(K_1KMH);
        float v2  = v * v;
        float r   = ATF(m10, PARAM_RATIO1_OFF);
        float w   = v * r;
        float fac = 1.0f;
        float tq, net, lk, drive, res;

        GB_CvtLimit(T, &w, &fac);
        G_Call_PI_F(ADDR_EngCurveTorque, engine, a1, &w, &tq);
        G_Call_P_FF(ADDR_EngCurveNet, engine, &tq, &w, &net);
        drive = (net * r) * fac;
        G_Call_P_F(ADDR_Lookup, (void*)TOPSPEED_DRAG_TABLE, &v, &lk);
        res = (lk * ATF(TOPSPEED_DRAG_K, 0)) *
              ((ATF(m10, PARAM_DRAG0_OFF) + ATF(m10, PARAM_DRAG2_OFF) * v2) +
               ATF(m10, PARAM_DRAG3_OFF) * v2) +
              ATF(m10, PARAM_DRAG1_OFF) * v2;
        if (res < drive)
            ATF(m10, PARAM_TOPSPEED_OFF) = v;
    }
}

void GB_TopSpeedWrap(u8* m10, int a1)
{
    u8* T = m10 + (CAR_TRANS_OFF - CAR_PARAM_OFF);
    int t = g_gbxOn ? AT8(T, TRANS_TYPE_OFF) : 0;

    if (t == 3) {
        AT32(m10, PARAM_TOPSPEED_OFF) = 0x4247FFFFu;      /* 49.999996 */
        return;
    }
    if (t == 1) {
        u8 buf[0xC0] __attribute__((aligned(16)));
        int r, lim;

        PowerCurve(m10 + PARAM_ENGINE_OFF, buf, a1);
        r   = *(u16*)(buf + 8);                          /* rpm at peak power */
        lim = (int)AT16(T, TRANS_REDLINE_OFF) - 500;
        AT16(T, TRANS_CVTRPM_OFF) = (u16)((r < lim) ? r : lim);
        GB_TopSpeedCvt(m10, a1);
        return;
    }
    TopSpeed(m10, a1);
}

/* ------------------------------------------------------- shifting, per step */

/* L2 0x001E5648: the type-3 tachometer - motor rpm, no idle floor. */
static void GB_T3Display(u8* blk)
{
    ATS16(blk, PHYS_RPM_OFF) =
        (s16)(int)((ATF(blk, PHYS_OMEGA_OFF) * KF(0x42700000)) / KF(K_2PI));
}

/*
    L2 0x001E5678, the type-1/3 R/D selector. The driver's reverse request
    selects R with no speed check; otherwise D once rolling forward above
    1 km/h or on throttle, and R once rolling back above 1 km/h. There is no
    neutral.
*/
static void GB_Selector(void* car, int t)
{
    u8* blk = BLK(car);
    int g = AT8(blk, PHYS_GEAR_OFF);
    int req = -1;

    if (ATS8(blk, PHYS_REVERSE_OFF)) {
        if (g)
            req = 0;
    } else {
        float v = ATF(blk, PHYS_SPEED_OFF);
        if (KF(K_1KMH) < v || 0.0f < ATF(blk, PHYS_THROTTLE_OFF)) {
            if (!g)
                req = 1;
        } else if (v < KF(K_M1KMH) && g) {
            req = 0;
        }
    }
    if (req != -1) {
        AT8(blk, PHYS_GEAR_OFF) = (u8)req;
        if (t == 1)
            AT8(blk, PHYS_CSTATE_OFF) = 2;               /* re-engage the clutch */
    }
}

/* L2 0x001E5558 for type 2: a forward change keeps the clutch state. */
static void GB_ShiftT2(void* car)
{
    u8* blk = BLK(car);
    int req = GearRequest(car, (s8*)car + CAR_DRIVEIN_OFF);

    if (req == -1)
        return;
    if (AT8(blk, PHYS_CSTATE_OFF) && (req == 0 || AT8(blk, PHYS_GEAR_OFF) == 0))
        AT8(blk, PHYS_CSTATE_OFF) = 2;
    SetGear(blk, req);
}

/* Replaces the rpm/clutch machine + shift handler pair (SHIFT_SITE). */
void GB_Shift(void* car)
{
    int t = GB_Type(car);

    if (t == 3) {
        GB_T3Display(BLK(car));
        GB_Selector(car, 3);
        return;
    }
    RpmClutch(car);
    if (t == 1) {
        GB_Selector(car, 1);
        return;
    }
    if (t == 2) {
        GB_ShiftT2(car);
        return;
    }
    ShiftHandler(car);
}

/* ------------------------------------------------------ torque, per step */

/* L2 0x001DFA38, the CVT controller: engine torque through the ratio factor. */
static float GB_CvtTorque(void* car)
{
    u8* blk = BLK(car);
    u8* T = TRANS(car);
    float* factor = GB_Factor(car);
    int t = GB_Type(car);
    float f = 1.0f;
    float thr, tq;

    if (AT8(blk, PHYS_CSTATE_OFF) == 1 && AT8(blk, PHYS_GEAR_OFF) != 0) {
        float acc = ATF(blk, PHYS_ACCEL_OFF);
        float s   = ATF(blk, PHYS_SHAFTRPM_OFF);
        float top = ATF(T, TRANS_RATIO_OFF + 4 * AT8(T, TRANS_COUNT_OFF));
        float tgt = 1.0f;
        float rate;
        int mark = 0;

        f = ATF(blk, PHYS_OMEGA_OFF) / s;
        if (1.0f < f)
            f = 1.0f;
        else if (f < top)
            f = top;

        if (t == 1) {
            float e = ((float)AT16(T, TRANS_CVTRPM_OFF) * KF(K_2PI_60)) *
                      (acc * KF(0x3ECCCCCC) + KF(0x3F199999));
            if (e < s) {
                tgt = e / s;
                if (tgt < top)
                    tgt = top;
                else if (1.0f < tgt)
                    tgt = 1.0f;
            }
            rate = g_R15 + acc * (g_R40 - g_R15);
        } else {
            if (AT8(blk, PHYS_GEAR_OFF) >= 2)
                tgt = ATF(T, TRANS_RATIO_OFF + 4 * AT8(blk, PHYS_GEAR_OFF));
            rate = (tgt < f) ? g_R150a : g_R150b;
            /* the automatic is choosing: AI, a game-driven car, or AT mode */
            mark = (ATS8(blk, PHYS_AI_OFF) == 1 || AT8(blk, PHYS_AUTODRIVE_OFF) != 0 ||
                    AT8(blk, PHYS_TRANSMODE_OFF) == 0);
        }

        if (tgt != f) {
            float step = (rate * ATF(car, CAR_DT_OFF)) * *factor;
            int done;

            if (f < tgt) {
                f = f + step;
                done = (tgt <= f);
            } else {
                f = f - step;
                done = (f <= tgt);
            }
            if (done)
                f = tgt;
            else if (mark)
                AT8(blk, PHYS_ATTIMER_OFF) = 1;          /* no new gear mid-glide */
        }
    }
    *factor = f;

    thr = ATF(blk, PHYS_THROTTLE_OFF);
    G_Call_P_F(ADDR_EngineTorque, car, &thr, &tq);
    return tq * f;
}

/* L2 0x001DFCA0, the electric single speed's motor law. */
static float GB_EvMotor(void* car)
{
    u8* blk = BLK(car);
    u8* T = TRANS(car);
    int g   = AT8(blk, PHYS_GEAR_OFF);
    int ax  = AT8(T, TRANS_AXLE_OFF);
    float acc = ATF(blk, PHYS_THROTTLE_OFF);
    float w   = ATF(blk, PHYS_AXLESPD_OFF + 4 * ax) * ATF(T, TRANS_RATIO_OFF + 4 * g);
    float f22 = acc * KF(0x4392FFFF);                           /* 294            */
    float f25 = (w * KF(0x4343FFFF)) / KF(0x4402E651);          /* 196 at 5000 rpm */
    float a   = (g == 1) ? acc : 0.0f;
    float v   = ATF(blk, PHYS_SPEED_OFF);
    float f20, x, tq, om;

    /* motor-speed target, rad/s: none below 3 km/h, 1000 rpm to 10 km/h,
       then rising to the shift limit at 130 km/h */
    if (v < KF(0x3F555554))
        f20 = 0.0f;
    else if (v < KF(0x4031C71B))
        f20 = KF(0x42D17082);
    else if (KF(0x421071C6) < v)
        f20 = (float)AT16(T, TRANS_REDLINE_OFF) * KF(K_2PI_60);
    else
        f20 = ((v - KF(0x4031C71B)) *
               ((float)((int)AT16(T, TRANS_REDLINE_OFF) - 1000) * KF(K_2PI_60))) /
              KF(0x42055554) + KF(0x42D17082);
    f20 = f20 * a;
    if (f20 <= KF(0x42D13AE4))
        f20 = 0.0f;

    /* the engine curve only drives a servo toward that speed */
    x = (f20 - ATF(blk, PHYS_OMEGA_OFF)) * KF(0x3C1C74A8);
    if (1.0f < x)
        x = 1.0f;
    else if (x < 0.0f)
        x = 0.0f;
    G_Call_P_F(ADDR_EngineTorque, car, &x, &tq);

    om = ATF(blk, PHYS_OMEGA_OFF) +
         (tq * ATF(T, TRANS_SHAFTINERTIA_OFF)) * ATF(car, CAR_DT_OFF);
    ATF(blk, PHYS_OMEGA_OFF) = om;
    if (f20 == 0.0f && om < KF(0x42517082))
        ATF(blk, PHYS_OMEGA_OFF) = om * 0.5f;

    om = ATF(blk, PHYS_OMEGA_OFF);
    if (KF(0x42D17082) < om && KF(0x42517082) < w)
        f22 = f22 + (((ATF(blk, PHYS_TQMAX_OFF) - tq) * a) * om) / w;
    return f22 - f25;
}

/* The clutch-engaged torque (Drive_Torque): types 1..3 supply their own. */
int GB_Torque(void* car, float* out)
{
    int t = GB_Type(car);

    if (t == 3) {
        *out = GB_EvMotor(car);
        return 1;
    }
    if (t == 1 || t == 2) {
        *out = GB_CvtTorque(car);
        return 1;
    }
    return 0;
}

/* Before the drivetrain step: Lupo 2 runs the motor law in every clutch
   state, and drops its torque while the clutch is open. */
void GB_StepPreDrive(void* car)
{
    if (GB_Type(car) == 3 && AT8(BLK(car), PHYS_CSTATE_OFF) == 0)
        (void)GB_EvMotor(car);
}

/*
    The engaged-gearbox inertia term (INERTIA_SITE, types 1..3 only; the
    thunk keeps type 0 on the stock instructions). L2 0x001FBFFC/0x001FC020/
    0x001FC054: none for type 3; type 2's steps use index 1; types 1/2 scale
    by the ratio factor.
*/
u32 GB_Inertia(void* car)
{
    u8* blk = BLK(car);
    u8* T = TRANS(car);
    int t = GB_Type(car);
    int g = AT8(blk, PHYS_GEAR_OFF);
    int i;
    float x;

    if (t == 3)
        return 0;
    i = (t == 2 && g >= 2) ? 1 : g;
    x = ATF(T, TRANS_INERTIA_OFF) * ATF(T, TRANS_RATIO_OFF + 4 * i);
    if (!g)
        x = -x;
    if (t == 1 || t == 2)
        x = x * *GB_Factor(car);
    return FBITS(x);
}

/* ------------------------------------------------------- engine, per step */

/*
    rpm from shaft speed (RPMSHAFT sites, types 1/2 only): type 2's steps use
    ratio index 1; the clutch-engaged caller also scales by the ratio factor.
*/
void GB_RpmShaft(void* car, int cvt, float* out)
{
    u8* blk = BLK(car);
    int t = GB_Type(car);
    int g = AT8(blk, PHYS_GEAR_OFF);
    float s, x;

    G_Call_P_R(ADDR_ShaftSpeed, car, &s);
    if (t == 2 && g >= 2)
        g = 1;
    x = s * ATF(TRANS(car), TRANS_RATIO_OFF + 4 * g);
    if (x <= 0.0f)
        x = 0.0f;
    if (cvt && (t == 1 || t == 2) && AT8(blk, PHYS_CSTATE_OFF) == 1)
        x = x * *GB_Factor(car);
    *out = x;
}

/* Replaces the engine-state call (ENGSTATE_SITE): type 3 is engaged exactly
   when its clutch is (L2 0x001E4948); types 1/2 cache the shaft rpm. */
void GB_EngineStateWrap(void* car)
{
    u8* blk = BLK(car);
    int t = GB_Type(car);

    if (t == 3) {
        AT8(blk, PHYS_CSTATE_OFF) = (ATF(blk, PHYS_CLUTCH_OFF) == 0.0f) ? 0 : 1;
        return;
    }
    EngineState(car);
    if ((t == 1 || t == 2) && AT8(blk, PHYS_CSTATE_OFF) == 1) {
        float r;
        GB_RpmShaft(car, 0, &r);
        ATF(blk, PHYS_SHAFTRPM_OFF) = r;
    }
}

/* Rolling start (ROLL_SITE, types 1/2): the factor restarts at 1, and a CVT
   holds its target rpm (L2 0x001F4D58..0x001F4D6C). */
u32 GB_Roll(void* car, u32 f1bits)
{
    int t = GB_Type(car);
    float* factor;

    if (t != 1 && t != 2)
        return f1bits;
    factor = GB_Factor(car);
    *factor = 1.0f;
    if (t == 1) {
        union { u32 u; float f; } w;
        float fac = 1.0f;

        w.u = f1bits;
        GB_CvtLimit(TRANS(car), &w.f, &fac);
        *factor = fac;
        return w.u;
    }
    return f1bits;
}

/* ------------------------------------------------------- clutch, per step */

/* L2 0x001F9328: the type-3 automatic clutch - it holds the car on the brake
   at a standstill. */
static void GB_T3Clutch(u8* blk)
{
    float c;

    if (AT8(blk, PHYS_HOLD_OFF) || AT16(blk, PHYS_HOLDTIME_OFF) ||
        AT8(blk, PHYS_AUTODRIVE_OFF) == 8) {
        c = 0.0f;
    } else if (!AT8(blk, PHYS_CSTATE_OFF)) {
        c = (ATF(blk, PHYS_BRAKE_OFF) < KF(0x3DCCCCCC)) ? 1.0f : 0.0f;
    } else {
        c = 1.0f;
        if (KF(0x3DCCCCCC) < ATF(blk, PHYS_BRAKE_OFF)) {
            float v = ATF(blk, PHYS_SPEED_OFF);
            if (v < KF(K_1KMH) && KF(K_M1KMH) < v)
                c = 0.0f;
        }
    }
    ATF(blk, PHYS_CLUTCH_OFF) = c;
}

/* L2 0x001F93C8: type-3 creep, up to about 12 km/h. */
static void GB_T3Creep(u8* blk, u8* in, s8* out)
{
    float v, d;

    if (ATF(blk, PHYS_CLUTCH_OFF) == 0.0f)
        return;
    v = ATF(blk, PHYS_SPEED_OFF);
    d = AT8(blk, PHYS_GEAR_OFF) ? v : -v;
    if (KF(K_M1KMH) <= d) {
        float e = KF(0x40555554) - d;
        if (0.0f < e) {
            float x = ATF(blk, PHYS_ACCEL_OFF) + e / KF(0x41D55554);
            if (1.0f < x)
                x = 1.0f;
            ATF(blk, PHYS_ACCEL_OFF) = x;
        }
    }
    if (!ATS16(in, 8) && !ATS16(in, 0xC) && v < KF(K_M1KMH))
        out[0] = 1;                                  /* stay in R while coasting back */
}

/*
    Replaces the driver's automatic-clutch call (CLUTCH_SITE). Type 3 has its
    own clutch and creep. Types 1/2 open the clutch on the side brake at a
    standstill and add an idle-up governor (L2 0x001F926C..0x001F92B4,
    0x001FA4BC..0x001FA5BC).
*/
void GB_ClutchWrap(void* car, u8* in, s8* out, int mode)
{
    u8* blk = BLK(car);
    int t = GB_Type(car);

    if (t == 3) {
        GB_T3Clutch(blk);
        GB_T3Creep(blk, in, out);
        return;
    }
    AutoClutch(car, in, out, mode);
    if (t == 1 || t == 2) {
        float sb = ATF(blk, PHYS_SIDEBRAKE_OFF);
        float vx = ATF(blk, PHYS_SPEED_OFF);
        float vz = ATF(blk, PHYS_SPEEDZ_OFF);
        float sp = EE_sqrt(vx * vx + vz * vz);

        if (mode != 3 && KF(0x3F4CCCCC) < sb && sp < KF(K_1KMH))
            ATF(blk, PHYS_CLUTCH_OFF) = 0.0f;
        if (sb < KF(0x3F4CCCCC) || KF(K_1KMH) <= sp) {
            float a = ATF(blk, PHYS_TQMAX_OFF);
            float b = ATF(blk, PHYS_TQFRIC_OFF);
            float x = (((float)AT16(car, CAR_IDLE_OFF) + KF(0x43FA0000)) -
                       (float)ATS16(blk, PHYS_RPM_OFF)) * KF(0x3A03126E) + b / (a + b);
            if (x < 0.0f)
                x = 0.0f;
            else if (1.0f < x)
                x = 1.0f;
            if (ATF(blk, PHYS_ACCEL_OFF) < x)
                ATF(blk, PHYS_ACCEL_OFF) = x;
            if (!ATS16(in, 8) && !ATS16(in, 0xC) && !AT8(blk, PHYS_GEAR_OFF))
                out[0] = 1;
        }
    }
}

/* Replaces the AI's clutch call (AICLUTCH sites), L2 0x001FF0B4 for type 3. */
void GB_AiWrap(void* car, void* a1)
{
    u8* blk = BLK(car);

    if (GB_Type(car) != 3) {
        AiClutch(car, a1);
        return;
    }
    ATF(blk, PHYS_SIDEBRAKE_OFF) = 0.0f;
    AT8(car, CAR_DRIVEIN_OFF)     = 0;
    AT8(car, CAR_DRIVEIN_OFF + 1) = 0;
    GB_T3Clutch(blk);
    if (ATF(blk, PHYS_CLUTCH_OFF) == 0.0f && AT8(blk, PHYS_AUTODRIVE_OFF) == 8)
        AiDriveA((u8*)car + CAR_AI_OFF);
    else
        AiDriveB((u8*)car + CAR_AI_OFF);
}

/* ------------------------------------------------------------ sound */

/* Replaces the engine-sound call (ENGSND_SITE): a type-3 motor is silent
   below 100 rpm (L2 0x001DE478). */
void GB_EngSndWrap(void* car)
{
    u8* blk = BLK(car);

    if (GB_Type(car) == 3 && ATF(blk, PHYS_OMEGA_OFF) < KF(0x41278D34)) {
        AT8(blk, PHYS_ENGSND_OFF)     = 0;
        AT8(blk, PHYS_ENGSND_OFF + 1) = 0;
        return;
    }
    EngineSound(car);
}

/*
    The drivetrain whine for type 3 (L2 0x001DFED8), in place of 0x001DFB58:
    *out is what that function stores through its pointer, *ret what it
    returns (by the stock code's silent case, pitch and volume). From the
    driven wheels; silent unless braking with the clutch closed.
*/
int GB_Whine(void* car, float* out, float* ret)
{
    u8* blk = BLK(car);
    u8 *w1, *w2;
    float s;

    if (GB_Type(car) != 3)
        return 0;
    if (ATF(blk, PHYS_BRAKE_OFF) == 0.0f || ATF(blk, PHYS_CLUTCH_OFF) == 0.0f) {
        *out = 1.0f;
        *ret = 0.0f;
        return 1;
    }
    w1 = blk + PHYS_WHEEL0_OFF + (2 * AT8(TRANS(car), TRANS_AXLE_OFF)) * PHYS_WHEEL_STRIDE;
    w2 = w1 + PHYS_WHEEL_STRIDE;
    s = (ATF(w1, WHEEL_SPEED_OFF) + ATF(w2, WHEEL_SPEED_OFF)) * 0.5f;
    if (s < 0.0f)
        s = -s;
    *out = s * KF(0x3C449BA6) + KF(0x3E999999);
    *ret = (ATF(w1, WHEEL_BRAKE_OFF) + ATF(w2, WHEEL_BRAKE_OFF)) * KF(0x3E4CCCCC);
    return 1;
}

/* --------------------------------------------------------------- install */

void GB_Install(void)
{
    volatile int b40 = 40, b15 = 15, b150 = 150;
    float k = KF(0x3C23D70A);                        /* 0.01f */
    int i;

    g_R40   = (float)b40 * k;
    g_R15   = (float)b15 * k;
    g_R150a = (float)b150 * k;
    g_R150b = (float)b150 * k;
    for (i = 0; i < 8; i++) {
        g_factor[i].car = 0;
        g_factor[i].f   = 1.0f;
    }
    g_gbxWidthLogged = 0;
    g_gbxOn = 1;
}
