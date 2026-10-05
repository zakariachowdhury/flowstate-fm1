# Flowstate for FM-1: UI and experience specification (text transcription)

This is a text transcription of [flowstate-ui-spec.pdf](flowstate-ui-spec.pdf), "Flowstate for FM-1 - UI & Experience
Specification", concept specification v0.1, 14 pages. **The PDF is authoritative.** This copy exists so that tools and
agents can search the spec. Screen mock-ups are reproduced as text blocks; the line breaks follow the PDF.

How the design follows this spec: [../design/play-mode-architecture.md §16](../design/play-mode-architecture.md#16-alignment-with-the-ui-spec).

---

## Cover

> **Flowstate for M-VAVE FM-1.** An instrument where a complete beginner can press almost any key, turn almost any
> performance knob, and make something that sounds intentional.
>
> Smart Keys · Musical Guardrails · Musical Worlds · Deep Advanced Mode
>
> **Product promise:** no wrong keys, no dangerous knobs, no blank page. Make something beautiful in under 30 seconds.

First-boot screen:

```
FLOWSTATE READY
NEON RAIN
CINEMATIC
~ ~ ~~ ~~~ ~~ ~        (waveform)
PRESS PLAY
COLOR 42   MOTION 31   SPACE 76   ENERGY 55
```

## 1. Experience principle

Five minutes to understand. Years of depth underneath.

| PLAY MODE (default) | ADVANCED MODE (optional) |
| --- | --- |
| For people who know nothing about synthesizers or music theory. | The same SLOOP engine, with the training wheels removed. |
| Choose a World · press Play · press any Smart Key · turn four feeling knobs · change Scene or Variation · record a loop · trigger live FX | Full synth engines and parameters · sequencer and pattern editing · drums, samples and effects · scales, chords and automation · macro and guardrail editing · create new Musical Worlds |

**Flow:** choose a World → press Play → press keys → turn the 4 knobs → make it yours.

**What to tell a new user:** "Turn this to choose music. Press Play. The keys fit. These four knobs change the
feeling."

| Time | Milestone |
| --- | --- |
| 0:30 | **Immediate success.** The user starts the music, plays notes and changes a macro. |
| 2:00 | **Mental model learned.** COLOR, MOTION, SPACE and ENERGY make intuitive sense. |
| 5:00 | **Creative loop.** The user changes World, Scene and Variation, records a phrase and discovers an effect. |

## 2. Hardware interaction model

Three selectors and four permanent performance knobs. This is the conceptual mapping for PLAY MODE; exact low-level
bindings can be adjusted after the hardware and software audit.

**Selectors:**
- WORLD: browse Musical Worlds.
- SCENE: A / B / C / D.
- VARIATION: Original / Airy / Dark / etc.

**Knobs:**
- COLOR: dark ↔ bright.
- MOTION: still ↔ alive.
- SPACE: close ↔ huge.
- ENERGY: sparse ↔ intense.

**Buttons:** PLAY · REC · ARP / PULSE · SEQ / BEAT · FX · EDIT

| Physical control | Beginner meaning | What actually happens underneath |
| --- | --- | --- |
| WORLD | Choose the musical universe | Loads tempo, harmony, patterns, sounds, safe ranges and defaults. |
| SCENE | Move the arrangement forward | A quantized transition between authored sections. |
| VARIATION | Same idea, different feel | Applies curated pattern, sound and FX changes while preserving identity. |
| 4 knobs | Change the feeling | Each knob drives many parameters through authored curves and guardrails. |
| EDIT hold | Go deeper | Enters full SLOOP editing and authoring. |

## 3. Home and browsing

The home screen answers four questions at a glance.

```
FLOWSTATE ▶
NEON RAIN
CINEMATIC · SCENE B
▂ ▄ ▆ █ ▆ ▄ ▂
KEYS: SMART MELODY
COLOR 42   MOTION 31   SPACE 76   ENERGY 55
```

| Question | Example answer |
| --- | --- |
| What am I playing? | Neon Rain |
| What state is it in? | Playing, Scene B |
| Are the keys safe? | Smart Melody is active |
| What can I touch? | The four feeling controls are always visible. |

World browser:

```
CHOOSE WORLD
  Frozen Lake
› Neon Rain
  Night Drive
  VHS Dreams
  Dusty Cafe
CINEMATIC
```

**Browsing rules:**
- One encoder scrolls directly through the Worlds.
- Genre or category is secondary, never a required nested menu.
- Factory Worlds are read-only; edited versions save as User Worlds.
- Current playback should not glitch while browsing.

## 4. Smart Keys

Every key should feel intentional. PLAY MODE does not expose a raw chromatic keyboard by default. The World defines a
safe scale, harmony, range and role.

**Example, A minor pentatonic:** the safe notes `A C D E G` repeat across the keyboard. Chord tones are prioritized
as the harmony changes, while held notes remain stable.

**Harmony-aware behavior:** if the backing is on Am, the chord tones A, C and E are emphasized. When the harmony
moves to F, F, A and C become preferred, without abruptly repitching notes already held.

**Smart modes:** MELODY, CHORDS, BASS, DRUMS. MELODY is the power-on default; the other modes are discoverable, not
required.

**Pipeline:** physical key → Smart Key Mapper (scale, current chord, safe range, track role) → musically safe note
→ SLOOP engine.

## 5. Four feeling controls

The knobs never expose raw parameters.

| Macro | User hears | Possible hidden mappings |
| --- | --- | --- |
| COLOR (dark ↔ bright) | The tone opens or darkens. | Filter cutoff, FM brightness, harmonic balance, sample filtering, saturation. |
| MOTION (still ↔ alive) | The sound becomes more animated. | LFO depth and rate, arp activity, grain movement, drift, rhythmic modulation. |
| SPACE (close ↔ huge) | The sound moves from close to cinematic. | Reverb send and decay, delay send and feedback, release, width. |
| ENERGY (sparse ↔ intense) | The arrangement becomes fuller and stronger. | Layer activation, drum density, bass activity, pattern complexity, intensity, gain compensation. |

**Important:** 100 % SPACE does not mean "all reverb parameters at maximum". It means "the biggest version of this
World that its designer decided still sounds great".

Macro overlay:

```
SPACE
CLOSE ━━━━━━━━━●━━ HUGE
78
NEON RAIN · SCENE B
```

## 6. Scenes and variations

Arrangement without learning arrangement.

```
NEON RAIN
A INTRO
B MAIN
C LIFT
D BREAKDOWN
CHANGES NEXT BAR
```

**Scenes** are authored structural states. Switching can change patterns, tracks, harmony, mix and FX, but waits for
a musically sensible boundary:
- A: intro
- B: main groove
- C: lift / development
- D: breakdown / outro

```
VARIATION 03
✦
DREAMY
SAME WORLD · NEW FEEL
```

**Variations** are curated or deterministic alternatives that preserve the World's identity: ORIGINAL, AIRY, DARK,
PULSING, SPARSE, HEAVY, DREAMY. A variation can alter patterns, instrumentation, FX and macro defaults, but not the
core key, tempo or identity.

## 7. Recording

REC means "capture what I just played", not "learn a sequencer".

**Flow:** PLAY → REC → play keys → REC → it loops → REC to overdub.

```
RECORDING...
● ● ● ●
PLAY SOMETHING
SMART KEYS ACTIVE
```

```
LOOP 1
▂▄▆▄ ▂▄▆█
PLAYING + REC
TAP KEYS TO ADD MORE · UNDO READY
```

**Beginner recording guardrails:**
- scale-safe notes
- gentle quantization
- prevent duplicate-note buildup
- clamp impossible note lengths
- respect polyphony limits
- keep loop timing coherent
- easy undo
- no grid editing required

In Advanced Mode, the same captured material can later be opened in the full sequencer for detailed editing.

## 8. Performance buttons

Existing synth functions become musical actions.

```
PULSE                    BEAT                     LIVE FX
  OFF                      MINIMAL                FILTER  ECHO  CRUSH  FREEZE
  SLOW                   › GROOVE                   34     18    00     00
› PULSE                    BUSY                   K1      K2    K3     K4
  DRIVE                    BREAK
HOLD KEYS AND LISTEN     TURN TO CHANGE FEEL
```

**Why rename on-screen?** The physical button may still say ARP or SEQ, but the display uses beginner language:
PULSE and BEAT. The user learns by hearing the result, not by reading synthesis terminology.

**Safety:** echo feedback, resonance, crush level and freeze behavior are bounded. There must always be a clean way
back to a stable state.

## 9. Musical guardrails

The system protects musical intent, not just parameter limits.

**Pipeline:** user action → Musical Guardrail Engine → note safety (scale + harmony), sound safety (safe ranges +
gain) and arrangement safety (quantized transitions) → SLOOP core → musical result.

| Notes | Sound | Arrangement |
| --- | --- | --- |
| Key and scale awareness | Feedback caps | Bar and phrase transitions |
| Chord-tone priority | Resonance limits | Density limits |
| Safe octaves | Gain compensation | Compatible layers |
| Stable held notes | No unstable combinations | Clean fills and endings |

**Cross-macro example:** if SPACE and ENERGY are both very high, reduce the bass and drum reverb and the delay
feedback so the mix stays clear instead of turning into mud.

## 10. Optional deeper pages

Discoverable depth without making it mandatory.

```
SOUND SHAPE                          MOVEMENT
SOFT  SHORT  BODY  TAIL              DRIFT  WOBBLE  PULSE  RATE
 35    42     65    70                20     15      30     42
(ENV button in PLAY MODE)            (LFO button in PLAY MODE)
```

These pages can expose more control while keeping beginner-friendly labels. They are optional discoveries; nothing
about the first-use experience depends on them.

```
ADVANCED MODE
⌁
FULL SLOOP CONTROL
ENTER / CANCEL
```

**Advanced Mode exposes:**
- engines, FM parameters, envelopes, filters, LFOs, effects
- the sequencer, patterns, drums, samples, scales, chords, automation, the mixer
- macro mapping, guardrail configuration and Musical World authoring

## 11. Musical Worlds

Each World is an instrument + arrangement + safety model.

| World | Category | Instruments | Notes |
| --- | --- | --- | --- |
| **NEON RAIN** | CINEMATIC · 72 BPM | warm evolving pad · deep drone bass · expressive lead · sparse electronic percussion | COLOR tone · MOTION modulation · SPACE cinematic size · ENERGY layers/intensity |
| **MIDNIGHT DRIVE** | RETRO / SYNTHWAVE | analog chords · driving bass · retro drum kit · FM/chiptune lead | COLOR warmth · MOTION arp/pulse · SPACE delay/reverb · ENERGY groove density |
| **FROZEN LAKE** | AMBIENT | long pad · granular texture · sparse bell · minimal percussion | Slow harmony means even random sparse notes feel intentional. |
| **DUSTY CAFE** | LO-FI / CHILL | warm keys · dusty drums · simple bass · tape-like texture | Guardrails preserve swing, balance and warmth even at high ENERGY. |

Factory Worlds are not prerecorded audio. They are live musical systems: sequences + synths + drums + harmony +
scenes + effects + macro mappings + guardrails.

## 12. Mac simulator and authoring

Develop and audition the same musical engine before flashing hardware.

**Architecture:** a shared core (Worlds · sequencer · DSP · guardrails · macros) runs on the FM-1 HAL (hardware) or
on the host HAL (macOS audio).

**Simulator UI:**

| Region | Contents |
| --- | --- |
| Top | World, genre, BPM, current scene |
| Middle | A/B/C/D scene buttons + variation selector |
| Controls | COLOR, MOTION, SPACE, ENERGY |
| Performance | Play, Rec, Pulse, Beat, FX |
| Tracks | Pad, Bass, Lead, Drums mute/level |
| Input | virtual Smart Keyboard + computer-key mapping |

```
FLOWSTATE STUDIO                                   72 BPM
NEON RAIN · SCENE B
[ A ] [ B● ] [ C ] [ D ]    VAR: DREAMY
COLOR 42   MOTION 31   SPACE 76   ENERGY 55
PLAY · REC · PULSE · BEAT · FX    |    PAD ✓ BASS ✓ LEAD ✓ DRUMS ✓
KEYS: SMART MELODY
```

## 13. What "finished" should feel like

The product is successful when experimentation feels fearless.

| A beginner thinks… | An advanced user thinks… |
| --- | --- |
| "Every key seems to work." | "The beginner layer is just a performance surface." |
| "I turned SPACE and it became huge." | "I can edit the underlying SLOOP engine." |
| "Scene C made the song lift." | "I can build my own World." |
| "I recorded something without knowing how looping works." | "I can author macro curves and guardrails." |
| "I can't really break it." | "I can go as deep as I want." |

Flowstate should feel less like programming a synthesizer and more like entering a musical environment that wants you
to succeed.

1. **Instant:** sound within seconds.
2. **Safe:** keys, knobs and transitions remain musical.
3. **Deep:** full SLOOP power is always underneath.

> **FLOWSTATE** — Pick a World. Press Play. Make it yours.
