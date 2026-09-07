# StoryFlow Auto Lipsync — Design Findings (2026-08-30, revised 2026-09-03)

Status: **DESIGN ONLY — nothing implemented.** Captured from a design discussion so implementation can start from here.
This file is deliberately **untracked** (plugin repo convention: no .md in commits). Do not commit it.

**2026-09-03 revision.** Everything about the rigs and the engines below is now read from the SHIPPED
ASSETS rather than from memory: the Unreal `.uasset` morph lists, the Unity Character Creator's baked mesh,
the Sidekick tool's own C++ (`SKActorComponent`, `SKBlueprintLibrary`, `SKMerger`), and both StoryFlow
plugins' audio paths. It corrects one wrong claim (tongue morphs), adds the Unity arm, which the original
draft did not cover, records the engine asymmetry, and answers open question 3. The prior sources are
`D:\sidekick-face` (the three.js build that already solved this once) and `D:\Synty Assets`.

## The core decision: prebaked tracks, not runtime analysis

Real phoneme lipsync needs phonemes **with timing**, and runtime audio analysis in-engine means heavyweight
dependencies (OVRLipSync-class) we will not force on users. The StoryFlow architecture has a better door:
**the editor is an offline tool that already owns both the audio file and the line's text.**

- The **editor bakes at export**: run an analyzer per dialogue-audio file (with the transcript, which improves
  accuracy), cache by audio hash so only new/changed audio is re-analyzed, ship small timed viseme tracks as an
  export artifact. The user does nothing manually — it rides the normal export.
- The **engine does zero audio analysis**: it plays the track in sync with audio playback, like an animation curve.
- Why prebaked wins: better quality (offline + transcript), zero game CPU/latency, identical on every platform,
  deterministic per line, and localization-friendly (per-language audio → per-language tracks through the same export).

**Analyzer candidate: Rhubarb Lip Sync** — MIT-licensed CLI built exactly for this (audio + optional transcript →
timed track of ~9 mouth shapes), ships win/mac/linux binaries (~2 MB), bundleable with the editor. One exploratory
task should validate quality/speed on real project audio before committing to it.

## Quality tiers

| Tier | What | Cost | Notes |
|---|---|---|---|
| 1. Amplitude flaps | Analyse the live audio (Unity: AudioSource.GetSpectrumData; Unreal: submix spectral analysis, see the audio section), smooth, drive the vowel axis | ~2 tasks, plugin-only, no editor changes | Works live on ANY audio. Becomes the **fallback rung** when no track exists — not throwaway. |
| 2. Baked viseme tracks | The prebaked pipeline above | editor ~3 tasks + ~2-3 per engine arm | The "proper version". The export artifact is engine-neutral, so the editor half is shared and the second arm is cheap. SCOPE AS OF 2026-09-03: two arms, Unity and Unreal, Unity first (see "Order of work"). Godot is OUT — Synty do not ship Sidekick for it. |
| 3. Third-party runtime | OVRLipSync / NVIDIA / marketplace plugins | — | **Never a hard dependency.** Our event surface (line started: character, audio, track) is all such plugins need — optional integration hooks only. |

MetaHuman note: UE 5.5+ ships native audio-driven facial animation. For users on newest UE with MetaHumans, the
honest recommendation may be "wire StoryFlow dialogue events into Epic's native lipsync" — our event surface makes
that trivial and costs us only a docs page.

## The target rigs

### Synty characters (the user's current rigs)
Morph list confirmed = **ARKit-52 standard, extended**. Extras beyond ARKit: `mouthRollOutLower/Upper`,
`jawBackward`, split `cheekPuffLeft/Right`, split lower-lid blinks (`eyeBlinkLowerL/R`), `browRaiseL/R`,
`browInnerDownL/R`, `eyeWideLowerL/R`, `cheekHollowL/R`, and — added 2026-09-03, missing from this list while
the doc still claimed the rig had no tongue at all — **the whole tongue set beyond ARKit's single `tongueOut`**:
`tongueUp` `tongueDown` `tongueIn` `tongueRaise` `tongueLower` `tongueCurlUp/Down/Left/Right`
`tongueSideCurlUp/Down` `tongueTwistLeft/Right`. Body/identity morphs to IGNORE for facial work:
`defaultBuff`, `defaultSkinny`, `defaultHeavy`, `masculineFeminine`.

> ### THIS IS NOT A SYNTY FORMAT, and the naming should never imply it is
> ARKit's 52 face blendshape names are the de facto interchange format for facial animation: MetaHumans take
> them through the Live Link Face path, and VRM/VRoid, Ready Player Me, Reallusion Character Creator and
> NVIDIA Audio2Face all speak them. Synty adopted the set and extended it. So the feature is named for what
> it drives (lipsync, visemes, morphs) and NOT for Sidekick — a `SidekickLipsync` would tell a user with a
> MetaHuman that it does not work for them, when it does.
>
> **The one place the default table leaves ARKit-52** is TH and L, which use `tongueUp` and `tongueRaise`
> above. On a plain ARKit rig those two resolve to nothing, are dropped with one warning naming them, and the
> jaw/lip half of the pose still plays — the usual tongue-less approximation, invisible at game camera
> distance. Both arms must degrade this way rather than dropping the whole pose: a missing tongue morph must
> never take the jaw down with it.

**CORRECTION (2026-09-03, verified against the shipped assets): the tongue morphs EXIST.** This doc
previously recorded "no tongue morphs" as the one gap. That was read off the HEAD mesh alone, which
genuinely has none of them. They live on the tongue part. Read from
`Sidekick/Content/.../Species/Humans/SK_HUMN_BASE_01_*_HU01.uasset`:

| part | mouth-relevant morphs |
|---|---|
| `01HEAD` | 72 total: every `jaw*`, `mouth*` shape the viseme table uses |
| `37TONG` | `tongueOut` `tongueUp` `tongueRaise` `tongueIn` `tongueDown` `tongueLower` `tongueCurl*` `tongueTwist*` + `jawOpen` |
| `36TETH` | `jawOpen` `jawForward` `jawBackward` `jawLeft` `jawRight` |
| `35NOSE` | `mouthClose` `mouthUpperUpLeft/Right` `mouthShrugUpper` `mouthRollOutUpper` |

So L and TH are **real, not approximated**, whenever the tongue part is equipped. Fall back to the
jaw/lip approximation only when it is absent.

> ### ⚠ A viseme is not one mesh's problem
> `jawOpen` must be driven on head **and** teeth **and** tongue. `mouthClose`, `mouthShrugUpper`,
> `mouthRollOutUpper` and `mouthUpperUpLeft/Right` must also be driven on the **nose**. Miss it and the
> jaw opens while the teeth and tongue stay put. This is per-engine — see the asymmetry below.


### Verified against the shipped characters (2026-09-03, after both arms were built)

Every morph the table drives was checked by name against the real assets, in both engines. All 18 resolve.

**Unreal** — `SK_HUMN_BASE_01_*_HU01.uasset`, and this IS the fan-out map:

| morph | head | teeth | tongue | nose |
|---|---|---|---|---|
| `jawOpen` | ✔ | ✔ | ✔ | |
| `mouthClose` | ✔ | | | ✔ |
| `mouthUpperUpLeft/Right` | ✔ | | | ✔ |
| `tongueOut` `tongueUp` `tongueRaise` | | | ✔ | |
| the other 10 mouth shapes | ✔ | | | |

So `jawOpen` alone must be written on THREE components. A head-only driver opens a jaw and leaves the teeth
and tongue behind inside the face.

**Unity** — three separate baked characters (Starter_01, Starter_04, HumanSpecies_01) each carry exactly 84
blendshapes, all prefixed `MESHBlends.`, all 18 present including the tongue. The prefix is the Character
Creator's, not a per-asset accident, so the arm's name transform is right for every baked Sidekick.

### ⚠ THE RIG HAS A `jaw` BONE AS WELL AS A `jawOpen` MORPH

`SKEL_Default_Sidekick` carries `jaw`, `eye_l`, `eye_r`, `head`, `neck_01/02` and 900-odd others. Both arms
drive MORPHS ONLY and never touch a bone, which is the standard ARKit approach and the one the three.js build
proved on these same rigs.

**But this is the single assumption that could make the whole feature silently do nothing**, so it is the
first thing to look at in the visual smoke: if the mouth does not open, the rig wants the `jaw` BONE driven
and not (or as well as) the morph. Everything else degrades visibly — a missing morph warns by name — while
this one would look like a face that simply ignores the component.

Eyes are morph-driven and need no bone: the head carries all 8 `eyeLook*` plus split upper/lower
`eyeBlink*`, which is what the idle layer will use. 156 morphs on the head in total.

### Rhubarb shape → morph mapping (the reference table)

| Shape (sound) | Morphs |
|---|---|
| A — closed (P, B, M) | `mouthClose` + light `mouthPressL/R` |
| B — slightly open (K, S, T, EE) | low `jawOpen` + slight `mouthStretchL/R` |
| C — open (EH) | mid `jawOpen` |
| D — wide (AA) | high `jawOpen` + `mouthLowerDownL/R` + `mouthUpperUpL/R` |
| E — rounded (AO, ER) | `mouthFunnel` + some `jawOpen` |
| F — puckered (OO, W) | `mouthPucker` |
| G — teeth-on-lip (F, V) | `mouthRollLower` + slight `mouthUpperUpL/R` (the roll morphs nail this) |
| H — L-ish | `jawOpen` + `tongueUp` + `tongueRaise` (Synty extensions; a plain ARKit rig drops them and keeps the jaw) |
| X — rest | neutral, breath of `mouthClose` |

### The already-tuned table (reuse, do not re-derive)

`D:\sidekick-face` (three.js) shipped a working mouth against these same rigs. Its viseme table is tuned
by eye and is the expensive part of this whole feature. Lift it verbatim as the starting mapping asset —
`sidekick-studio.template.html`, `const VIS`:

```
AA {jawOpen .85, mouthLowerDownL/R .32}         EE {jawOpen .28, mouthStretchL/R .78, mouthSmileL/R .32}
IH {jawOpen .36, mouthStretchL/R .44}           OH {jawOpen .62, mouthFunnel .72, mouthPucker .32}
OO {jawOpen .20, mouthPucker .72, mouthFunnel .38}   MM {mouthClose .68, mouthPressL/R .52}
FF {jawOpen .20, mouthRollLower .72, mouthUpperUpL/R .38}
TH {jawOpen .44, tongueOut .66, tongueUp .28}   L  {jawOpen .52, tongueUp .82, tongueRaise .58}
```

Its **amplitude driver** is the Tier 1 reference implementation, proven to read as speech: FFT over
90–4200 Hz, energy → open-ness against a decaying peak (`peak = max(energy, peak*.9992)`), gate
`(norm-.10)/.22`, spectral centroid → position along the axis `OO → OH → AA → EE`, blend the two
neighbouring poses, and add `mouthClose` as the gate closes. Also ships an idle-mouth rule for lines
with no audio (hold a random viseme 120–250 ms, 20% chance of a `MM`/rest gap).

### MetaHumans
Possible; the **same baked tracks carry over unchanged**. MetaHuman faces have no direct morphs — the face rig
sits behind an animation blueprint that is *built* to accept **ARKit-named curves** (the Live Link Face path),
and those are the same 52 names as the Synty list. So: inject named curve values into the face AnimBP instead of
`SetMorphTarget`. Caveats: one-time per-character setup is more involved (user adds our curve-injection hookup /
documented Live Link-style connection to the face AnimBP), and **MetaHumans drift between UE versions** — the
driver needs a per-version watch. Cost: +1–2 tasks.

## The two engines are NOT symmetric (verified 2026-09-03 against the shipped Sidekick tools)

Godot is out of scope for this feature: Synty do not ship Sidekick for it.

| | Unreal | Unity |
|---|---|---|
| A Sidekick character is | one `USkeletalMeshComponent` **per part slot** | **one** combined `SkinnedMeshRenderer` |
| Built by | `USKActorComponent`, parts wired with `SetLeaderPoseComponent` | the Character Creator, baked to one mesh asset |
| Morph names | `jawOpen` (unprefixed) | `MESHBlends.jawOpen` (Unity keeps the FBX group prefix) |
| Weight range | 0..1 | 0..100 |
| Addressed by | name, `SetMorphTarget` | **index**, `SetBlendShapeWeight` — resolve once via `GetBlendShapeIndex`, do not look up per frame |
| Fan-out across meshes | **MANDATORY** | none — the baked mesh is the union of head/teeth/tongue/nose |
| Identity morphs present | **yes** — must be held, never zeroed | no — already applied when the mesh was baked |

Consequences that shape the implementation:

- **`SetLeaderPoseComponent` shares BONES, not morph weights.** The Unreal driver must walk the part
  components and set the morph on each one that has it. Synty's own `USKBlueprintLibrary::ApplyMorphTargets`
  already does exactly this loop for the body morphs — copy that shape, don't invent one.
- **Unity needs no "owned set" bookkeeping.** The trap that bit the three.js build (a per-frame reset
  silently flattening the character back to base) cannot happen on a baked Unity mesh, because the identity
  morphs are not on it. It very much can happen in Unreal.
- **The mapping asset must therefore carry a per-engine name transform**, not just a name. Same table,
  three spellings across the three consumers we have written so far (Blender strips the prefix, Unity keeps
  it, Unreal has none).
- **`SKMerger` exists** (in the Sidekick tool's `Source/SKMerger`) and merges an Unreal character to a
  single skeletal mesh while deliberately preserving morph targets, with UE-version caveats written into
  it. A project on that path collapses to the Unity story: one component, no fan-out. The driver should
  therefore discover its targets rather than assume the multi-component layout.

### Audio: where the sync clock binds (answers open question 3, both engines)

Both plugins already own dialogue playback, so nothing new is needed to hang lipsync off:

- **Unreal** — `UStoryFlowComponent::PlayDialogueAudio_Implementation` spawns the component and keeps it in
  `CurrentDialogueAudio` (`SpawnSoundAttached` when `bUse3DAudio`, else `SpawnSound2D`). It is a
  `BlueprintNativeEvent`, so a game may override it and the driver must not assume it owns the sound.
  ⚠ `UAudioComponent` exposes no playback-position getter: track elapsed time from the play call, or bind
  the envelope/playback-percent delegates. This is the one place Unreal is genuinely poorer than the browser.
- **Unity** — `StoryFlowComponent.PlayDialogueAudio` creates `_dialogueAudioSource` on demand.
  `AudioSource.time` gives the position directly, and `GetSpectrumData` gives the same FFT the three.js
  driver uses, so Tier 1 ports nearly as-is.
- **Unreal's live analysis: SETTLED 2026-09-03, and it is not where this doc assumed.** `UAudioComponent`'s
  own FFT and envelope readers (`GetCookedFFTData`, `GetCookedEnvelopeData`, and the envelope delegates) are
  **COOKED**: they read analysis baked into the USoundWave asset, which a user must tick on per wave and whose
  frequencies are fixed at bake time. That is precisely the per-asset chore Tier 2's editor-side baking exists
  to remove, and if a user is going to bake anything it should be Rhubarb tracks, not FFT boxes.
  The live path is **submix spectral analysis**: `UAudioMixerBlueprintLibrary::StartAnalyzingOutput` then
  `GetMagnitudeForFrequencies(..., Submix)` each frame, which needs the **AudioMixer** module and no per-asset
  setup at all. The component therefore exposes an `AnalysisSubmix`: route dialogue to its own submix and name
  it, or leave it empty to analyse the master output, which works immediately but hears the music too.
  Consequence to remember: the analysis is per-SUBMIX, not per-AudioComponent, so two characters speaking
  through one submix would read the same spectrum. StoryFlow plays one line at a time, so this is fine today.

## Engine-side architecture

**A separate component, not built into UStoryFlowComponent** — forced by architecture: StoryFlow is the dialogue
brain and deliberately doesn't know the scene; characters are ids/data, the GAME owns which actor/mesh represents
them. Lipsync is presentational, so it lives where the face lives.

**`UStoryFlowLipsyncComponent`** — attached to each character's actor. Config: StoryFlow **character id**,
target **SkeletalMeshComponent**, **mapping DataAsset** (viseme → morph names/weights; one shared asset per rig,
per-character overrides possible). It **self-subscribes** to subsystem dialogue events; when a line's speaker
matches its id it plays that line's track in sync with the audio; no track → amplitude fallback automatically;
silence → rest. No per-line Blueprint wiring. One dialogue, many faces: N components, each animating only on its
own lines. Games with no 3D faces pay nothing.

**Driver backends** (enum or auto-detect from the mesh):
- `MorphTarget` driver — Synty-style rigs, direct `SetMorphTarget`.
- `ARKitCurve` driver — MetaHumans, curve injection into the face AnimBP.
Everything above the driver (tracks, fallback, layers, mapping) is rig-agnostic and shared.

**The design rule: the component doesn't care who plays the sound — it cares whether a track exists.**

### Public API (works for non-StoryFlow audio too)
- Automatic: StoryFlow dialogue lines — baked quality, hands-off.
- `StartLipsyncFor(AudioComponent)` / `StopLipsync()` — amplitude mode on ANY audio (cutscene VO, barks, a radio),
  live, zero prep, flappy quality.
- `PlayVisemeTrack(track, AudioComponent)` — full baked quality against the game's OWN playback of any audio that
  has a track (imported tracks are just assets; reuse a dialogue line's audio in a hand-sequenced cutscene, etc.).

### StoryFlowComponent/Subsystem changes
Only the small event hook the lipsync listens to: line started → (character id, audio, viseme track handle).
Nothing facial ever enters the dialogue components.

## Beyond lipsync: the expression layers (same component, blend channels)

The full ARKit set (brows ×4 regions, complete eye-look, upper+lower lid blinks, squints, sneers, cheeks) unlocks
two layers that matter more for perceived life than the mouth:

1. **Idle layer** — procedural blinks (randomized interval), micro-saccades (`eyeLook*`), tiny brow drift.
   Pure runtime, no data.
2. **Emotion presets driven by dialogue tags** — StoryFlow-native win: tags already fire in all engine plugins.
   `[angry]` → `browFrownL/R`+`noseSneerL/R`+`eyeSquintL/R`; `[shocked]` → `browInnerUp`+`eyeWideUpper`+`jawOpen`.
   Zero new authoring surface; writers already know tags.

One component, three layers (mouth / idle eyes-brows / emotion over both) — not three components fighting over morphs.

### Audio-driven expression: what's in and what's out
- **IN (easy, toggle `bAudioDrivenExpression`, default on, ~+1 task):** energy/emphasis from audio — loudness,
  pitch movement, speech rate extracted in the same offline pass. Drives brow accents on stressed syllables,
  `eyeWide`/`browRaise` on loud emphatic moments, calm-toward-rest in quiet stretches. Also **modulates the
  intensity of tag presets**: `[angry]` + quiet audio = cold fury; same tag + loud spiky audio = shouting.
- **OUT (deliberately):** true emotion *classification* from audio (SER models). Misfires are frequent and a
  misfire = a character grinning through a tragic line — wrong emotion is uncanny, absent emotion is just plain.
  The writer already knows the emotion; tags cost two seconds and are never wrong. **Hybrid rule: tags author the
  emotion, audio shapes its intensity and timing.**

## Estimates (in the Opus-loop, two-stage-review terms)

Revised 2026-09-03 now that both engines are scoped. **Two arms, Unity and Unreal. No Godot.**

- Tier 1, **Unity**: ~1 day. Nearly a port of the three.js driver — `GetSpectrumData` is the same FFT,
  one renderer, no fan-out, no identity morphs to protect. Mostly the mapping asset and the component.
- Tier 1, **Unreal**: ~1.5 days. Same driver, plus fan-out across part components, plus deciding the
  spectrum-analysis question above, plus tracking playback position by hand.
- Tier 2 (prebaked Rhubarb tracks): unchanged, ~3 editor tasks + 2–3 per engine arm. The editor half is
  shared, so the second arm is cheap once the first exists.
- MetaHuman driver add-on: **+1–2 tasks**. Energy/expression toggle: **~+1 task**.
- Known unknowns: Rhubarb quality/speed on real audio (one exploratory task first); user smoke is inherently
  visual — tests pin track data and sync math, only eyes judge the mouth.

Compared with the three.js build, this is **less** work than it looks: the whole asset pipeline that
dominated that project (union rig, retarget bake, scale/orientation, morph export flags, every one of them
failing silently) does not exist here. The characters are already native assets with the morphs already
imported under the right names. What is left is the driver, twice, as product code.

## THE TIER 1 DRIVER SPEC (normative — both arms implement THIS, not their own reading of it)

Written 2026-09-03 as step 1 of the order of work below. Every constant here is from the three.js build that
already reads as speech (`D:\sidekick-face`), so an arm that matches these numbers inherits its tuning. An arm
that "improves" one silently is the drift this sequencing exists to prevent: change the spec, then both arms.

### The pose table

Ten poses, each a set of ARKit morph names with a weight in 0..1. `rest` is empty. This is the table, verbatim:

```
AA  jawOpen .85  mouthLowerDownLeft .32  mouthLowerDownRight .32
EE  jawOpen .28  mouthStretchLeft .78  mouthStretchRight .78  mouthSmileLeft .32  mouthSmileRight .32
IH  jawOpen .36  mouthStretchLeft .44  mouthStretchRight .44
OH  jawOpen .62  mouthFunnel .72  mouthPucker .32
OO  jawOpen .20  mouthPucker .72  mouthFunnel .38
MM  mouthClose .68  mouthPressLeft .52  mouthPressRight .52
FF  jawOpen .20  mouthRollLower .72  mouthUpperUpLeft .38  mouthUpperUpRight .38
TH  jawOpen .44  tongueOut .66  tongueUp .28
L   jawOpen .52  tongueUp .82  tongueRaise .58
```

### The update rule, per frame

1. **Spectrum.** FFT magnitudes normalised to 0..1, over the band **90 Hz to 4200 Hz**. Bins outside it are
   ignored: below is room rumble, above is sibilance that would open the jaw on an S.
2. **Energy** = mean magnitude across the band. **Centroid** = the magnitude-weighted mean bin index, mapped to
   0..1 across the band and then **scaled by 2.6 and clamped**, because speech never reaches the top of the band
   and an unscaled centroid never leaves the OO end of the axis.
3. **Peak follower.** `peak = max(energy, peak * 0.9992)` per frame, floor `0.04`, initial `0.12`. This is what
   makes a quiet recording and a loud one both reach a full-open mouth. `norm = energy / max(0.04, peak)`.
4. **Gate.** `gate = clamp01((norm - 0.10) / 0.22)`. Below it the mouth closes rather than idling half-open.
5. **Amplitude.** `amp = min(1, norm * 1.15 * sensitivity) * gate`.
6. **The axis.** `OO → OH → AA → EE`, indexed by `centroid * 3`. Blend the two neighbouring poses by the
   fractional part, then scale every weight by `amp * strength`, and by `jawBias` for `jawOpen` only.
7. **Closing breath.** While `gate < 1`, add `mouthClose += (1 - gate) * 0.45 * strength`.
8. **Smoothing.** Ease each morph's current weight toward the target with `1 - exp(-smooth * dt)`, `smooth`
   default **16**. Frame-rate independent: never lerp by a raw per-frame constant.

Defaults: `strength 0.55`, `sensitivity 1.0`, `jawBias 1.0`, `smooth 16`.

> **SUPERSEDED 2026-09-05.** Steps 1-8 above are what both arms shipped on 2026-09-03 and what the audit below
> found wanting. The v2 rule that follows is NORMATIVE from 2026-09-05; the constants are unchanged, the INPUT
> they see is not.

### v2 (2026-09-05): the corrected update rule, NORMATIVE. Both arms implement THIS, verbatim.

Why: the reference driver (`sidekick-studio.template.html:1073-1088`) never saw linear FFT magnitudes. It saw
`getByteFrequencyData()/255`, Web Audio's DECIBEL mapping (-100 dB -> 0, -30 dB -> 1), after the analyser's own
per-bin temporal smoothing (`smoothingTimeConstant = .55`). Every constant in the spec was tuned on that input.
v2 reproduces that input inside the driver, so the constants keep their meaning on both engines.

`dt` is REAL seconds, never a dilated or scaled delta (Unity `Time.unscaledDeltaTime`; Unreal
`World->DeltaRealTimeSeconds`): the audio clock is not dilated, so the mouth must not be either.

```
input   m[0..N-1]  linear magnitudes >= 0 from the engine, all lying inside the 90-4200 Hz band
                   Unity : the bins lo..hi-1 of GetSpectrumData (slice as before)
                   Unreal: the 24 band magnitudes GetMagnitudeForFrequencies answers
        FullScale  the raw magnitude a full-scale sine produces at its own bin in THIS engine
                   Unity : 1.0 (GetSpectrumData is normalised)
                   Unreal: 5.66 by the algebra (Hann, 512, MultipliedBySqrtFFTSize: A * sqrt(512) / 4) -
                           UNMEASURED, so it is a tunable on the component and the component also
                           exposes the raw peak it saw (GetRawPeak) so the smoke can set it

1. reference domain, per bin
        a[i] = clamp01( (20 * log10( max(m[i], 1e-9) / FullScale ) + 100) / 70 )
2. spectral smoothing, per bin (state kept between frames; zeroed by ResetLevel and when N changes),
   PER SECOND like the peak follower (amended 2026-09-05: per frame was the last frame-rate dependency)
        keep = dt > 0 ? pow(0.55, dt * 60) : 1
        s[i] = keep * s_prev[i] + (1 - keep) * a[i]
   A NaN or infinite m[i] reads as 0: one bad sample must never lodge in this state.
3. energy   = mean(s)
   centroid = 0 if sum(s) == 0 else clamp01( (sum(s[i]*i) / sum(s)) / (N-1) * 2.6 )
4. peak follower, PER SECOND (the reference decayed per frame at ~60 Hz)
        peak = max(energy, peak * pow(0.9992, dt * 60))          floor 0.04, initial 0.12
        norm = energy / max(0.04, peak)
        Level = min(1, norm)
5. gate = clamp01((norm - 0.10) / 0.22)
6. amp  = min(1, norm * 1.15 * sensitivity) * gate
7. axis OO -> OH -> AA -> EE indexed by centroid * 3; blend the two neighbours by the fraction; scale every
   weight by amp * strength, and by jawBias for jawOpen only                      (unchanged)
8. closing breath: while gate < 1, mouthClose += (1 - gate) * 0.45 * strength -
   ONLY when mouthClose is an owned morph of the active table. Never write a key nothing consumes.
9. ease: K = (dt <= 0) ? 0 : 1 - exp(-smooth * dt)     dt <= 0 HOLDS, it never snaps
        w = w + (target - w) * K, then CLAMP w to 0..1 (jawBias 2 must not write 1.7)

Idle mouth: the pool is the ACTIVE table's pose names minus `rest`, sorted by name (deterministic and
identical in both arms), built once when the driver is created; a table with no other pose idles on `rest`.
Timings unchanged (hold 0.12-0.25 s, 20 % gap of MM-or-rest for 0.14-0.36 s, MM only if the table has it).
Level = 0 in the idle and silent paths (the meter reports analysed loudness, nothing else).
Centroid (the clamped axis position of the last analysed frame, 0 = OO, 1 = EE) is exposed beside Level and
is 0 in the idle and silent paths: a mouth that opens but looks wrong is a centroid pinned at one end.
FullScale is a tunable on BOTH components (Unreal AnalysisFullScale 5.66, Unity AnalysisFullScale 1.0),
floored at 0.001 on the way to the driver; RawPeak is the instrument for setting it.
ResetLevel: peak = 0.12, Level = 0, s_prev zeroed.
```

Tests that pin v2, in both arms: a REALISTIC spectrum (linear, mean ~0.005 in the band, peak bins ~0.02)
must open the jaw above 0.3 within 60 frames; frame-rate independence of BOTH the ease and the peak
follower at an amplitude BELOW amp saturation (compare a 30 fps and a 120 fps run after the same wall time
within 0.02); a custom map with four axis poses idles on those poses only; a table with no mouthClose
never gains a mouthClose target; jawBias 2 never exceeds 1.0; dt = 0 holds the previous weights exactly;
Level is 0 after AdvanceIdle. The old tone tests stay but at realistic amplitudes.

### Component contract (both arms, 2026-09-05)

1. **Re-render rule.** A dialogue update carrying the NodeId already being handled, from a speaker that is
   still mine, is a re-render (variable change, resume, dead-end): no ResetLevel, no re-arm, no audio
   re-search, no repeated warning. Only a NEW NodeId starts a line.
2. **Audio-follow rule.** A line with audio drives the mouth only while that audio is playing; when the plugin
   knows it has stopped, the mouth goes SILENT (closes), never idle. Idle is for lines that have NO audio.
   Unreal learns it from `UStoryFlowComponent::IsDialogueAudioPlaying()` (new, additive); when the game
   overrode playback and the plugin holds no audio component, the mouth follows the line as before.
   After dialogue end the mouth may keep following audio that is still playing (`bStopAudioOnDialogueEnd`
   false), and closes when it stops.
3. **Warn once, by name:** a non-empty CharacterId that the project's character index does not contain;
   no Source found (and retry the search every second, catching up on a line already on screen);
   the face missing (existing). Log once, at Log level, when CharacterId is empty: this face moves on every line.
4. **Time source:** real / unscaled delta, per the driver.
5. **Stale face:** a target is stale when its component/renderer is gone OR its mesh asset changed
   (Sidekick swaps parts by `SetSkeletalMesh` on the same component; Unity swaps `sharedMesh` on the same
   renderer). Cache the asset per target.
6. **Deactivate cleanly:** disabling/deactivating the component stops lipsync AND zeroes the owned morphs it
   drives, so a face is never left mid-vowel.
7. **Unity writes in LateUpdate** (the Animator runs after Update and would win); skips the write once at rest.
   **Unreal fan-out:** FaceRoot is the parts' PARENT (the character's mesh) or empty for the actor root -
   never a single part; the tooltip and the warning say so.
8. **Unreal analysis lifetime:** submix analysis is reference-counted across components (start on 0->1, stop on
   1->0, per AUDIO DEVICE + submix, because PIE worlds share a device; the stop releases the key the start
   took even if AnalysisSubmix changed); never started without an audio mixer device; a dedicated server does
   not tick faces at all. READING follows the mouth (stops the frame the line's sound ends); the analysis
   itself lives for the whole dialogue and stops when the dialogue is no longer active, so the engine's
   start-first warning appears at most once per conversation, not per line.
9. **Game-code lipsync is the game's to end** (amended 2026-09-05): StartLipsync / StartLipsyncFor runs until
   StopLipsync; another speaker's line and the dialogue ending leave it alone; a line of THIS face takes over.
   A manual source is trusted with whatever clip it plays. IsLipsyncActive reports either state.
10. **A text-only line arriving while the previous line's sound still plays keeps following that sound** and
   idles only once it stops (audioReset is off by default in both engines). The mouth follows the audio
   component / clip the LINE started, not whatever the dialogue component started last.
11. **Losing the dialogue component re-arms discovery** (the subscription flag clears when the reference goes
   null); a face re-activated mid-line catches up on the line showing. Unity retries the AudioSource search
   for a second before warning. The missing-morph warning is once per face, not once per mesh swap; Unreal's
   no-face warning waits for the first retry so a runtime-assembled character is not told to fix FaceRoot.


### Idle mouth (a line with audio the arm cannot analyse, or no audio at all)

Hold a random pose from the table for **0.12–0.25 s**, with a **20%** chance each pick of a gap instead
(`MM` or `rest`, 0.14–0.36 s). Same smoothing. This is what keeps a subtitled line from looking dead.

### The mapping asset schema

One asset per rig, referenced by the component, overridable per character. Deliberately flat:

```
VisemeMap
  entries[]                    one per pose name (AA, EE, … , rest)
    pose: string               the name above
    morphs[]                   { name: string, weight: 0..1 }
  nameTransform: enum          None | UnityMeshBlendsPrefix     (see the asymmetry section)
```

Resolved to engine handles ONCE, on enable, never per frame: Unity resolves each name to a blendshape INDEX
(`GetBlendShapeIndex`, after applying the transform); Unreal keeps names but resolves WHICH components own each
one. A name absent from the rig is dropped at resolve time with one warning naming it, and the rest still runs —
a missing `tongueOut` must not take the jaw down with it.

Answering open question 4: **single morph + weight per viseme, no per-shape attack/decay curves.** The global
smoothing above is what sells the motion, and per-shape curves are four more numbers per pose for the user to
get wrong. Revisit only if a real face looks wrong in a way smoothing cannot fix.

### Identifying the speaker (Unity, settled 2026-09-03)

`StoryFlowDialogueState` carries the resolved `Character` DATA but no id or path, so there is nothing on the
event surface to compare a configured id against. Rather than widen the dialogue state — a cross-engine contract
change for a presentational feature — the Unity arm resolves its configured character id to a path
(`GetCharacterPathById`) and compares `state.Character` **by reference** against the manager's
`RuntimeCharacters[path]`. That is the same instance the dialogue handler assigned, and both live in the one
`StoryFlow.Runtime` assembly, so no API changes at all.

### Identifying the speaker (Unreal, settled 2026-09-03) — and it DIFFERS from Unity

Unity's answer above compares `state.Character` by reference against the manager's runtime record. That
cannot work in Unreal: `FStoryFlowDialogueState::Character` is a STRUCT, copied by value, so there is no
identity to compare.

The Unreal answer is additive and read-only, and it is the better of the two: `UStoryFlowComponent` now
remembers the speaker path it resolved the line's character by (`CurrentSpeakerPath`, set in
`BuildDialogueState` from the same `ResolveCharacterRef` the character data itself came from) and exposes
`GetCurrentSpeakerPath()`. The lipsync component pairs it with `GetCharacterPathById(MyId)`. Id-native, and
immune to a localized display name moving under a language switch.

**The dialogue state's shape — the thing every engine plugin mirrors — is untouched**, so this is a plugin
API addition, not an engine-contract change, and it needs no register entry. Worth considering for Unity
later purely for symmetry; the reference trick there works but says less about what it is doing.

## Order of work (agreed 2026-09-03)

**STATUS 2026-09-03: steps 1-3 are BUILT, and NEITHER arm has been seen moving a face.**

**SECOND PASS DONE 2026-09-05 after an independent review of the fix pass (one Opus reviewer per arm + my own
read): Unity dev 9f7c053, Unreal dev ae67ace. Suites: Unity harness green (14 lipsync tests), Unreal 119/0
(Lipsync 16). What it closed: game-code lipsync cancelled by other speakers' lines; a text-only line idling
over the previous line's still-playing audio; the Unreal refcount keyed by world instead of audio device and
its stop key drifting; no recovery after the dialogue component is destroyed; per-conversation master-submix
warning and per-swap missing-morph warning; Unreal no-face warning before the first retry; NaN poisoning the
smoothing state; per-frame spectral smoothing; Blueprint-written FullScale of 0; servers ticking faces;
no catch-up on re-activate; the Unity audio-source search racing a game's own handler; a followed tail
never let go. Added: Centroid readout both arms, Unity AnalysisFullScale, IsLipsyncActive both arms.
Known and accepted: the centroid in the reference domain depends on absolute level and can sit near EE on a
flat spectral tilt (the reference had the same property; FullScale shifts it; read Centroid in the smoke).**

**FIX PASS DONE 2026-09-05, both arms, UNSMOKED.** Every audit item is fixed or documented:
Unity `dev` 3ffc690 (driver v2), 6b24758 (component contract), 2f466a0 (map validation), cd0e3cb (audio tail);
Unreal `dev` 89747fb (IsDialogueAudioPlaying), f703ac7 (driver v2 + tests), e59cb87 (component contract),
7adad64 (analyser lives for the whole dialogue, not per line). Suites: Unity harness 180/0 (was 173), Unreal
118/0 (was 109; Lipsync 15, was 6). Headline test red-proved in both arms: realistic linear spectrum,
jawOpen 0.000 before -> 0.44 after. The two drivers were diffed against each other and the v2 text: same
constants, same order, same idle/ease/clamp semantics.
Still open, by design: C7 is documented not detected (an unregistered submix cannot be told from one the
audio thread has not registered yet); the engine's "call StartSpectrumAnalysis first" warning can appear on
the first frames of a conversation's first voiced line (once, not per line); `AnalysisFullScale` 5.66 is the
ALGEBRA, read `GetRawPeak()` on a loud line and set it; whether `FaceRoot`'s component picker lists the parts
in the details panel is a one-minute editor check. Smoke order in the Audit's section G still applies.

**AUDITED 2026-09-05: NOT smoke-ready. Both arms match each other exactly and NEITHER matches the input the
constants were tuned on. See the Audit section below before smoking, or the smoke will be misread.**

- Unity Tier 1 — `Runtime/Lipsync/` (table, driver, map asset, component), 6 driver tests, whole suite green
  (163). Committed on `dev`; the tests live in the gitignored `_build_verify~` harness like every other test
  in that repo.
- Unreal Tier 1 — `Source/StoryFlowRuntime/{Public,Private}/Lipsync/`, 6 automation tests under
  `StoryFlow.Lipsync.*`, all 107 plugin tests pass on UE 5.3, editor target builds clean. Committed on `dev`.
- The arms were built back to back rather than gated on a smoke between them, at the user's direction. The
  tuning numbers therefore live UNVERIFIED in two places; both read them from one table each, so a correction
  is two small edits, but it IS two.

What no test can reach: whether the mouth reads as speech. That needs eyes on a real character in each
engine, and it is the gate before any Tier 2 work starts.

Sequential, not parallel arms. The gate is visual and only the user can give it, so two unsmoked arms
landing at once means fixing tuning in two codebases that have already drifted — the failure mode this
project keeps hitting with engine arms.

1. **The shared spec, no code.** Viseme table, driver behaviour (gate, attack/release, centroid axis),
   config surface, and the per-engine name/range transforms. This is what both arms consume, and what
   stops them diverging.
2. **Unity Tier 1** — the easier arm. User smokes it; the tuning numbers get corrected ONCE, on a real face.
3. **Unreal Tier 1**, consuming the corrected spec, so its only unknowns are fan-out and spectrum analysis.
4. **Tier 2 decision** after Tier 1 has been seen moving.

Runs in parallel with any of the above, because it produces data rather than visuals and so does not
compete for the user's attention: **the Rhubarb validation** (open question 1).

## Audit 2026-09-05 (both arms, code + engine sources; FIXED the same day, see the status above)

Three readers: a cross-engine pass over both arms, and one independent auditor per arm. Findings were
checked against UE 5.3 source, Unity's execution order, Synty's `SKActorComponent`, and the three.js
reference itself. Ranked. Nothing below has been changed in either plugin; the user decides.

### A. THE HEADLINE: the constants were transplanted, the input they were tuned on was not

The reference (`sidekick-studio.template.html:1073-1088`) fed the driver `getByteFrequencyData()/255`:
Web Audio's DECIBEL mapping (default -100 dB -> 0, -30 dB -> 1) with the analyser's own per-bin temporal
smoothing (`smoothingTimeConstant = .55`). On that scale speech sits at 0.2-0.6, and `PeakInitial 0.12`,
`PeakFloor 0.04`, `GateStart 0.10 / GateRange 0.22` are placed accordingly.

- **Unity** feeds LINEAR `GetSpectrumData` magnitudes: the mean over the 90-4200 Hz band for normal speech
  is ~0.005, twenty times BELOW `PeakInitial`, and `PeakFloor` is an absolute floor the peak follower cannot
  pass. Replicating the driver math exactly: jawOpen after 0.5 s / 5 s / 20 s = **0.000 / 0.000 / 0.002**
  (a hot full-scale recording: 0.10 / 0.17 / 0.35, ramping per line because ResetLevel fires per line).
  THE UNITY MOUTH DOES NOT OPEN ON A NORMAL RECORDING. `DialogueVolumeMultiplier` scales it lower still.
- **Unreal** feeds `GetMagnitudeForFrequencies` = raw `sqrt(re^2+im^2)` of a `MultipliedBySqrtFFTSize`
  FFT (engine: `SpectrumAnalyzer.cpp:884`, `VectorFFT.cpp:1136`), which by accident lands speech around
  0.1-0.3, ABOVE the floor. The gate opens (jaw 0.467 at 0.5 s). But linear dynamics are not decibel
  dynamics: with vowel/consonant energy alternating 0.15/0.015 the jaw slams **0.39 <-> 0.08** where the
  reference only dips 0.45 -> 0.39. Expect staccato flapping, not the reference's motion. And the floor's
  noise-rejection role is gone: after a loud peak the follower needs ~2 min of decay to reach 0.04, and the
  Unreal arm keeps analysing after the audio ends (C1), so room tone or music eventually opens the mouth.
- Neither arm smooths the spectrum before the driver (the reference did, at 0.55/frame). The centroid will
  jitter faster than the reference even after the scale is fixed; if the vowel axis looks twitchy, this is
  why, and the cure is smoothing the spectrum, not raising `Smooth`.

**Fix (spec change, then both arms, then tests):** convert each bin to the reference's domain before
energy/centroid: `a = clamp01((20*log10(max(mag, 1e-9) / FullScale) + 100) / 70)`, with `FullScale = 1`
for Unity and the measured full-scale magnitude for Unreal (Hann, 512, sqrt scaling: about 5.7 by the
algebra; confirm with the Level meter), then `a = 0.55*prev + 0.45*a` per bin. Every existing constant
then means what it meant. Do NOT "normalize by FFTSize/2" on Unreal (one auditor suggested it): that
divides by ~45 and reproduces Unity's dead mouth. Then re-pin the tests with REALISTIC magnitudes: the
current tone tests (Unity 0.9 in 5 bins -> energy 0.051; Unreal 0.9 in 3 of 24 bands -> 0.11) sit above
the floor and pass in a regime the engines never produce; a test feeding mean 0.005 linear must open the
jaw or the fix is not in.

The `Level` readout (`GetLevel()` / `Level`) is the instrument: on a real line Unity will read ~0.05-0.15
and never approach 1; Unreal should hit ~1 on peaks. Read it BEFORE touching the jaw-bone hypothesis.

### B. Shared driver defects (identical code in both arms)

1. Peak decay `0.9992` is per FRAME, not per second: time constant 42 s at 30 fps, 21 s at 60, 9 s at 144.
   The spec demanded frame-rate independence for the ease and forgot the follower. `EasingIsFrameRateIndependent`
   cannot see it: at the test amplitude both arms saturate `Amp = min(1, Norm*1.15)`. Fix `pow(0.9992, dt*60)`
   and run that test below saturation.
2. `dt <= 0` sets `K = 1` (snap to target) instead of `0` (hold). Unity at `timeScale 0`: audio keeps playing,
   smoothing is bypassed, maximum jitter on a still frame. Unreal at time dilation 0: same pop. Also Unity uses
   scaled `Time.deltaTime` for an audio clock: slow-motion lags the voice; use `unscaledDeltaTime`.
3. Idle pool is the built-in `PoseNames`, not the active table's keys (the reference used `Object.keys(VIS)`).
   A custom map with fewer/renamed poses gets a dead or mostly-dead idle mouth, no diagnostic.
4. Closing breath writes `mouthClose` into the target even when no pose owns it; the key is never cleared or
   consumed, so the spec step is silently skipped on such a map and the float grows forever.
5. Every re-broadcast is treated as a line start (`ResetLevel`, re-arm, Unity re-search + re-warn): variable
   change re-renders (Unreal `NotifyVariableChanged`, Unity Set* fallthrough), ResumeDialogue, dead-end
   re-render. A per-frame variable write pins the Unreal peak to 0.12 every frame -> mouth held wide open.
   Key on `NodeId`: (re)start only when it changes.
6. No clamp on the final weight: `jawOpen 0.85 * Strength 1 * JawBias 2 = 1.7` -> Unity writes 170, Unreal 1.7;
   both extrapolate the shape. Clamp 0..1 in Ease.
7. `Level` is untouched by `AdvanceIdle`, so the meter is stale while the idle mouth visibly moves.
8. A wrong/mistyped/wrong-case `CharacterId` (or any pre-P4 project, whose bridge is empty) makes
   `SpeakerIsMine` false forever with NO warning, while a missing face and a missing morph both warn. Warn once.
9. `Source` discovery is one-shot at BeginPlay/OnEnable, arbitrary with two dialogue components, skips
   inactive objects (Unity) or later-spawned actors (both), and never warns when nothing is found.
10. Empty `CharacterId` = every face moves on every line: three characters with defaults all mouth everything,
    which reads as broken rather than unconfigured.
11. Both tick and write forever from enable. Unreal is benign (`SetMorphTarget` drops keys under 1e-5, so an
    idle component stops overriding); Unity's `SetBlendShapeWeight` stamps ~0 into 18 shapes every frame and
    flattens anything else posing the mouth. Skip the write at rest.
12. Mapping assets validate nothing: a typo'd pose name or missing axis pose is a silently half-dead mouth.

### C. Unreal-only

1. **The mouth never stops when the line's audio ends.** `bSpeaking` clears only on dialogue end or another
   speaker. A 2 s line on a screen read for 12 s analyses the submix for 10 more seconds; on the default
   (master) submix the character mouths the music. `CurrentDialogueAudio` is a bare `UPROPERTY()` with no
   getter, so the lipsync cannot ask. Add `GetCurrentDialogueAudio()`/`IsDialogueAudioPlaying()` and fall
   through to idle/silent when it stops.
2. **`StopAnalyzingOutput` is global per submix** (`FMixerSubmix::StopSpectrumAnalysis` -> `SpectrumAnalyzer.Reset()`,
   no refcount) while `bAnalysing` is per component: the first lipsync actor destroyed or streamed out kills
   analysis for every other, which then reads empty arrays forever (never restarts, `bAnalysing` still true)
   plus one `LogAudioMixer` warning per tick per component. Refcount per submix, start/stop on 0<->1.
3. **Log spam with no mixer or before the analyzer is up:** `GetMagnitudeForFrequencies` logs an ERROR every
   call without an audio mixer (dedicated server, `-nosound`), and `StartAnalyzingOutput` is queued to the
   audio thread, so the first ticks of the first line always warn. Skip analysis without a device; do not set
   `bAnalysing` until the first non-empty read.
4. **Sidekick part swaps reuse the component** (`SetGeneratedComponent` calls `SetSkeletalMesh` on the existing
   part), so 616272e's weak-pointer test never fires: new morphs are never driven, the tongue decision is never
   revisited, no warning. Cache the `USkeletalMesh` per target and re-resolve when it changes.
5. **The FaceRoot guidance breaks the fan-out.** Parts are attached to the actor's existing mesh (the leader)
   as SIBLINGS (`SKActorComponent.cpp:196-197`); pointing `FaceRoot` at the head part, as the warning text
   invites, drives the head only and leaves teeth and tongue behind. Reword: "the character's mesh (the
   parts' parent) or empty for the actor root", and say so in the warning.
6. Speaker compare misses on `/` vs `\` when a node's id fails to resolve and falls back to the authored path
   (`ResolveCharacterRef` returns it verbatim, `GetCharacterPathById` returns the normalized key). Narrow
   lane, one-line fix: `NormalizeCharacterPath` both sides. Unity is immune (reference compare).
7. A named `AnalysisSubmix` that is not registered falls back to the MASTER silently in all three engine calls.
8. `Source` is a cross-actor component pointer: not settable from the details panel, so it always relies on
   discovery (B9). `FaceRoot` likely needs `FComponentReference`/`UseComponentPicker`; check in the editor.
9. `SetActive(false)` is ignored (no Activate/Deactivate override, tick enabled unconditionally).
10. `CurrentSpeakerPath` is never cleared after dialogue ends; the getter contradicts its own comment. Harmless
    for lipsync today (StopLipsync gates), a trap for the next consumer.
11. With `bStopAudioOnDialogueEnd = false` the mouth closes over the audible tail.

### D. Unity-only

1. **A mesh swapped on a SURVIVING renderer** (`smr.sharedMesh = newFace`, an ordinary outfit/LOD swap, the very
   case c56691d claims) leaves stale blendshape INDICES: fewer shapes -> `SetBlendShapeWeight` errors every
   frame; different order -> the wrong shapes move. Cache the `Mesh` per target and treat a change as stale.
2. **`Apply()` runs in `Update`, the Animator runs after it:** any facial clip touching a driven shape wins every
   frame and lipsync reads as broken. Write in `LateUpdate`. (Unreal merges same-name curves by max and keeps
   `SetMorphTarget` values across ticks, so it is safe there.)
3. Routing dialogue through `DialogueAudioMixerGroup` may make `GetSpectrumData` return zeros on some Unity
   versions. Version-dependent; one checkbox to toggle in the smoke.
4. `_speaking` is latched as a SOURCE and `MediaNodeHandler` plays through the same `_dialogueAudioSource`, so
   anything it plays next drives the mouth. Also require `_speaking.clip == the line's clip`.
5. `Ease` allocates a `List<string>` of all keys every frame per component; `Apply` does a string-keyed lookup
   per morph per frame. Cache a key array.
6. `new Random()` on Unity's Mono seeds from `TickCount`: components constructed in the same frame idle in
   lockstep; with B10 a whole crowd flaps in unison. Seed from `GetInstanceID()`.

### E. Verified solid (do not spend smoke time here)

Table, axis, idle timings and every constant identical in both arms and to the spec; 18/18 morphs resolve on
the shipped rigs. Unity speaker match is immune to `SetLanguage` (names refreshed IN PLACE, handler assigns the
manager's live instance, `externalCharacters` IS `manager.RuntimeCharacters`). Unity audio starts before the
update broadcast, so the source lookup hits on the cheap path. Unreal `CurrentSpeakerPath` is set inside
`BuildDialogueState` (reset to empty on narrator lines) before the broadcast. UE fan-out holds: followers apply
their own `SetMorphTarget` curves, same-name curves merge by MAX (no double-apply through the leader),
`ResetMorphTargetCurves` resets the computed set not the input map, identity morphs are untouchable through
the owned set. Band/bin arithmetic, NaN guards, exponential ease, warn-once latches, subscribe balance, GC
safety, per-tick allocation on Unreal: all correct.

### F. What the tests cannot see

All twelve are driver/table tests: no component coverage in either arm (speaker match, fan-out, resolve,
submix lifecycle, refresh). Tone amplitudes sit above the floor (A). `EasingIsFrameRateIndependent` saturates
(B1). `NeverOwnsAnIdentityMorph` tests a hand-written list, not `ApplyWeights`. `LoudAudioOpensTheJaw` asserts
`> 0.05` against a settled 0.46, so `Strength` off by 8x still passes.

### G. Smoke order (when the user gets to it)

1. Log `Level` on a real voiced line in each engine BEFORE anything else (A decides everything after it).
2. Write `jawOpen` directly (Unity `SetBlendShapeWeight(idx, 100)`, Unreal `SetMorphTarget("jawOpen", 1)`) for
   one frame: separates the jaw-BONE question from A.
3. Unity: same line with `DialogueAudioMixerGroup` set and cleared (D3).
4. Unreal: `FaceRoot` empty vs the head part (C5); two speakers, destroy one mid-scene (C2).
5. Only then judge the motion.

### H. Recommended fix order

A (spec, both arms, realistic tests) -> C1, C2, C3 -> B1, B2, B5, B6 -> D1, D2 -> the rest as warnings and
tooltips. A refactor on a feature nobody has seen move: pause for the smoke between A and the rest.

## Open questions for implementation kickoff
1. Validate Rhubarb on real project audio (quality, speed on long files, non-English audio behavior for localized lines).
2. Track artifact format + where it rides (sidecar vs audio index) + engine contract register entry (§ next-free).
3. ~~Where exactly Unreal-side audio playback happens today~~ — **ANSWERED 2026-09-03**, both engines, see
   "Audio: where the sync clock binds" above. The remaining sub-question is Unreal spectrum analysis vs
   envelope-only for Tier 1.
4. Mapping DataAsset schema: per-viseme single morph+weight, or small curves (attack/decay per shape)?
5. Whether the editor bakes for ALL audio in the project or only dialogue-wired audio (current design: dialogue-wired only).
6. Localization: per-language audio produces per-language tracks — confirm the export keys tracks by (audio, language).
7. Whether the driver auto-detects a merged (SKMerger) character vs the multi-component layout, or is told which.
