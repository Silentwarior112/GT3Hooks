#pragma once

/*
    Per-game configuration for Gran Turismo 3.

    Shared code in source/core includes "GameConfig.h" unqualified; the makefile
    adds -Isource/games/<game> so it lands on the right one.
*/

#define GAME_NAME     "gt3"
#define GAME_LONGNAME "Gran Turismo 3: A-Spec"

/*
    HostFS is deliberately absent from this project. GT3 already supports it -
    file_resolve (US 0x00240AF0) dispatches on a device kind and kind 3 builds
    "host:data/<name>" - and it is enabled by patching memory, not by hooking
    the volume layer the way the GT4 family has to. FINDINGS.md section 6.
*/

/* ----------------------------------------------------------- Toyota Pod */

/*
    What drives the Pod's emotion state.

         0  neutral          4  okoru (angry)       8  kowai (scared)
         1  wink             5  odoroku (surprised) 9  naku (crying)
         2  genki (lively)   6  iya (dislike)      10  nemui (sleepy)
         3  kyoro (glancing) 7  sabishii (lonely)

    The state selects the face animation, and through the tail's switches it
    decides whether the tail bobs up and down (wink, kyoro), leans one way
    (genki, okoru, odoroku) or the other (iya, sabishii, kowai, naku, nemui), or
    stays still (neutral).

    POD_EMOTION_SOURCE
      0  the game's own triggers - all of Lupo 2's that retail can reach. Every
         step, the first of these that applies:
           finished the race           genki if 1st, else sabishii
           WRONG WAY on the HUD        iya (only while you drive the Pod)
           in the pit lane             kyoro (race, battle, free practice
                                       and machine test modes)
           side by side with a car     wink
             on the same lap
           full slipstream behind a    genki
             car above 100 km/h
           all four wheels off the     nemui
             track surface
         and, the moment they happen:
           hitting a wall              naku
           touching another car        odoroku, okoru or kowai, by where
                                       the two cars meet
           hitting a loose course      odoroku
             object
         Lupo 2's one other trigger is its POD race's stop-area rule; retail
         has no such race mode, so it is not ported. The pit-lane reading of
         kyoro's flag is inferred from how the game uses it.
      1  DEMO - step through states 1..10 in turn, each held for POD_DEMO_HOLD
         steps, with a neutral gap, one every POD_DEMO_PERIOD steps. Both are
         in 60 Hz steps; the 50 Hz EU build scales them by 5/6 (60 steps is
         about a second). Shows every emotion's face, tail and sound by eye and
         ear; the mode the Pod was tested in.
      2  hold POD_FORCE_SELECT permanently.
*/
#define POD_EMOTION_SOURCE 0

#define POD_DEMO_PERIOD    300
#define POD_DEMO_HOLD      180

#define POD_FORCE_SELECT   1

/*
    The squat. Hold the side brake at a standstill (under 1 km/h) and the Pod
    settles onto its bump stops, one corner after another, over about half a
    second; release and it rises again. Set to 0 to leave the suspension stock.
*/
#define POD_SQUAT 1

/*
    The Pod's voice: one pod_* effect on each change of emotion, as in Lupo 2.

      0  off - no sound patches at all
      1  centred, through the game's own race-SE call. LOUDER than Lupo 2: the
         pod_* programs are mono, and that call plays a mono program twice,
         hard left and hard right, so each channel gets full level - 3 dB over
         a centred voice - and every Pod on track is heard at that level
         however far away it is.
      2  positional - Lupo 2's Pod voice: one voice per Pod, at Lupo 2's
         level, released when the next emotion starts rather than stacking,
         and panned and faded every frame by where the Pod is relative to the
         camera (Lupo 2's gain, 32/d up to 1, so quieter past about 32 m). The
         Pod you are driving sits straight ahead of the chase camera, so it
         is heard centred - as in Lupo 2; the panning shows on a Pod you are
         not driving, or from a trackside replay camera.

    Needs two files in sound/ that retail does not ship: gt3pod.ins, and an
    se.inf that names the eleven pod_* effects. The Pod was brought up with
    Lupo 2's own two files dropped in unchanged. tools/gt3/seinf_tool.py can
    instead merge only the pod_* entries into retail's se.inf, which leaves
    every retail sound's entry exactly as shipped. Without the files the Pod
    is silent and the log says what is missing; nothing else changes.
*/
#define POD_SOUND 2

/*
    The Pod's voice level with POD_SOUND 2, in percent of Lupo 2's. 100 is
    Lupo 2 exactly; lower it to taste. Mode 1 ignores it.
*/
#define POD_SOUND_VOLUME 100

/*
    Diagnostic logging over SIO. Always logged, whatever this says: the boot
    installs, each Pod CarModel's switch override, how many Pod sounds are
    ready, and anything that fails. With POD_DIAG as well: every CarModel's
    classification (POD or not), every change of emotion ("pod/emo: neutral ->
    wink"), each sound played and each pod_* sound's se.inf entry, and the
    first squat.
*/
#define POD_DIAG 0

/* ----------------------------------------------------- gearbox types */

/*
    Lupo 2's gearbox types, chosen per GEAR row (docs/gearbox_dualnote.md):

         0  stock GT3
         1  CVT: R and D only, picked automatically; the ratio varies between
            1st and top to hold the engine at its peak-power rpm
         2  clutchless stepped box: normal gear choice, no clutch phase, the
            ratio glides to the new step
         3  electric single speed: one ratio, a built-in motor law, creep

    Where the type comes from: retail's GEAR rows are 0x30 bytes and have no
    room for it. If the table is widened to 0x38 (Lupo 2's layout) the type is
    read from row +0x30; otherwise from the plugin's own list of GEAR row ids,
    which already holds Lupo 2's 16 typed gearboxes (Gearbox.c) - so a Lupo 2
    car transplanted with its own GEAR rows gets its gearbox as-is.
*/
#define GBX_ENABLE 1

/*
    Testing aid: -1 off; 0..3 gives EVERY car that gearbox type.
*/
#define GBX_FORCE_TYPE -1

/* ------------------------------------------------------ Honda Dualnote */

/*
    Lupo 2's Honda Dualnote: a DRIVETRAIN row with layout 2 (4WD) and
    sub-type 5 makes a rear-driven hybrid with a 35 kW crank motor, two 20 kW
    front motors with torque vectoring, regeneration and a 1125 kJ battery
    that starts every race full. Only the car's DRIVETRAIN row is needed; the
    hybrid's parameters are Lupo 2's own constants. The HUD energy gauge is not
    ported.
*/
#define HYB_ENABLE 1

/*
    Lupo 2 also marks the Dualnote's front wheels as drive-connected, which
    stops the game snapping them to rolling speed. 1 = as Lupo 2; 0 = leave
    them as on any rear-driven car.
*/
#define HYB_FRONT_BITS 1

/*
    Testing aid: 1 = every front-engine rear-drive (FR) car is a Dualnote
    hybrid. 0 = only cars whose DRIVETRAIN row says so.
*/
#define HYB_FORCE_FR 0

/*
    Diagnostic logging for both. Always logged, whatever this says: whether
    each feature installed (and if not, which word was not as expected), where
    gearbox types come from, and, as each race starts, every car with a
    gearbox type and every Dualnote. With DRIVE_DIAG as well: the Dualnote's
    expanded battery constants as bit patterns, and each change of a
    Dualnote's charge in steps of 10%.
*/
#define DRIVE_DIAG 0
