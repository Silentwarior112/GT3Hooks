#include "Drive.h"

#include "core/ps2/Memory.h"
#include "core/Log.h"
#include "GameConfig.h"

/*
    Installs Lupo 2's gearbox types (Gearbox.c) and Honda Dualnote
    (Hybrid.c), and owns the four call sites both of them need.

    Every word a hook replaces, and every neighbouring word a thunk relies on,
    is checked before anything is written. A feature goes in whole or not at
    all: if any of its words is not what the target header expects, none of
    its hooks are written, the log names the word, and the game runs stock.
*/

static void (*DriveStep)(void* car) = (void*)ADDR_DriveStep;

typedef struct { u32 addr; u32 word; } Seam;

static const Seam g_sharedSeams[] = {
    { DRIVESTEP_SITE,        DRIVESTEP_WORD },
    { DRIVESTEP_SITE + 4,    STEP_DELAY_WORD },
    { TORQUE_SITE,           TORQUE_WORD },
    { TORQUE_SITE + 4,       TORQUE_DELAY_WORD },
    { WHINE_SITE,            WHINE_WORD },
    { WHINE_SITE + 4,        WHINE_DELAY_WORD },
    { SETUP_SITE,            SETUP_WORD },
    { SETUP_SITE + 4,        SETUP_DELAY_WORD },
};

static const Seam g_gbxSeams[] = {
    { APPLYGEAR_SITE,        APPLYGEAR_WORD },
    { APPLYGEAR_DELAY,       APPLYGEAR_DELAY_WORD },
    { TRANS_COUNT_SITE,      TRANS_COUNT_WORD },
    { TRANS_COUNT_NEXT,      TRANS_COUNT_NEXT_WORD },
    { TRANS_T2C_SITE,        TRANS_T2C_WORD },
    { TRANS_T2C_NEXT,        TRANS_T2C_NEXT_WORD },
    { TRANS_T2C_NEXT + 4,    TRANS_T2C_STORE_WORD },
    { TRANS_CTOR_CALL_1,     TRANS_CTOR_CALL_WORD },
    { TRANS_CTOR_CALL_2,     TRANS_CTOR_CALL_WORD },
    { TRANS_CTOR_CALL_3,     TRANS_CTOR_CALL_WORD },
    { TOPSPEED_SITE,         TOPSPEED_WORD },
    { INITGEAR_SITE,         INITGEAR_WORD },
    { INITGEAR_SITE + 4,     INITGEAR_DELAY_WORD },
    { SHIFT_SITE,            SHIFT_WORD },
    { SHIFT_SITE + 4,        STEP_DELAY_WORD },
    { SHIFT2_SITE,           SHIFT2_WORD },
    { GEARIDX1_SITE,         GEARIDX_WORD },
    { GEARIDX1_SITE + 4,     GEARIDX1_NEXT_WORD },
    { GEARIDX2_SITE,         GEARIDX_WORD },
    { GEARIDX2_SITE + 4,     GEARIDX2_NEXT_WORD },
    { INERTIA_SITE,          INERTIA_W0 },
    { INERTIA_SITE + 0x04,   INERTIA_W1 },
    { INERTIA_SITE + 0x08,   INERTIA_W2 },
    { INERTIA_SITE + 0x0C,   INERTIA_W3 },
    { INERTIA_SITE + 0x10,   INERTIA_W4 },
    { INERTIA_SITE + 0x14,   INERTIA_W5 },
    { INERTIA_SITE + 0x18,   INERTIA_W6 },
    { INERTIA_SITE + 0x1C,   INERTIA_W7 },
    { ENGSTATE_SITE,         ENGSTATE_WORD },
    { RPMSHAFT1_SITE,        RPMSHAFT_WORD },
    { RPMSHAFT2_SITE,        RPMSHAFT_WORD },
    { ROLL_SITE,             ROLL_WORD },
    { ROLL_SITE + 4,         ROLL_DELAY_WORD },
    { ROLL_NEXT,             ROLL_NEXT_WORD },
    { CLUTCH_SITE,           CLUTCH_WORD },
    { AICLUTCH_SITE_1,       AICLUTCH_WORD },
    { AICLUTCH_SITE_2,       AICLUTCH_WORD },
    { AICLUTCH_SITE_3,       AICLUTCH_WORD },
    { ENGSND_SITE,           ENGSND_WORD },
};

static const Seam g_hybSeams[] = {
    { DT_SUBTAB_SLOT5,       DT_SUBTAB_SLOT5_WORD },
    { DT_CLASSIFY_SITE,      DT_CLASSIFY_WORD },
    { DT_CLASSIFY_SITE + 4,  DT_CLASSIFY_DELAY_WORD },
    { DIFFSTEP_SITE,         DIFFSTEP_WORD },
    { DIFFSTEP_SITE + 4,     STEP_DELAY_WORD },
    { FREEREV_SITE,          TORQUE_WORD },
    { FREEREV_SITE + 4,      FREEREV_DELAY_WORD },
    { HYB_MASS_SITE,         HYB_MASS_WORD },
    { HYB_MASS_SITE + 4,     HYB_MASS_DELAY_WORD },
};

#define N_SEAMS(t) ((int)(sizeof(t) / sizeof((t)[0])))

static int Drive_Check(const char* who, const Seam* s, int n)
{
    int i, ok = 1;

    for (i = 0; i < n; i++) {
        u32 w = *(volatile u32*)s[i].addr;

        if (w != s[i].word) {
            LOG("[gt3hooks] %s: %08X holds %08X, expected %08X\n",
                who, s[i].addr, w, s[i].word);
            ok = 0;
        }
    }
    return ok;
}

/* ------------------------------------------------------ the shared seams */

static const char* const g_gbxName[4] = {
    "stock", "CVT", "clutchless stepped", "electric single speed"
};

/*
    At car setup, after the car's parameters are built and before 0x001ECFC8
    (SETUP_SITE, through Drive_SetupThunk; race cars only).
*/
void Drive_Setup(void* car, const u8* spec)
{
    int t = GB_Type(car);

    Hyb_Setup(car, spec);
    if (t)
        LOG("[gt3hooks] gbx: car %08X (slot %d) has gearbox type %d, %s\n",
            (u32)car, AT8(car, CAR_SLOT_OFF), t, g_gbxName[t & 3]);
}

/* The drivetrain step (DRIVESTEP_SITE). */
void Drive_Step(void* car)
{
    GB_StepPreDrive(car);
    DriveStep(car);
    Hyb_FrontBits(car);
}

/*
    The clutch-engaged engine torque (TORQUE_SITE, through Drive_TorqueThunk),
    in Lupo 2's order (L2 0x001FB960): a gearbox of type 3, then 1 or 2, then
    the Dualnote - only with a stock gearbox - then the stock torque.
*/
int Drive_Torque(void* car, float* out)
{
    if (GB_Torque(car, out))
        return 1;
    return Hyb_Torque(car, out);
}

/*
    The drivetrain-whine voice (WHINE_SITE, through Drive_WhineThunk), in
    Lupo 2's order (L2 0x001DE2C0): the Dualnote, then gearbox type 3, then
    the stock whine.

    Lupo 2 also sets the whine-enable byte (retail car+0x16D0) for both at
    race init (L2 0x001F15C0..0x001F15EC). It is not ported: that byte's only
    reader is the stock whine (0x001DFB80), which neither of these reaches.
*/
int Drive_Whine(void* car, float* out, float* ret)
{
    if (Hyb_Whine(car, out, ret))
        return 1;
    return GB_Whine(car, out, ret);
}

/* --------------------------------------------------------------- install */

#if GBX_ENABLE
static void Drive_PatchGearbox(void)
{
    MAKE_JAL(APPLYGEAR_SITE, (void*)GB_ApplyGear);

    MAKE_JAL(TRANS_COUNT_SITE, (void*)GB_CtorCountThunk);
    NOP(TRANS_COUNT_NEXT);
    MAKE_JAL(TRANS_T2C_SITE, (void*)GB_CtorT2cThunk);
    NOP(TRANS_T2C_NEXT);
    MAKE_JAL(TRANS_CTOR_CALL_1, (void*)GB_CtorWrap);
    MAKE_JAL(TRANS_CTOR_CALL_2, (void*)GB_CtorWrap);
    MAKE_JAL(TRANS_CTOR_CALL_3, (void*)GB_CtorWrap);

    MAKE_JAL(TOPSPEED_SITE, (void*)GB_TopSpeedWrap);
    MAKE_JMP(INITGEAR_SITE, (void*)GB_InitGearThunk);   /* a leaf: j, keeping $ra */

    MAKE_JAL(SHIFT_SITE, (void*)GB_Shift);
    NOP(SHIFT2_SITE);                                    /* GB_Shift calls it */

    MAKE_JAL(GEARIDX1_SITE, (void*)GB_GearIdx1Thunk);
    MAKE_JAL(GEARIDX2_SITE, (void*)GB_GearIdx2Thunk);

    /* jal thunk / nop / b INERTIA_RESUME / nop */
    MAKE_JAL(INERTIA_SITE, (void*)GB_InertiaThunk);
    NOP(INERTIA_SITE + 4);
    PATCH_INT(INERTIA_SITE + 8,
              0x10000000 | (((INERTIA_RESUME - (INERTIA_SITE + 12)) >> 2) & 0xFFFF));
    NOP(INERTIA_SITE + 12);

    MAKE_JAL(ENGSTATE_SITE, (void*)GB_EngineStateWrap);
    MAKE_JAL(RPMSHAFT1_SITE, (void*)GB_RpmShaftThunk);
    MAKE_JAL(RPMSHAFT2_SITE, (void*)GB_RpmShaftCvtThunk);

    MAKE_JAL(ROLL_SITE, (void*)GB_RollThunk);
    NOP(ROLL_NEXT);

    MAKE_JAL(CLUTCH_SITE, (void*)GB_ClutchWrap);
    MAKE_JAL(AICLUTCH_SITE_1, (void*)GB_AiWrap);
    MAKE_JAL(AICLUTCH_SITE_2, (void*)GB_AiWrap);
    MAKE_JAL(AICLUTCH_SITE_3, (void*)GB_AiWrap);
    MAKE_JAL(ENGSND_SITE, (void*)GB_EngSndWrap);
}
#endif

#if HYB_ENABLE
static void Drive_PatchDualnote(void)
{
    /* 4WD sub-type 5 -> the decoder's rear-drive handler: drivetrain type 0 */
    PATCH_INT(DT_SUBTAB_SLOT5, DT_FR_HANDLER);
    MAKE_JAL(DT_CLASSIFY_SITE, (void*)DT_Classify);
    MAKE_JAL(DIFFSTEP_SITE, (void*)Hyb_DiffStep);
    MAKE_JAL(FREEREV_SITE, (void*)Hyb_FreeRevThunk);
    MAKE_JAL(HYB_MASS_SITE, (void*)Hyb_MassThunk);
}
#endif

void Drive_InstallHooks(void)
{
    int shared, gbx = 0, hyb = 0;

    shared = Drive_Check("drive", g_sharedSeams, N_SEAMS(g_sharedSeams));
#if GBX_ENABLE
    gbx = Drive_Check("gbx", g_gbxSeams, N_SEAMS(g_gbxSeams)) && shared;
#endif
#if HYB_ENABLE
    hyb = Drive_Check("dualnote", g_hybSeams, N_SEAMS(g_hybSeams)) && shared;
#endif

    /* the features' state first, so no hook can run on a stale table */
    if (gbx)
        GB_Install();
    if (hyb)
        Hyb_Install();

#if GBX_ENABLE
    if (gbx)
        Drive_PatchGearbox();
#endif
#if HYB_ENABLE
    if (hyb)
        Drive_PatchDualnote();
#endif
    if (gbx || hyb) {
        MAKE_JAL(DRIVESTEP_SITE, (void*)Drive_Step);
        MAKE_JAL(TORQUE_SITE, (void*)Drive_TorqueThunk);
        MAKE_JAL(WHINE_SITE, (void*)Drive_WhineThunk);
        MAKE_JAL(SETUP_SITE, (void*)Drive_SetupThunk);
    }

#if GBX_ENABLE
    LOG("[gt3hooks] gbx: gearbox types %s%s\n",
        gbx ? "installed" : "NOT installed - see above",
        (gbx && GBX_FORCE_TYPE >= 0) ? " - GBX_FORCE_TYPE gives every car one type" : "");
#endif
#if HYB_ENABLE
    LOG("[gt3hooks] dualnote: Dualnote hybrid %s%s\n",
        hyb ? "installed" : "NOT installed - see above",
        (hyb && HYB_FORCE_FR) ? " - HYB_FORCE_FR makes every FR car one" : "");
#endif
}
