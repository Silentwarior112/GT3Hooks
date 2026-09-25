/*
    Trampoline for the call this plugin takes over at the end of
    RaceCarModel::update (ADDR_RaceCarModel_update_tail, US 0x001AB194):

        0x001AB190  move  $a0, $s2      ; CarModel  (RaceCarModel + 0x04)
        0x001AB194  jal   0x0021EE68    ; <- replaced with a jal to here
        0x001AB198  mov.s $f12, $f0     ; the float argument, in $f12

    and what it replaces is a two-instruction leaf:

        0x0021EE68  jr    $ra
        0x0021EE6C  swc1  $f12, 0x20($a0)

    WHY THIS FILE EXISTS: THE GAME AND THIS COMPILER DISAGREE ABOUT FLOATS
    ----------------------------------------------------------------------
    The game was built with GCC 2.9x's EABI, which numbers integer and float
    arguments INDEPENDENTLY: the first float goes in $f12 however many integers
    come before it, and the integers fill $a0, $a1, ... in order. Model::animate
    shows it plainly - model in $a0, dt in $f12, select in $a1.

    This plugin's GCC 14 numbers argument SLOTS BY POSITION: argument N goes in
    $aN if it is an integer and $f(12+N) if it is a float. Compiling

        callee(void* p, float f, int i)

    gives p in $a0, f in $f13 and i in $a2 - where the game would look in $a0,
    $f12 and $a1.

    So the two agree on a call that is all integers, and on one whose floats all
    come first, and on nothing else. Any call between plugin C and game code
    that mixes integers and floats - in either direction - is wrong, silently.
    Every such boundary in this plugin is crossed here, in assembly, with the
    floats handed across as raw bits.

    Here: the replaced callee takes its float in $f12 after an integer. A C
    wrapper `void hook(void* carModel, float value)` would read value from $f13.
    It might appear to work - nothing necessarily touches $f12 before the call
    through - but that would be luck, not a calling convention.

    Every game address and car offset below comes from the target header, as
    everywhere else; this file is run through the C preprocessor.
*/

#include "core/Target.h"

    .set    noreorder
    .text
    .align  2
    .globl  Pod_UpdateTailThunk
    .type   Pod_UpdateTailThunk, @function
    .ent    Pod_UpdateTailThunk
Pod_UpdateTailThunk:
    addiu   $sp, $sp, -0x20
    sd      $ra, 0x00($sp)
    sd      $a0, 0x08($sp)

    swc1    $f12, 0x20($a0)         /* exactly what 0x0021EE68 did */

    /*
        $s1 still holds the Car* here: RaceCarModel::update loads it with
        `lw $s1, 0x6ac($a0)` at 0x001AB114 and restores it only in its epilogue
        at 0x001AB1A4. It is callee-saved, and nothing above touches it.
    */
    move    $a1, $s1                /* Car* */
    jal     Pod_OnCarModelUpdated
    ld      $a0, 0x08($sp)          /* delay slot: $a0 = the CarModel */

    ld      $ra, 0x00($sp)
    jr      $ra
    addiu   $sp, $sp, 0x20
    .end    Pod_UpdateTailThunk
    .size   Pod_UpdateTailThunk, .-Pod_UpdateTailThunk

/*
    Model::animate(a0 = model, f12 = dt, a1 = select) - called correctly.

    void Pod_ModelAnimate(void* model, int select, unsigned int dtBits, void* fn)

    The game reads dt out of $f12:

        0x00224A04  move  $s4, $a1     ; select
        0x00224A0C  move  $s2, $a0     ; model
        0x00224A14  mov.s $f20, $f12   ; dt

    but the natural C prototype

        void (*Model_animate)(void* model, float dt, int select)

    has GCC 14 pass dt in $f13 and select in $a2 (see the top of this file), so
    the callee read a stale $f12 as its dt and a stale $a1 as its select.

    The symptom was exact: with dt = 0.0 the animator stores phase + 0.0 back to
    record+0x14 every frame, so the phase reads 0x00000000 forever and the
    current frame never advances, while the gate and flags - set by the rebind,
    which takes no float - look perfectly healthy.

    This is the same trap as Pod_UpdateTailThunk above, in the other direction:
    there a float was coming IN, here one is going OUT. Passing it as raw bits
    and moving it into $f12 here makes the boundary explicit.
*/

    .align  2
    .globl  Pod_ModelAnimate
    .type   Pod_ModelAnimate, @function
    .ent    Pod_ModelAnimate
Pod_ModelAnimate:
    mtc1    $a2, $f12               /* dt bits -> $f12 */
    move    $t9, $a3                /* the callee */
    nop                             /* mtc1 -> FPU-read spacing */
    jr      $t9                     /* tail call: $a0 and $a1 pass straight through */
    nop
    .end    Pod_ModelAnimate
    .size   Pod_ModelAnimate, .-Pod_ModelAnimate

/*
    setBlend(w) with w passed as raw bits.

    void Pod_SetBlendBits(unsigned int wBits)

    US 0x00225C00 is a leaf that takes FLOAT w in $f12, clamps it to [0,1] and
    stores it at gp-0x4794 for the FRAMESEQ morph meshes. Same trap as
    Pod_ModelAnimate: keep floats off the C boundary and move the bits into $f12
    here. It is only ever called from Pod_GetSwitch, which the game's SWITCH
    handler calls - so $gp is still the game's, which setBlend relies on.
*/

    .align  2
    .globl  Pod_SetBlendBits
    .type   Pod_SetBlendBits, @function
    .ent    Pod_SetBlendBits
Pod_SetBlendBits:
    mtc1    $a0, $f12
    li      $t9, ADDR_setBlend
    jr      $t9                     /* tail call - returns straight to our caller */
    nop
    .end    Pod_SetBlendBits
    .size   Pod_SetBlendBits, .-Pod_SetBlendBits

/*
    The squat: the per-wheel suspension force call, wrapped.

    Replaces `jal 0x001E8E58` at US 0x001E93D8 (ADDR_SquatSeam). On entry:

        $a0  car + 0xF84           $a3  wheel index i, 0..3
        $a1  wheel state           $f12 FLOAT wheel travel
        $a2  axle spec; spring rate at $a2 + 0x14
    and the force function returns FLOAT force in $f0.

    Lupo 2 makes the Pod squat by returning a spring rate of 0 for one wheel
    after another while the counter climbs (L2 0x001E1D00). Retail's force
    function reads the rate itself from spec+0x14 rather than taking it as an
    argument, so the equivalent here is to zero spec+0x14 around the one call
    and put it back straight afterwards. The two wheels of an axle share the
    spec, which is why it must be restored every time, not left zeroed.

    Pod_SquatWheelOff(car, i) is plain C with integer arguments and an integer
    result; every float stays on this side. The loop keeps its own state in
    $s0..$s7, $fp and $f20, all callee-saved, so C preserves them.
*/

    .align  2
    .globl  Pod_SuspWrap
    .type   Pod_SuspWrap, @function
    .ent    Pod_SuspWrap
Pod_SuspWrap:
    addiu   $sp, $sp, -0x40
    sd      $ra, 0x00($sp)
    sd      $a0, 0x08($sp)
    sd      $a1, 0x10($sp)
    sd      $a2, 0x18($sp)
    sd      $a3, 0x20($sp)
    swc1    $f12, 0x28($sp)
    sw      $zero, 0x2C($sp)        /* 1 once spec+0x14 has been zeroed */

    addiu   $a0, $a0, -CAR_PHYS_OFF /* car */
    jal     Pod_SquatWheelOff       /* (car, i) -> 0 / 1 */
    move    $a1, $a3                /* delay slot: $a1 = i */

    beqz    $v0, 1f
    ld      $a2, 0x18($sp)          /* delay slot, both paths */
    lw      $t0, SUSP_SPEC_SPRING_OFF($a2)
    sw      $t0, 0x30($sp)          /* the real spring rate */
    sw      $zero, SUSP_SPEC_SPRING_OFF($a2)  /* this wheel carries no spring */
    addiu   $t0, $zero, 1
    sw      $t0, 0x2C($sp)
1:
    ld      $a0, 0x08($sp)
    ld      $a1, 0x10($sp)
    ld      $a2, 0x18($sp)
    ld      $a3, 0x20($sp)
    lwc1    $f12, 0x28($sp)
    li      $t9, ADDR_SuspForce     /* the force function */
    jalr    $t9
    nop

    lw      $t0, 0x2C($sp)          /* integer registers only from here: */
    beqz    $t0, 2f                 /*   $f0 is the force, and must survive */
    nop
    ld      $a2, 0x18($sp)
    lw      $t1, 0x30($sp)
    sw      $t1, SUSP_SPEC_SPRING_OFF($a2)    /* spring rate back */
2:
    ld      $ra, 0x00($sp)
    jr      $ra
    addiu   $sp, $sp, 0x40
    .end    Pod_SuspWrap
    .size   Pod_SuspWrap, .-Pod_SuspWrap

/*
    Crash events: replaces the `jal 0x001DFEB8` at each of the crash
    notifier's five call sites (CRASH_SITE_* in the target header). On entry,
    exactly what the notifier would see:

        $a0  Car*       $a1  type: 0 wall, 1 car, 2 course object
        $a2  kind       $f12 FLOAT strength

    The notifier reads nothing else on entry - $a3, $v0, $v1 and the t
    registers are written before it reads them. So: hand (car, type, kind) to
    C, integers only; put $a0..$a2 and $f12 back; tail-jump to the notifier,
    which returns straight to the game's caller with its own $ra.

    What the C side may clobber is what n32 calls caller-saved. With the odd
    high FPRs reserved by the build (game.mk), that is within what the game's
    EABI lets any callee clobber - so the game's caller cannot tell.
*/

    .align  2
    .globl  Pod_CrashThunk
    .type   Pod_CrashThunk, @function
    .ent    Pod_CrashThunk
Pod_CrashThunk:
    addiu   $sp, $sp, -0x30
    sd      $ra, 0x00($sp)
    sd      $a0, 0x08($sp)
    sd      $a1, 0x10($sp)
    sd      $a2, 0x18($sp)
    jal     Pod_OnCarEvent          /* (car, type, kind): $a0..$a2 as they are */
    swc1    $f12, 0x20($sp)         /* delay slot: the strength */

    lwc1    $f12, 0x20($sp)
    ld      $a2, 0x18($sp)
    ld      $a1, 0x10($sp)
    ld      $a0, 0x08($sp)
    ld      $ra, 0x00($sp)          /* the game caller's return address */
    li      $t9, ADDR_CrashNotify
    jr      $t9                     /* tail call: the notifier returns to the game */
    addiu   $sp, $sp, 0x30
    .end    Pod_CrashThunk
    .size   Pod_CrashThunk, .-Pod_CrashThunk

/*
    void Pod_SinCos(const float* ang, float* out)    out[0] = cos, out[1] = sin

    For the Pod's positional voice (PodSound.c), which pans by these exactly
    as retail's car-SE code does at 0x001A6DDC..0x001A6E0C.

    The game's float leaves: US 0x0026A5F0 is sinf and 0x0026A1A0 is cosf,
    each FLOAT $f12 in, FLOAT $f0 out. (Established by running both on known
    inputs - 0x0026A5F0 starts by adding pi/2, which reads like cos but is its
    range reduction.) They clobber only caller-saved registers.
*/

    .align  2
    .globl  Pod_SinCos
    .type   Pod_SinCos, @function
    .ent    Pod_SinCos
Pod_SinCos:
    addiu   $sp, $sp, -0x20
    sd      $ra, 0x00($sp)
    sd      $s0, 0x08($sp)
    sd      $s1, 0x10($sp)
    move    $s0, $a0
    move    $s1, $a1

    li      $t9, ADDR_sinf
    jalr    $t9
    lwc1    $f12, 0($s0)            /* delay slot */
    swc1    $f0, 4($s1)

    li      $t9, ADDR_cosf
    jalr    $t9
    lwc1    $f12, 0($s0)            /* delay slot */
    swc1    $f0, 0($s1)

    ld      $s1, 0x10($sp)
    ld      $s0, 0x08($sp)
    ld      $ra, 0x00($sp)
    jr      $ra
    addiu   $sp, $sp, 0x20
    .end    Pod_SinCos
    .size   Pod_SinCos, .-Pod_SinCos
