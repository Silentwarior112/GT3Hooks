/*
    The assembly side of the gearbox types and the Dualnote (Drive.c,
    Gearbox.c, Hybrid.c).

    Three kinds of code live here:

      * FLOAT CALLS - G_Call_*: plugin C calling a game function that takes or
        returns a float. The game numbers int and float arguments separately,
        this compiler by position (PodThunk.S explains the trap); these take
        every float through memory and put it in the register the game reads.

      * CALL-SITE THUNKS - they replace a `jal` in the game, so the game's
        caller already expects every caller-saved register to be clobbered.
        They exist only to move a float, read a callee-saved register the C
        side cannot see, or tail-jump to the original with its $ra intact.

      * MID-FUNCTION THUNKS - they replace ordinary instructions inside a game
        function. Each either touches nothing but what the replaced
        instructions wrote ($v0, $v1, $at, an FPR), or saves every register
        the surrounding code could hold live (SAVE_LIVE) around its call into
        C. Each window was checked for branches landing inside it: none do.
        Their functions all save $ra in their prologue, so the `jal` into a
        thunk costs them nothing.

    Where a thunk sits, and what it replaces, is in the target header.
*/

#include "core/Target.h"
#include "GameConfig.h"

    .set    noreorder
    .text

/* ================================================================ float calls */

/*
    void G_Call_P_F(u32 fn, const void* p, const float* x, float* out)
        *out = fn(p, FLOAT x)
*/
    .align  2
    .globl  G_Call_P_F
    .type   G_Call_P_F, @function
    .ent    G_Call_P_F
G_Call_P_F:
    addiu   $sp, $sp, -0x10
    sd      $ra, 0x00($sp)
    sd      $a3, 0x08($sp)          /* out */
    move    $t9, $a0
    move    $a0, $a1
    lwc1    $f12, 0($a2)
    jalr    $t9
    nop
    ld      $a3, 0x08($sp)
    ld      $ra, 0x00($sp)
    swc1    $f0, 0($a3)
    jr      $ra
    addiu   $sp, $sp, 0x10
    .end    G_Call_P_F
    .size   G_Call_P_F, .-G_Call_P_F

/*
    void G_Call_PI_F(u32 fn, const void* p, int i, const float* x, float* out)
        *out = fn(p, i, FLOAT x)            out arrives in $8, the 5th slot
*/
    .align  2
    .globl  G_Call_PI_F
    .type   G_Call_PI_F, @function
    .ent    G_Call_PI_F
G_Call_PI_F:
    addiu   $sp, $sp, -0x10
    sd      $ra, 0x00($sp)
    sd      $8, 0x08($sp)           /* out */
    move    $t9, $a0
    move    $a0, $a1
    move    $a1, $a2
    lwc1    $f12, 0($a3)
    jalr    $t9
    nop
    ld      $8, 0x08($sp)
    ld      $ra, 0x00($sp)
    swc1    $f0, 0($8)
    jr      $ra
    addiu   $sp, $sp, 0x10
    .end    G_Call_PI_F
    .size   G_Call_PI_F, .-G_Call_PI_F

/*
    void G_Call_P_FF(u32 fn, const void* p, const float* x, const float* y,
                     float* out)
        *out = fn(p, FLOAT x, FLOAT y)      the game: x in $f12, y in $f13
*/
    .align  2
    .globl  G_Call_P_FF
    .type   G_Call_P_FF, @function
    .ent    G_Call_P_FF
G_Call_P_FF:
    addiu   $sp, $sp, -0x10
    sd      $ra, 0x00($sp)
    sd      $8, 0x08($sp)           /* out */
    move    $t9, $a0
    move    $a0, $a1
    lwc1    $f12, 0($a2)
    lwc1    $f13, 0($a3)
    jalr    $t9
    nop
    ld      $8, 0x08($sp)
    ld      $ra, 0x00($sp)
    swc1    $f0, 0($8)
    jr      $ra
    addiu   $sp, $sp, 0x10
    .end    G_Call_P_FF
    .size   G_Call_P_FF, .-G_Call_P_FF

/*
    void G_Call_P_R(u32 fn, const void* p, float* out)
        *out = fn(p)
*/
    .align  2
    .globl  G_Call_P_R
    .type   G_Call_P_R, @function
    .ent    G_Call_P_R
G_Call_P_R:
    addiu   $sp, $sp, -0x10
    sd      $ra, 0x00($sp)
    sd      $a2, 0x08($sp)          /* out */
    move    $t9, $a0
    jalr    $t9
    move    $a0, $a1                /* delay slot */
    ld      $a2, 0x08($sp)
    ld      $ra, 0x00($sp)
    swc1    $f0, 0($a2)
    jr      $ra
    addiu   $sp, $sp, 0x10
    .end    G_Call_P_R
    .size   G_Call_P_R, .-G_Call_P_R

/* ========================================================= call-site thunks */

/*
    TORQUE_SITE, the clutch-engaged engine torque:
        0x001E5C9C  lwc1  $f12, 0x4A4($s0)  ; the throttle
        0x001E5CA0  jal   0x001E17F0        ; <- here
        0x001E5CA4  move  $a0, $s2          ; the car
    and the result comes back in $f0. Drive_Torque(car, &out) says whether
    a gearbox type or the Dualnote supplies it; if not, the stock function
    runs with $a0 and $f12 as the game set them.
*/
    .align  2
    .globl  Drive_TorqueThunk
    .type   Drive_TorqueThunk, @function
    .ent    Drive_TorqueThunk
Drive_TorqueThunk:
    addiu   $sp, $sp, -0x20
    sd      $ra, 0x00($sp)
    sd      $a0, 0x08($sp)
    swc1    $f12, 0x10($sp)
    jal     Drive_Torque            /* (car, &out) -> 0 / 1 */
    addiu   $a1, $sp, 0x14
    ld      $ra, 0x00($sp)
    bnez    $v0, 1f
    ld      $a0, 0x08($sp)          /* delay slot, both paths */
    lwc1    $f12, 0x10($sp)
    li      $t9, ADDR_EngineTorque
    jr      $t9                     /* stock: it returns to the game itself */
    addiu   $sp, $sp, 0x20
1:
    lwc1    $f0, 0x14($sp)
    jr      $ra
    addiu   $sp, $sp, 0x20
    .end    Drive_TorqueThunk
    .size   Drive_TorqueThunk, .-Drive_TorqueThunk

/*
    FREEREV_SITE, the clutch-open engine torque, inside 0x001E7500:
        0x001E7550  lwc1  $f12, 0x4A4($s0)
        0x001E7554  jal   0x001E17F0        ; <- here
        0x001E7558  mul.s $f20, $f0, $f1    ; already run, and read afterwards
    $f20 is callee-saved on both sides, so the C call keeps it.
*/
    .align  2
    .globl  Hyb_FreeRevThunk
    .type   Hyb_FreeRevThunk, @function
    .ent    Hyb_FreeRevThunk
Hyb_FreeRevThunk:
    addiu   $sp, $sp, -0x20
    sd      $ra, 0x00($sp)
    sd      $a0, 0x08($sp)
    swc1    $f12, 0x10($sp)
    jal     Hyb_FreeRev             /* (car, &out) -> 0 / 1 */
    addiu   $a1, $sp, 0x14
    ld      $ra, 0x00($sp)
    bnez    $v0, 1f
    ld      $a0, 0x08($sp)
    lwc1    $f12, 0x10($sp)
    li      $t9, ADDR_EngineTorque
    jr      $t9
    addiu   $sp, $sp, 0x20
1:
    lwc1    $f0, 0x14($sp)
    jr      $ra
    addiu   $sp, $sp, 0x20
    .end    Hyb_FreeRevThunk
    .size   Hyb_FreeRevThunk, .-Hyb_FreeRevThunk

/*
    WHINE_SITE, the drivetrain-whine voice:
        0x00177FC4  lw    $a1, 0x3E8($sp)   ; float* - the function stores here
        0x00177FC8  jal   0x001DFB58        ; <- here
        0x00177FCC  move  $a0, $fp          ; the car
    returning a second float in $f0. The caller keeps using $f20 and $f21,
    which C preserves (the odd one by the build's -ffixed-f21).
*/
    .align  2
    .globl  Drive_WhineThunk
    .type   Drive_WhineThunk, @function
    .ent    Drive_WhineThunk
Drive_WhineThunk:
    addiu   $sp, $sp, -0x20
    sd      $ra, 0x00($sp)
    sd      $a0, 0x08($sp)
    sd      $a1, 0x10($sp)
    jal     Drive_Whine             /* (car, out, &ret) -> 0 / 1 */
    addiu   $a2, $sp, 0x18
    ld      $ra, 0x00($sp)
    ld      $a0, 0x08($sp)
    bnez    $v0, 1f
    ld      $a1, 0x10($sp)          /* delay slot, both paths */
    li      $t9, ADDR_Whine
    jr      $t9
    addiu   $sp, $sp, 0x20
1:
    lwc1    $f0, 0x18($sp)
    jr      $ra
    addiu   $sp, $sp, 0x20
    .end    Drive_WhineThunk
    .size   Drive_WhineThunk, .-Drive_WhineThunk

/*
    SETUP_SITE, in car setup 0x001DDAD0:
        0x001DDD98  jal   0x001ECFC8        ; <- here
        0x001DDD9C  move  $a0, $s3          ; the car
    $s5 still holds the car spec the parameters were built from (set at
    0x001DDAF0, handed to the builder at 0x001DDC00, untouched since), which
    is where the Dualnote test reads. Integers only.
*/
    .align  2
    .globl  Drive_SetupThunk
    .type   Drive_SetupThunk, @function
    .ent    Drive_SetupThunk
Drive_SetupThunk:
    addiu   $sp, $sp, -0x10
    sd      $ra, 0x00($sp)
    sd      $a0, 0x08($sp)
    jal     Drive_Setup             /* (car, spec) */
    move    $a1, $s5
    ld      $a0, 0x08($sp)
    ld      $ra, 0x00($sp)
    li      $t9, ADDR_SetupTail
    jr      $t9
    addiu   $sp, $sp, 0x10
    .end    Drive_SetupThunk
    .size   Drive_SetupThunk, .-Drive_SetupThunk

/*
    RPMSHAFT1_SITE / RPMSHAFT2_SITE: `jal 0x001E6CA8`, engine speed from the
    driven shaft, $a0 = car -> FLOAT $f0. Only a type-1/2 gearbox needs its
    own (GB_RpmShaft); every other car goes straight to the stock function.
    The second site, in the clutch-engaged path, also scales by the ratio
    factor.
*/
    .align  2
    .globl  GB_RpmShaftThunk
    .globl  GB_RpmShaftCvtThunk
    .type   GB_RpmShaftThunk, @function
    .type   GB_RpmShaftCvtThunk, @function
    .ent    GB_RpmShaftThunk
GB_RpmShaftThunk:
    b       2f
    move    $a1, $zero              /* delay slot */
GB_RpmShaftCvtThunk:
    addiu   $a1, $zero, 1
2:
    lbu     $v0, CAR_TRANS_OFF + TRANS_TYPE_OFF($a0)
    addiu   $v0, $v0, -1
    sltiu   $v0, $v0, 2             /* type 1 or 2 */
    bnez    $v0, 1f
    nop
    li      $t9, ADDR_RpmFromShaft
    jr      $t9
    nop
1:
    addiu   $sp, $sp, -0x10
    sd      $ra, 0x00($sp)
    jal     GB_RpmShaft             /* (car, cvt, &out) */
    addiu   $a2, $sp, 0x08
    lwc1    $f0, 0x08($sp)
    ld      $ra, 0x00($sp)
    jr      $ra
    addiu   $sp, $sp, 0x10
    .end    GB_RpmShaftThunk
    .size   GB_RpmShaftThunk, .-GB_RpmShaftThunk

/* ======================================================= mid-function thunks */

    .set    noat

/*
    TRANS_COUNT_SITE, in the Transmission constructor ($s3 = spec, $s1 = T):
        0x001E1FB8  lbu   $v1, 0xC8($s3)    ; <- jal here
        0x001E1FBC  sb    $v1, 2($s1)       ; <- nop
        0x001E1FC0  andi  $t1, $v1, 0xff    ; so $v1 must come back as the count
    The type goes in beside the count, as Lupo 2 does it (L2 0x001E33E0):
    8 gears or more become 7, a type-3 box has one, and a "CVT" with fewer
    than two is a stock box. $v0 is written again at 0x001E1FC4.
*/
    .align  2
    .globl  GB_CtorCountThunk
    .type   GB_CtorCountThunk, @function
    .ent    GB_CtorCountThunk
GB_CtorCountThunk:
    lbu     $v1, SPEC_GEARCOUNT_OFF($s3)
    lbu     $v0, SPEC_GEARBOX_TYPE_OFF($s3)
    sltiu   $at, $v0, 4
    bnez    $at, 1f
    nop
    move    $v0, $zero              /* not a type */
1:
    sltiu   $at, $v1, 8
    bnez    $at, 2f
    nop
    b       9f
    addiu   $v1, $zero, 7           /* delay slot */
2:
    addiu   $at, $zero, 3
    bne     $v0, $at, 3f
    nop
    b       9f
    addiu   $v1, $zero, 1           /* delay slot */
3:
    addiu   $at, $zero, 1
    bne     $v0, $at, 9f
    sltiu   $at, $v1, 2             /* delay slot, harmless on both paths */
    beqz    $at, 9f
    nop
    move    $v0, $zero
9:
    sb      $v1, TRANS_COUNT_OFF($s1)
    jr      $ra
    sb      $v0, TRANS_TYPE_OFF($s1)
    .end    GB_CtorCountThunk
    .size   GB_CtorCountThunk, .-GB_CtorCountThunk

/*
    TRANS_T2C_SITE, in the Transmission constructor (a1 = T+0x2C, a2 = idle,
    v0 = T+0x2C - 500 < idle):
        0x001E2334  beqz  $v0, 0x001E2340   ; <- jal here
        0x001E2338  addiu $v0, $a2, 500     ; <- nop
        0x001E233C  sh    $v0, 0x2C($s1)    ; kept: now always stores $v0
    Types 1/2 take the idle rpm itself (L2 0x001E3430); the rest keep the
    stock max(T+0x2C, idle + 500).
*/
    .align  2
    .globl  GB_CtorT2cThunk
    .type   GB_CtorT2cThunk, @function
    .ent    GB_CtorT2cThunk
GB_CtorT2cThunk:
    lbu     $at, TRANS_TYPE_OFF($s1)
    addiu   $at, $at, -1
    sltiu   $at, $at, 2
    bnez    $at, 1f
    nop
    beqz    $v0, 2f
    nop
    jr      $ra
    addiu   $v0, $a2, 500
1:
    jr      $ra
    move    $v0, $a2
2:
    jr      $ra
    move    $v0, $a1
    .end    GB_CtorT2cThunk
    .size   GB_CtorT2cThunk, .-GB_CtorT2cThunk

/*
    INITGEAR_SITE, the first word of the initial-gear leaf (a0 = car,
    FLOAT $f12 = speed, returns the gear in $v0). Reached by `j`, so $ra is
    still the leaf's caller; the leaf's second word, a compare, runs in the
    j's delay slot and is redone here. Rolling back: R. Types 1 and 3: 1st
    (L2 0x001FA960). Otherwise retail's own search, from its third word.

    For the r5900 the assembler encodes c.lt.s as the EE's C.LT.S, 0x46006034
    (function 0x34) - the game's own word. Generic MIPS would give 0x3C,
    which the EE does not have; the build's disassembly check confirms it.
*/
    .align  2
    .globl  GB_InitGearThunk
    .type   GB_InitGearThunk, @function
    .ent    GB_InitGearThunk
GB_InitGearThunk:
    mtc1    $zero, $f0
    nop
    c.lt.s  $f12, $f0
    nop
    bc1t    8f
    nop
    lbu     $at, CAR_TRANS_OFF + TRANS_TYPE_OFF($a0)
    xori    $v0, $at, 1
    beqz    $v0, 7f
    xori    $v0, $at, 3             /* delay slot */
    beqz    $v0, 7f
    nop
    mtc1    $zero, $f0              /* the stock search, compare included */
    nop
    c.lt.s  $f12, $f0
    li      $at, INITGEAR_RESUME
    jr      $at
    nop
7:
    jr      $ra
    addiu   $v0, $zero, 1
8:
    addiu   $v1, $a0, CAR_TRANS_OFF /* as the leaf's own delay slot leaves it */
    jr      $ra
    move    $v0, $zero
    .end    GB_InitGearThunk
    .size   GB_InitGearThunk, .-GB_InitGearThunk

/*
    GEARIDX1_SITE / GEARIDX2_SITE: `lbu $v0, 0x436($s0)`, the gear as a ratio
    index, with T in $s1 / $s2. A type-2 box's steps all index ratio 1 - the
    ratio factor makes the step (L2 0x001FBC34, 0x001E52D0). Only $v0 and $at.
*/
    .align  2
    .globl  GB_GearIdx1Thunk
    .type   GB_GearIdx1Thunk, @function
    .ent    GB_GearIdx1Thunk
GB_GearIdx1Thunk:
    lbu     $v0, PHYS_GEAR_OFF($s0)
    lbu     $at, TRANS_TYPE_OFF($s1)
    xori    $at, $at, 2
    bnez    $at, 1f
    sltiu   $at, $v0, 2             /* delay slot */
    bnez    $at, 1f
    nop
    addiu   $v0, $zero, 1
1:
    jr      $ra
    nop
    .end    GB_GearIdx1Thunk
    .size   GB_GearIdx1Thunk, .-GB_GearIdx1Thunk

    .align  2
    .globl  GB_GearIdx2Thunk
    .type   GB_GearIdx2Thunk, @function
    .ent    GB_GearIdx2Thunk
GB_GearIdx2Thunk:
    lbu     $v0, PHYS_GEAR_OFF($s0)
    lbu     $at, TRANS_TYPE_OFF($s2)
    xori    $at, $at, 2
    bnez    $at, 1f
    sltiu   $at, $v0, 2             /* delay slot */
    bnez    $at, 1f
    nop
    addiu   $v0, $zero, 1
1:
    jr      $ra
    nop
    .end    GB_GearIdx2Thunk
    .size   GB_GearIdx2Thunk, .-GB_GearIdx2Thunk

/*
    Around a call into C from the middle of a game function: every register
    the game code may hold live that n32 C may clobber - $1..$15, $24, $25,
    HI, LO, FCR31 (the FP condition), $f0..$f19. The frame's 0x00 holds $ra,
    0xF8 the C result.
*/
#define SV_FRAME 0x100

    .macro  SAVE_LIVE
    sd      $1, 0x08($sp)
    sd      $2, 0x10($sp)
    sd      $3, 0x18($sp)
    sd      $4, 0x20($sp)
    sd      $5, 0x28($sp)
    sd      $6, 0x30($sp)
    sd      $7, 0x38($sp)
    sd      $8, 0x40($sp)
    sd      $9, 0x48($sp)
    sd      $10, 0x50($sp)
    sd      $11, 0x58($sp)
    sd      $12, 0x60($sp)
    sd      $13, 0x68($sp)
    sd      $14, 0x70($sp)
    sd      $15, 0x78($sp)
    sd      $24, 0x80($sp)
    sd      $25, 0x88($sp)
    mfhi    $2
    sd      $2, 0x90($sp)
    mflo    $2
    sd      $2, 0x98($sp)
    cfc1    $2, $31
    sw      $2, 0xA0($sp)
    swc1    $f0, 0xA4($sp)
    swc1    $f1, 0xA8($sp)
    swc1    $f2, 0xAC($sp)
    swc1    $f3, 0xB0($sp)
    swc1    $f4, 0xB4($sp)
    swc1    $f5, 0xB8($sp)
    swc1    $f6, 0xBC($sp)
    swc1    $f7, 0xC0($sp)
    swc1    $f8, 0xC4($sp)
    swc1    $f9, 0xC8($sp)
    swc1    $f10, 0xCC($sp)
    swc1    $f11, 0xD0($sp)
    swc1    $f12, 0xD4($sp)
    swc1    $f13, 0xD8($sp)
    swc1    $f14, 0xDC($sp)
    swc1    $f15, 0xE0($sp)
    swc1    $f16, 0xE4($sp)
    swc1    $f17, 0xE8($sp)
    swc1    $f18, 0xEC($sp)
    swc1    $f19, 0xF0($sp)
    .endm

    .macro  RESTORE_LIVE
    lwc1    $f0, 0xA4($sp)
    lwc1    $f1, 0xA8($sp)
    lwc1    $f2, 0xAC($sp)
    lwc1    $f3, 0xB0($sp)
    lwc1    $f4, 0xB4($sp)
    lwc1    $f5, 0xB8($sp)
    lwc1    $f6, 0xBC($sp)
    lwc1    $f7, 0xC0($sp)
    lwc1    $f8, 0xC4($sp)
    lwc1    $f9, 0xC8($sp)
    lwc1    $f10, 0xCC($sp)
    lwc1    $f11, 0xD0($sp)
    lwc1    $f12, 0xD4($sp)
    lwc1    $f13, 0xD8($sp)
    lwc1    $f14, 0xDC($sp)
    lwc1    $f15, 0xE0($sp)
    lwc1    $f16, 0xE4($sp)
    lwc1    $f17, 0xE8($sp)
    lwc1    $f18, 0xEC($sp)
    lwc1    $f19, 0xF0($sp)
    lw      $2, 0xA0($sp)
    ctc1    $2, $31
    ld      $2, 0x98($sp)
    mtlo    $2
    ld      $2, 0x90($sp)
    mthi    $2
    ld      $1, 0x08($sp)
    ld      $2, 0x10($sp)
    ld      $3, 0x18($sp)
    ld      $4, 0x20($sp)
    ld      $5, 0x28($sp)
    ld      $6, 0x30($sp)
    ld      $7, 0x38($sp)
    ld      $8, 0x40($sp)
    ld      $9, 0x48($sp)
    ld      $10, 0x50($sp)
    ld      $11, 0x58($sp)
    ld      $12, 0x60($sp)
    ld      $13, 0x68($sp)
    ld      $14, 0x70($sp)
    ld      $15, 0x78($sp)
    ld      $24, 0x80($sp)
    ld      $25, 0x88($sp)
    .endm

/*
    INERTIA_SITE, in the drivetrain step 0x001E5B98 ($s0 = blk, $s1 = T,
    $s2 = car, $s4 = T+0x3C), clutch engaged:
        0x001E603C  lbu   $v1, 0x436($s0)   ; <- jal here
        0x001E6040  lwc1  $f1, 0xC($s1)     ; <- nop
        0x001E6044  sll   $v0, $v1, 2       ; <- b 0x001E605C
        0x001E6048  addu  $v0, $s4, $v0     ; <- nop
        0x001E604C  lwc1  $f0, 0($v0)
        0x001E6050  bnez  $v1, 0x001E605C
        0x001E6054  mul.s $f6, $f1, $f0     ; the inertia term
        0x001E6058  neg.s $f6, $f6          ;   negated in reverse
    A stock box runs exactly these here. Types 1..3 take $f6 from GB_Inertia
    and leave $v0, $v1, $f0 and $f1 as these instructions would.
*/
    .align  2
    .globl  GB_InertiaThunk
    .type   GB_InertiaThunk, @function
    .ent    GB_InertiaThunk
GB_InertiaThunk:
    lbu     $at, TRANS_TYPE_OFF($s1)
    bnez    $at, 2f
    nop
    lbu     $v1, PHYS_GEAR_OFF($s0)
    lwc1    $f1, TRANS_INERTIA_OFF($s1)
    sll     $v0, $v1, 2
    addu    $v0, $s4, $v0
    lwc1    $f0, 0($v0)
    bnez    $v1, 1f
    mul.s   $f6, $f1, $f0           /* delay slot */
    neg.s   $f6, $f6
1:
    jr      $ra
    nop
2:
    addiu   $sp, $sp, -SV_FRAME
    sd      $ra, 0x00($sp)
    SAVE_LIVE
    jal     GB_Inertia              /* (car) -> float bits */
    move    $a0, $s2
    sw      $v0, 0xF8($sp)
    RESTORE_LIVE
    lwc1    $f6, 0xF8($sp)
    ld      $ra, 0x00($sp)
    addiu   $sp, $sp, SV_FRAME
    lbu     $v1, PHYS_GEAR_OFF($s0)
    lwc1    $f1, TRANS_INERTIA_OFF($s1)
    sll     $v0, $v1, 2
    addu    $v0, $s4, $v0
    lwc1    $f0, 0($v0)
    jr      $ra
    nop
    .end    GB_InertiaThunk
    .size   GB_InertiaThunk, .-GB_InertiaThunk

/*
    ROLL_SITE, the rolling start in 0x001DD8A8 ($s0 = car, $f20 = speed):
        0x001DD968  lwc1  $f1, 0($v0)       ; <- jal here
        0x001DD96C  mul.s $f0, $f0, $f2     ; kept, runs first as the delay slot
        0x001DD970  mul.s $f1, $f20, $f1    ; <- nop
        0x001DD974  c.olt.s $f1, $f0        ; engine speed vs the idle floor
    Both replaced words run here; then a type-1/2 box has GB_Roll restart its
    ratio factor, and a CVT caps $f1 at its target rpm (L2 0x001F4D58).
*/
    .align  2
    .globl  GB_RollThunk
    .type   GB_RollThunk, @function
    .ent    GB_RollThunk
GB_RollThunk:
    lwc1    $f1, 0($v0)
    nop
    mul.s   $f1, $f20, $f1
    lbu     $at, CAR_TRANS_OFF + TRANS_TYPE_OFF($s0)
    addiu   $at, $at, -1
    sltiu   $at, $at, 2
    bnez    $at, 1f
    nop
    jr      $ra
    nop
1:
    addiu   $sp, $sp, -SV_FRAME
    sd      $ra, 0x00($sp)
    SAVE_LIVE
    mfc1    $a1, $f1
    jal     GB_Roll                 /* (car, f1 bits) -> f1 bits */
    move    $a0, $s0
    sw      $v0, 0xF8($sp)
    RESTORE_LIVE
    lwc1    $f1, 0xF8($sp)
    ld      $ra, 0x00($sp)
    jr      $ra
    addiu   $sp, $sp, SV_FRAME
    .end    GB_RollThunk
    .size   GB_RollThunk, .-GB_RollThunk

/*
    HYB_MASS_SITE, the car's mass in the parameter builder ($s5 = spec,
    $a0 = CAR_MASS_TABLE):
        0x001DCF4C  lbu   $v1, 4($a0)       ; <- jal here: tbl[4]
        0x001DCF50  lbu   $v0, 3($a0)       ; kept, runs first as the delay slot
        0x001DCF5C  addu  $v0, $v0, $v1     ; mass = spec + tbl[3] + tbl[4]
    The Dualnote leaves tbl[4] out (L2 0x001DD534). Only $v1 and $at.
*/
    .align  2
    .globl  Hyb_MassThunk
    .type   Hyb_MassThunk, @function
    .ent    Hyb_MassThunk
Hyb_MassThunk:
    lbu     $v1, 4($a0)
    lbu     $at, SPEC_DT_LAYOUT_OFF($s5)
#if HYB_FORCE_FR
    beqz    $at, 1f                 /* the test aid: every FR car */
    nop
#endif
    xori    $at, $at, 2
    bnez    $at, 2f
    nop
    lbu     $at, SPEC_DT_SUBTYPE_OFF($s5)
    xori    $at, $at, 5
    bnez    $at, 2f
    nop
1:
    move    $v1, $zero
2:
    jr      $ra
    nop
    .end    Hyb_MassThunk
    .size   Hyb_MassThunk, .-Hyb_MassThunk

    .set    at
