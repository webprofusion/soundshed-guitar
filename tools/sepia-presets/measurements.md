# Factory pack measurements

From `SepiaFactoryPackTests` on the committed pack: each preset played through a headless
controller at 48 kHz over the three synthesised DI clips (equal RMS, -30 dBFS).

- `chords`, `mute`, `lead`, `mean`: integrated loudness (LUFS, BS.1770) on each clip and their
  mean. Target: mean within 1 LU of -20, each clip within 3.5 LU.
- `peak`: highest sample peak across the three clips, dBFS. Ceiling: -0.5.
- `comp`: how much of a 12 dB drop in playing level comes out, in dB. 12 is a clean amp;
  0 is fully saturated.

Result: 28245 of 28245 checks pass; 122 preset changes made mid-note, none clicked; the
slowest preset renders 14 times faster than real time on one core of the build machine.

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
  Honey Compressor             -18.9   -19.5   -21.5   -20.0   -6.1    5.6
  Auto Wah Funk                -21.5   -19.9   -18.6   -20.0   -7.8    7.2
  Twelve String Shimmer        -20.4   -21.0   -18.6   -20.0   -5.7   11.7
  Vinyl Sunday                 -20.8   -20.7   -18.5   -20.0   -4.6   11.6
  Glassy Slapback              -21.0   -20.3   -18.7   -20.0   -4.2   11.4
  Country Snap                 -19.7   -19.7   -20.6   -20.0   -9.0    4.7
  Lush Plate                   -19.6   -20.7   -19.8   -20.0   -4.6   11.6
  Jazz Box                     -19.6   -20.4   -20.0   -20.0   -6.5   11.7
  Flanged Skies                -20.5   -20.6   -18.8   -20.0   -3.8   11.4
  Pillow Talk                  -19.1   -20.2   -20.7   -20.0   -5.3   11.7
  Unplugged Evening            -20.6   -20.7   -18.7   -20.0   -1.7   12.0
  Campfire Strum               -20.6   -20.4   -19.1   -20.0   -1.6   11.3
  Sunday Tape                  -20.2   -20.0   -19.8   -20.0  -10.3   10.8
  Camcorder Summer             -20.2   -20.2   -19.6   -20.0   -9.2   11.0
  Kitchen Radio                -20.9   -20.0   -19.1   -20.0  -13.8    9.1
  Answering Machine            -20.8   -20.0   -19.3   -20.0  -14.2    9.4
  Boombox Demo                 -20.1   -19.5   -20.4   -20.0  -15.0    5.8
  Eight Bit Garage             -20.0   -19.5   -20.5   -20.0  -11.4    6.8
  Worn Out Mixtape             -19.8   -20.2   -20.1   -20.0  -10.5   10.8
  Dusty Sampler                -20.0   -19.7   -20.3   -20.0  -10.7   10.3
  Charity Shop LP              -20.0   -20.4   -19.6   -20.0   -7.2   11.6
  Run-Out Groove               -20.1   -19.7   -20.1   -20.0  -13.5    9.4
  Voicemail Blues              -20.8   -19.9   -19.3   -20.0  -13.7    9.0
  Shortwave                    -21.2   -20.6   -18.2   -20.0   -9.5   10.8
  Pawn Shop Amp                -20.0   -19.5   -20.5   -20.0  -10.8    6.9
  VHS Rental                   -20.3   -20.3   -19.4   -20.0   -9.5   11.1
  Game Cartridge               -19.4   -19.5   -21.1   -20.0  -12.0    7.5
  Skipping Record              -19.7   -20.6   -19.6   -20.0   -4.6   11.5
  Tape Deck Dream              -19.8   -19.5   -20.7   -20.0   -8.5   11.2
  Boombox Chorus               -20.2   -20.3   -19.5   -20.0   -9.4   11.1
  Wall of Haze                 -18.9   -18.9   -22.2   -20.0   -7.0    0.2
  Glide Vibrato                -19.6   -19.3   -21.1   -20.0   -7.7   10.4
  Open Sky Pad                 -18.9   -18.6   -22.6   -20.0   -7.3    4.3
  Shimmer Cathedral            -18.9   -19.3   -21.8   -20.0   -8.3   10.3
  Reverse Tide                 -19.2   -19.3   -21.5   -20.0   -7.8   11.5
  Fuzz Blanket                 -19.2   -19.2   -21.6   -20.0   -8.1    0.2
  Tape Halo                    -20.0   -19.4   -20.6   -20.0   -7.6   10.8
  Deep Space Drone             -18.5   -18.0   -23.5   -20.0   -6.9    7.0
  Feedback Garden              -18.0   -20.1   -21.9   -20.0   -7.0    0.4
  Choir of Glass               -20.1   -20.2   -19.6   -20.0   -5.0   11.5
  First Light Bloom            -20.1   -20.2   -19.7   -20.0   -4.1   11.6
  Cloud Nine Pad               -18.7   -18.6   -22.7   -20.0   -6.1    4.1
  Octave Shimmer Wash          -19.7   -20.1   -20.1   -20.0   -7.1   11.6
  Pitch Black Sea              -18.8   -19.5   -21.7   -20.0   -6.0   11.4
  Phase Drift                  -20.3   -19.3   -20.4   -20.0   -8.1    1.9
  Tremolo Fog                  -20.2   -20.1   -19.7   -20.0   -7.5   10.2
  Ring Bells                   -19.2   -19.2   -21.6   -20.0   -7.0   10.4
  Static Bloom                 -18.0   -19.5   -22.6   -20.0   -6.1    0.5
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
  Parked Wah Lead              -20.2   -19.5   -20.3   -20.0  -14.9    5.4
  Slide Sustain                -18.2   -21.1   -20.7   -20.0   -8.5    2.9
  Lone Star Shuffle            -19.6   -19.5   -20.9   -20.0  -13.0    2.1
  Clean Boost Combo            -20.2   -19.7   -20.1   -20.0  -11.2    6.8
  Warm Breaker                 -19.6   -19.6   -20.9   -20.0  -11.2    6.3
  Open Push                    -19.5   -19.5   -21.0   -20.0  -13.4    3.3
  Rotary Rock                  -18.8   -19.2   -22.0   -20.0   -9.2    5.6
  Seventies Phase              -19.3   -19.8   -20.9   -20.0   -9.9    8.6
  Echo Canyon Lead             -19.4   -19.6   -21.0   -20.0  -11.9    0.3
  Octave Fuzz Riff             -19.9   -19.7   -20.4   -20.0  -12.7    0.0
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
  Feedback Squeal              -18.9   -20.1   -21.0   -20.0  -11.7    0.3
  Underwater Chorus            -18.7   -19.1   -22.2   -20.0   -7.9    6.0
  Skate Punk Tight             -19.7   -19.7   -20.6   -20.0  -13.2    0.1
  Hardcore Bark                -19.3   -19.5   -21.2   -20.0  -11.8    0.5
  Octave Down Sludge           -18.8   -19.5   -21.6   -20.0  -10.4    0.2
  Wah Riot                     -19.8   -19.8   -20.4   -20.0  -16.3   -0.0
  Garage Tape Fuzz             -20.1   -19.8   -20.1   -20.0  -18.6    0.5
  Jangle Punk                  -20.1   -20.0   -19.9   -20.0   -9.8   10.0
  Quiet Verse                  -19.1   -20.1   -20.8   -20.0   -7.0   11.6
  Noise Rock Ring              -19.3   -19.5   -21.2   -20.0  -12.8    2.0
  Tight Modern Rhythm          -19.5   -19.7   -20.8   -20.0  -12.6   -0.0
  Djent Machine                -19.9   -19.7   -20.3   -20.0  -13.1    0.0
  Scooped Thrash               -19.0   -19.3   -21.7   -20.0  -11.0   -0.2
  Old School Death             -19.3   -19.7   -21.0   -20.0  -12.8    0.5
  Vintage High Gain            -19.0   -19.4   -21.6   -20.0  -11.8    0.5
  Doom Monolith                -18.6   -19.4   -22.0   -20.0  -10.0    0.2
  Black Frost Tremolo          -19.1   -19.5   -21.4   -20.0  -10.3    0.0
  Singing Metal Lead           -19.5   -19.8   -20.7   -20.0  -11.9   -0.1
  Shred Legato                 -19.2   -19.4   -21.3   -20.0  -11.1   -0.0
  Metalcore Chug               -20.0   -19.7   -20.4   -20.0  -13.5    0.0
  Clean Before the Storm       -19.5   -19.0   -21.6   -20.0   -7.3   10.8
  Industrial Grind             -19.5   -19.6   -20.9   -20.0  -12.3    0.5
  Endless Solo                 -19.1   -20.1   -20.8   -20.0  -10.5   -0.0
  Parked Wah Shred             -19.7   -19.8   -20.4   -20.0  -14.3    0.0
  Thrash Gallop                -19.6   -19.7   -20.8   -20.0  -12.6   -0.0
  Low Tuned Bounce             -19.2   -19.5   -21.3   -20.0  -11.8   -0.1
  Power Ballad Clean           -19.1   -19.9   -21.0   -20.0   -7.0   10.9
  Symphonic Lead               -19.2   -19.8   -21.0   -20.0  -10.3   -0.0
  Stoner Fuzz                  -19.2   -19.7   -21.1   -20.0  -12.7    0.6
  Swedish Buzz                 -19.2   -19.6   -21.2   -20.0  -11.2    0.9
  Progressive Clean Drive      -20.1   -19.5   -20.4   -20.0  -10.1    6.3
  Octave Down Djent            -19.9   -19.7   -20.4   -20.0  -13.1   -0.0
```

## Tone balance and speed

From `tone_report.py`: the share of energy in four bands on the palm-mute clip, in dB
relative to the whole, and how many times faster than real time each preset renders.
The flagged presets are the ones furthest from their group, which is deliberate for most
of them (a telephone preset has no low end).

```

Clean & Indie  (median: low -6.0, low-mid -2.6, high-mid -7.9, high -26.4 dB)
  preset                             low   low-mid  high-mid      high   speed
  Warm Clean Room                   -5.3      -2.2      -9.8     -32.4     40x
  Glass Chime                       -6.6      -2.5      -6.7     -20.7     38x
  Slack Wobble                      -6.5      -2.0      -8.5     -30.0     28x
  Bedroom Window                    -4.5      -2.7      -9.7     -32.2     35x
  Jangle Twelve                     -5.9      -2.7      -7.2     -20.6     42x
  Edge of Sunday                    -7.7      -1.7      -8.3     -25.4     39x
  Tremolo Porch                     -6.1      -2.0      -9.2     -27.2     35x
  Dream Arpeggio                    -4.3      -2.9      -9.5     -25.2     40x
  Dry Studio Clean                  -4.3      -2.9      -9.7     -27.6     82x
  Rotor Lounge                      -5.8      -2.3      -8.2     -30.6     34x
  Phase Daydream                    -5.0      -3.2      -6.8     -30.4     39x
  Soft Focus Lead                   -4.6      -2.4     -10.8     -33.1     36x
  Honey Compressor                  -3.7      -3.3     -10.0     -30.8     40x
  Auto Wah Funk                    -17.0      -5.3      -1.7     -23.0     55x  <-
  Twelve String Shimmer             -8.8      -2.9      -4.7     -17.2     28x  <-
  Vinyl Sunday                      -8.4      -1.7      -7.5     -25.1     45x
  Glassy Slapback                   -7.7      -2.7      -5.5     -20.4     68x
  Country Snap                      -7.0      -2.1      -7.4     -22.5     36x
  Lush Plate                        -6.3      -2.3      -7.6     -25.8     47x
  Jazz Box                          -4.9      -2.3     -10.4     -37.6     47x  <-
  Flanged Skies                     -6.6      -2.2      -7.5     -27.1     33x
  Pillow Talk                       -3.4      -3.2     -11.8     -34.7     41x  <-
  Unplugged Evening                 -6.3      -2.6      -6.9     -20.4     68x
  Campfire Strum                    -5.0      -3.3      -6.8     -20.6     59x

Lo-Fi  (median: low -7.5, low-mid -1.8, high-mid -8.3, high -29.1 dB)
  preset                             low   low-mid  high-mid      high   speed
  Sunday Tape                       -9.5      -1.2      -9.1     -29.2     33x
  Camcorder Summer                  -7.2      -1.8      -8.3     -30.4     28x
  Kitchen Radio                    -15.0      -1.7      -5.4     -29.5     45x
  Answering Machine                -16.4      -1.2      -6.6     -28.4     46x  <-
  Boombox Demo                      -7.5      -1.8      -7.9     -22.9     29x
  Eight Bit Garage                 -10.7      -1.0      -9.3     -22.0     63x
  Worn Out Mixtape                  -5.1      -2.3      -9.8     -32.4     25x
  Dusty Sampler                     -7.6      -1.7      -8.1     -28.9     35x
  Charity Shop LP                   -5.9      -2.1      -9.2     -31.6     46x
  Run-Out Groove                   -10.1      -1.0      -9.5     -29.2     42x
  Voicemail Blues                  -17.0      -1.1      -6.9     -28.4     44x  <-
  Shortwave                        -14.1      -2.3      -4.4     -31.1     46x
  Pawn Shop Amp                    -10.4      -1.0      -9.2     -27.2     36x
  VHS Rental                        -7.4      -1.8      -8.0     -29.6     38x
  Game Cartridge                    -5.8      -2.2      -9.1     -20.7     63x  <-
  Skipping Record                   -4.7      -2.9      -8.3     -25.3     84x
  Tape Deck Dream                   -4.3      -2.6     -10.8     -33.7     29x
  Boombox Chorus                    -5.8      -2.4      -8.1     -26.2     42x

Shoegaze & Ambient  (median: low -5.5, low-mid -2.3, high-mid -9.3, high -28.0 dB)
  preset                             low   low-mid  high-mid      high   speed
  Wall of Haze                      -3.4      -3.0     -13.9     -32.2     21x
  Glide Vibrato                     -6.9      -1.6     -10.1     -29.7     43x
  Open Sky Pad                      -6.3      -1.9      -9.4     -27.3     41x
  Shimmer Cathedral                 -5.9      -2.2      -8.7     -28.0     42x
  Reverse Tide                      -4.7      -2.8      -8.8     -28.6     42x
  Fuzz Blanket                      -5.3      -2.3      -9.4     -23.8     20x
  Tape Halo                         -5.8      -1.9     -10.0     -34.2     29x
  Deep Space Drone                  -3.4      -3.0     -14.6     -36.3     22x  <-
  Feedback Garden                   -4.7      -2.3     -11.6     -28.1     20x
  Choir of Glass                    -7.0      -2.2      -7.1     -23.3     25x
  First Light Bloom                 -6.6      -2.3      -7.3     -23.1     26x
  Cloud Nine Pad                    -4.6      -2.7      -9.2     -26.7     26x
  Octave Shimmer Wash               -5.1      -2.6      -8.7     -27.5     28x
  Pitch Black Sea                   -3.5      -3.2     -11.5     -37.2     28x  <-
  Phase Drift                       -6.5      -2.3      -7.3     -30.1     18x
  Tremolo Fog                       -8.0      -1.6      -8.4     -33.6     32x
  Ring Bells                        -6.3      -2.3      -7.6     -26.3     40x
  Static Bloom                      -4.5      -2.5     -11.0     -25.9     20x

Blues & Classic Rock  (median: low -6.6, low-mid -2.1, high-mid -8.4, high -24.4 dB)
  preset                             low   low-mid  high-mid      high   speed
  Smoky Bar Blues                   -7.7      -1.7      -8.0     -25.6     40x
  Tweed Howl                        -6.7      -2.1      -8.1     -20.4     38x
  Mid Hump Lead                    -10.1      -1.5      -7.3     -21.5     25x
  Stack on Seven                    -5.6      -2.1      -9.7     -25.1     69x
  British Bite                      -8.7      -2.4      -5.5     -18.0     26x
  Southern Slide                    -4.4      -2.5     -11.5     -32.7     33x  <-
  Arena Rhythm                      -4.8      -2.4     -10.3     -24.9     41x
  Vibe Machine                      -8.1      -2.1      -6.4     -24.4     16x
  Surf Splash                       -7.2      -1.7      -9.1     -28.3     41x
  Singing Sustain                   -5.2      -2.4      -9.2     -23.6     35x
  Parked Wah Lead                  -14.7      -1.3      -6.6     -24.4     54x  <-
  Slide Sustain                     -5.3      -2.2      -9.8     -31.0     30x
  Lone Star Shuffle                 -7.7      -2.0      -7.2     -19.6     27x
  Clean Boost Combo                 -7.5      -1.7      -8.5     -23.5     26x
  Warm Breaker                      -6.0      -1.9      -9.9     -29.9     28x
  Open Push                         -6.7      -2.0      -8.3     -21.9     40x
  Rotary Rock                       -3.1      -3.7     -11.0     -28.9     31x
  Seventies Phase                   -4.1      -3.5      -7.9     -26.7     66x
  Echo Canyon Lead                  -6.6      -2.0      -8.5     -22.9     25x
  Octave Fuzz Riff                  -5.8      -3.3      -6.1     -16.5     18x

Grunge & Punk  (median: low -5.4, low-mid -2.8, high-mid -8.4, high -22.7 dB)
  preset                             low   low-mid  high-mid      high   speed
  Seattle Rain                      -3.9      -3.4      -8.9     -22.2     29x
  Watery Verse                      -4.5      -2.8      -9.1     -27.5     42x
  Sludge Wall                       -3.1      -3.3     -13.4     -27.8     28x
  Alley Distortion                  -6.6      -2.2      -7.4     -24.1     31x
  Basement Show                     -5.8      -2.1      -9.3     -23.2     69x
  Three Chord Riot                  -4.7      -2.7      -9.1     -21.7     38x
  Quiet Loud Switch                 -9.3      -1.7      -7.1     -19.9     21x
  Detuned Dirge                     -2.8      -3.8     -12.8     -30.9     42x  <-
  Garage Fuzz Tone                  -9.6      -4.0      -3.3     -15.7     23x
  Post Punk Wire                    -4.5      -3.1      -8.1     -22.8     37x
  Feedback Squeal                   -7.6      -2.3      -6.6     -18.9     28x
  Underwater Chorus                 -2.4      -5.5      -8.6     -23.6     28x
  Skate Punk Tight                  -8.7      -2.6      -5.2     -17.2     41x
  Hardcore Bark                     -5.1      -3.0      -7.5     -19.0     67x
  Octave Down Sludge                -2.9      -3.6     -12.8     -27.1     18x
  Wah Riot                         -14.6      -2.9      -3.7     -16.6     25x  <-
  Garage Tape Fuzz                 -12.3      -2.2      -4.8     -21.8     25x
  Jangle Punk                       -7.4      -1.8      -8.2     -24.4     73x
  Quiet Verse                       -3.9      -3.1     -10.0     -27.9     37x
  Noise Rock Ring                   -6.2      -2.1      -8.8     -22.6     14x

Metal  (median: low -5.9, low-mid -2.6, high-mid -7.6, high -20.2 dB)
  preset                             low   low-mid  high-mid      high   speed
  Tight Modern Rhythm               -7.5      -2.5      -6.0     -18.8     41x
  Djent Machine                    -11.2      -2.5      -4.6     -17.9     41x
  Scooped Thrash                    -3.2      -4.8      -7.5     -18.9     69x
  Old School Death                  -6.7      -2.1      -7.8     -20.2     25x
  Vintage High Gain                 -4.2      -2.8     -10.3     -23.7     69x
  Doom Monolith                     -3.3      -3.4     -11.3     -28.0     21x
  Black Frost Tremolo               -4.3      -4.4      -6.2     -16.0     42x
  Singing Metal Lead                -7.3      -1.9      -7.9     -22.2     27x
  Shred Legato                      -4.5      -2.7     -10.0     -27.7     36x
  Metalcore Chug                    -9.6      -3.2      -4.1     -15.8     37x
  Clean Before the Storm            -4.3      -2.7     -10.6     -26.9     39x
  Industrial Grind                  -5.5      -3.0      -6.9     -18.6     56x
  Endless Solo                      -7.4      -1.9      -7.7     -21.4     25x
  Parked Wah Shred                 -12.1      -2.1      -5.2     -18.1     60x
  Thrash Gallop                     -7.0      -3.5      -4.9     -15.4     40x
  Low Tuned Bounce                  -4.3      -3.3      -8.1     -20.1     76x
  Power Ballad Clean                -5.0      -3.0      -7.5     -24.2     38x
  Symphonic Lead                    -5.0      -2.5      -9.1     -23.8     29x
  Stoner Fuzz                       -5.9      -2.1      -9.2     -26.9     21x
  Swedish Buzz                      -5.9      -2.6      -7.4     -18.2     27x
  Progressive Clean Drive           -6.5      -1.9      -8.7     -23.4     28x
  Octave Down Djent                -10.4      -2.6      -4.7     -18.2     29x

13 band(s) more than 8 dB from their group's median:
  Auto Wah Funk (Clean & Indie): low -11.1 dB
  Twelve String Shimmer (Clean & Indie): high +9.2 dB
  Jazz Box (Clean & Indie): high -11.1 dB
  Pillow Talk (Clean & Indie): high -8.3 dB
  Answering Machine (Lo-Fi): low -8.9 dB
  Voicemail Blues (Lo-Fi): low -9.4 dB
  Game Cartridge (Lo-Fi): high +8.4 dB
  Deep Space Drone (Shoegaze & Ambient): high -8.2 dB
  Pitch Black Sea (Shoegaze & Ambient): high -9.1 dB
  Southern Slide (Blues & Classic Rock): high -8.3 dB
  Parked Wah Lead (Blues & Classic Rock): low -8.0 dB
  Detuned Dirge (Grunge & Punk): high -8.2 dB
  Wah Riot (Grunge & Punk): low -9.1 dB
```
