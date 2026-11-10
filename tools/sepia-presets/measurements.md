# Factory pack measurements

From `SepiaFactoryPackTests` on the committed pack: each preset played through a headless
controller at 48 kHz over the three synthesised DI clips (equal RMS, -30 dBFS).

- `chords`, `mute`, `lead`, `mean`: integrated loudness (LUFS, BS.1770) on each clip and their
  mean. Target: mean within 1 LU of -20, each clip within 3.5 LU.
- `peak`: highest sample peak across the three clips, dBFS. Ceiling: -0.5.
- `comp`: how much of a 12 dB drop in playing level comes out, in dB. 12 is a clean amp;
  0 is fully saturated.

Result: 13724 of 13724 checks pass; 60 preset changes made mid-note, none clicked.

```
  preset                      chords    mute    lead    mean   peak   comp
  Warm Clean Room              -19.7   -19.9   -20.4   -20.0   -7.4   11.0
  Glass Chime                  -19.4   -20.0   -20.6   -20.0   -7.1    9.4
  Slack Wobble                 -20.3   -20.3   -19.5   -20.0   -8.1   11.3
  Bedroom Window               -20.2   -20.2   -19.6   -20.0   -5.7   11.7
  Jangle Twelve                -19.5   -19.7   -20.8   -20.0   -8.1    4.8
  Edge of Sunday               -20.4   -20.1   -19.4   -20.0   -9.1   10.5
  Tremolo Porch                -19.5   -19.9   -20.6   -20.0   -5.0   11.4
  Dream Arpeggio               -19.2   -18.4   -22.5   -20.0   -6.8    4.4
  Dry Studio Clean             -19.2   -19.9   -20.9   -20.0   -7.8   10.6
  Rotor Lounge                 -20.0   -20.4   -19.5   -20.0   -4.4   11.2
  Phase Daydream               -20.3   -20.8   -19.0   -20.0   -5.3   11.5
  Soft Focus Lead              -18.9   -19.2   -22.0   -20.0   -9.0    5.9
  Sunday Tape                  -20.2   -20.0   -19.8   -20.0  -10.3   10.8
  Camcorder Summer             -20.2   -20.2   -19.6   -20.0   -9.2   11.0
  Kitchen Radio                -20.9   -20.0   -19.1   -20.0  -13.8    9.1
  Answering Machine            -20.8   -20.0   -19.3   -20.0  -14.2    9.4
  Boombox Demo                 -20.1   -19.5   -20.4   -20.0  -15.0    5.8
  Eight Bit Garage             -20.1   -19.5   -20.5   -20.0  -11.4    6.9
  Worn Out Mixtape             -19.8   -20.2   -20.1   -20.0  -10.5   10.8
  Dusty Sampler                -20.0   -19.7   -20.3   -20.0  -10.7   10.3
  Wall of Haze                 -18.9   -18.9   -22.2   -20.0   -7.0    0.2
  Glide Vibrato                -19.6   -19.3   -21.1   -20.0   -7.7   10.4
  Open Sky Pad                 -18.9   -18.6   -22.6   -20.0   -7.3    4.3
  Shimmer Cathedral            -18.9   -19.3   -21.8   -20.0   -8.3   10.3
  Reverse Tide                 -19.2   -19.3   -21.5   -20.0   -7.8   11.5
  Fuzz Blanket                 -19.2   -19.2   -21.6   -20.0   -8.1    0.2
  Tape Halo                    -20.0   -19.4   -20.6   -20.0   -7.6   10.8
  Deep Space Drone             -18.5   -18.0   -23.5   -20.0   -6.9    7.0
  Smoky Bar Blues              -20.4   -20.0   -19.5   -20.0   -9.5   10.3
  Tweed Howl                   -19.9   -19.5   -20.6   -20.0  -12.3    5.5
  Mid Hump Lead                -19.5   -19.7   -20.8   -20.0  -13.7    3.0
  Stack on Seven               -19.3   -19.3   -21.4   -20.0  -12.4    6.3
  British Bite                 -19.9   -19.3   -20.8   -20.0  -12.3    5.1
  Southern Slide               -18.7   -18.8   -22.5   -20.0  -10.8    3.0
  Arena Rhythm                 -18.7   -19.2   -22.1   -20.0   -9.8    2.2
  Vibe Machine                 -19.8   -19.7   -20.5   -20.0  -10.1    0.9
  Surf Splash                  -20.0   -19.8   -20.2   -20.0   -7.2   10.3
  Singing Sustain              -19.3   -19.6   -21.0   -20.0  -11.8    0.0
  Seattle Rain                 -19.0   -19.2   -21.8   -20.0  -11.8    1.7
  Watery Verse                 -19.4   -20.1   -20.4   -20.0   -5.5   11.4
  Sludge Wall                  -18.5   -19.2   -22.3   -20.0  -10.3    0.2
  Alley Distortion             -19.3   -19.7   -20.9   -20.0  -11.7    0.6
  Basement Show                -19.3   -19.4   -21.3   -20.0  -12.6    4.7
  Three Chord Riot             -19.1   -19.5   -21.4   -20.0  -11.8    0.6
  Quiet Loud Switch            -19.9   -19.6   -20.4   -20.0  -14.2    1.4
  Detuned Dirge                -18.4   -19.0   -22.5   -20.0   -9.7    1.1
  Garage Fuzz Tone             -20.2   -19.6   -20.2   -20.0  -12.7    0.7
  Post Punk Wire               -19.7   -19.5   -20.8   -20.0   -7.0    9.4
  Tight Modern Rhythm          -19.5   -19.7   -20.8   -20.0  -12.6   -0.0
  Djent Machine                -19.9   -19.7   -20.3   -20.0  -13.1    0.0
  Scooped Thrash               -19.0   -19.3   -21.7   -20.0  -11.0   -0.2
  Old School Death             -19.3   -19.7   -21.0   -20.0  -12.8    0.5
  Vintage High Gain            -19.0   -19.4   -21.6   -20.0  -11.8    0.5
  Doom Monolith                -18.6   -19.3   -22.2   -20.0  -10.0    0.2
  Black Frost Tremolo          -19.1   -19.5   -21.4   -20.0  -10.3    0.0
  Singing Metal Lead           -19.5   -19.8   -20.7   -20.0  -11.9   -0.1
  Shred Legato                 -19.2   -19.4   -21.3   -20.0  -11.1   -0.0
  Metalcore Chug               -20.0   -19.7   -20.4   -20.0  -13.5    0.0
  Clean Before the Storm       -19.5   -19.0   -21.6   -20.0   -7.3   10.8
  Industrial Grind             -19.5   -19.6   -20.9   -20.0  -12.3    0.5
```
