/*
    GT3 plugin entry point.

    The injector overwrites the first `ei` in the executable - 0x00100094 in
    every GT3 build - with `jal INVOKER`. Two consequences, and GT3 needs the
    opposite treatment to the GT4 family for each.

    1. THE `ei` IS GONE, so it is reissued here before anything else.

    2. THE WORD AFTER IT BECOMES THE DELAY SLOT, and it executes before we do.
       In GT4 and Tourist Trophy that word is crt0's call into the game's early
       setup - a jump in a delay slot, undefined on the R5900 - so GT4Hooks nops
       it (tools/startup_slot.py) and INVOKER makes the call itself.

       GT3 US and EU are the other way round. Their slot holds `lui $v0, 0x35`,
       which is harmless to execute early, so there is nothing to nop. But $v0
       is LIVE ACROSS THE HOOK:

           0x00100098  lui   $v0, 0x35        <- delay slot, runs before us
           0x0010009C  addiu $v0, $v0, 0x3580 <- our return address
           0x001000A0  lw    $a0, 0($v0)      <- argc, from the argv block
           0x001000A4  jal   main
           0x001000A8  addiu $a1, $v0, 4      <- argv

       $v0 is the only live register at the return address. GT4Hooks' compiled
       INVOKER destroys it - it saves only $ra and $fp, and the registers
       actually clobbered across a call into init() are $v0, $v1, $a0-$a3, $t0,
       $t1 and $t9. A GT3 plugin built on that INVOKER would hand main() an argc
       read from an arbitrary address. So $v0 is saved and restored here.

       It is saved rather than reconstructed from a constant so that this file
       needs no per-region knowledge: it is correct for any build whose slot
       leaves a live value in $v0, and harmless for one that does not.

    Written in assembly because GCC does not support __attribute__((naked)) on
    MIPS, and the prologue has to be exactly this.

    Lupo 2 (PAPX-90509) is NOT covered by this file. Its slot at 0x00100098 is
    `jal 0x00103458`, so it needs the startup_slot.py treatment and does not
    need $v0 preserved - the two requirements are inverted. Adding that region
    means a second entry point, not a change to this one.
*/

    .set    noreorder
    .text
    .align  2
    .globl  INVOKER
    .type   INVOKER, @function
    .ent    INVOKER
INVOKER:
    addiu   $sp, $sp, -0x20
    sd      $ra, 0x00($sp)
    sd      $v0, 0x08($sp)          /* crt0's argv-block base */

    ei                              /* reissue the instruction we replaced */

    jal     init
    nop

    ld      $v0, 0x08($sp)          /* hand $v0 back to crt0 untouched */
    ld      $ra, 0x00($sp)
    jr      $ra
    addiu   $sp, $sp, 0x20
    .end    INVOKER
    .size   INVOKER, .-INVOKER
