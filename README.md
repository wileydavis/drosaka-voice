# drosaka Voice

Private Ableton instrument for the drosaka tracks. `SPEC.md` is the source of truth.

## Build (Mac)

```
./build.sh
```

The first run installs CMake via Homebrew if needed and clones JUCE 8.0.4 into `./JUCE`. It then builds AU and VST3 and copies them into `~/Library/Audio/Plug-Ins/Components` and `~/Library/Audio/Plug-Ins/VST3`. After that, rescan in Live (Settings › Plug-Ins › Rescan). Use AU or VST3, whichever Live shows; they are the same instrument.

Optional AU validation: `auval -v aumu DrVc Sfrq`

Offline tests (no host needed):

```
cmake -B build -DDV_BUILD_TESTS=ON && cmake --build build --target DrosakaVoiceTests
./build/DrosakaVoiceTests_artefacts/Release/DrosakaVoiceTests
```

## Status

| Milestone | State |
|---|---|
| 0 Toolchain | ✅ done — builds on Mac, AU + VST3 load in Live |
| 1 Sample player | ✅ done — tested in Live (AU + VST3), no issues |
| 2 Voice bank | ✅ done — tested in Live, works |
| 3 Codec, single voice | ✅ done — tested in Live, sounds good |
| 4 Encode count | Written and passing offline tests. **Awaiting ear test in Live** |
| 5 Spread (+ stem alignment) | ✅ done — tested in Live, sounds great |
| 6 Spatial modes + tempo sync | ✅ done — tested in Live, sounds good |
| 7 Slicing + presets w/ embedded audio | ✅ done — tested in Live, worked perfectly |
| 8 Pattern mode + open questions (Drop, Keep primary human, Depth) | ✅ done — tested in Live, sounds good |
| 9 4x4 control grid + MIDI learn | ✅ done |
| 10 Automatable slice markers, autotune, recording | Written and passing offline tests. **Awaiting a test in Live** |

### Milestone 10 behaviour (as built)

**Stem gain:** each row has a **gain** cell to even out stems recorded at different levels: drag it up or down (Shift for fine), double-click for 0 dB. Range −24 to +12 dB. It saves with the set, and each bank keeps its own.

**Stem outputs** (to put effects on one voice): click a row's **out** button (it shows the stem's number, N). That stem now plays on the plugin's **Stem N** output instead of the main mix. In Live, make an audio track, set *Audio From* to the drosaka Voice track and choose **Stem N** underneath, and set *Monitor* to **In**. Put your effects on that track.
- The stem keeps everything the plugin does to it (gain trim, Count, codec, pan), except the shared Depth reverb
- N is the stem's number in the bank (the order you added stems). Removing a stem renumbers the ones after it, so check your routing after removing one
- Each bank keeps its own routing; output N carries stem N of whichever bank is playing
- Up to 16 stems can have their own output

**Banks** (1–4 in the header, or the **Bank** parameter). One instance holds four phrases. Each bank has its own stems, markers and settings: every knob, menu and switch except BANK, REC and DROP, so Play mode, Latch, Scheme, Tune root and the rest are per bank.
- **Switching** lands on the next bar line while Live plays, and at once when it's stopped. A pulsing number is waiting for the bar; a lit one is playing; a dim one is empty
- **Leaving a bank keeps it** exactly as you left it, tweaks included. A bank you open for the first time starts with the settings you had
- **Your controller follows:** on a switch the plugin tells Live the new values, and Live sends them to a controller mapped with **Live's MIDI Map** (Cmd-M) if that controller's output has *Remote* on in Live's MIDI preferences. The plugin's own right-click MIDI learn can't send anything back, so map with Live for this
- **Arrangement automation wins** over a bank's recalled value, as on any device
- **On the switch**, whatever is sounding fades on the line, and a latched pattern carries on from the new phrase's first slice (or stops, if that bank is in Manual)
- **To fill a bank:** click its number (stopped, it switches at once), then drop stems on the window as usual. The Live set saves every bank with its audio. Sets from earlier builds open into bank 1

### Banks test in Live

1. Fill banks 1 and 2 with different phrases and settings (say Anchor in one, Rotate in the other, different Spread)
2. Map **Bank** with Live's MIDI Map and turn it while Live plays: the change should land on the next downbeat, and your controller's rings should jump to bank 2's settings
3. Tweak a knob, go to bank 1 and back: the tweak should still be there
4. Save the set, reopen it: all banks come back

**The first marker starts the phrase.** Everything before it is dead space and never plays (it's dimmed on the waveform). N markers make N slices: the first marker starts **C3**, the next C#3, and so on, and the last slice runs to the end of the audio. Any other key plays from the first marker to the end. *This moved every slice key down a semitone from earlier builds, so a clip written for C#3 now plays the slice after the one it used to.* With no markers, the whole stem plays.

**Slice markers are automatable, as offsets.** Each marker has a **home** position, set by placing or dragging it on the waveform and saved with the set. Each also has an automatable **offset** parameter named for the key whose slice it starts: *C3 slice offset*, *C#3 slice offset* … *D4 slice offset*.
- **The lane:** an offset lane starts at **0 in the middle**. Up moves the marker later and down moves it earlier. The range is the marker's own room: **+100%** takes it right up to the next marker (or, for the last marker, the end of the audio), and **−100%** takes it back to the previous marker (or the start). ±50% is halfway
- **Display:** on the waveform, an offset marker shows where it is now, with a faint line back to its home
- **No overtaking:** offsets can't push a marker past its neighbour; it stops just after it
- **Dragging** a marker moves its home and keeps its offset, so it lands where you drop it
- **Adding or deleting** a marker renumbers the ones after it (keys follow time order), and their current offsets move with them so nothing jumps. Automation lanes stay with their keys
- **Existing sets:** markers from earlier builds, including ones stored out of order, are picked up in time order, so each named offset moves the right marker

**Autotune** (Performance panel: TUNE, ROOT, SCALE):
- **Root:** every voice sits on the root note, each in the octave nearest its own register, so the crowd keeps its spread. This is the most robotic setting
- **Minor / Major:** each pitch period snaps to the nearest note of that scale on the root
- **TUNE** sets the strength: 0 is off, 100% is a hard snap, and values in between pull part of the way
- **Dry voices** are retuned period by period with the same pitch-shifting as Spread (formants and timing kept), and encoded voices are retuned inside the codec. It stacks with Spread and Screwed
- TUNE is MIDI-learnable like the grid knobs

**Harmony Spread** (replaces the relative intervals below; SPREAD, SNAP, CHORD, plus DRIFT in the Performance panel). The crowd now sings real notes on the **Tune ROOT**, and the primary stays human:
- **natural → chorus → chord → cluster:** at *chorus*, every crowd voice sings the root, each in the octave nearest its own register, slightly detuned and drifting, so it beats and shimmers like a choir on one note. At *chord*, the voices take the CHORD menu's tones on the root (C minor triad on a C root sings C, E♭, G). At *cluster*, they take neighbouring notes of the Tune SCALE
- **Harmony keys C1–B1:** hold notes in that octave and the crowd sings them instead of the menu chord, spread low to high across the voices. Holding keys lifts Spread to at least *chord*, even from 0. Let go and the menu chord comes back. These keys don't trigger the phrase or patterns
- **DRIFT:** each voice wanders slowly in pitch, up to about ±28 cents. It's strongest at *chorus*. At 0 the detune is static
- **No voice moves more than 6 semitones**, because each sings its note in its nearest octave. Dry and encoded voices sing the same notes, so the octave jump on encoding is gone
- **TUNE** still works on its own at *natural*, and it's the only pitch effect on the primary
- Each row shows the note its voice is singing

Spread tour without Live: `DrosakaVoiceRender --spread out.wav primary.wav others...` renders natural, chorus, C-minor chord, cluster, a slow sweep, harmony keys E1+A1, then the encoded chord. Set `DV_CROWD_ONLY=1` to mute the primary.

### Harmony Spread test in Live

1. Set ROOT to your track's key, Count to all, then sweep **SPREAD** slowly: speech, then a shimmering unison on the root, then the chord, then the cluster
2. At *chord*, hold two or three notes on **C1–B1** with your left hand while triggering the phrase with your right: the crowd follows your chord changes
3. Turn **DRIFT** from 0 to 100% at *chorus*: from a static detune to a seasick choir. Where should the default sit?
4. Set **SCALE** to Minor or Major and go to *cluster*: does a scale cluster read better than the old semitone smear?
5. Encode some voices at *chord*: they should stay on the same notes
6. Judgement calls: is the chorus detune too wide, or too narrow? Do you want the primary able to join the harmony as an option?

**Recording takes:**
1. In Live, open the device's **Sidechain** section (the triangle at the left of the device, or the device title bar for AU), switch it on, and pick the track to record from, such as a mic track, in *Audio From*. The meter under REC is always live: "input −xx dB" means audio is arriving, "no input" means Live isn't sending any, and "input silent" means the route is there but quiet
2. Press **REC** in the Performance panel. The meter shows the input and the timer counts up
3. Press **REC** again to stop. The take is trimmed of silence, saved as a WAV in `~/Music/drosaka Voice/Takes`, added to the bank, auto-aligned to the primary, and embedded in the set like any stem. Takes can run up to 60 s; a full buffer stops itself
- REC is a parameter too, so you can MIDI-learn it or automate it to punch in on a cue
- A silent take adds nothing, and the footer explains how to route audio in

### Controls: the 4×4 grid and MIDI

All sixteen knobs sit in one 4×4 grid, row by row:

| | | | |
|---|---|---|---|
| COUNT | SPREAD | SNAP | CHORD |
| ENCODE | BIT RATE | FRAME DROP | DEPTH |
| SPACE | RATE | SCHEME | DIVISION |
| LENGTH | FILL | STUTTER | VARIATION |

- **Colours:** each knob is coloured by its section (crowd, pitch, codec, space, pattern)
- **Readouts:** each cell shows a live value ("2.5 of 5", "chorus → chord", "1.5 of 3 audible")
- **Double-click** a knob to reset it
- **The rest:** the menus and switches (PLAY, PITCH LOCK, ENCODE STEPS, DROP, KEEP HUMAN, LATCH), TUNE/DRIFT/ROOT/SCALE, the pattern feel knobs (GATE, SWING, ACCENT, SCREW DROP) and the stereo-field view sit in the Performance panel beside the grid
- **MIDI learn:** right-click a knob, choose MIDI learn, then turn a controller. The knob shows its CC in the corner. Any channel works, and one CC drives one knob (re-learning moves it). Right-click and choose Forget to remove it. Mappings save with the Live set and presets
- **VST3 vs AU:** the VST3 is built without JUCE's 2,080 hidden "MIDI CC" parameters, which had pushed it past Live's 64-parameter auto-list limit, so Live only showed the last-touched parameter. All of them now list automatically on a **fresh** instance; an existing instance keeps its old list until you replace it or use Configure. The catch: a VST3 can't receive CCs without those parameters, so in the **VST3**, map knobs with Live's **MIDI Map mode** (Cmd-M, then click the parameter in the device panel). In the **AU**, both Live's MIDI Map and the right-click learn work
- **Host parameters:** every control (47 in all, including the 15 slice markers, Tune and REC) is an automatable parameter, so Live records MIDI-learned moves as automation, and Live's own MIDI Map / Configure work too. The AU's parameters should appear in Live's device panel automatically. If the VST3's don't, use Configure, or just use MIDI learn
- **Controller layout:** the grid is laid out to match a 16-encoder controller like the MIDI Fighter Twister one-to-one: learn each knob to the encoder in the same position

### Pattern schemes (Nov 1 rework)

Switch **PLAY** to Pattern and hold one key. The loop starts from that key's slice on the next grid line and runs on through **every** slice, wrapping around. C#3 starts at slice 2; a key outside the slice range starts at the first slice. Holding another slice key restarts the loop there; LATCH keeps it going after you let go.

**Four macros, the same in every scheme** (bottom grid row, all automatable and MIDI-learnable):
- **LENGTH:** how many slices the loop uses, from the trigger slice (one word on repeat … the whole line). Automate it over 16 bars to unfold the phrase. A change waits for the loop's next restart, so nothing stumbles
- **FILL:** how many of the bar's steps get a hit, spread evenly (Euclidean, first hit on the downbeat): 3, 5 or 7 of 16 for sparse techno pulses, all 16 for a stream. Rests don't skip words; the phrase picks up on the next hit
- **STUTTER:** rolls. A hit becomes 2, 3, 4 or 6 retriggers of the slice's opening, more of them and faster as you turn it up, gathering at the end of every 4 bars like a fill before the turnaround
- **VARIATION:** how far each bar strays from the loop: skipped words, repeats, jumps ahead, words reversed, a word dropped a fifth, quiet ghost hits. Bars 4 and 8 stray most, and the 8 bars repeat, so it sounds composed

**Feel** (Performance panel): **GATE** (how much of each step sounds), **SWING** (50–75%, off-steps late; straight grids only), **ACCENT** (beats loud, "and"s softer, 16ths softest; rolls build up), **SCREW DROP** (for Screwed).

**SCHEME** (the order the slices come in):
- **Phrase:** in order. The loop restarts on a bar line once it has had room for every slice, so a long phrase spans 2, 4 or 8 bars and always lands on the one
- **Anchor:** the trigger slice on every beat like a kick, the rest of the phrase on the steps between
- **Offbeat:** only the "and"s, as short stabs (FILL picks which ones)
- **Rotate:** each bar starts one slice later, so the phrase slides across the downbeat
- **Polymeter:** the loop ignores the bar: 3, 5 or 7 slices against 4/4, realigning every few bars
- **Ping-pong:** forward, then back
- **Call & answer:** the phrase, then the same number of bars answering with the trigger slice chopped
- **Shuffle:** a new order each bar, the same 8 bars every time
- **Screwed:** each slice over two steps, slowed and pitched down by SCREW DROP
- **Build:** the division halves every bar (four times) for a roll into the drop; retrigger to restart

Pattern hits start on each slice's first sound, so a word that opens with a pause still lands on its step. Everything is worked out from the song position, so the same bars of your arrangement always play the same way. DIVISION now defaults to 1/16. Sets saved before this rework open with the nearest scheme (Walk/Repeat/Stutter/Skip → Phrase, Ratchet → Build, Random → Shuffle).

Pattern tour without Live: `DrosakaVoiceRender --pattern out.wav primary.wav others...` slices at word gaps and plays every scheme over a kick (`DV_NO_KICK=1` for none).

### Pattern schemes test in Live

1. Slice a phrase into words, PLAY → Pattern, LATCH on, hold C3. Automate **LENGTH** from 0 to 100% over 16 bars: the phrase should unfold word by word, always landing on the downbeat
2. Pull **FILL** down to 3–7 of 16 with a kick running: does it groove?
3. Try **Anchor** and **Offbeat** against a four-on-the-floor kick
4. **STUTTER** up: are the rolls in the right places? Too many, too few?
5. **VARIATION** at 30–60%: does bar 8 feel like a fill, and does the 8-bar block repeat?
6. **SWING** and **ACCENT**: where do they sit best?
7. Judgement calls: is **Call & answer** useful as it is? Should **Polymeter** count slices or steps? Any scheme you'd drop or add?

### Milestone 8 behaviour (as built, patterns superseded by the schemes above)

**Pattern mode** (PLAY → Pattern). One key starts a tempo-locked walk through the slices on the next grid line:
- **Walk** plays one slice per step, in order
- **Repeat** plays each slice Repeat-count times before moving on
- **Stutter** plays the slice on one step, then Repeat-count shorter retriggers of its opening on the next, then moves on
- **Screwed** pitches each slice down by Drop (default −5 st) and stretches it to fill the step
- **Skip / Rewind:** Density is the chance a step goes silent or steps back a slice. It's repeatable: the same pattern every time
- **Ratchet** halves the division every bar, from when you select it, accelerating into a drop
- **Random** shuffles the slices freshly on each pass; Density is the chance each step fires
- **Fitting:** a slice shorter than its step is stretched to fill it (up to 2×, pitch unchanged); a longer one is truncated. **Gate** sets how much of each step sounds
- **Keys:** a slice key starts the walk from that slice, and holding one mid-walk jumps there on the next step. Releasing every key stops the walk, unless **LATCH** is on
- **Timing:** hits land exactly on Live's grid

**DROP** (Space strip, automatable and MIDI-mappable) is the drop: the crowd fades out and the primary comes back human, dead centre, whatever else is set.

**KEEP PRIMARY HUMAN** (Encode strip): Encode never takes the primary, so 100% means every voice but the real one.

**DEPTH** (Space strip, default 0 = pure panning) sends the outer voices back: up to −6 dB, 12 ms later, and into a shared reverb. The primary is never affected. Depth follows each voice's place in the crowd, not its moving pan, so Bounce and Orbit never bend pitch.


### Milestone 8 test in Live

1. Slice a phrase into words, switch PLAY to Pattern, and hold a key: Walk on 1/8 should put the loose phrase on the grid
2. Try each pattern. Does Stutter give you "they they they stole"? Does Screwed sound chopped-and-screwed?
3. Hold a slice key while it walks: it should jump. Try LATCH
4. Automate **DROP** at the drop: the crowd vanishes, and the human voice is left in the centre
5. **KEEP PRIMARY HUMAN** with Encode at 100%: every voice but the real one should be on the codec
6. **DEPTH** up on a wide crowd: the outer voices should sit further back without smearing

### Milestone 7 behaviour (as built)

- **Slicing:** on the top waveform, **double-click** to add a marker, **drag** to move one, **right-click** to delete. Each region is labelled with its key, starting at **C3**. Because markers sit on the aligned timeline, one marker cuts every voice, and one key fires that word across the whole crowd. Any key outside the slice range plays the whole phrase
- **Each slice** starts on its marker and fades out exactly on the next one
- **The Live set carries the audio:** every stem is saved inside the set, losslessly (24-bit FLAC), with offsets, mute/solo, primary, slices and every knob. The set opens on another machine, or after the source files have moved
- **Preset** menu: Save or Load a `.dvoice` file holding the same bank with its audio, so a phrase bank can move between sets
- Older sets that point to files still open, and are saved with the audio embedded from then on

### Milestone 7 test in Live

1. Slice the phrase into words. In a MIDI clip, play C3, C#3, D3… and each word should fire across the whole crowd, in sync
2. Re-trigger a word on 1/16s ("they they they"): there should be no clicks
3. Save the set, rename the folder with your stems, reopen the set: everything should still be there and play
4. Preset, then Save preset; drop a fresh device on another track, then Preset, then Load: the same bank, slices and knobs should come back
5. Check the set's file size is reasonable with a full 20-voice bank (about 0.5 MB per voice-minute)

### Milestone 6 behaviour (as built)

- **SPACE** picks how the audible voices sit in the stereo field. The primary always stays dead centre
  - **Radiate:** pairs fan out from the centre, wider with distance
  - **Bounce:** on every division, each voice hops to a new place. The crowd stays spread edge to edge, and the same bar always bounces the same way
  - **Orbit:** voices circle the field, one revolution per division
  - **Swap:** the field mirrors left and right on every division
- **RATE** sets the division: 4 bars down to 1/16, plus triplets
- **ENCODE STEPS:** Free, or 1/16 to 1 bar. Quantised, the codec takes voices only on grid lines, so a ramped Encode knob infects the crowd one voice per beat
- **Tempo:** follows Live's tempo and position, and the timing accounts for the plugin's latency so changes land on the beat as you hear it. With Live stopped, an internal clock keeps motion running. The readout shows which clock is in charge
- **Field view:** a small stereo-field display shows each audible voice as a dot. Orange is the primary, teal is encoded

Space tour without Live: `DrosakaVoiceRender --space out.wav primary.wav others...` renders Radiate, Bounce 1/8, Orbit, Swap 1/4, then Bounce 1/16 with Encode stepping on the beat.

### Milestone 6 test in Live

1. With the transport running, try each **SPACE** mode at a few **RATES**: hops should land on the grid alongside your drums
2. **ENCODE STEPS** at 1/4: ramp Encode over 8 bars, and voices should flip exactly on beats
3. Stop the transport: motion keeps going on the internal clock
4. Change Live's tempo: the motion follows
5. Judgement calls: keep Orbit and Swap? Add depth cues (level, short delay, reverb on outer voices), or stay with pure panning?

### Milestone 5 behaviour (as built, superseded by Harmony Spread above)

- **SPREAD** (automatable) moves the crowd from unison to chorus to chord to cluster. Every voice glides continuously, and the primary never moves
- **CHORD:** Fifths, Minor triad, Major triad, Stacked fourths, Minor 7th or Octaves. Voices above the primary take the chord's upper tones and voices below take its lower ones, with the outer pairs widest
- **SNAP:** 0 is pure portamento; higher values linger on each station and rush between them
- **Dry voices** are pitch-shifted per pitch period (PSOLA), which keeps the voice's formants and timing. They stay within ±7 semitones, so wider chord tones are folded down an octave
- **Encoded voices** play the full interval through the codec, up to ±24. A voice can therefore jump an octave at the moment it's encoded: "the codec covers the rest"
- Each row shows its current interval in semitones


### Milestone 5 test in Live

1. Load a bank, set Count to all, and sweep **SPREAD** slowly while the phrase plays: chorus, then chord, then cluster, with no steps
2. Try each **CHORD** set at the chord position. Do the voicings sound right?
3. **SNAP** up: the crowd should sit on each station and dart between them
4. Listen to a single non-primary voice (solo it) at a wide setting: is the PSOLA shift acceptable on your real ElevenLabs voices?
5. The spec's typical build: automate **SPREAD** and **ENCODE** together over 8–16 bars
6. Judgement call: the octave jump when a voice gets encoded at a wide chord tone. Keep it, or match the dry pitch?

### Stem alignment (added to Milestone 5)

- **On import:** each new stem is auto-aligned to the primary by shifting the whole stem. The matching uses the speech rhythm (syllable envelope), so another voice or another take of the same words lines up
- **Low confidence:** if the match is weak (different words, or a take at a very different pace), the stem stays put and its lane turns amber with "? not auto-aligned - drag"
- **Lanes:** every row shows its waveform on a shared timeline, over a faint orange ghost of the primary. **Drag** to nudge, **Shift-drag** for fine moves, **double-click** to snap back to the auto position. The lane shows each stem's offset in ms
- **Align all** re-runs auto-alignment against the current primary. This overwrites manual nudges on stems it can align and leaves flagged stems where you put them
- **Changing the primary** makes it the new reference but leaves everything where it is. Press Align all to re-align to it
- **Saving:** offsets save with the set

**Test:** drop in a few takes of the same line, recorded separately with different lead-ins. They should snap into sync, and the ghost syllables should sit under each lane's syllables. Drop in a take of different words: it should be flagged, and you can drag it to taste.

### Milestone 4 behaviour (as built)

- **ENCODE** (automatable, 0–100% = 0 to all voices) moves voices onto the codec one at a time, each crossfading to full strength in 30 ms
- **Order:** a fixed-seed scatter that alternates left and right of the primary, so the infection jumps around the head. The primary always goes last. The **codec order** column shows each voice's place in line and lights teal once that voice is taken
- **Re-planning:** changing the primary or the sort re-plans the order, still with the primary last
- **Audible voices only:** Encode counts only the voices you can hear, meaning those inside the Count window and not muted. "2.0 of 4 audible" means two of them are on the codec. Silent voices show "–" in the order column. At 100%, voices fading in arrive already encoded
- **Level:** a fully encoded crowd sits within about 0.5 dB of the dry crowd
- The M3 "codec on primary" switch is gone (Encode at 100% with one voice does the same)

Gesture render without Live: `DrosakaVoiceRender --build out.wav primary.wav others...` renders six loops: the crowd builds, the codec infects it over four loops, then the drop to the single human voice.

### Milestone 4 test in Live

1. Load a bank and turn Count up to all. Automate **ENCODE** from 0 to 100% over 8–16 bars: voices should flip one at a time, scattered left and right, with the primary last
2. At about 95% Encode, only the primary should still sound human, dead centre
3. Automate Count and Encode together, as in the spec's typical build
4. Pull Encode and Count down at a "drop": it should cut back cleanly to the single human voice
5. Judgement call: does the scatter feel like it jumps around your head, or does it need more spread?

### Milestone 3 behaviour (as built)

- **CODEC ON PRIMARY** puts the primary voice through the codec, with a 30 ms crossfade. It's a temporary test switch: Milestone 4's Encode count replaces it
- **Bit rate** runs from clean to crushed: coefficient bits drop from 8 down to LPC-10-ish and then to 1–3 bits. Gain and pitch steps coarsen, and above ~60% the frame rate halves and then thirds
- **Frame drop** is simulated packet loss, up to 50% of frames, bursty: a lost frame replays once as a half-level ghost, then the line cuts out
- **Pitch lock: Fixed / Free / Tracking**, defined in `SPEC.md`. Free is my reading of an undefined term, so judge it by ear
- **Codec:** 8 kHz, 10th-order LPC, 22.5 ms frames, impulse or noise excitation, band-limited at about 3.6 kHz
- **Latency:** the plugin reports ~39 ms to Live, and every voice, dry or encoded, stays aligned

Listening renders without Live: `DrosakaVoiceRender <stem.wav> <out-dir>` writes one file per setting plus `tour.wav`.

### Milestone 3 test in Live

1. Load a bank, set Count to 1 voice, play the phrase, and toggle **CODEC ON PRIMARY** while it plays: it should switch smoothly, with no click or timing jump
2. Sweep **Bit rate** from clean to full: it should go from telephone-vocoder to radio to barely intelligible. *Main question: radio, or just vocoder?*
3. Raise **Frame drop**: it should drop out like a bad link
4. Try **Fixed / Free / Tracking**: does Free mean what you meant?
5. Put the device on a track with drums: the voice should still sit on the grid (Live is compensating the ~39 ms)
6. Turn Count up with the codec on: the crowd stays dry and in sync with the encoded primary

### Milestone 2 behaviour (as built)

- **Import:** drop any number of stems anywhere on the window (or **Add stems…**, multi-select). Up to 32 voices. Stereo files are summed to mono
- **Analysis:** each stem is analysed on import for median pitch and formant centroid, and the bank sorts low → high. **Sort: Pitch / Formant** switches the axis
- **Primary:** the first stem imported is primary. Click **P** on any row to change it. The waveform shows the primary
- **Mute / solo:** **M** silences a voice but keeps its place in the crowd. **S** auditions soloed voices regardless of Count. **x** removes a voice
- **Count** (automatable, 0–100% = 0 to all voices): at its lowest the primary fades in, then pairs join, nearest in the sort first, each pair fading in together. At the edge of the sort the window grows on the side that has voices
- **Stereo:** each pair pans wider the further it sits from the primary, lower voice to the left. This is a stand-in until the spatial modes in M6
- **Level:** loudness is normalised as voices join, so the crowd stays roughly as loud as one voice
- **Smoothing:** every Count, primary, sort and mute change glides (50 ms gain, 100 ms pan). Nothing hard-switches
- **Sync:** any MIDI note fires the whole bank in sync, one-shot, with the same fades as M1
- **Saving:** the Live set saves the bank as file paths (embedded audio comes in M7). Milestone 1 sets still open

### Milestone 2 test in Live

1. Drop a full ElevenLabs phrase bank on the device: rows should appear sorted, with plausible pitch values
2. Count at 0%, play the phrase, sweep Count up while it plays: the crowd should build outward from the primary in pairs, with no steps or clicks
3. Check the stereo picture: the primary stays dead centre and the outer voices sit widest
4. Try a different primary and the other sort axis while the phrase plays: everything should glide, not jump
5. Listen for sync: do the stems stay locked to each other, or do any drift or flam?
6. Mute and solo a few voices, save the set, reopen it: bank, primary, mute/solo and Count should all come back
7. Judgement call: does the sort order match what your ear says is low → high?

### Milestone 1 behaviour (as built)

- Drop one audio file (wav/aif/flac/ogg/mp3/m4a, up to 2 min) on the window, or use **Load stem…**
- Any MIDI note-on plays it from the top. Note pitch, velocity and note-off are ignored
- One-shot: plays to the end. A new note restarts it with a 3 ms crossfade. Starts and ends get a 3 ms fade, so there are no clicks
- Plays at the file's real speed, whatever the session's sample rate
- The Live set remembers the file **path** (embedded audio comes in M7). If the file moves, the device shows "Missing: …" and keeps the path until you drop the file again

### Milestone 1 test in Live

1. Put drosaka Voice on a MIDI track and drop a stem on it
2. Play any key: the phrase plays and the orange playhead runs across the waveform
3. Hammer a key: it should restart cleanly with no clicks
4. Program notes in a clip on different pitches: they should all sound identical and start exactly on the grid
5. Save the set, close it and reopen it: the stem should still be loaded
6. Change Live's sample rate (44.1 ↔ 48 kHz): the phrase should keep the same pitch and length
