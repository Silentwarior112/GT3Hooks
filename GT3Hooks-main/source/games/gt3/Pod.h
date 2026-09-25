#pragma once

/*
    Toyota Pod - Lupo 2's emotion machine, face, tail and squat on retail GT3.

    See Pod.c and PodSound.c for how the pieces fit, FINDINGS.md section 12
    for how the feature was established, and section 13 for what the port
    needed.
*/

/*
    Install the hooks. Called from init().
*/
void Pod_InstallHooks(void);

/*
    The assembly trampoline that replaces the tail call at the end of
    RaceCarModel::update, and the C function it calls with the CarModel and the
    Car*. See PodThunk.S for why the trampoline exists rather than a plain C hook.
*/
void Pod_UpdateTailThunk(void);
void Pod_OnCarModelUpdated(void* carModel, void* car);

/*
    Takes over the game's setModel call for race cars, once per car per race
    (target header, RACECAR_SETMODEL_SITE), so that a new race's car is
    classified afresh even when it lands at the last race's addresses.
    Integers only: (CarModel*, model, relocate) as setModel takes them.
*/
int  Pod_SetModelHook(void* carModel, void* model, int reloc);

/*
    The switch query installed in a Pod CarModel's vtable - retail answers ids
    0..5, this answers 6..10 as well. Called by the game: int f(void*, int).
*/
int  Pod_GetSwitch(void* obj, int id);

/*
    Called by Pod_SuspWrap (PodThunk.S) for every wheel of every car on each
    physics step: 1 if this wheel should carry no spring for this force call.
*/
int  Pod_SquatWheelOff(unsigned char* car, int i);

/*
    Crash events. Pod_CrashThunk (PodThunk.S) replaces the game's five calls to
    its crash notifier (US 0x001DFEB8) and hands each event to Pod_OnCarEvent -
    integers only - before passing it on unchanged.
*/
void Pod_CrashThunk(void);
void Pod_OnCarEvent(void* car, int type, int kind);

/*
    The Pod's voice (PodSound.c). Install at boot, from Pod_InstallHooks; the
    rn_01 trampoline is called by the game's sound init; the check runs once
    when the first Pod is seen; the update runs every frame for every Pod.

    One PodVoice per Pod. Its first two fields are retail's SE channel layout
    - descriptor at +0, owner byte at +4 - because the game's channel stop
    (US 0x0022BEA0) is handed the whole struct, and key-on writes the voice
    index it allocates into the owner byte, which the voice system resets to
    -1 when that voice ends or is stolen.
*/
typedef struct {
    void*         desc;     /* +0 the tone playing                         */
    signed char   voice;    /* +4 owner byte: voice index, -1 when none    */
    unsigned char pad[3];
    unsigned int  lastLR;   /* +8 the level last sent, R << 16 | L         */
} PodVoice;

void Pod_InstallSound(void);
void Pod_Rn01Tramp(void* bank, const char* path, int mode);
void Pod_SoundCheck(void);
void Pod_SoundWatch(void);          /* POD_DIAG only: logs the game's SE controls */
void Pod_VoiceInit(PodVoice* v);
void Pod_VoiceStop(PodVoice* v);
void Pod_VoiceUpdate(PodVoice* v, int newEmo, void* carModel);
