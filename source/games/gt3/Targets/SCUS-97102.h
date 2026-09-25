#pragma once

/*
    Gran Turismo 3: A-Spec (USA)
    Boot ELF : SCUS_971.02  (the bootstrap; the main image is core.gt3)
    Build ID : SCUS-97102   (core.gt3 RSA exponent 66001)

    Every address here was read out of BASE_SCUS-97102 by disassembly and put to
    an independent adversarial verifier. See FINDINGS.md.

    IMPORTANT - the base image
    --------------------------
    BASE_SCUS-97102 must be built from core.gt3 with tools/gt3/gt3pack.py, NOT
    with PDTools.GT4ElfBuilderTool. That tool's single-segment path (which is
    the path GT3 takes, nSection == 1) declares the segment 0x18 bytes short and
    emits only 2 of the 6 section headers it advertises. The lost 0x18 bytes
    contain POOL_BASE_GLOBAL and POOL_LIMIT_GLOBAL below, so an image built that
    way brings the heap up with base 0 and size 0 and dies on its first
    allocation. FINDINGS.md section 9.
*/

#define GAME_REGION_US 1
#define REGION_NAME "US"
#define BUILD_NAME  "SCUS-97102"
#define ELF_NAME    "SCUS_971.02"

/*
    The frame rate. Everything the game counts in frames or steps assumes it,
    and so does every Lupo 2 value the plugin carries (Lupo 2 is 60 Hz). The
    PAL build, SCES-50294, runs at 50: PD replaced the frame time 1/60 with 1/50
    throughout and scaled frame counts by 5/6. Code that counts frames reads
    this (Pod.c), and takes PD's own 50 Hz values from GT Concept 2002
    Tokyo-Geneva (SCES-50858), which carries the same features at 50 Hz.
*/
#define GAME_FRAME_HZ 60

/* ------------------------------------------------------------------ startup */

/*
    Where the injector patches. GT3's crt0 enables interrupts with the EE
    encoding ei = 0x42000038, not the MIPS32r2 form 0x41606020 the GT4 family
    uses, and ps2plugininjector's default pattern "38 00 00 42" already matches
    it - so no -p or -n argument is needed. This is the first raw-file match in
    all three GT3 builds.

    The word after it, 0x00100098, is `lui $v0, 0x35` - NOT a jump, so GT3 US
    needs no startup_slot.py treatment. It does need the opposite favour: $v0 is
    live across the hook (0x0010009C completes it into the argv block that
    0x001000A0 dereferences), so INVOKER must preserve it. See Invoker.S.
*/
#define INVOKER_HOOK_SITE   0x00100094
#define INVOKER_DELAY_SLOT  0x00100098
#define INVOKER_RETURN_ADDR 0x0010009C

/* ------------------------------------------------------- memory / the pool */

/*
    The engine has one heap. Its base and limit are two constant words in
    .sdata, base = _end and limit = 0x01F00000, and it is built LAZILY on the
    first malloc/free - not at startup - so it does not exist yet when the hook
    at INVOKER_HOOK_SITE runs. That is what lets init() raise the base out of
    the plugin's way, exactly as Tourist Trophy does, and it is why the pool's
    own memset must NOT be NOPed: the clear already starts above the plugin.
*/
#define POOL_BASE_GLOBAL    0x003531D0  /* ships holding 0x004B2C0C = _end     */
#define POOL_LIMIT_GLOBAL   0x003531D4  /* ships holding 0x01F00000            */
#define POOL_GUARD_GLOBAL   0x003531C8  /* the "already built" guard           */
#define POOL_INIT_FUNC      0x0026C228

/*
    newlib's sbrk break pointer ships holding _end as well. sbrk has a single
    caller and is the only user of syscall 62, so malloc can probably never
    reach the plugin either way - but raising it costs one store and leaves
    nothing at all pointing into the reserved block.
*/
#define SBRK_BREAK_GLOBAL   0x00307A14

#define GAME_END            0x004B2C0C  /* _end: bss clear end, InitHeap base  */

/*
    Where the plugin is linked, and how much of the pool it takes.

    PLUGIN_BASE_ADDRESS is POOL_BASE_VALUE rounded up to 128, to match the
    linkfile's alignment. It must agree with BASE_US in game.mk - that is what
    the linker is actually given.

    The reserve is deliberately modest. GT4 Online blue-clocked after losing
    32 KB of its 23 MB pool, and GT3's headroom within its 26.30 MB
    (0x004B2C10..0x01F00000) has not been measured, so this starts at 64 KB
    rather than the 128 KB Tourist Trophy takes. The plugin is a few KB; raise
    it only with a reason.
*/
#define PLUGIN_BASE_ADDRESS  0x004B2C80
#define PLUGIN_RESERVE_BYTES 0x10000
#define POOL_BASE_VALUE      0x004B2C0C  /* POOL_BASE_GLOBAL's shipped value   */
#define POOL_BASE_NEW        0x004C2C80  /* = PLUGIN_BASE_ADDRESS + reserve    */

/* --------------------------------------------------------------- game libc */

#define ADDR_sprintf        0x0026C718
#define ADDR_printf         0x0026C690
#define ADDR_malloc         0x0026C3A8
#define ADDR_free           0x0026C380
#define ADDR_memset         0x0027DB38
#define ADDR_strcmp         0x0027EA40
#define ADDR_strcpy         0x0027EB84
#define ADDR_operator_new   0x0027C138
#define ADDR_operator_del   0x0027C0C0

/*
    The kernel's FlushCache stub: `addiu $v1, $zero, 0x64; syscall`. Integer
    argument: 0 writes the data cache back, 2 invalidates the instruction cache.
*/
#define ADDR_FlushCache     0x00285EE0

/*
    GT3's own SIO putchar: polls 0x1000F130 & 0x8000, then stores to
    0x1000F180. It is the only code in the image that touches 0x1000F1xx, so
    core/ps2/Sio.c cannot conflict with it. Listed for reference; Sio.c writes
    the register directly rather than calling this.
*/
#define ADDR_sio_putchar    0x00287268

/* ------------------------------------------------------- model / animation */

/*
    Model::animate(Model* model, float dt, int select)

    Walks the model's textures (u16 count at header +0x16, array at +0x2C) and
    calls the per-Tex1 animator with (Tex1*, dt, select). The animator compares
    `select` against the u16 id at each animation record +0x00: a negative
    select runs every record, otherwise only records whose id matches.

    THE WHOLE DOWNSTREAM ANIMATOR ALREADY EXISTS IN RETAIL, UNCHANGED. What
    retail lacks is any caller for a CAR model - its only four callers are the
    course data package's animate driver. Supplying that caller is half of the
    Pod's visuals; the other half is the SWITCH override below, without which
    the face meshes are never drawn. FINDINGS.md section 13.

    Model::animate only ADVANCES the records. Model::reset, despite its name,
    is the pass that APPLIES them - nothing reaches a palette without it.
*/
#define ADDR_Model_animate          0x002249F8
#define ADDR_Model_reset            0x00224B48
#define ADDR_Model_rebind           0x002248D0  /* re-seek on select change    */

/*
    Per-record reset: phase 0, frame 0, weight 1.0, gate 0. $a0 = the record,
    integers only. Lupo 2's selective rebind (L2 0x002321D8 -> 0x00264720)
    calls its twin, 0x002641A8, only on records whose id matches the select and
    then sets record+0x1F (dirty); retail's Model_rebind resets and APPLIES all
    of them. Pod.c reproduces Lupo 2's version.
*/
#define ADDR_AnimRecord_reset       0x00248FF8
#define ADDR_Model_relocate         0x00224600

/* The course data package's animate driver - retail's only Model::animate
   caller, and the reference for the call shape. Not hooked. */
#define ADDR_CourseModelSet_animate 0x001A9768

/*
    Lupo 2 passes a hard-coded 1/60 (the float constant at its 0x0035BEE8) as
    dt from CarModel::animate. It is a literal, not a global, so there is no
    per-build address to resolve - see POD_FRAME_DT_BITS in Pod.c, which keys
    it on GAME_FRAME_HZ (1/50 on the PAL build, as GT Concept SCES-50858 and
    retail EU's own model animation use).
*/

/* ---------------------------------------------------------- RaceCarModel */

/*
    RaceCarModel::update - per-frame, non-virtual in retail (its vtable has one
    entry). 0xBC bytes, 0x001AB0F8..0x001AB1B4.

        0x001AB114  lw    $s1, 0x6ac($a0)   ; Car*      at this+0x6AC
        0x001AB128  addiu $s2, $a0, 4       ; CarModel  at this+0x04
        ...
        0x001AB190  move  $a0, $s2
        0x001AB194  jal   0x0021EE68        ; <- the tail call the Pod hook takes over
        0x001AB198  mov.s $f12, $f0
        0x001AB19C  ld    $ra, 0x30($sp)    ; epilogue

    Note these differ from Lupo 2, where CarModel is at this+0x0C and the Car*
    is *(this). Do not carry Lupo 2's offsets across.
*/
#define ADDR_RaceCarModel_update        0x001AB0F8
#define ADDR_RaceCarModel_update_tail   0x001AB194  /* the jal that is replaced */
#define ADDR_RaceCarModel_update_epi    0x001AB19C
#define ADDR_CarModel_tailCall          0x0021EE68  /* what that jal called     */

#define RACECARMODEL_CARMODEL_OFF   0x04
#define RACECARMODEL_CAR_OFF        0x6AC

/*
    CarModel's model pointer. Lupo 2 holds it at CarModel+0x40; retail holds the
    same field at CarModel+0x24. Proved by CarModel::setModel being the same
    function in both - L2 0x0022D788 / US 0x0021EA90 - storing through
    `sw $a0,0x40($s2)` and `sw $a0,0x24($s2)` respectively, each followed by a
    call to that build's model-header relocator. Lupo 2 inserted 0x1C bytes of
    new fields at +0x24..+0x3C, shifting everything above by +0x1C.

    Retail has NO field for the animation select index. It lives plugin-side.
*/
#define ADDR_CarModel_setModel      0x0021EA90
#define CARMODEL_MODEL_OFF          0x24

/*
    A RACE CAR IS REBUILT EVERY RACE - AT THE SAME ADDRESSES.

    Each race constructs its race entries afresh (entry setup 0x00177488): the
    RaceCarModel constructor (0x001A9CA8, called only from 0x00177218) runs the
    CarModel constructor, which writes the retail callback vptr at CarModel+0
    (0x0021EC08) and zeroes the model pointer. The car's model is then copied
    into a newly allocated 0xA8000-byte buffer (0x001A9ED8, once per race car,
    from 0x00177614 or the deferred 0x0017767C) and assigned here:

        0x001AA070  addiu $a0, $s0, 4        ; the CarModel (RaceCarModel+4)
        0x001AA074  move  $a2, $zero         ; no relocation
        0x001AA078  jal   0x0021EA90         ; setModel(cm, model, reloc)
        0x001AA07C  lw    $a1, 8($v0)        ; the model copy

    The heap hands out the same addresses race after race, so the next race's
    CarModel and model copy can sit exactly where the last race's did: an
    unchanged pointer does NOT mean an unchanged car. The plugin takes this
    call over (integers only) to learn that a race car was given its model.
*/
#define RACECAR_SETMODEL_SITE       0x001AA078
#define RACECAR_SETMODEL_WORD       0x0C087AA4  /* jal 0x0021EA90 */

/* ------------------------------------------------- the model SWITCH query */

/*
    Model programs choose which meshes to draw with SWITCH opcodes. The handler
    (US 0x00225570) asks a callback object which arm to take:

        0x00225584  lw   $a2, -0x4f1c($gp)   ; the callback object (set per model)
        0x00225594  lw   $v1, ($a2)          ; its vptr
        0x00225598  lh   $a0, 0x10($v1)      ; slot 2 delta
        0x0022559C  lw   $v0, 0x14($v1)      ; slot 2 pfn - the switch query
        0x002255A0  jalr $v0                 ; ($a0 = obj + delta, $a1 = s16 id)
        0x002255B4  sltu $v0, $v1, $s1       ; result compared UNSIGNED to the arm
                                             ;   count; out of range = no arm

    For a car the callback object is the CarModel, whose vptr is
    CARMODEL_CB_VTABLE. Retail's query answers ids 0..5 only and returns 0 for
    anything else. The Pod's model switches on ids 7, 8 and 10, and every mesh
    that uses an animated palette sits in arm >= 1 of a `SWITCH id=10` - so on
    retail the Pod's face meshes are simply never drawn.

    Contract of the query: int f(void* obj, int id) - integers only, so it can
    be called and implemented in plain C (see PodThunk.S on why floats cannot).
*/
#define CARMODEL_CB_VTABLE          0x00340C28  /* 4 entries {delta,pfn}, 0x20 bytes */
#define CARMODEL_CB_VTABLE_WORDS    8
#define CARMODEL_CB_SWITCH_SLOT     5           /* word index of the query's pfn    */
#define ADDR_CarModel_getSwitch     0x0021EC90

/*
    setBlend(float w)   - leaf; $f12 = FLOAT weight, clamped to [0,1], stored at
    gp-0x4794, which FRAMESEQ morph meshes read. The Pod's program issues no
    switch on ids 1..5 and no opcode 0x34, so nothing sets this for it and its
    morph meshes inherit whatever the last-drawn model left - the endless "tail
    tween". Called only through Pod_SetBlendBits (PodThunk.S).
*/
#define ADDR_setBlend               0x00225C00

/* ------------------------------------------------------------- the squat */

/*
    The suspension loop's force call, per wheel. Verified by hand past capstone's
    blind spot (it cannot decode the EE three-operand mult at 0x001E93B8):

        0x001E93BC  lwc1  $f12, 0($s4)     ; FLOAT wheel travel, car+0x1338+4i
        0x001E93C0  move  $a1, $s1         ; wheel state, car+0xFE4+i*0xA4
        0x001E93C4  move  $a3, $s3         ; i, 0..3
        0x001E93C8  move  $a0, $s2         ; car + 0xF84
        0x001E93D4  addu  $s0, $s5, $s0    ; axle spec, car+0x2C8+(i>>1)*0x44
        0x001E93D8  jal   0x001E8E58       ; <- SQUAT_SEAM
        0x001E93DC  move  $a2, $s0         ; delay slot

    The force function reads the spring rate itself, `lwc1 $f1, 0x14($s1)` at
    0x001E8EE8 with $s1 = $a2. Returns FLOAT force in $f0. The spec is inline in
    the car, so a temporary write to spec+0x14 affects only this car; the two
    wheels of an axle share it, which is why it is restored after every call.
*/
#define ADDR_SquatSeam              0x001E93D8
#define SQUAT_SEAM_WORD             0x0C07A396  /* jal 0x001E8E58 - checked before patching */
#define ADDR_SuspForce              0x001E8E58
#define CAR_PHYS_OFF                0xF84       /* $a0 at the seam = car + this      */
#define SUSP_SPEC_SPRING_OFF        0x14

/* Car fields the squat reads (retail = Lupo 2's car+0xD8 block moved to +0xF84) */
#define CAR_SIDEBRAKE_OFF           0x13B4      /* f32 0..1                          */
#define CAR_VEL_X_OFF               0x140C      /* f32, car frame                    */
#define CAR_VEL_Z_OFF               0x1410      /* f32, car frame                    */
#define CAR_SLOT_OFF                0x136C      /* u8, entrant slot                  */

/* ------------------------------------------------ the Pod's emotion triggers */

/*
    Lupo 2's per-step updater (L2 0x001E1520) reads these, first match wins:
    finished, wrong way, pit lane, side by side, full slipstream, all wheels
    off track. Every offset below is proved by a retail twin of the code that
    writes it (retail = Lupo 2 + 0xEA8 for this block); the meanings are
    labelled VERIFIED where the writer shows them. FINDINGS.md section 13.
*/
#define CAR_FINISHED_OFF    0x1464  /* u8 0/1, race over for this car. Set by
                                       finish() 0x001EACB8 (goal crossing in lap
                                       handler 0x001EFB70; time limit, retire);
                                       cleared only at car setup 0x001DDAD0 -
                                       VERIFIED */
#define CAR_RANK_OFF        0x1465  /* s8 live race position, 1-BASED: written as
                                       index + 1 every race step by the sort
                                       0x001ED1F0; the HUD shows it (0x00192D08)
                                       - VERIFIED */
#define CAR_MESSAGE_OFF     0x14A0  /* u8 HUD race message id (setter 0x001DE018):
                                       1 = WRONG WAY, set only by 0x001EF060 while
                                       the car moves backwards along the course
                                       above 5 km/h, and only for a player-driven
                                       car (driver kind car+0x136D == 0) -
                                       VERIFIED */
#define CAR_FLAGS_OFF       0x14B9  /* u8. Bit 0x10: a wheel stands on a road
                                       polygon whose flag bit 0 is set (contact
                                       record +0x33), recomputed every physics
                                       step by 0x001E8B68 - VERIFIED. Only in
                                       conductors that track it: never in
                                       licence, time trial or free run - VERIFIED.
                                       The game uses it for pit entry and exit and
                                       the pit-lane WRONG WAY, so it is the pit
                                       lane - INFERRED. Not "off course". */
#define CAR_GAUGE_OFF       0x14B0  /* f32 slipstream level 0..1: integrator
                                       0x001DB118 adds the draft input each step
                                       and stores exactly 1.0f when it saturates;
                                       draft needs a car ahead above 100 km/h,
                                       less than a second of its travel away -
                                       integrator VERIFIED, slipstream INFERRED */

/*
    The WINK: side by side with another car on the same lap.

    Retail still carries Lupo 2's query, never called: US 0x001F5C58 is the
    twin of L2 0x001FF680, instruction for instruction.

        int level = f(void* rel, int me, int* otherOut)       integers only

    rel is the race object's car-relation block. The race step keeps its state
    matrix current every step (updater 0x001F59B0, from 0x001F428C); the query
    only reads, and writes *otherOut. Level 5 = this car's body overlaps
    another's front to back on the same lap. The query saves $t0 with an EE
    `sq`, so it must be called with a 16-byte aligned $sp - which n32 keeps.

    The race object is *(car+4); cars sit in it at +0xC8 + slot * 0x16DC, and
    car[CAR_SLOT_OFF] is the slot, so a Car* can be proved to belong to it
    before anything is read through it.
*/
#define ADDR_BattleLevel    0x001F5C58
#define CAR_RACE_OFF        0x04
#define RACE_CAR0_OFF       0xC8
#define RACE_CAR_STRIDE     0x16DC
#define RACE_NCARS_OFF      0xE384  /* s32, at most 8                      */
#define RACE_REL_OFF        0xBA34  /* the relation block; +0 = the race    */

/*
    NEMUI: all four wheels on off-track ground. Each wheel's road-contact
    record (4 x 0x34 at car+0x14BC, refreshed every physics step by
    0x001E8B68) holds the road polygon's surface id at +0x31. Lupo 2's class
    table - byte-identical in retail at 0x002D53D8 - marks ids 2, 3, 4 and 6;
    retail runs its own licence/time-trial "all four wheels off" rule on the
    same set.
*/
#define CAR_CONTACT_OFF     0x14BC
#define CAR_CONTACT_STRIDE  0x34
#define CAR_CONTACT_COUNT   4
#define CONTACT_SURFACE_OFF 0x31
#define OFFTRACK_CLASS_MASK 0x5Cu   /* ids 2,3,4,6 - equals the table for 0..15 */

/*
    NAKU / ODOROKU / OKORU / KOWAI: crash events.

    US 0x001DFEB8 is the twin of Lupo 2's notifier L2 0x001DE730, minus the three
    Pod calls Lupo 2 put at its head:

        notify(Car* a0, int type a1, int kind a2, FLOAT strength $f12)

    type 0 = a wall, 1 = another car (both cars are told), 2 = a loose course
    object; kind = which hull corner (0..7) or edge (8 + 0..7) met it. The
    numbering is identical in both builds, and a0 is the plugin's Car*. Its
    five callers are all `jal 0x001DFEB8`, and nothing else references it.
    They run inside the race step, for every car, on the main thread.
*/
#define ADDR_CrashNotify        0x001DFEB8
#define CRASH_NOTIFY_JAL_WORD   0x0C077FAE
#define CRASH_SITE_OBJECT       0x001A130C  /* type 2, kind 9                */
#define CRASH_SITE_CAR_STRUCK   0x001FC800  /* type 1, kind 8 + own edge     */
#define CRASH_SITE_CAR_STRIKER  0x001FC814  /* type 1, kind = own corner     */
#define CRASH_SITE_WALL_CORNER  0x001FE274  /* type 0, kind 0..7             */
#define CRASH_SITE_WALL_EDGE    0x001FE64C  /* type 0, kind 8..15            */

/* ------------------------------------------------- Pod, not hooked (yet) */

/*
    The paramDB side of Lupo 2's Pod: the "special kind" byte its isPod()
    reads is set in the middle of the car-spec builder. The plugin identifies
    the Pod from its model instead, so none of these is used; they are here so
    that a port of that tag, or of the other six concept-car kinds, does not
    start from nothing. docs/pod/POD_PORT_SPEC.md step R1.
*/
#define ADDR_hashName               0x00231190  /* paramDB name hash           */
#define ADDR_carSpecBuilder         0x001DA308  /* host of L2's special-kind chain */
#define ADDR_carSpecBuilder_seam    0x001DA3AC
#define ADDR_specToM10_seam         0x001DC030

/* --------------------------------------------------- sound effects (Pod) */

/*
    The SE name table: 40 x {char* name; s32 seIndex}. At boot the sound init
    (0x0022C3E8) loads sound/se.inf and, for each of its entries, strcmps the
    entry's name against every table name, storing the entry's index into the
    matching record. An unmatched record keeps whatever it held. Game code
    plays effects by table slot. Lupo 2's table has the same 40 plus 11 pod_*.

    The table cannot grow in place - the .eh_frame CIE at 0x002E94C0, which the
    word at 0x0028EED4 points to, follows it - so PodSound.c moves it. Its
    address is built in exactly two places (checked by lui tracking, a low-half
    scan and a data-word scan; there is no $gp-relative access), and its
    length is used exactly once. Each site's shipped word is checked before
    anything is patched.
*/
#define SE_NAME_TABLE           0x002E9380
#define SE_NAME_COUNT           40

#define SE_REG_LUI              0x0022C4F0  /* registration loop - a beqz delay slot */
#define SE_REG_LUI_WORD         0x3C02002F  /*   lui   $v0, 0x2F                     */
#define SE_REG_ADDIU            0x0022C4F8
#define SE_REG_ADDIU_WORD       0x24519380  /*   addiu $s1, $v0, -0x6C80             */
#define SE_GET_LUI              0x0022C588  /* getSEDescriptor(slot) -> packed       */
#define SE_GET_LUI_WORD         0x3C03002F  /*   lui   $v1, 0x2F                     */
#define SE_GET_ADDIU            0x0022C590
#define SE_GET_ADDIU_WORD       0x24639380  /*   addiu $v1, $v1, -0x6C80             */
#define SE_COUNT_SLTI           0x0022C4E8
#define SE_COUNT_SLTI_WORD      0x2A420028  /*   slti  $v0, $s2, 0x28                */

/*
    Banks. Four 0x20-byte GTSOUNDINSTRUMENT objects fit at 0x0049ECC0; retail
    constructs and loads three. The fourth slot, 0x0049ED20..0x0049ED3F, is
    padding in front of a 0x40-aligned object at 0x0049ED40 - nothing in the
    image references it - and it has to be exactly there, because every
    consumer computes 0x0049ECC0 + bank * 0x20 with no bounds check. Lupo 2
    loads sound/gt3pod.ins into its fourth slot.

    The loader makes a virtual call through the object (0x0022C07C), so bank 3
    must be CONSTRUCTED - the static constructor's count is raised, which is
    what Lupo 2 does. The cleanup and destructor bounds follow it; neither runs
    in practice.
*/
#define ADDR_SE_bankArray       0x0049ECC0
#define SE_BANK3                0x0049ED20
#define ADDR_SE_bankLoader      0x0022C140  /* void (bank*, const char* path, int mode) */

#define SE_BANK_CTOR_N          0x0022C81C
#define SE_BANK_CTOR_N_WORD     0x24110002  /*   addiu $s1, $zero, 2   (builds 3)    */
#define SE_BANK_EH_N            0x0022C89C
#define SE_BANK_EH_N_WORD       0x24020002  /*   addiu $v0, $zero, 2                 */
#define SE_BANK_DTOR_END        0x0022C738
#define SE_BANK_DTOR_END_WORD   0x24500060  /*   addiu $s0, $v0, 0x60                */

/*
    The sound init's load of sound/rn_01.es, the last bank it loads:

        0x0022C488  addiu $a0, $a0, -0x6CA0   ; its bank object, 0x002E9360
        0x0022C48C  addiu $a1, $a1, 0x16A8    ; "sound/rn_01.es"
        0x0022C490  jal   0x0022C140          ; <- replaced
        0x0022C494  addiu $a2, $zero, 1       ; mode 1
        0x0022C498  ...                       ; reads no $v0

    Lupo 2 loads gt3pod.ins at the same point in its sequence.
*/
#define SE_RN01_LOAD_JAL        0x0022C490
#define SE_RN01_LOAD_JAL_WORD   0x0C08B050

/*
    The loader's globals, all $gp-relative in the game ($gp = 0x003579F0). The
    header arena (0x800 bytes at 0x0049E4C0, set by 0x0022C218) is never
    bounds-checked and ends exactly at the bank array; Pod bank headers go to a
    plugin buffer instead.
*/
#define SE_SPU_PTR              0x00352B8C  /* gp-0x4E64  next SPU address          */
#define SE_HDR_PTR              0x00352B90  /* gp-0x4E60  next header byte          */
#define SE_HDR_LEFT             0x00352B94  /* gp-0x4E5C  header bytes left         */
#define SE_INF_OBJ              0x00352B98  /* gp-0x4E58  se.inf blob; 0 until loaded */

/*
    SPU RAM from here up is music and race-car samples, loaded at a fixed
    0xAC000 with no check against the SE banks below (0x0022B378, 0x001A663C).
*/
#define SE_SPU_RACE_BASE        0x000AC000

/*
    int file_resolve(const char* path, u32 out[4], void* volume) - integers
    only; returns -1 if the file does not exist. The game's file loader
    (0x002418D0) calls it in a loop until it does not return -1, so a bank
    whose file is missing hangs the boot - PodSound.c resolves it first.
*/
#define ADDR_file_resolve       0x00240AF0
#define SE_VOLUME               0x002F33B0

/*
    Playing. Both run on the main thread inside the per-entry race loop, the
    same loop and iteration as the Pod hook (0x0017DB1C -> 0x00177A50 ->
    0x001A6C38, which calls the positional player for the car's own effects;
    then RaceCarModel::update at 0x0017DB40).

    raceSE(int slot): integers only. If the race mute is clear, plays the slot
    centred at the race SE level. Twin of Lupo 2's 0x001A5630. It goes through
    playSE (0x0022C658), which plays a MONO program (progA == progB, as every
    pod_* entry is) twice, at pan 0 and pan 0x7F - full level on each channel,
    3 dB above one centred voice under the constant-power pan table at
    0x00345E10.

    The positional car-SE player, 0x001A6ED8 ($a1 = slot, FLOAT $f12..$f15 =
    volume, distance gain, L gain, R gain), computes Lupo 2's key-on levels
    but keys on with no owner, so its voice can never be reached again.
    PodSound.c mirrors it field for field and keys on with an owner byte
    instead, through the primitives below - all integers only.
*/
#define ADDR_SE_playRace        0x001A7750
#define ADDR_SE_playCar         0x001A6ED8  /* mirrored in C, not called */
#define SE_RACE_MUTE            0x003521E8  /* gp-0x5808; the player does not check it */
#define SE_RACE_LEVEL           0x003521E0  /* gp-0x5810  f32, race SE option: 1.0 or 0.0 */
#define SE_RACE_FADE            0x003521E4  /* gp-0x580C  f32, race fade: 1.0 in a race    */

#define ADDR_SE_acquire         0x0022C5B0  /* desc* (int slot, int variant); 0 = progA  */
#define ADDR_SE_noteToPitch     0x002687B0  /* int (note << 8, u16 desc[+8])             */
#define ADDR_SE_keyOn           0x002685A8  /* (s8* owner, request*): *owner = voice     */
#define ADDR_SE_voiceSetVolume  0x00267C70  /* (s8* owner, u32* {L, R})                  */
#define ADDR_SE_channelStop     0x0022BEA0  /* (channel*, zeroFirst): key off + release;
                                               uses only channel+4, the owner byte       */

/*
    The per-car sound state 0x001A6C38 refreshes each frame, embedded in the
    race entry at +0x18C4 (constructed at 0x00177220); RaceCarModel is at entry
    +0x11F8 and the hook's CarModel at RaceCarModel+4, so it sits at
    CarModel+0x6C8. Fields: +0 s8 flag (negate G0), +1 s8 flag (negate G1),
    +4 f32 x/d, +0x10 f32 d.
*/
#define CARMODEL_SNDSTATE_OFF   0x6C8

/*
    Float leaves, FLOAT $f12 -> FLOAT $f0; called from Pod_SinCos only. Told
    apart by running both on known inputs - 0x0026A5F0 opens by adding pi/2,
    which reads like cos but is its range reduction.
*/
#define ADDR_sinf               0x0026A5F0
#define ADDR_cosf               0x0026A1A0

/* ================================================= Lupo 2 drivetrain features */

/*
    GEARBOX TYPES and the HONDA DUALNOTE. Every address below was read out of
    BASE_SCUS-97102 and checked by an adversarial verifier against its Lupo 2
    twin; the full record is docs/gearbox_dualnote.md.

    Retail's car-parameter block sits INLINE at Car+0x10 (Lupo 2 holds a pointer
    there), so Lupo 2's m10+X is Car+0x10+X. The Transmission / drivetrain block
    is m10+0x1F8 = Car+0x208; its byte +0x12 is padding that no retail code
    touches, and Lupo 2 keeps the gearbox type in it. The physics block is
    Car+CAR_PHYS_OFF (0xF84), as for the squat.
*/
#define CAR_PARAM_OFF           0x10
#define CAR_TRANS_OFF           0x208
#define TRANS_COUNT_OFF         0x02    /* u8 forward gear count              */
#define TRANS_AXLE_OFF          0x03    /* u8 driven axle                     */
#define TRANS_INERTIA_OFF       0x0C    /* f32                                */
#define TRANS_TYPE_OFF          0x12    /* u8 gearbox type - Lupo 2 only      */
#define TRANS_RPMREF_OFF        0x2C    /* u16                                */
#define TRANS_REDLINE_OFF       0x2E    /* u16 shift limit rpm                */
#define TRANS_CVTRPM_OFF        0x30    /* u16 CVT target rpm (type 1)        */
#define TRANS_RATIO_OFF         0x3C    /* f32[8] overall ratio; [0] reverse  */
#define TRANS_SHAFTINERTIA_OFF  0xB4    /* f32                                */
#define CAR_WHEELBASE_OFF       0x28    /* f32                                */
#define CAR_FRONTAXLE_OFF       0x284   /* f32 front axle factor (m10+0x274)  */
#define CAR_DT_OFF              0xEC0   /* f32 physics step                   */
#define CAR_IDLE_OFF            0x1D6   /* u16 idle rpm                       */
#define CAR_AI_OFF              0xEC8   /* the car's driver/AI object         */
#define CAR_DRIVEIN_OFF         0x1304  /* driver-input out[]: blk+0x380      */

/*
    The physics block, blk = Car + CAR_PHYS_OFF. Each field was matched to its
    Lupo 2 twin by the code that uses it (Lupo 2's offsets are these + 4 from
    about 0x3A0 up). Where a meaning is only inferred, it says so.
*/
#define PHYS_WHEEL0_OFF         0x60    /* wheel 0; wheel i at + i*PHYS_WHEEL_STRIDE */
#define PHYS_WHEEL_STRIDE       0xA4    /*   0,1 front, 2,3 rear                */
#define WHEEL_SPEED_OFF         0x20    /*   f32 wheel speed                    */
#define WHEEL_TYRESEL_OFF       0x24    /*   u8  tyre select                    */
#define WHEEL_BRAKE_OFF         0x90    /*   f32 the brake input, per wheel     */
#define PHYS_WTQ0_OFF           0x2F0   /* wheel-torque record i at + i*0x24    */
#define PHYS_WTQ_STRIDE         0x24
#define WTQ_FORCE_OFF           0x08    /*   f32 (meaning INFERRED)             */
#define WTQ_DRIVE_OFF           0x0C    /*   f32 drive torque, summed each step */
#define PHYS_REVERSE_OFF        0x380   /* s8  reverse request: the driver input's out[0] */
#define PHYS_SHAFTRPM_OFF       0x390   /* f32 engine speed the driven shaft implies */
#define PHYS_AI_OFF             0x3E9   /* s8  1 = AI driver                    */
#define PHYS_CLUTCH_OFF         0x418   /* f32 clutch, 0 open .. 1 closed       */
#define PHYS_STEER_OFF          0x41C   /* f32                                  */
#define PHYS_ACCEL_OFF          0x428   /* f32 accelerator pedal                */
#define PHYS_BRAKE_OFF          0x42C   /* f32 brake: copied to every wheel's
                                           WHEEL_BRAKE_OFF each step (0x001E4CD4) */
#define PHYS_SIDEBRAKE_OFF      0x430   /* f32 side brake (= CAR_SIDEBRAKE_OFF)  */
#define PHYS_GEAR_OFF           0x436   /* u8  gear; 0 is reverse               */
#define PHYS_CSTATE_OFF         0x437   /* u8  0 clutch open, 1 engaged, 2/3
                                           engaging; drive bits only when != 0  */
#define PHYS_ATTIMER_OFF        0x43A   /* u8  the automatic's shift timer      */
#define PHYS_OMEGA_OFF          0x448   /* f32 engine speed, rad/s              */
#define PHYS_AXLESPD_OFF        0x458   /* f32[] driven shaft speed, by axle    */
#define PHYS_TRANSMODE_OFF      0x461   /* u8  0 = automatic                    */
#define PHYS_SPEED_OFF          0x488   /* f32 forward speed, m/s               */
#define PHYS_SPEEDZ_OFF         0x48C   /* f32                                  */
#define PHYS_RPM_OFF            0x490   /* s16 rpm, as the tachometer shows it  */
#define PHYS_THROTTLE_OFF       0x4A4   /* f32 engine throttle (the pedal, each step) */
#define PHYS_TQMAX_OFF          0x4B0   /* f32                                  */
#define PHYS_TQFRIC_OFF         0x4B4   /* f32                                  */
#define PHYS_LATACC_OFF         0x4C8   /* f32 lateral acceleration             */
#define PHYS_HOLD_OFF           0x4DC   /* u8  } while any of these is set the  */
#define PHYS_AUTODRIVE_OFF      0x534   /* u8  } stock automatic clutch holds   */
#define PHYS_HOLDTIME_OFF       0x536   /* u16 } the clutch open (0x534: when 8) */
#define PHYS_ENGSND_OFF         0x4E4   /* u8[2] engine-sound bytes             */
#define PHYS_W0_BITS_OFF        0xBF    /* u8  wheel 0's drive bits (wheel+0x5F) */
#define PHYS_W1_BITS_OFF        0x163   /* u8  wheel 1's                        */

/* -- the data path -------------------------------------------------------- */

#define ADDR_applyGear          0x00232E18  /* void (eq*, spec*, row*) - ints        */
#define APPLYGEAR_SITE          0x002326C4  /* its only caller, in applyAll          */
#define APPLYGEAR_WORD          0x0C08CB86
#define APPLYGEAR_DELAY         0x002326C8
#define APPLYGEAR_DELAY_WORD    0x0220282D  /* move a1,s1 (the spec) - kept          */
#define ADDR_CarDataBase_getBlock 0x00231548 /* DataBlock* (db*, int table) - ints  */
#define CAREQ_DB_OFF            0x1D0       /* CarEquipments + this = its CarDataBase */
#define PARAMDB_TBL_GEAR        17
#define DATABLOCK_NUM_OFF       0x08        /* u16 rows                              */
#define DATABLOCK_ELEMSIZE_OFF  0x0A        /* u16 stride                            */
#define DATABLOCK_ROWS_OFF      0x10
#define GEAR_ROW_TYPE_OFF       0x30        /* only when the stride is >= 0x38       */
#define SPEC_RATIO_OFF          0xB6        /* s16[8]                                */
#define SPEC_GEARCOUNT_OFF      0xC8
#define SPEC_GEARBOX_TYPE_OFF   0xE1        /* a hole in retail's spec               */
#define SPEC_DT_LAYOUT_OFF      0xB2        /* DRIVETRAIN row +0x14                  */
#define SPEC_DT_SUBTYPE_OFF     0xB3        /* DRIVETRAIN row +0x15                  */

/* -- the Transmission constructor, 0x001E1C98 (a0 spec, a1 T, a2 engine, a3) -- */

#define ADDR_TransCtor          0x001E1C98
#define TRANS_CTOR_CALL_1       0x001DC360  /* race car setup                        */
#define TRANS_CTOR_CALL_2       0x001E2580  /* settings screen                       */
#define TRANS_CTOR_CALL_3       0x001E2614  /* dead code, wrapped for completeness   */
#define TRANS_CTOR_CALL_WORD    0x0C078726
#define TRANS_COUNT_SITE        0x001E1FB8
#define TRANS_COUNT_WORD        0x926300C8  /* lbu v1,0xC8(s3)                       */
#define TRANS_COUNT_NEXT        0x001E1FBC
#define TRANS_COUNT_NEXT_WORD   0xA2230002  /* sb v1,2(s1)                           */
#define TRANS_T2C_SITE          0x001E2334
#define TRANS_T2C_WORD          0x10400002  /* beqz v0,+2                            */
#define TRANS_T2C_NEXT          0x001E2338
#define TRANS_T2C_NEXT_WORD     0x24C201F4  /* addiu v0,a2,500                       */
#define TRANS_T2C_STORE_WORD    0xA622002C  /* 0x001E233C sh v0,0x2c(s1) - kept      */

/* -- setup and top speed ---------------------------------------------------- */

#define ADDR_TopSpeed           0x001DB2C8  /* (m10, int)                            */
#define TOPSPEED_SITE           0x001DB4FC
#define TOPSPEED_WORD           0x0C076CB2
#define PARAM_TOPSPEED_OFF      0xDF8       /* f32 m10+: the AI's top-speed estimate */
#define PARAM_ENGINE_OFF        0x138       /* m10+: the engine object               */
#define PARAM_RATIO1_OFF        0x258       /* f32 m10+: T+0x5C[1], road speed to engine
                                               speed in 1st - the top-speed loop's own */
#define PARAM_DRAG0_OFF         0xE50
#define PARAM_DRAG1_OFF         0xE60
#define PARAM_DRAG2_OFF         0xE64
#define PARAM_DRAG3_OFF         0xE68
#define ADDR_PowerCurve         0x001E14E8  /* (engine, buf[0xC0], int) - ints; u16 buf+8 = rpm at max power */
#define ADDR_EngCurveTorque     0x001E0BC0  /* (engine, int, FLOAT w) -> FLOAT       */
#define ADDR_EngCurveNet        0x001E0C38  /* (engine, FLOAT tq, FLOAT w) -> FLOAT  */
#define ADDR_Lookup             0x001D8FB8  /* (table*, FLOAT x) -> FLOAT            */
#define TOPSPEED_DRAG_TABLE     0x002D2CE0
#define TOPSPEED_DRAG_K         0x002D2CF0  /* f32                                   */

/* -- per step ----------------------------------------------------------------- */

#define INITGEAR_SITE           0x001E4E80  /* leaf (car, FLOAT speed) -> gear       */
#define INITGEAR_WORD           0x44800000  /* mtc1 zero,f0                          */
#define INITGEAR_DELAY_WORD     0x46006034  /* c.lt.s f12,f0 - runs as the j's delay */
#define INITGEAR_RESUME         0x001E4E88
#define DIFFSTEP_SITE           0x001E85E4  /* jal 0x001E57F8 - front motors hook    */
#define DIFFSTEP_WORD           0x0C0795FE
#define ADDR_DiffStep           0x001E57F8
#define DRIVESTEP_SITE          0x001E85EC  /* jal 0x001E5B98 (the drivetrain step)  */
#define DRIVESTEP_WORD          0x0C0796E6
#define STEP_DELAY_WORD         0x0200202D  /* move a0,s0 (the car): the delay slot of
                                               DIFFSTEP, DRIVESTEP, SHIFT and SHIFT2 */
#define ADDR_DriveStep          0x001E5B98
#define SHIFT_SITE              0x001E85FC  /* jal 0x001E7500 (rpm/clutch machine)   */
#define SHIFT_WORD              0x0C079D40
#define SHIFT2_SITE             0x001E8604  /* jal 0x001E77E0 (shift handler)        */
#define SHIFT2_WORD             0x0C079DF8
#define ADDR_RpmClutch          0x001E7500
#define ADDR_ShiftHandler       0x001E77E0
#define ADDR_GearRequest        0x001E5690  /* int (car, s8* out) - ints             */
#define ADDR_SetGear            0x001E4E78  /* (blk, gear) - ints                    */
#define TORQUE_SITE             0x001E5CA0  /* jal 0x001E17F0, clutch engaged        */
#define TORQUE_WORD             0x0C0785FC
#define TORQUE_DELAY_WORD       0x0240202D  /* move a0,s2 (the car)                  */
#define FREEREV_SITE            0x001E7554  /* jal 0x001E17F0, clutch open           */
#define FREEREV_DELAY_WORD      0x46010502  /* mul.s f20,f0,f1 - f20 is live after   */
#define ADDR_EngineTorque       0x001E17F0  /* (car, FLOAT throttle) -> FLOAT        */
#define ADDR_EngineNet          0x001E1768  /* (car, FLOAT throttle) -> FLOAT        */
#define GEARIDX1_SITE           0x001E5D54  /* lbu v0,0x436(s0); T in s1             */
#define GEARIDX1_NEXT_WORD      0x46001506  /* mov.s f20,f2 - becomes the delay slot */
#define GEARIDX2_SITE           0x001E7604  /* lbu v0,0x436(s0); T in s2             */
#define GEARIDX2_NEXT_WORD      0xC6010448  /* lwc1 f1,0x448(s0) - the delay slot    */
#define GEARIDX_WORD            0x92020436
#define INERTIA_SITE            0x001E603C  /* 4 words replaced; resumes 0x001E605C  */
#define INERTIA_W0              0x92030436  /* lbu v1,0x436(s0)                      */
#define INERTIA_W1              0xC621000C  /* lwc1 f1,0xc(s1)                       */
#define INERTIA_W2              0x00031080  /* sll v0,v1,2                           */
#define INERTIA_W3              0x02821021  /* addu v0,s4,v0                         */
#define INERTIA_W4              0xC4400000  /* lwc1 f0,0(v0)     } skipped over, and */
#define INERTIA_W5              0x14600002  /* bnez v1,+2        } redone by the     */
#define INERTIA_W6              0x46000982  /* mul.s f6,f1,f0    } thunk             */
#define INERTIA_W7              0x46003187  /* neg.s f6,f6       }                   */
#define INERTIA_RESUME          0x001E605C
#define ENGSTATE_SITE           0x001E6EF0  /* jal 0x001E6D48                        */
#define ENGSTATE_WORD           0x0C079B52
#define ADDR_EngineState        0x001E6D48
#define RPMSHAFT1_SITE          0x001E6E5C  /* jal 0x001E6CA8 -> FLOAT               */
#define RPMSHAFT2_SITE          0x001E7588
#define RPMSHAFT_WORD           0x0C079B2A
#define ADDR_RpmFromShaft       0x001E6CA8  /* (car) -> FLOAT                        */
#define ADDR_ShaftSpeed         0x001E6C28  /* (car) -> FLOAT                        */
#define ROLL_SITE               0x001DD968  /* rolling start, 2 words                */
#define ROLL_WORD               0xC4410000  /* lwc1 f1,0(v0)                         */
#define ROLL_DELAY_WORD         0x46020002  /* mul.s f0,f0,f2 - kept                 */
#define ROLL_NEXT               0x001DD970
#define ROLL_NEXT_WORD          0x4601A042  /* mul.s f1,f20,f1                       */
#define CLUTCH_SITE             0x001E4C6C  /* jal 0x001E4180 (auto clutch)          */
#define CLUTCH_WORD             0x0C079060
#define ADDR_AutoClutch         0x001E4180  /* (car, in*, out*, mode) - ints         */
#define AICLUTCH_SITE_1         0x001E4E0C
#define AICLUTCH_SITE_2         0x001E4E34
#define AICLUTCH_SITE_3         0x001E4E50
#define AICLUTCH_WORD           0x0C078D76
#define ADDR_AiClutch           0x001E35D8  /* (car, a1) - ints                      */
#define ADDR_AiDriveA           0x001F7900  /* (car+0xEC8)                           */
#define ADDR_AiDriveB           0x001F7AE0
#define ENGSND_SITE             0x001EB284  /* jal 0x001DFC60 (engine sound)         */
#define ENGSND_WORD             0x0C077F18
#define ADDR_EngineSound        0x001DFC60
#define WHINE_SITE              0x00177FC8  /* jal 0x001DFB58 (a0 car, a1 float*) -> FLOAT */
#define WHINE_WORD              0x0C077ED6
#define WHINE_DELAY_WORD        0x03C0202D  /* move a0,fp (the car)                  */
#define ADDR_Whine              0x001DFB58
#define ADDR_tanf               0x0026A6C0  /* FLOAT -> FLOAT, so plain C agrees     */
#define ADDR_TireOf             0x001DB9B8  /* (car+0x10, wheel, sel) -> tyre* - ints */
#define TIRE_GRIP_OFF           0x00        /* f32                                   */
#define TIRE_CURVE_OFF          0x48        /* the curve ADDR_Lookup reads           */

/* -- the Dualnote --------------------------------------------------------- */

#define DT_SUBTAB_SLOT5         0x0033C624  /* 4WD sub-type table [5]                */
#define DT_SUBTAB_SLOT5_WORD    0x001E28A0  /* the decoder's jr ra: unimplemented    */
#define DT_FR_HANDLER           0x001E2734  /* the decoder's own FR handler          */
#define DT_CLASSIFY_SITE        0x001DC36C  /* jal 0x001F5EF0 in the car-param builder */
#define DT_CLASSIFY_WORD        0x0C07D7BC
#define DT_CLASSIFY_DELAY_WORD  0x92A500B3  /* lbu a1,0xB3(s5) - kept                */
#define ADDR_DriveClass         0x001F5EF0  /* int (layout, sub) - ints, leaf        */
#define SETUP_SITE              0x001DDD98  /* jal 0x001ECFC8 in car setup; s5 = spec */
#define SETUP_WORD              0x0C07B3F2
#define SETUP_DELAY_WORD        0x0260202D  /* move a0,s3 (the car)                  */
#define ADDR_SetupTail          0x001ECFC8  /* (car) - ints                          */

/*
    The car's mass, in the car-parameter builder 0x001DBFD8 (s5 = spec,
    s4 = m10, a0 = CAR_MASS_TABLE):

        m10+0x48 = (float)spec.s16[0x2A] + tbl[3] + tbl[4]

    and straight after its inverse, 1/m10+0x48 (0x001DCF7C) - the car's mass.
    Lupo 2 leaves tbl[4] out for its hybrid (L2 0x001DD534). Retail's table
    holds 65 and 55 there, Lupo 2's 65 and 5, so without tbl[4] a Dualnote
    weighs spec + 65 kg in both games. What the 55 kg stands for (fuel?) is
    INFERRED.
*/
#define HYB_MASS_SITE           0x001DCF4C  /* lbu v1,4(a0): tbl[4]                  */
#define HYB_MASS_WORD           0x90830004
#define HYB_MASS_DELAY_WORD     0x90820003  /* 0x001DCF50 lbu v0,3(a0) - kept        */
#define CAR_MASS_TABLE          0x002D28B0
