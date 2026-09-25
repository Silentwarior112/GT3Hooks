#include "Drive.h"

#include "core/Log.h"
#include "GameConfig.h"

/*
    Lupo 2's Honda Dualnote on retail GT3.

    A DRIVETRAIN row with layout 2 (4WD) and sub-type 5 makes a rear-driven car
    with a hybrid system on top (Lupo 2 0x001DFFF0..0x001E1238):

      * a crank motor (IMA) that assists or charges with the throttle pedal,
      * two front motors with torque vectoring and regeneration,
      * one battery that starts every race full,
      * a changed drivetrain-whine voice.

    Lupo 2 marks such a car with drivetrain type 7. Retail cannot: its 7 is a
    different 4WD, and 8 falls out of every bounded switch. So the Dualnote
    keeps retail's rear-drive type 0 (Drive.c points sub-type 5 of the 4WD
    decoder at its rear-drive handler), and whether a car is a Dualnote is
    decided at car setup from the spec bytes - Lupo 2's own test (0x001D9990)
    - and kept per race slot here.

    Every formula is Lupo 2's, operation for operation; an emulator check of
    this exact transcription against Lupo 2's code gave identical bits. Keep
    the order. The battery's numbers are 27 bytes in Lupo 2's .sdata,
    expanded at boot by the same arithmetic Lupo 2 uses, on the same FPU.
*/

/* ------------------------------------------------------------- constants */

#define K_KW        0x38DBC307u     /* 1/9542.84: N*m * rpm -> kW          */
#define K_RPMKW     0x46151B5Cu     /* 9542.84                             */
#define K_1KMH      0x3E8E38E3u
#define K_WKW       0x3A832923u     /* 0.0010006767                        */
#define K_2PI       0x40C90FDAu
#define K_G         0x411CCCCCu
#define K_4413      0x4230851Eu
#define K_7162      0x44330CCCu
#define K_SNDP      0x3C449BA6u     /* 0.012                               */
#define K_SNDB      0x3ECCCCCCu     /* 0.4                                 */

#define C_100K      0x47C35000u
#define C_01        0x3DCCCCCCu
#define C_980       0x4474FFFEu
#define C_100       0x42C80000u

/*
    The flow meter's scale, FLG = C_6000 / 75. The meter adds up one step's
    energy, so its scale is per step: PD's 50 Hz build (GT Concept SCES-50858,
    GC 0x001DA6AC) has 5000 where Lupo 2 has 6000 - the only value of the
    battery's expansion that differs. Everything else here is multiplied by the
    step's own dt and needs no change. The meter only feeds the log (and would
    feed the HUD gauge, which is not ported).
*/
#if GAME_FRAME_HZ == 50
#define C_6000      0x459C4000u             /* 5000 -> FLG 0x42855555          */
#else
#define C_6000      0x45BB8000u             /* 6000 -> FLG 0x42A00000          */
#endif

/* Lupo 2's hybrid parameters (L2 0x0035DFB1, gp-0x4BBF). */
static const volatile u8 g_hybB[27] = {
    20, 35, 6, 15, 6, 15, 50, 15, 20, 35, 20, 50, 75, 70,
    1, 50, 10, 1, 7, 255, 20, 50, 50, 10, 50, 50, 10
};

/* ...and what L2 0x001DFFF0 makes of them. */
static struct {
    float MF, MR;           /* front motor, IMA: kW                          */
    float FTQD, RTQA;       /* torque caps: front drive, IMA assist          */
    float FTQB, RTQG;       /*   front brake, IMA generate                   */
    float FRAT;             /* front force per unit                          */
    float CAP;              /* battery, kJ                                   */
    float FREG, RREG;       /* regeneration caps: each front wheel, rear pair */
    float IDLE, IDLK;       /* clutch-open idle-up: rpm, gain                */
    float GTQ, GKW;         /* clutch-open generator: torque share, kW       */
    float SLO, SHI;         /* charge at which the battery factor is 0 / 1   */
    float FLOW;             /* front limit on a weak battery                 */
    float FSH;              /* front share of the power                      */
    float KYA, KYT, DBD;    /* torque vectoring under drive: gains, deadband */
    float KYA2, KYT2, DBB;  /*   and under braking                           */
    float FLG;              /* flow meter scale                              */
    int   G_LO, G_HI, G_MIN;/* assist gears 1..7; the gear floor (-1)        */
} P;

/* ------------------------------------------------------------ per-car state */

/*
    Lupo 2 keeps the charge and the flow meter in the car (car+0x5F0,
    car+0x5F5) and the front load in the physics block (blk+0x4F8); retail has
    no such fields. One entry per race slot, rewritten at every car setup.
    frontLoad feeds only the whine, which is the only reader of retail's twin
    field, and which a Dualnote never reaches.
*/
typedef struct {
    void* car;
    u8    hybrid;
    s8    flow;             /* -100..100: this step's energy flow, a sign for the HUD */
    s8    decile;           /* DRIVE_DIAG: the last charge logged, in 10 % steps */
    u8    pad;
    float charge;           /* kJ */
    float frontLoad;
} HybCar;

static HybCar g_hyb[8];
static int g_hybOn = 0;

static HybCar* Hyb_Find(const void* car)
{
    HybCar* h = &g_hyb[AT8(car, CAR_SLOT_OFF) & 7];

    return (g_hybOn && h->car == car && h->hybrid) ? h : 0;
}

int Hyb_IsHybrid(const void* car)
{
    return Hyb_Find(car) != 0;
}

/* ---------------------------------------------------------------- the game */

static int   (*DriveClass)(int layout, int sub) = (void*)ADDR_DriveClass;
static void  (*DiffStep)(void* car) = (void*)ADDR_DiffStep;
static u8*   (*TireOf)(void* m10, int wheel, int sel) = (void*)ADDR_TireOf;
static float (*Tanf)(float x) = (void*)ADDR_tanf;   /* float only: both ABIs agree */

#define WHEEL(b, i) ((b) + PHYS_WHEEL0_OFF + (i) * PHYS_WHEEL_STRIDE)
#define WTQ(b, i)   ((b) + PHYS_WTQ0_OFF + (i) * PHYS_WTQ_STRIDE)

/* --------------------------------------------------------------- battery */

static void Hyb_Flow(HybCar* h, float e)
{
    int s = h->flow + (int)(e * P.FLG);

    if (s < -100)
        s = -100;
    if (!(s < 101))
        s = 100;
    h->flow = (s8)s;
}

/* Store e kJ; returns what fitted. */
static float Hyb_Regen(HybCar* h, float e)
{
    float c = h->charge;
    float t, st;

    Hyb_Flow(h, e);
    t = c + e;
    if (P.CAP < t) {
        st = P.CAP;
        e = e - (t - P.CAP);
    } else {
        st = t;
    }
    h->charge = st;
    return e;
}

/* Draw d kJ; returns what was there. */
static float Hyb_Assist(HybCar* h, float d)
{
    float c = h->charge;
    float rem;

    Hyb_Flow(h, -d);
    if (c < d) {
        float x = d - c;
        rem = 0.0f;
        d = d - x;
    } else {
        rem = c - d;
    }
    h->charge = rem;
    return d;
}

/* The battery factor: 0 at 1 % charge and below, 1 from 50 %. */
static float Hyb_Soc(HybCar* h)
{
    float c = h->charge;

    if (P.SHI <= c)
        return 1.0f;
    if (c <= P.SLO)
        return 0.0f;
    return (c - P.SLO) / (P.SHI - P.SLO);
}

/* ------------------------------------------------------- the pedal's split */

/* Engine share of the system: full-throttle engine power over itself + 70 kW. */
static float Hyb_Share(u8* b)
{
    float p = ((ATF(b, PHYS_TQMAX_OFF) + ATF(b, PHYS_TQFRIC_OFF)) *
               (float)ATS16(b, PHYS_RPM_OFF)) * KF(K_KW);

    return p / ((p + P.MR) + P.MR);
}

/* L2 0x001E0600: the engine throttle for a pedal asking for system power. */
static float Hyb_Throttle(u8* b)
{
    float t = ATF(b, PHYS_THROTTLE_OFF) / Hyb_Share(b);

    return (1.0f < t) ? 1.0f : t;
}

/* L2 0x001E06D0: IMA demand, -1 full generator .. +1 full assist. */
static float Hyb_Demand(u8* b, HybCar* h)
{
    float sh = Hyb_Share(b);
    float p = (AT8(b, PHYS_CSTATE_OFF) == 2) ? ATF(b, PHYS_ACCEL_OFF)
                                             : ATF(b, PHYS_THROTTLE_OFF);
    float x;
    int g;

    if (p <= sh)
        return -1.0f;
    x = ((p - sh) + (p - sh)) / (1.0f - sh);
    x = x * Hyb_Soc(h);
    g = AT8(b, PHYS_GEAR_OFF);
    if (!(P.G_MIN < g))
        x = 0.0f;
    else if (1.0f < x && (g < P.G_LO || P.G_HI < g))
        x = 1.0f;                                   /* no assist outside gears 1..7 */
    return x - 1.0f;
}

/* L2 0x001E07E8: the lateral acceleration the steering asks for, capped at
   twice the front tyre's grip. */
static float Hyb_TargetLat(void* car, u8* b)
{
    float steer = ATF(b, PHYS_STEER_OFF);
    float speed = ATF(b, PHYS_SPEED_OFF);
    float t = Tanf(steer);
    float a = speed * ((speed * t) / ATF(car, CAR_WHEELBASE_OFF));
    float x, g;
    int i;
    u8* tire;

    if (a < 0.0f)
        a = -a;
    i = (0.0f < steer);
    tire = TireOf((u8*)car + CAR_PARAM_OFF, i, AT8(WHEEL(b, i), WHEEL_TYRESEL_OFF));
    x = steer * 1.5f;
    if (x < 0.0f)
        x = -x;
    G_Call_P_F(ADDR_Lookup, tire + TIRE_CURVE_OFF, &x, &g);
    g = g * ATF(tire, TIRE_GRIP_OFF);
    g = g + g;
    if (g < 0.0f)
        g = -g;
    if (g < a)
        a = g;
    if (steer < 0.0f)
        a = -a;
    return a;
}

/* ------------------------------------------------------------ the motors */

/*
    L2 0x001E0900, the front motors. Adds each front wheel's motor torque to
    its drive torque, regenerates, and draws on the battery.
*/
static void Hyb_Front(void* car, HybCar* h)
{
    u8* b = BLK(car);
    float dt = ATF(car, CAR_DT_OFF);
    float thr, n, pe, d, f, s, lim, r, cap, e, E, sum;
    float F[2], out[2];
    int slow[2];
    int i;

    h->frontLoad = 0.0f;
    thr = (AT8(b, PHYS_CSTATE_OFF) == 3) ? ATF(b, PHYS_ACCEL_OFF)
                                         : ATF(b, PHYS_THROTTLE_OFF);
    G_Call_P_F(ADDR_EngineNet, car, &thr, &n);
    pe = (n * (float)ATS16(b, PHYS_RPM_OFF)) * KF(K_KW);
    d = Hyb_Demand(b, h);

    f = (((pe + P.MR * d) * P.FSH) / (1.0f - P.FSH)) / (P.MF + P.MF);
    if (1.0f < f)
        f = 1.0f;
    else if (f < -1.0f)
        f = -1.0f;
    s = Hyb_Soc(h);
    lim = s + P.FLOW * (1.0f - s);
    if (lim < f)
        f = lim;

    for (i = 0; i < 2; i++) {                       /* front regeneration */
        float w = ATF(WHEEL(b, i), WHEEL_SPEED_OFF);
        float x = ATF(WTQ(b, i), WTQ_FORCE_OFF);
        float p;

        if (w < 0.0f)
            w = -w;
        slow[i] = (w < KF(K_1KMH));
        if (x < 0.0f)
            x = -x;
        p = (x * w) * KF(K_WKW);
        if (P.FREG < p)
            p = P.FREG;
        Hyb_Regen(h, p * dt);
    }

    r = (((ATF(car, CAR_FRONTAXLE_OFF) * KF(K_2PI)) * KF(K_G)) * KF(K_7162)) / KF(K_4413);
    F[0] = F[1] = f;

    if (0.0f < f) {                                 /* drive */
        cap = P.FRAT * P.FTQD;
        e = Hyb_TargetLat(car, b) * P.KYT - ATF(b, PHYS_LATACC_OFF) * P.KYA;
        if (0.0f < e) {
            e = e - P.DBD;
            if (!(e < 0.0f)) {
                float a, c;

                e = e * 0.5f;
                a = f * (e + 1.0f);
                c = f * (1.0f - e);
                F[1] = a;
                F[0] = c;
                if (1.0f < a) {
                    F[1] = 1.0f;
                    F[0] = c - (a - 1.0f);
                }
                if (F[0] < -1.0f)
                    F[0] = -1.0f;
            }
        } else if (e < 0.0f) {
            e = e + P.DBD;
            if (!(0.0f < e)) {
                float a, c;

                e = e * 0.5f;
                a = f * (1.0f - e);
                c = f * (e + 1.0f);
                F[0] = a;
                F[1] = c;
                if (1.0f < a) {
                    F[0] = 1.0f;
                    F[1] = c - (a - 1.0f);
                }
                if (F[1] < -1.0f)
                    F[1] = -1.0f;
            }
        }
        for (i = 0; i < 2; i++) {
            out[i] = 0.0f;
            if (!slow[i]) {
                float w = ATF(WHEEL(b, i), WHEEL_SPEED_OFF);

                F[i] = F[i] * ((P.MF * r) / w);
                if (w < 0.0f)
                    F[i] = -F[i];
                if (cap < F[i])
                    F[i] = cap;
                else if (F[i] < -cap)
                    F[i] = -cap;
                out[i] = (F[i] * w) / r;
                if (w < 0.0f)
                    out[i] = -out[i];
            } else if (ATF(b, PHYS_BRAKE_OFF) == 0.0f) {
                F[i] = cap;                         /* launch: full force, free */
            }
        }
        E = (out[0] + out[1]) * dt;
        if (0.0f < E) {
            float q = Hyb_Assist(h, E) / E;

            F[1] = F[1] * q;
            out[0] = out[0] * q;
            F[0] = F[0] * q;
            out[1] = out[1] * q;
        }
        sum = 0.0f;
        if (0.0f < out[0])
            sum = out[0] + sum;
        if (0.0f < out[1])
            sum = sum + out[1];
        h->frontLoad = sum / P.MF;
    } else {                                        /* lift-off: drag */
        cap = P.FRAT * P.FTQB;
        e = Hyb_TargetLat(car, b) * P.KYT2 - ATF(b, PHYS_LATACC_OFF) * P.KYA2;
        if (0.0f < e) {
            e = e - P.DBB;
            if (!(e < 0.0f)) {
                e = e * 0.5f;
                F[0] = F[0] * (e + 1.0f);
                F[1] = F[1] * (1.0f - e);
                if (F[0] < -1.0f) {
                    float x = F[0] + 1.0f;
                    F[0] = -1.0f;
                    F[1] = F[1] - x;
                }
                if (0.0f < F[1])
                    F[1] = 0.0f;
            }
        } else if (e < 0.0f) {
            e = e + P.DBB;
            if (!(0.0f < e)) {
                e = e * 0.5f;
                F[1] = F[1] * (1.0f - e);
                F[0] = F[0] * (e + 1.0f);
                if (F[1] < -1.0f) {
                    float x = F[1] + 1.0f;
                    F[1] = -1.0f;
                    F[0] = F[0] - x;
                }
                if (0.0f < F[0])
                    F[0] = 0.0f;
            }
        }
        for (i = 0; i < 2; i++) {
            if (!slow[i]) {
                float w = ATF(WHEEL(b, i), WHEEL_SPEED_OFF);

                F[i] = F[i] * ((P.MF * r) / w);
                if (w < 0.0f)
                    F[i] = -F[i];
                if (F[i] < -cap)
                    F[i] = -cap;
                else if (cap < F[i])
                    F[i] = cap;
            }
        }
    }

    if (AT8(b, PHYS_GEAR_OFF) == 0) {
        F[0] = -F[0];
        F[1] = -F[1];
    }
    ATF(WTQ(b, 0), WTQ_DRIVE_OFF) = ATF(WTQ(b, 0), WTQ_DRIVE_OFF) + F[0];
    ATF(WTQ(b, 1), WTQ_DRIVE_OFF) = ATF(WTQ(b, 1), WTQ_DRIVE_OFF) + F[1];
}

/* L2 0x001E1018: rear regeneration, then the IMA; returns its crank torque. */
static float Hyb_Rear(void* car, HybCar* h)
{
    u8* b = BLK(car);
    float d = Hyb_Demand(b, h);
    float dt = ATF(car, CAR_DT_OFF);
    float p = 0.0f;
    float rpm, tq, E;
    int j;

    for (j = 2; j < 4; j++) {
        float x;

        if (ATF(WHEEL(b, j), WHEEL_BRAKE_OFF) == 0.0f)
            continue;                               /* only while braking */
        x = ATF(WTQ(b, j), WTQ_FORCE_OFF) * ATF(WHEEL(b, j), WHEEL_SPEED_OFF);
        if (x < 0.0f)
            x = -x;
        p = p + x * KF(K_WKW);
    }
    if (P.RREG < p)
        p = P.RREG;
    Hyb_Regen(h, p * dt);

    rpm = (float)ATS16(b, PHYS_RPM_OFF);
    if (0.0f < d) {                                 /* assist */
        tq = (P.MR * KF(K_RPMKW)) / rpm;
        if (P.RTQA < tq)
            tq = P.RTQA;
        E = (((tq * rpm) / KF(K_RPMKW)) * d) * dt;
        tq = tq * d;
        tq = tq * (Hyb_Assist(h, E) / E);
    } else {                                        /* generate */
        tq = (P.MR * KF(K_RPMKW)) / rpm;
        if (P.RTQG < tq)
            tq = P.RTQG;
        E = (-(((tq * rpm) / KF(K_RPMKW)) * d)) * dt;
        tq = tq * d;
        Hyb_Regen(h, E);
    }
    return tq;
}

/* ------------------------------------------------------------ the seams */

/*
    Replaces the drive-class call in the car-parameter builder
    (DT_CLASSIFY_SITE): Lupo 2 gives the Dualnote the rear-drive class, which
    picks the per-drivetrain driving-aid defaults (L2 0x001FF928).
*/
int DT_Classify(int layout, int sub)
{
    if (layout == 2 && sub == 5)
        return 0;
    return DriveClass(layout, sub);
}

static int Hyb_SpecIsHybrid(const u8* spec)
{
    if (spec[SPEC_DT_LAYOUT_OFF] == 2 && spec[SPEC_DT_SUBTYPE_OFF] == 5)
        return 1;
    return HYB_FORCE_FR && spec[SPEC_DT_LAYOUT_OFF] == 0;
}

/*
    At car setup (Drive_Setup, before 0x001ECFC8): mark the slot, and a
    Dualnote starts with a full battery (L2 0x001E0300) and no front load
    (L2 0x001F4AC4).
*/
void Hyb_Setup(void* car, const u8* spec)
{
    HybCar* h = &g_hyb[AT8(car, CAR_SLOT_OFF) & 7];
    int hyb = g_hybOn && Hyb_SpecIsHybrid(spec);

    h->car       = car;
    h->hybrid    = (u8)hyb;
    h->flow      = 0;
    h->decile    = 10;
    h->frontLoad = 0.0f;
    h->charge    = hyb ? 1.0f * P.CAP : 0.0f;
    if (hyb)
        LOG("[gt3hooks] dualnote: car %08X (slot %d) is a Dualnote hybrid, battery full\n",
            (u32)car, AT8(car, CAR_SLOT_OFF));
}

/*
    Replaces the differential step's call (DIFFSTEP_SITE), the first thing in
    the physics step: Lupo 2 clears the flow meter each step (L2 0x001E0348)
    and runs the front motors at the head of its twin (L2 0x001FB490).
*/
void Hyb_DiffStep(void* car)
{
    HybCar* h = Hyb_Find(car);

    if (h) {
        h->flow = 0;
        Hyb_Front(car, h);
#if DRIVE_DIAG
        {
            int dec = (int)((h->charge * 10.0f) / P.CAP);

            if (dec != h->decile) {
                h->decile = (s8)dec;
                LOG("[gt3hooks] dualnote: slot %d battery %d0%%\n",
                    AT8(car, CAR_SLOT_OFF), dec);
            }
        }
#endif
    }
    DiffStep(car);
}

/*
    The clutch-engaged torque (Drive_Torque), for gearbox type 0 only as in
    Lupo 2 (L2 0x001FBB40): the mapped throttle through the engine, plus the
    IMA.
*/
int Hyb_Torque(void* car, float* out)
{
    HybCar* h = Hyb_Find(car);
    float t, tq, m;

    if (!h)
        return 0;
    t = Hyb_Throttle(BLK(car));
    G_Call_P_F(ADDR_EngineTorque, car, &t, &tq);
    m = Hyb_Rear(car, h);
    *out = tq + m;
    return 1;
}

/*
    The clutch-open torque (FREEREV_SITE, Hyb_FreeRevThunk), L2 0x001E04B0:
    with the battery not full the engine is loaded as a generator, and the
    throttle held up to an idle-up level plus that load.
*/
int Hyb_FreeRev(void* car, float* out)
{
    HybCar* h = Hyb_Find(car);
    u8* b;
    float rpm, thr, lim, g, a, tq;

    if (!h)
        return 0;
    b = BLK(car);
    rpm = (float)ATS16(b, PHYS_RPM_OFF);
    thr = ATF(b, PHYS_THROTTLE_OFF);
    if (1.0f <= h->charge / P.CAP) {
        G_Call_P_F(ADDR_EngineTorque, car, &thr, out);
        return 1;
    }
    lim = (P.GKW * KF(K_RPMKW)) / rpm;
    g = ATF(b, PHYS_TQMAX_OFF) * P.GTQ;
    if (lim < g)
        g = lim;
    a = (P.IDLE - rpm) * P.IDLK;
    a = a + (g + ATF(b, PHYS_TQFRIC_OFF)) / (ATF(b, PHYS_TQMAX_OFF) + ATF(b, PHYS_TQFRIC_OFF));
    if (a < 0.0f)
        a = 0.0f;
    if (thr < a)
        thr = a;
    if (1.0f < thr)
        thr = 1.0f;
    ATF(b, PHYS_THROTTLE_OFF) = thr;
    G_Call_P_F(ADDR_EngineTorque, car, &thr, &tq);
    Hyb_Regen(h, ((g * rpm) * KF(K_KW)) * ATF(car, CAR_DT_OFF));
    *out = tq - g;
    return 1;
}

/* L2 0x001E11C8, the whine voice (Drive_Whine): pitch from the front wheels'
   mean speed, volume from the front motors' load. */
int Hyb_Whine(void* car, float* out, float* ret)
{
    HybCar* h = Hyb_Find(car);
    u8* b;
    float s = 0.0f;
    int i;

    if (!h)
        return 0;
    b = BLK(car);
    for (i = 0; i < 2; i++) {
        float w = ATF(WHEEL(b, i), WHEEL_SPEED_OFF);

        if (w < 0.0f)
            w = -w;
        s = s + w;
    }
    s = s * 0.5f;
    *out = s * KF(K_SNDP) + KF(K_SNDB);
    *ret = h->frontLoad * 0.25f;
    return 1;
}

/*
    After the drivetrain step (Drive_Step): Lupo 2's drive-bit switch sends
    its type 7 to the handler that marks the front wheels too (L2 table
    0x003491D0 slot 7), so they are integrated rather than snapped to rolling
    speed. Retail's type-0 handler marks the rear only; add the front.
*/
void Hyb_FrontBits(void* car)
{
#if HYB_FRONT_BITS
    u8* b;

    if (!Hyb_Find(car))
        return;
    b = BLK(car);
    if (AT8(b, PHYS_CSTATE_OFF) != 0) {
        AT8(b, PHYS_W0_BITS_OFF) |= 4;
        AT8(b, PHYS_W1_BITS_OFF) |= 4;
    }
#else
    (void)car;
#endif
}

/* --------------------------------------------------------------- install */

void Hyb_Install(void)
{
    volatile u32 kb[5] = { C_100K, C_01, C_980, C_6000, C_100 };
    float c100k, c01, c980, c6000, c100, S;
    const volatile u8* B = g_hybB;
    int i;

    c100k = KF(kb[0]);
    c01   = KF(kb[1]);
    c980  = KF(kb[2]);
    c6000 = KF(kb[3]);
    c100  = KF(kb[4]);

    /* L2 0x001DFFF0, in its order */
    S      = ((float)B[0] + (float)B[0]) + (float)B[1];
    P.MF   = (float)B[0];
    P.MR   = (float)B[1];
    P.FTQD = (float)(B[2] * 10);
    P.RTQA = (float)(B[3] * 10);
    P.FTQB = (float)(B[4] * 10);
    P.RTQG = (float)(B[5] * 10);
    P.FRAT = (float)B[6] * c01;
    P.CAP  = (float)B[7] * S;
    P.FREG = (float)(B[8] * 5);
    P.RREG = (float)(B[9] * 5);
    P.IDLE = (float)B[10] * c100;
    P.IDLK = (float)B[11] / c100k;
    P.GTQ  = (float)B[12] / c100;
    P.GKW  = (float)B[13];
    P.SLO  = (P.CAP * (float)B[14]) / c100;
    P.SHI  = (P.CAP * (float)B[15]) / c100;
    P.FLOW = (float)B[16] / (float)B[0];
    P.G_LO  = (s8)B[17];
    P.G_HI  = (s8)B[18];
    P.G_MIN = (s8)B[19];
    P.FSH  = (float)B[20] / c100;
    P.KYA  = (float)B[21] / c980;
    P.KYT  = (float)B[22] / c980;
    P.DBD  = (float)B[23] / c100;
    P.KYA2 = (float)B[24] / c980;
    P.KYT2 = (float)B[25] / c980;
    P.DBB  = (float)B[26] / c100;
    P.FLG  = c6000 / S;

    for (i = 0; i < 8; i++) {
        g_hyb[i].car    = 0;
        g_hyb[i].hybrid = 0;
    }
    g_hybOn = 1;

#if DRIVE_DIAG
    /* Lupo 2's own values on the EE, emulated: 409FFFFF 448CA000 3A03126E
       3E4CCCCC 3D50FAC8 3DCCCCCC 42A00000 (the last hex digit of IDLK, FSH
       and DBD depends on how div.s rounds). At 50 Hz FLG is 42855555. */
    LOG("[gt3hooks] dualnote: FRAT %08X CAP %08X IDLK %08X FSH %08X KYA %08X DBD %08X FLG %08X\n",
        FBITS(P.FRAT), FBITS(P.CAP), FBITS(P.IDLK), FBITS(P.FSH), FBITS(P.KYA),
        FBITS(P.DBD), FBITS(P.FLG));
#endif
}
