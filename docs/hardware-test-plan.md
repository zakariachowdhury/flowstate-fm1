<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Flowstate on the FM-1: hardware test plan (Phase 17)

A printable plan for the first session on the real device: back up, build, install, recover if needed, then test
every part of PLAY MODE against what the simulator and the docs promise. About 4 hours with the beginner test. Print
it; tick the boxes; write in the Notes column. The numbers it collects feed [hardware-calibration.md](hardware-calibration.md).

**Rules of the session**
- Stop at the first failure that can hurt (no boot, a crash loop, a howl that grows, flash that reads wrong). Photograph
  the screen, keep the monitor's `.log`, go to section 3.
- Change one thing at a time. Write down what you did before you do the next.
- Start with MASTER at about a quarter. Raise it only once a World sounds clean.
- Results: **P** passed, **F** failed (write what you saw), **N** not tested. Photograph any screen that is wrong.

Folder for everything you bring back: `mkdir -p ~/fm1-session/{csv,photos,audio,video,pkg,sim,notes}`.

---

## 0. Before you start

### 0.1 Kit

- [ ] FM-1, charged or on USB power; a USB-C **data** cable (not charge-only); a Mac with Chrome or Edge.
- [ ] Headphones, and a way to **record the line out** (audio interface or phone) at 24 bit, with a second channel.
- [ ] A phone that films at 240 fps (screen timings); a pen; a stopwatch.
- [ ] A second person who has never seen the FM-1 (T16).
- [ ] This repository built and its tests passing: `python3 tests/fm1_monitor_test.py` (no device needed).

### 0.2 Back to stock: have the way home before you leave

- [ ] M-VAVE's updater (**M-UPGRADE**) and the official FM-1 firmware, downloaded from m-vave.com now, kept offline.
- [ ] The last good SLOOP package: `sloop-2.1.fwsc` from the GitHub releases page, in `~/fm1-session/pkg/`.
- [ ] [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter) cloned, and its README read **now**.
- [ ] Section 3 of this plan printed and beside you.
- [ ] Write down what runs on the FM-1 today: ______________________ (official version, or SLOOP/Felucca with its ABOUT date).

### 0.3 Back up user data

What an install touches: the update loader writes only `0x004000–0x092FFF` (the app and the SDK config). Your projects
(`0x097000–0x09EFFF`, autosave `0x09F000` / `0x0FE000`), user samples (`0x0A0000–0x0DBFFF`), user presets
(`0x0DC000–0x0DFFFF`) and settings (`0x0FC000`) stay. Flowstate adds writes to `0x0E5000–0x0FBFFF` (user Worlds and the
PLAY session), a region SLOOP never used. Going back to stock with M-UPGRADE or FM-1-transporter may overwrite any of
it: treat nothing as safe there.

- [ ] Serve the site (the two server commands of section 2) and open the editor, `http://localhost:8000/webapp/editor/`, FM-1 connected in normal mode.
- [ ] Export the **user preset bank** and the **library** (PRESETS: export bank, export library) to `~/fm1-session/`.
- [ ] Keep the original WAV of every sample you uploaded.
- [ ] Projects A to D live only on the device (the editor's PROJECTS tab loads and saves slots, no file). A beat you
  care about: record it as audio from the line out now.

---

## 1. Build the package

```
git status --short; git rev-parse --short HEAD          # note the commit
./build.sh
shasum -a 256 build/felucca.fwsc
cp build/felucca.fwsc ~/fm1-session/pkg/flowstate-$(git rev-parse --short HEAD).fwsc
```

- [ ] `build.sh` ends with `ok image NNN B`, `ok register access`, and `package build/felucca.fwsc ... identity FM-1_900`; no error.
- [ ] Commit ________  image ________ B (561,296 B when the Phase 17 tools were written)  sha256 ________________
- [ ] The simulator plays: `build/host-bin/flowstate-sim --demo` (Option+Space), so the reference you compare with works.

## 2. Install

**A. Web installer on localhost** (Web MIDI needs a secure context):

```
python3 web/make_site.py build/felucca.fwsc dev /tmp/felucca-site
cd /tmp/felucca-site && python3 -m http.server 8000
# Chrome or Edge: http://localhost:8000/webapp/installer/
```

Power the FM-1 on in normal mode, connect it, open the page, let it find the device, press Install. Do not touch the
FM-1 or the cable until the page says it is done and the FM-1 has restarted. A cut-off install leaves the FM-1 in update
mode: press Install again and it finishes.

**B. Command line** (`pip3 install mido python-rtmidi` once):

```
python3 tools/fm1_install.py --info                     # identity of the connected FM-1
python3 tools/fm1_install.py build/felucca.fwsc
```

Exit codes: 0 done, 3 no FM-1, 4 connection lost, 5 timeout, 6 wrong model or another identity.

- [ ] Installed with A or B (circle one). The installer reported identity `FM-1_900`.
- [ ] The FM-1 restarts by itself and shows the SLOOP logo, then the FIRST screen (T1: **do not press anything yet**).
- [ ] Close the installer tab. `ls /dev/cu.usbmodem*` shows one port (the CDC console).
- [ ] The monitor sees it: `python3 tools/fm1_monitor.py --duration 5 --out ~/fm1-session/csv/smoke.csv` prints samples
  (`mode PLAY`, `world NEON RAIN`), and `python3 tools/fm1_monitor.py --cmd help` lists `flow`.

**Running the monitor for the whole session** (own Terminal tab; stop it before any install or reinstall):

```
python3 tools/fm1_monitor.py --out ~/fm1-session/csv/session.csv --tag T1
```

Type a tag and Enter at the start of every test (`T7 NR-B-all100`); an empty line clears it. Its closing summary and the
`.log` beside the CSV (resets, crash records) go in the folder. Console by hand if you need it:
`python3 tools/fm1_monitor.py --cmd status --cmd dbg --cmd crash`.

## 3. Recovery: if anything goes wrong

| Situation | Do |
| --- | --- |
| The app runs but misbehaves; or you want another build on it | Install again (section 2): the running app starts the update. |
| The app runs, installing from the page does not start | **OCT− and OCT+ held together for 5 s.** From 2 s the screen counts `UPDATE MODE IN 3..` (let go before 5 s: `CANCELLED`). At 5 s: `UBOOT`, and the FM-1 resets into the boot loader's USB mode: FM-1-transporter (its README says how to write from there). (From a terminal, `screen /dev/cu.usbmodem* 115200` and `uboot yes` does the same; the monitor refuses that command on purpose.) |
| It crashes, hangs, or the screen is black | Power off. **Hold OCT− alone while switching on.** The screen shows `SLOOP USB RESCUE · CONNECT USB · OPEN THE INSTALLER · AUDIO OFF`. Open the installer page and install the last good package (SLOOP 2.1, or this build again). |
| Two crashing boots in a row | The next start is that same rescue screen, by itself. If rescue itself fails, the ROM takes over: FM-1-transporter. |
| *Not recovery:* OCT− **and** OCT+ held at power-on | HARDWARE CALIBRATION: the FM-1 asks for each button and encoder in turn and learns the panel (it is also in the HOME menu). Use it if T3 or T4 shows a wrong mapping or a reversed encoder; rerun T3 and T4 after. |
| Rescue shows `UNKNOWN FLASH`, or nothing at all | FM-1-transporter (its README: how to reach the ROM mode), then the official firmware with M-UPGRADE. |
| Back to stock on purpose | M-UPGRADE and the official FM-1 firmware from 0.2. |
| The page cannot find the FM-1 | Another cable or port; close other MIDI software; Chrome or Edge on `http://localhost`, not a file; replug with the FM-1 on. |

After **any** unexpected reset, before you touch anything: photograph the screen, run
`python3 tools/fm1_monitor.py --cmd crash --cmd dbg --cmd status` and save the output to `~/fm1-session/notes/crash-N.txt`
(`dbg`: `prev_stage prev_page prev_home` say where the main loop was).

**Rehearse rescue once, right after T1** (it proves the way home while everything still works, and it is the first
test of the loader and the user-World region):
- [ ] Power off, hold OCT−, power on: the rescue screen (photo). Install the same package from the page.
- [ ] The FM-1 restarts into the same PLAY state as before (World, scene, macros). Rescue time, power-on to done: ______

---

## T1 Boot and the first-boot screen

First boot after the install, nothing pressed yet. You get this exactly once per device: later boots go to HOME.

| | Do | Expect | Notes |
| --- | --- | --- | --- |
| ☐ | Power on | the SLOOP logo, then **FIRST**: `FLOWSTATE READY`, `NEON RAIN`, `CINEMATIC`, the waveform, `PRESS PLAY`, the four macros ([play-first.png](images/play-first.png)). Time from power-on: ____ s | |
| ☐ | Listen on headphones | no pop, thump or hiss at boot | |
| ☐ | Press PLAY | HOME (`FLOWSTATE ▶`, NEON RAIN, `CINEMATIC · SCENE B`, the World's default scene, activity bars, `KEYS: SMART MELODY`; [play-home.png](images/play-home.png)); music within a beat; the PLAY LED follows the beat | |
| ☐ | PLAY again | stops, the tail rings out, no click | |
| ☐ | STOP, wait 25 s hands off (the session saves when stopped and quiet, 2.5 s idle, 20 s from the last save), power off, on | HOME (not FIRST), the same World, scene and macros | |
| ☐ | Ten power cycles | boots every time; `--cmd crash` says `no crash record`; `--cmd status` `boots` counts up | |

Record: boot time, photo of FIRST, anything heard at power-on. If the FIRST screen is already gone, mark N.

## T2 Display

Compare every screen with its image, on the real device: text, layout, colours (accent violet, red while recording),
the hand-drawn glyphs (▶ ■ ● ━ ✦ ⌁ ›), and no leftovers of the previous screen.

| | Screen | How to reach it | Image | Expect |
| --- | --- | --- | --- | --- |
| ☐ | FIRST | T1 | [first](images/play-first.png) | as T1 |
| ☐ | HOME | after PLAY | [home](images/play-home.png) | bars move with the music; still when stopped |
| ☐ | MACRO | turn K1, K2, K3, K4 | [macro](images/play-macro.png) | `DARK–BRIGHT`, `STILL–ALIVE`, `CLOSE–HUGE`, `SPARSE–INTENSE`; the number; back to HOME after 1.5 s |
| ☐ | CHOOSE WORLD | turn PRESETS | [worlds](images/play-worlds.png) | list, cursor, category; 4 s timeout |
| ☐ | SCENES | turn SELECT | [scenes](images/play-scenes.png) | A to D, `CHANGES NEXT BAR` while playing |
| ☐ | VARIATION | turn ALGORITHM | [variation](images/play-variation.png) | `VARIATION 03`, name, `SAME WORLD · NEW FEEL` |
| ☐ | PULSE | tap ARP | [pulse](images/play-pulse.png) | OFF, SLOW, PULSE, DRIVE |
| ☐ | BEAT | tap SEQ | [beat](images/play-beat.png) | MINIMAL, GROOVE, BUSY, BREAK |
| ☐ | LIVE FX | hold FX | [livefx](images/play-livefx.png) | FILTER ECHO CRUSH FREEZE |
| ☐ | SOUND SHAPE | hold ENV | [shape](images/play-shape.png) | SOFT SHORT BODY TAIL |
| ☐ | MOVEMENT | hold LFO | [movement](images/play-movement.png) | DRIFT WOBBLE PULSE RATE |
| ☐ | RECORDING | REC while playing | [recording](images/play-recording.png) | the bar dots, red |
| ☐ | LOOP | after the take closes | [loop](images/play-loop.png) | `LOOP 1`, `PLAYING`, `UNDO READY` |
| ☐ | SAVE | tap SAVE (stopped) | [save](images/play-save.png) | the four rows |
| ☐ | ADVANCED | hold EDIT 2 s | [advanced](images/play-advanced.png) | the dialog; the ring from 0.5 s |

- [ ] **Tearing and flashes.** Film (240 fps if you can) while you turn K1 fast, switch overlays and let HOME run 30 s:
  no torn frame, no flicker at rest, no full-black frame on any PLAY overlay. A black flash entering ADVANCED or SLOOP
  is expected (a full-screen redraw, about 77 ms).
- [ ] The bar follows a detent within about 2 frames at 60 fps (calibration doc 3.1 has the table of timings).
- [ ] Contrast, brightness and viewing angle read well; no dead or stuck pixels.

Record: P/F per screen, photos of every F, the video file names, the measured timings.

## T3 Knobs and encoders

Seven encoders: PRESETS, SELECT, ALGORITHM, K1 to K4. One detent is one step at any speed; clockwise increases.

| | Encoder | Turn | Expect |
| --- | --- | --- | --- |
| ☐ | PRESETS | 1 detent right, left | one World row down, up (the MY WORLDS row is skipped); PLAY confirms |
| ☐ | SELECT | 4 detents right | scene A to D, one per detent, no wrap; `CHANGES NEXT BAR` |
| ☐ | SELECT + GLO held | 3 detents | the tempo moves inside the World's range (`72 BPM` toast); GLO tapped twice = the World's tempo |
| ☐ | ALGORITHM | 1 detent right, left | the next, the previous variation (it wraps round) |
| ☐ | K1 COLOR, K2 MOTION, K3 SPACE, K4 ENERGY | 10 slow detents right | the number on the screen rises by 10, one a detent (the console's `macro` shows 100 more, on its 0 to 1000 scale), the bar follows |
| ☐ | K1 to K4 | a fast flick | acceleration: more than 10 detents' worth, never past 0 or 100, no wrap |
| ☐ | all seven | slow, medium, fast, both directions | no skipped or doubled detents, no reversed direction |
| ☐ | K1 and K2 together | | both register |
| ☐ | hands off | 60 s | no value moves (watch the monitor's `color motion space energy` columns) |

Then, playing NEON RAIN scene B, sweep each of K1 to K4 from 0 to 100 and back over 10 s each:
- [ ] The sound follows smoothly: no zipper noise, steps or clicks; COLOR darker to brighter, MOTION stiller to livelier,
  SPACE closer to huger, ENERGY sparser to denser (listen to what it does, not to the label).
- [ ] The macros are back where you left them after a power cycle.

Record: any encoder that skips, doubles or runs backwards (name it), the acceleration feel, the sweeps.

## T4 Buttons

Fourteen buttons. Tap every one; hold every one that has a hold. A label that does a neighbour's job, or an encoder
that runs backwards, is a panel-mapping fault: note it, run HARDWARE CALIBRATION (section 3, table) and repeat T3 and T4.

| | Button | Tap | Hold | Expect |
| --- | --- | --- | --- | --- |
| ☐ | PLAY | start, stop | | the LED follows the beat; in CHOOSE WORLD it confirms the World (on the next bar while playing) |
| ☐ | REC | arm, start the take, close it on the bar, toggle overdub | 1.5 s: clear the loop (ring from 0.7 s) | LED states: armed, recording, loop exists |
| ☐ | ARP | the PULSE list; tap again steps | | the LED is lit off the default |
| ☐ | SEQ | the BEAT list; tap again steps | | the LED is lit off GROOVE |
| ☐ | FX | | LIVE FX page, K1 to K4 = FILTER ECHO CRUSH FREEZE; a white key = a punch effect | released: the four go home in about 300 ms, no click |
| ☐ | ENV | | SOUND SHAPE page and its four knobs | the LED is dim while a control is off its home |
| ☐ | LFO | | MOVEMENT page | as ENV |
| ☐ | EDIT | UNDO (`UNDONE`, or `NOTHING TO UNDO`) | 2 s untouched: the ADVANCED dialog | the ring from 0.5 s; touching anything meanwhile cancels it; EDIT held with OCT+: REDO |
| ☐ | HOME | back; closes lists, CHOOSE WORLD forgets | 0.7 s: the menu (PLAY MODE, LEAVE WORLD) | |
| ☐ | SAVE | the SAVE list; PRESETS moves the row; SAVE again runs the row | | needs the transport stopped (`STOP TO SAVE`) |
| ☐ | SCL | `KEYS: SMART MELODY` | | |
| ☐ | GLO | twice: the World's tempo | held + SELECT: the tempo | |
| ☐ | OCT− / OCT+ | octave down, up (to ±3) | both: back to 0 | |
| ☐ | OCT− + OCT+ | | **let go at 3 s** | `UPDATE MODE IN 3..` (or 2..), then `UPDATE MODE CANCELLED`: nothing else happens. Never hold to 5 s here |

- [ ] Fast taps: tap PLAY six times quickly: the transport ends where six toggles leave it (debounce).
- [ ] LEDs: off, dim and on are three distinct states; none stays lit after its function ends.
- [ ] The tap-versus-hold thresholds feel right (EDIT 2 s, HOME 0.7 s, REC 1.5 s, FX/ENV/LFO pages on hold).

Record: a button that does not register, double-registers, or has a wrong LED.

## T5 Keyboard and Smart Melody

27 keys (F3 to G5): 16 white, 11 black. NEON RAIN first (D minor), then the other three.

- [ ] **All 27 keys, one at a time**, transport stopped, then playing. Tick each: it sounds, its LED lights while it is held and
  goes out on release, nothing sticks.
  White: `F3 ☐ G3 ☐ A3 ☐ B3 ☐ C4 ☐ D4 ☐ E4 ☐ F4 ☐ G4 ☐ A4 ☐ B4 ☐ C5 ☐ D5 ☐ E5 ☐ F5 ☐ G5 ☐`.
  Black: `F#3 ☐ G#3 ☐ A#3 ☐ C#4 ☐ D#4 ☐ F#4 ☐ G#4 ☐ A#4 ☐ C#5 ☐ D#5 ☐ F#5 ☐`.
- [ ] **White keys = the scale.** Going up the white keys goes up the World's scale, never out of it; the C4 white key
  is the tonic (NEON RAIN: D). Press them in any order over the music: nothing sounds wrong.
- [ ] **Black keys = chord tones.** They play the tones of the chord sounding, ascending in the same register; a black
  key held over a chord change keeps its pitch, a new press takes the new chord.
- [ ] **Held notes over chord changes:** hold one white and one black key across two or more chord changes (a whole
  progression): both keep their pitch exactly; let go, press again: the new chord.
- [ ] **LED chord-tone glow:** the tones of the current chord and the tonic glow dim on the key LEDs, move when the chord
  changes, and the pressed keys are brighter. Nothing stays lit after STOP or a World change.
- [ ] **Polyphony:** five or more keys at once: the cap (4 on the keys) steals the oldest smoothly; no stuck note, no crackle.
- [ ] **OCT− / OCT+:** the keys move by octaves, folded into the World's range; nothing out of range.
- [ ] **Same pitch twice** (a black and a white key that land on one pitch): both sound; letting go of one keeps the other.
- [ ] **Matrix:** five keys at once light exactly those five LEDs and sound those five notes (no ghost notes).
- [ ] **Equal loudness** on all keys (no velocity: all the same).
- [ ] Mash the keys for 60 s over each World: every note in key; any sour note: World, scene, key, chord: ________

Record: the 27 ticks, the sour notes, stuck notes, LED faults.

## T6 Audio

Record the line out (and listen on headphones). For the comparisons, the simulator's reference renders are in
`build/renders/worlds/` and T17 makes new ones.

| | Check | Expect |
| --- | --- | --- |
| ☐ | **Level.** MASTER full, each World's default scene, 30 s | the device peaks about 6 dB under the `sloop-render` file of the same scene (`OUT_SHIFT`; e.g. −4.2 dBFS in the render, about −10 dBFS on the device). Peak ____ RMS ____ per World; a constant offset is the codec's gain, a ratio that changes by World is a finding |
| ☐ | **Noise.** Stopped, MASTER full, 10 s of silence | no hiss, hum, whine or USB noise on headphones; RMS ____ dBFS. Plug and unplug the USB cable: no change |
| ☐ | **Clicks at scene changes.** SELECT A to B to C to D on their bars, a key held through them | no click or dropout at any landing; the held note sounds on (look at the recording as well as listening) |
| ☐ | **Variation changes** (all of a World's) | no click; the clock keeps running |
| ☐ | **World switch while playing** (PRESETS, PLAY) | on the next bar, tails ring on, no gap longer than a beat, no click, held keys released |
| ☐ | **Start, stop, tail** | no click; the tail dies away smoothly under −60 dBFS within about 6 s (10 s with ECHO and SPACE up) |
| ☐ | **Macro extremes** (T13 does the full set) | no clipping, no harshness the simulator does not have |
| ☐ | **Tempo accuracy.** NEON RAIN (72 BPM), 16 bars recorded | 53.33 s ± 0.05 s between bar 1 and bar 17 in the DAW |
| ☐ | **Pitch.** A tuner on the C4 white key (NEON RAIN: D) at a low and a high octave | within 5 cents |
| ☐ | **Stereo.** Headphones, a World with panned parts | left and right as in the simulator, not swapped |
| ☐ | **MASTER pot** end to end | smooth, no crackle, silent at 0 |
| ☐ | **Dropouts.** `--cmd flow` after each long run | `late` has not moved (the monitor's summary: `late halves (dropouts) 0`) |

Record: peaks and RMS per World, noise RMS, where a click or dropout is (World, scene, time), the tempo and pitch readings.

## T7 CPU

The monitor does the work; this is the plan's use of [hardware-calibration.md §2](hardware-calibration.md). Tag every item.

- [ ] Run the calibration grid (its 2.1): every World, scenes A to D, the macro corners, LIVE FX, punch FX, SOUND SHAPE,
  MOVEMENT, PULSE DRIVE, BEAT BUSY, keys, a recording loop. Heaviest first (NEON RAIN, FROZEN LAKE).
- [ ] The ADVANCED stress run (its 2.1) for the top of the range.
- [ ] Cross-check the screen against the console: in ADVANCED, GLO tapped to the SYSTEM page, CPU shows the same
  percentage as `cpu_pct`. (PLAY MODE has no CPU page; the monitor is the instrument.)
- [ ] Per World, from the monitor's summary or `--fit`'s table: peak `cpu_pct` ____ % (pass under 85), `max_us` ____
  (pass under 4,934 of 5,805), `shed` ____ (pass 0), `late` ____ (pass 0), guard engagements ____ (pass 0 for the factory Worlds).
- [ ] The guard behaving: if it did engage, what did the sound do and when did it let go (about 2 s after)?

A World that fails here is a finding for the calibration, not a reason to stop the session.

## T8 RAM, flash and user Worlds

- [ ] **Soak.** NEON RAIN playing 30 min, a macro or scene touched every minute: no reset (`resets seen 0`), `late` unmoved.
- [ ] **ADVANCED save and the stack.** Build a user World (below) three times; no reset.
- [ ] **SAVE AS USER WORLD.** In NEON RAIN: hold EDIT 2 s, tap EDIT (ADVANCED), change one track's level, hold EDIT 2 s
  (back to PLAY: `EDITS KEPT · SAVE TO KEEP`). STOP. Tap SAVE (the list opens on `SAVE AS USER WORLD`), tap SAVE again:
  the toast `SAVED: NEON RAIN 2`. Listening to a reverb tail while saving: any tick? ____ (an erase stalls audio
  about 50 ms; the save needs the transport stopped for this reason)
- [ ] **Power cycle; reload.** CHOOSE WORLD lists a `MY WORLDS` row and `NEON RAIN 2`; it loads, with your edit audible.
  Power cycle with it loaded: the device returns into it.
- [ ] A second user World (`NEON RAIN 3`), then DELETE USER WORLD on one of them (PRESETS to the last row, SAVE, SAVE
  again to confirm): the other is intact.
- [ ] **An update leaves user Worlds intact.** With two user Worlds and the session saved (stopped 25 s):
  [calibration doc 3.2](hardware-calibration.md#32-the-loader-and-the-flowstate-region): flash dump, install the same
  package (section 2), dump again, `cmp` identical, offset 0xF00 erased in every sector. Both Worlds still list and load.
  Repeat once around the rescue path of section 3, and once with a different package (a rebuild).
- [ ] Ten power cycles with user Worlds present (the SPL's boot scan): boots every time.
- [ ] `--cmd status`: `flash 1`; `usb_resets` and `usb_frame_stalls` small after the session.

Record: the flash dump file names and the `cmp` result, any reset, the user World names.

## T9 Effects

Hold the button, turn the knob; each against the simulator's behaviour.

| | Page | Control | Expect |
| --- | --- | --- | --- |
| ☐ | LIVE FX (hold FX) | K1 FILTER | home at 50 %; left low-pass, right high-pass; smooth |
| ☐ | | K2 ECHO | tempo-synced repeats that **cannot run away**; letting go returns to the World's own echo within 0.3 s |
| ☐ | | K3 CRUSH | lo-fi grit, level within about 1.4 LU of dry |
| ☐ | | K4 FREEZE | off under 25 %, then a loop of 1 beat, ½, ¼; letting go or the knob down releases it cleanly |
| ☐ | | white key held with FX | punch effects (stutter, reverse, tape stop, loop ...); none sticks; FREEZE returns after it; no click on later ones |
| ☐ | SOUND SHAPE (ENV) | SOFT, SHORT, BODY, TAIL | attack, decay and release, sustain, tail all audible, musical at 0 and at 100 |
| ☐ | MOVEMENT (LFO) | DRIFT, WOBBLE, PULSE, RATE | drift, filter wobble, tremolo, speed (home 50 %): audible, never seasick |
| ☐ | all pages | released | FX returns home by itself; SOUND SHAPE and MOVEMENT keep their positions over a power cycle |

Record: any control that does nothing, sticks, clicks or is louder than expected.

## T10 Scene transitions

| | Check | Expect |
| --- | --- | --- |
| ☐ | A scene while playing | `CHANGES NEXT BAR` (or `IN n BARS`, `NEXT PHRASE`); it lands exactly on that bar line, never mid-beat |
| ☐ | NEON RAIN scene C | waits for its 2-bar line |
| ☐ | Ask B, then C before B lands | C lands (the request is replaced) |
| ☐ | Fill | the bar before the change plays a drum fill when drums play |
| ☐ | Held keys and the keys loop | a key held through a scene change sounds on; the loop keeps its phase |
| ☐ | Variation while playing | lands on the next bar; the clock keeps running; a held pad is not cut |
| ☐ | ENERGY across its bands | layers come and go on the bar through fades; drum density on the beat; no click |
| ☐ | Against the simulator | `build/host-bin/sloop-render --world "NEON RAIN" --world-sequence --bars 2 ~/fm1-session/sim/nr-abcd.wav` plays A to D on their bars: the device does the same by ear |
| ☐ | ADVANCED: SAVE + white key 5 | `SCENES: AT ONCE`, then `ON THEIR BAR` |

Record: the bar (1, 2, 3 ...) a change landed on against the one it was asked for, per World.

## T11 Recording

| | Step | Expect |
| --- | --- | --- |
| ☐ | PLAY, REC | `RECORDING` with its bar dots, red LED |
| ☐ | Play keys for 1, 2, then 4 bars; REC | the take closes on the next bar and loops (1, 2 or 4 bars); `LOOP 1`, `PLAYING` |
| ☐ | Timing | gentle quantise: notes sit near the grid and keep part of your feel (FROZEN LAKE keeps about half) |
| ☐ | Guards | notes in key as played; no doubled notes; no stuck ties; a polyphony cap |
| ☐ | REC again | overdub: `PLAYING + REC`, `TAP KEYS TO ADD MORE · UNDO READY` |
| ☐ | EDIT tap | UNDO of the last layer; EDIT + OCT+ REDO |
| ☐ | Scene and chord changes | the loop keeps its phase and follows the harmony |
| ☐ | REC held 1.5 s | the ring, then the loop clears |
| ☐ | Switch World | the loop is cleared |
| ☐ | **Survives power-off:** make a loop, STOP, **wait 25 s hands off** (the session saves when stopped, quiet, 2.5 s idle, 20 s apart), power off, on | the loop is back and plays; with the power cut while playing, the changes since the last save are gone (by design) |
| ☐ | In ADVANCED | the loop appears as normal steps |

Record: a loop you made (describe it), what came back after power-off.

## T12 Smart Keys under stress

- [ ] Every World, every scene: white keys in scale, black keys on the chord (T5), including across variations.
- [ ] PULSE with one key held arpeggiates the chord tones; each PULSE setting differs; none sticks on release.
- [ ] An avoid note in a recorded loop plays the nearest chord tone over the chord (the loop itself is unchanged).
- [ ] Held notes: scene change, variation change, octave change, World change: held notes are released at a World
  change, kept at the others.
- [ ] Two keys, then one, of the same pitch, over a scene change: no cut, no stuck note.
- [ ] 60 s of mashing with PULSE DRIVE and a loop playing: no stuck note, in key, `late` 0.

## T13 Guardrails

Hold each corner 20 s, in every World. The product promise is "no wrong keys, no dangerous knobs".

- [ ] **Macro extremes clean:** all four at 0, at 100, ENERGY 100 + SPACE 100, COLOR 0 (the dark corner is quiet but not
  silent), ENERGY 0 (something still plays). No clipping, no harsh resonance, no runaway, no jump louder than about 8 LU
  when ENERGY steps up.
- [ ] **ECHO cannot run away.** Hold FX, ECHO 100, SPACE 100, 30 s, in each World, FROZEN LAKE (66 BPM, the slowest) last.
  The echo never grows. Let go, STOP: the tail is under −60 dBFS within about 8 to 10 s.
  *Stop rule:* if the sound grows into a howl, MASTER down, let go of FX, STOP, photograph the pages and report.
- [ ] **FILTER, CRUSH, FREEZE** at their ends with keys held and a loop playing: clean.
- [ ] **SOUND SHAPE and MOVEMENT** at 0 and 100 together with MOTION 100: musical.
- [ ] A big room with a long echo (SPACE 100, ECHO 100): the combination is capped, the mix stays clear.

Record: the World, the corner, and what you heard; the recorded peak (it should stay under −3 dBFS).

## T14 Advanced Mode

- [ ] Enter: hold EDIT 2 s, tap EDIT: the SLOOP UI over the World (the macros frozen where they were).
- [ ] Edit a sound, a pattern step, a mix level; play: it takes. Exit (EDIT held 2 s): `EDITS KEPT · SAVE TO KEEP`, the
  World plays the edits in every scene and variation.
- [ ] SAVE list: **SAVE AS** (row 0), **SAVE** (row 1: over the user World, or SAVE AS on a factory one), **RESET WORLD**
  (asks; the World as saved or as built), **DELETE USER WORLD** (asks; on a factory World `NOT A USER WORLD`).
- [ ] *Optional.* Change more than 64 parameters before leaving (sweep many knobs on several pages): `TOO MANY EDITS`,
  no crash. Fill several 64-step patterns and SAVE: either `SAVED` or `WORLD TOO BIG` with nothing written, never a crash.
- [ ] **LEAVE WORLD** (HOME held 0.7 s, the menu): with unsaved edits `LEAVE WORLD? NOT SAVED`, OK again leaves; the **SLOOP
  project comes back exactly** (sounds, steps, tempo, mutes) as before PLAY MODE (T15).
- [ ] CPU page (GLO, SYSTEM) works here (T7).

## T15 SLOOP mode regression

A SLOOP project must play and feel as on stock SLOOP 2.1.

- [ ] From PLAY MODE: HOME held 0.7 s, LEAVE WORLD. You are in SLOOP with the device's own project.
- [ ] Make a recognisable beat: SLOOP.md "Sixty seconds to a beat" (a drum loop by REC, an overdub, an ARP hat roll, a
  bass line, SCL chords, a punch effect, undo with EDIT + OCT−), then SEQ steps, GLO mute and tap tempo, SAVE into a slot,
  sections A to D and the SONG page.
- [ ] HOME held, PLAY MODE, play a World for a minute, HOME held, LEAVE WORLD: the project is **exactly** as it was.
- [ ] Power-cycle in SLOOP mode: the autosave brings the beat back, and the device boots into SLOOP.
- [ ] The web editor connects and shows the engines (`http://localhost:8000/webapp/editor/`).
- [ ] **A/B with stock (recommended):** install `sloop-2.1.fwsc`, do the same script by ear and by recording, install Flowstate
  again, repeat. Same sounds, same timing, same loads (`cpu_pct`). (Two installs; user Worlds survive: T8.)
- [ ] CPU in SLOOP mode for your densest project: `cpu_pct` ____ (flow `mode=SLOOP`, `est` 0).

Record: any difference from SLOOP 2.1, however small, with the recording.

## T16 The five-minute beginner test

With someone who has never seen an FM-1 and plays no instrument well. The spec's milestones (UI spec §1): **0:30**
immediate success, **2:00** the mental model, **5:00** the creative loop. You watch; you do not help.

Say only: *"Turn this to choose music. Press Play. The keys fit. These four knobs change the feeling."* Start the
stopwatch when you finish the sentence. The FM-1 starts at HOME with NEON RAIN (power-cycle it first if a user World or
another World is loaded; a first-boot FIRST screen is the best start, if you still have one).

| Time | Milestone | Observed at (m:ss, or not) | Notes |
| --- | --- | --- | --- |
| 0:30 | presses PLAY ____ plays a note ____ turns a macro ____ | | |
| 2:00 | ask: "which knob makes it sound bigger?" "which makes it busier?" | answers: | |
| 5:00 | changed World ☐ scene ☐ variation ☐ recorded a loop ☐ used an effect (FX held) ☐ | | |

Also note: where they hesitated, which button they pressed that did nothing for them, any moment they thought they had
broken it (did anything actually break, stick, or fall silent?), and anything they said out loud. Afterwards ask three
things and write the words: Did what you played sound like you meant it? What was confusing? Would you pick it up again?
Film it if they agree.

Record: the three timings, the answers, the quotes, and whether any milestone was missed.

## T17 Simulator against hardware

Same World, scene, variation and macros on both; does it sound and behave the same? Make the references first (raw
mix, 6 dB hotter than the device; `--analyze` prints peak and RMS):

```
R=build/host-bin/sloop-render; O=~/fm1-session/sim
$R --world "NEON RAIN" --bars 8 --analyze $O/1-nr-a.wav
$R --world "NEON RAIN" --scene B --ctl COLOR=1.0,MOTION=1.0,SPACE=1.0,ENERGY=1.0 --bars 8 --analyze $O/2-nr-b-all100.wav
$R --world "NEON RAIN" --scene D --ctl COLOR=0,MOTION=0,SPACE=0,ENERGY=0 --bars 8 --analyze $O/3-nr-d-all0.wav
$R --world "MIDNIGHT DRIVE" --scene B --var DRIVING --bars 8 --analyze $O/4-md-b-driving.wav
$R --world "FROZEN LAKE" --scene C --ctl SPACE=1.0,ENERGY=1.0 --bars 8 --tail 8 --analyze $O/5-fl-c-space.wav
$R --world "DUSTY CAFE" --scene B --bars 8 --analyze $O/6-dc-b.wav
```

Play each on the device from a fresh World load (macros set with the knobs to the same numbers), record the line out
for 8 bars plus the tail, and fill in the row. Device peak and RMS are expected about 6 dB under the file's.

| # | World, scene, variation, macros | Simulator file peak / RMS (dBFS; the device should read 6 dB lower) | Device peak / RMS | Same? tempo, parts, brightness, space, ENERGY layers | Differences |
| --- | --- | --- | --- | --- | --- |
| 1 | NEON RAIN, its default scene (B), ORIGINAL, defaults | | | | |
| 2 | NEON RAIN B, all four 100 | | | | |
| 3 | NEON RAIN D, all four 0 | | | | |
| 4 | MIDNIGHT DRIVE B, DRIVING, defaults | | | | |
| 5 | FROZEN LAKE C, SPACE and ENERGY 100, tail 8 s | | | | |
| 6 | DUSTY CAFE B, defaults (swing, kit) | | | | |
| 7 | LIVE FX: FILTER swept, ECHO 100, released | by ear | | | |
| 8 | SMART KEYS: tonic, one black key, over a chord change | by ear | | | |

Check on every row: the same tempo (bars line up over 8 bars), the same parts and notes (same scale and chords), the same
tone (COLOR), the same size (SPACE), the same density (ENERGY bands), the same tail length. A **different** pitch or
tempo is a clock or table problem; a different **tone** is the codec or the analog stage; a different **timing of the
landing** is the scheduler. Describe, do not guess.

---

## What to bring back, and sign-off

- [ ] `session.csv` and its `.log`, `cal.csv`, the `--fit` output, the flash dumps and `cmp` result.
- [ ] Photos (every F), the display video, the latency recording, the line-out recordings, the simulator references.
- [ ] This sheet, filled in; the build commit and `shasum` from section 1.
- [ ] A Phase 17 entry for `progress.md`: what passed, what failed, the new `guard_limits.h` values.

| Section | P / F / N | Notes |
| --- | --- | --- |
| T1 Boot and first boot | | |
| T2 Display | | |
| T3 Knobs and encoders | | |
| T4 Buttons | | |
| T5 Keyboard and Smart Melody | | |
| T6 Audio | | |
| T7 CPU | | |
| T8 RAM, flash, user Worlds | | |
| T9 Effects | | |
| T10 Scene transitions | | |
| T11 Recording | | |
| T12 Smart Keys under stress | | |
| T13 Guardrails | | |
| T14 Advanced Mode | | |
| T15 SLOOP regression | | |
| T16 Beginner test | | |
| T17 Simulator against hardware | | |
| Recovery rehearsal | | |

Tester ____________________  date ____________  FM-1 serial / firmware before ____________________
