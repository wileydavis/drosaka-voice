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
| 0 Toolchain | `build.sh` written; verified compiling on Linux (VST3). Needs first run on the Mac |
| 1 Sample player | Written and passing offline tests. **Awaiting ear test in Live** |
| 2 Voice bank | not started |
| 3 Codec, single voice | not started |
| 4 Encode count | not started |
| 5 Spread | not started |
| 6 Spatial modes + tempo sync | not started |
| 7 Slicing + presets w/ embedded audio | not started |

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
