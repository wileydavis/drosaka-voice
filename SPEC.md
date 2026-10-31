drosaka Voice Instrument — Spec v1
Oct 29, 2026 · @Wiley
Purpose and concept
A private Ableton instrument that turns one recorded vocal phrase into a synchronized crowd of voices, then assimilates that crowd, voice by voice, into a military-codec robot voice. Built for the drosaka tracks only, so the sound stays unique to the persona.
The gesture: one real voice in the center, a crowd of clones filling in around it, the codec infecting the crowd one voice at a time until the real voice falls last. At the drop, cut back to the single human voice.
First use: track one, "They stole your mess and made it a meme" (looped) and "Put your phones down, get your hands up" (at the drop). Series subhead: "Beats for the walkaway."
Voice bank
The instrument holds a bank of pre-rendered voice stems (e.g. one phrase run through many ElevenLabs voice conversions), all time-aligned to the same source performance.
• Drag-and-drop audio files into the device, Simpler-style; any number of stems, designed for up to 20
• Per-voice mute and solo
• One stem is selectable as the primary voice; it sits dead center and the crowd builds outward from it
• On import the plugin analyzes each stem (average pitch and formant centroid) and auto-sorts the bank into a frequency array, low to high; sort axis switchable
• Presets save the whole bank with audio embedded, so a phrase-bank travels with the Live set
• [added Oct 30, Wiley] Stem alignment, for overlapping separate takes: on import each stem is auto-aligned to the primary by whole-stem time shift (takes only need to be close; no time-warping). Matching uses the syllable-rhythm envelope, so different voices of the same words align; when confidence is low (e.g. different words) the stem is left in place and flagged. Each row shows its waveform on a shared timeline over a ghost of the primary: drag to nudge (shift = fine), double-click to reset to the auto position, "Align all" re-runs against the current primary. Offsets save with the set.
Controls
A small set of knobs, all MIDI-mappable and automatable. All changes to voice count and position crossfade smoothly, never hard-switch.
Control
What it does
Position
Scans a window along the frequency-sorted array: low = chorus of deep voices, high = chorus of thin voices; sweep mid-phrase to morph the crowd
Count
How many voices sound (0 to all), building symmetrically outward from the primary in left/right pairs; sets the window width
  [decided Oct 30, M2] "Outward" = neighbours in the sorted array, nearest first; at the array's edge the window extends on the side that has voices. Each pair fades in together as the knob turns (continuous, no steps). Overall level is normalised as voices are added (gain ÷ √(sum of squared voice gains)) so loudness stays roughly even. Until Spatial mode exists (M6), each pair pans wider the further it sits from the primary, lower-sorted member to the left (interim "radiate").
Spatial mode
How chosen voices arrange in the stereo field: radiate from center, bounce around, others TBD; tempo-synced where motion is involved
  [decided Oct 30, M6] Modes as built — Radiate (pairs fan out, wider with distance); Bounce (every division each audible voice hops to a new slot; slots evenly spaced edge to edge and re-dealt per division from a fixed seed, so the same bar always looks the same); Orbit (voices circle the field, one revolution per division) and Swap (the radiate layout mirrors L<->R every division) are Claude's proposals for "others TBD" — keep, change or cut. Primary stays dead centre in every mode. Rate menu: 4 bars … 1/16, plus 1/4T 1/8T 1/16T. Pure panning for now (depth cues still open). Hops glide 15 ms.
Encode count
Spreads the codec across the voices, 0 to all, one voice at a time at full strength; order is a fixed-seed random scatter across the field so it jumps around the head; the primary voice is always last
  [decided Oct 30, M4] Order alternates left/right of the primary (by sorted-array side, which is also the panning side), random within each side from a fixed seed; primary last. Encode counts only the audible voices (inside the Count window and not muted, or soloed): 0–100% = 0 to all audible voices, taken in the fixed order filtered to those voices, so every step is heard (Wiley, Oct 30). A voice's place in line can shift as Count changes; changes glide. Silent voices wait dry, except at 100% where everything is encoded so voices fading in arrive on the codec.
  [decided Oct 30, M4] Codec output is DC-blocked at 50 Hz (impulse-train DC was summing coherently across encoded voices, +3 dB) with a fixed +1.6 dB make-up, so a fully encoded crowd sits within ~0.5 dB of the dry crowd. The M3 test switch is gone.
Codec: bit rate
Quantization severity of the LPC coefficients, clean to barely intelligible
Codec: frame drop
Simulated packet loss
Codec: pitch lock
Excitation pitch fixed, free, or tracking
The codec runs as one encoder instance per voice with shared settings (about 20 channels at 8 kHz; CPU is not a concern).
  [decided Oct 30, M3] Pitch lock modes as built — Fixed: monotone at the voice's own analysed median pitch, consonants still hiss. Tracking: follows the voice's pitch contour (median-smoothed), consonants hiss. Free: raw pitch detector, unsmoothed, held through consonants and never switching to noise, so everything buzzes and the pitch jumps where the detector lands. (Free was undefined in the spec; this reading needs Wiley's ear.)
  [decided Oct 30, M3] Frame drop: 0–100% = 0–50% of transmitted frames lost, in bursts. A lost frame replays the last good one once at half level; a second loss in a row is silence. Bit rate also lowers the frame rate above ~60% (frames repeat 2× above 60%, 3× above 85%), alongside coarser coefficients, gain and pitch steps.
  [decided Oct 30, M3] Every voice's codec runs continuously; dry voices are delayed to match it, and the plugin reports that delay (~39 ms) to Live, which compensates so notes stay on the grid.
Spread (pitch)
One playable knob that moves the crowd from unison to chorus to chord to cluster, continuously, for builds.
• Each voice has a continuous pitch path: 0 cents, then a detune (cents), then its chord interval, then its cluster position (voices a semitone apart). Spread is a position along that path, so every voice glides and nothing steps
• Chord interval set chosen from a menu (fifths, minor triad, stacked fourths, etc.); outer voices in the array take the widest intervals; the primary voice stays unpitched in the center
• Snap sub-control shapes the curve: how strongly pitches linger on chord tones and rush through the in-between zone; 0 = pure portamento, full = near-stepped
• Shift range limited to about ±7 semitones to keep speech artifacts tolerable; the codec covers the rest
• Typical build: automate Spread and Encode count together over 8 to 16 bars
  [decided Oct 30, M5] As built: stations at 0 / ⅓ / ⅔ / 1 = unison, chorus (±7–30 cents, wider and slightly irregular further out), chord, cluster (pair k at ±k semitones). Chord menu: Fifths, Minor triad (default), Major triad, Stacked fourths, Minor 7th, Octaves; up-side voices take the chord's upper tones and down-side voices its lower ones, narrow to wide; past the list they repeat an octave out. Dry voices are pitch-shifted by PSOLA (formants kept, timing kept) within ±7 st: each station is folded into ±7 by octaves and the voice glides between folded stations, so it never jumps mid-sweep. Encoded voices transpose the codec's excitation and play the full interval (up to ±24), which is "the codec covers the rest" — so a voice can land an octave away from its dry pitch when it gets encoded. Snap is a tanh curve between stations (0 linear, 1 near-stepped).
Instrument behaviour
It is an instrument, not an effect: it sits on a MIDI track and plays on notes.
• Trigger: a MIDI note fires playback; note pitch does not transpose, trigger only
• Slicing: manual slice markers on the phrase; each slice maps to a key; because all stems are aligned, one marker cuts every voice, so one key fires that word across the whole crowd
• Tempo sync (in v1): the device reads Live's transport; spatial motion (bounce etc.) can lock to note divisions, and the encode-count stepping can quantize to the beat instead of free-running
  [decided Oct 30, M6] Grid events land on the musical time of the output (the ~39 ms latency is subtracted, matching Live's compensation). With Live stopped, an internal clock at the last tempo keeps motion running. Encode steps (Free, 1/16 … 1 bar): on each grid line the encoded count latches to the whole number of voices the knob asks for, so voices flip on the beat (several at once if the knob jumped).
• Presets: bank + slices + knob states saved with embedded audio
• Crowd/codec knobs stay live while playing, so a performance can grow and infect the crowd in real time
Pattern mode (tempo-synced retrigger)
Two ways to play: Manual, every hit from the keyboard as above, or Pattern, where one key press starts a tempo-locked walk through the phrase's slices. The core idea: the line was spoken freely, not to a beat; the slices become steps, and the pattern plays them in order on the grid, so a loose phrase lands on the beat. Chopped-and-screwed behaviours are rules applied on top of that walk. A handful of built-in patterns, not a full sequencer.
Pattern
Behaviour
Walk
One slice per step in order, on a chosen division (1/4, 1/8, 1/16, triplets); the base quantize-speech mode
Repeat
Each slice fires N times before advancing (chopped: "they they they stole")
Stutter
Beat-Repeat style on the current slice: retrigger while the slice length shrinks, then advance
Screwed
Slice pitched down and time-stretched to fill the step; Drop amount in semitones
Skip / Rewind
Probability of stepping back a slice or leaving a step silent
Ratchet
Division halves every bar or beat, accelerating into a drop
Random
Slice order shuffled, fires on the division with a Density knob
• Common knobs: Division, Gate (hit length, tight to ringing), Density, Repeat count, Drop, and a Latch toggle (keeps running after key release until stopped)
• Each slice is stretched or truncated to its step length so the phrase stays on grid regardless of how it was spoken
• Holding a slice key while a pattern runs jumps the walk to that slice on the next step
• Pattern timing follows Live's transport so it stays locked to the groove engine clock
Technical notes and open questions
Build in JUCE, ship AU and VST3 from one codebase (Ableton on Mac loads both; Windows loads VST3 only). Codec core: LPC analysis around 10th order at 8 kHz, hard-quantized coefficients, resynthesis with a pulse or saw excitation; quantization and frame drop are what make it sound like radio rather than a vocoder.
Reference sounds for the codec: LPC-10 and STANAG 4591 MELP demos (YouTube); musical cousins are Kraftwerk "Numbers" and Laurie Anderson "O Superman". Reference for chopped vocal technique: Green Velvet "Flash", Blawan "Why They Hide Their Bodies Under My Garage".
Open questions:
[~] Full list of spatial modes beyond radiate and bounce — Orbit and Swap proposed (M6), awaiting Wiley's ear
[ ] Depth cues for outer voices (level, short delay, reverb) or pure panning
[ ] Should a dry primary-voice path be available after the codec stage
[ ] Does the primary stay dry at the drop by automation, or by a dedicated "solo primary" trigger
[ ] Shared transport with the groove engine / sf-transport
Build order and working method
Build in milestones ordered by what is audible soonest. One milestone per chat session: build it, Wiley tests it in Ableton Live, reports what he heard, and the next milestone does not start until the current one sounds right. Corrections to the spec happen as they are found.
Milestone 0, toolchain: Xcode is already installed (used for the iOS Roadbook app). Add CMake and JUCE via Homebrew/git clone, then a one-line build script that compiles AU and VST3 and copies them into ~/Library/Audio/Plug-Ins/Components and /VST3 so a rescan in Live picks them up. Claude Code runs the builds from the laptop; the chat cannot produce plugin binaries itself.
1. Sample player: load one stem, trigger on a MIDI note, loads as AU and VST3 in Live. Proves the JUCE build and plugin loading
2. Voice bank: drag-drop import, auto-sort by pitch/formant, primary selection, mute/solo, Count knob with smooth crossfades. First time the crowd is heard
3. Codec on a single voice: LPC analysis/resynthesis with bit rate, frame drop, pitch lock. Tune by ear until it sounds like radio, not a vocoder
4. Encode count: one encoder per voice, fixed-seed scatter order, primary last
5. Spread: pitch paths, chord menu, Snap
6. Spatial modes and tempo sync
7. Slicing and presets with embedded audio (slot in earlier if their absence gets annoying)
Rules for the build chat: keep the spec as the source of truth and update it when a decision changes; prefer small testable commits; never add a feature not in the spec without asking.