#pragma once

/*
    Lupo 2's drivetrain features on retail GT3: the gearbox types (Gearbox.c)
    and the Honda Dualnote (Hybrid.c). Drive.c installs the hooks and owns the
    call sites both features need. DriveThunk.S holds every assembly thunk.

    The research behind every address and formula is docs/gearbox_dualnote.md.

    FLOATS. As everywhere in this plugin, no float crosses a boundary between
    plugin C and game code as a C argument or return value: the game numbers
    int and float arguments independently, this compiler by position (see the
    top of PodThunk.S). Game functions that take or return floats are reached
    through the G_Call_* helpers below, which pass floats through memory.

    BITS. Lupo 2's formulas are reproduced operation for operation, with its
    constants as bit patterns: the EE FPU rounds toward zero, so a value GCC
    folds at compile time can differ in the last bit from the one the game
    computes at run time. Constants go through KF(), which the compiler
    cannot fold.
*/

#include "core/Target.h"

typedef unsigned char       u8;
typedef signed char         s8;
typedef unsigned short      u16;
typedef short               s16;
typedef unsigned int        u32;
typedef unsigned long long  u64;

#define AT8(p, o)   (*(u8*)((u8*)(p) + (o)))
#define ATS8(p, o)  (*(s8*)((u8*)(p) + (o)))
#define AT16(p, o)  (*(u16*)((u8*)(p) + (o)))
#define ATS16(p, o) (*(s16*)((u8*)(p) + (o)))
#define AT32(p, o)  (*(u32*)((u8*)(p) + (o)))
#define ATF(p, o)   (*(float*)((u8*)(p) + (o)))

/* The physics block and the Transmission of a Car*. */
#define BLK(car)    ((u8*)(car) + CAR_PHYS_OFF)
#define TRANS(car)  ((u8*)(car) + CAR_TRANS_OFF)

/* A float from its bit pattern, evaluated at run time. */
static inline float KF(u32 bits)
{
    volatile union { u32 u; float f; } k;
    k.u = bits;
    return k.f;
}

static inline u32 FBITS(float f)
{
    union { float f; u32 u; } k;
    k.f = f;
    return k.u;
}

/* The EE's own square root, not libm's. */
static inline float EE_sqrt(float x)
{
    float r;
    __asm__ ("sqrt.s %0, %1" : "=f"(r) : "f"(x));
    return r;
}

/* ------------------------------------------------ DriveThunk.S: float calls */

/* out = fn(p, x)            fn: a0 = p, FLOAT $f12 = x -> FLOAT $f0          */
void G_Call_P_F(u32 fn, const void* p, const float* x, float* out);
/* out = fn(p, i, x)         fn: a0 = p, a1 = i, FLOAT $f12 = x -> FLOAT $f0 */
void G_Call_PI_F(u32 fn, const void* p, int i, const float* x, float* out);
/* out = fn(p, x, y)         fn: a0 = p, FLOAT $f12 = x, $f13 = y -> $f0     */
void G_Call_P_FF(u32 fn, const void* p, const float* x, const float* y, float* out);
/* out = fn(p)               fn: a0 = p -> FLOAT $f0                          */
void G_Call_P_R(u32 fn, const void* p, float* out);

/* ------------------------------------------------ DriveThunk.S: patch targets */

void GB_CtorCountThunk(void);
void GB_CtorT2cThunk(void);
void GB_InitGearThunk(void);
void GB_GearIdx1Thunk(void);
void GB_GearIdx2Thunk(void);
void GB_InertiaThunk(void);
void GB_RpmShaftThunk(void);
void GB_RpmShaftCvtThunk(void);
void GB_RollThunk(void);
void Drive_TorqueThunk(void);
void Drive_WhineThunk(void);
void Drive_SetupThunk(void);
void Hyb_FreeRevThunk(void);
void Hyb_MassThunk(void);

/* ------------------------------------------------------------- Gearbox.c */

int  GB_Type(const void* car);                 /* 0..3                           */
void GB_Install(void);
void GB_ApplyGear(u8* eq, u8* spec, const u8* row);
void GB_CtorWrap(u8* spec, u8* trans, void* engine, int a3);
void GB_TopSpeedWrap(u8* m10, int a1);
void GB_Shift(void* car);
void GB_StepPreDrive(void* car);               /* before the drivetrain step     */
int  GB_Torque(void* car, float* out);         /* 1 when it supplies the torque  */
u32  GB_Inertia(void* car);                    /* float bits                     */
void GB_EngineStateWrap(void* car);
void GB_RpmShaft(void* car, int cvt, float* out);
u32  GB_Roll(void* car, u32 f1bits);
void GB_ClutchWrap(void* car, u8* in, s8* out, int mode);
void GB_AiWrap(void* car, void* a1);
void GB_EngSndWrap(void* car);
int  GB_Whine(void* car, float* out, float* ret);   /* 1 when it is the whine */

/* -------------------------------------------------------------- Hybrid.c */

int  Hyb_IsHybrid(const void* car);
void Hyb_Install(void);
int  DT_Classify(int layout, int sub);
void Hyb_Setup(void* car, const u8* spec);
void Hyb_DiffStep(void* car);
int  Hyb_Torque(void* car, float* out);        /* 1 when it supplies the torque  */
int  Hyb_FreeRev(void* car, float* out);       /* 1 when it supplies the torque  */
int  Hyb_Whine(void* car, float* out, float* ret);  /* 1 when it is the whine */
void Hyb_FrontBits(void* car);

/* --------------------------------------------------------------- Drive.c */

void Drive_InstallHooks(void);
void Drive_Setup(void* car, const u8* spec);
void Drive_Step(void* car);
int  Drive_Torque(void* car, float* out);      /* 1 when it supplies the torque  */
int  Drive_Whine(void* car, float* out, float* ret);   /* 1 when it is the whine */
