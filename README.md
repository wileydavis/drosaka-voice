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
| 3 Codec, single voice | Written and passing offline tests. **Awaiting ear test in Live** |
| 4 Encode count | not started |
| 5 Spread | not started |
| 6 Spatial modes + tempo sync | not started |
| 7 Slicing + presets w/ embedded audio | not started |

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
