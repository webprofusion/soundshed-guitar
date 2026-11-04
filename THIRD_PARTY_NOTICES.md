# Third-Party Notices

Soundshed Guitar is licensed under AGPL-3.0. The following notices apply to
third-party software used by the NAM oversampling integration. These notices
do not alter the AGPL-3.0 licensing of Soundshed Guitar's original code.

## NAM-Oversampler

Design and implementation reference:
https://github.com/DLC86/NAM-Oversampler

Reference revision:
`7d8df1be321d933b0bb4eaa6f93689a5910273a7`

MIT License

Copyright (c) 2022 Steven Atkinson

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## NeuralAmpModelerCore

Source dependency:
https://github.com/DLC86/NeuralAmpModelerCore

Pinned revision:
`bea723b300e2dfe54f417c9e9f9d7f68b4a8f30d`

MIT License

Copyright (c) 2023 Steven Atkinson

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## AudioDSPTools

Source dependency:
https://github.com/DLC86/AudioDSPTools

Pinned revision:
`63bdfc9db16cb45da8f1dcb3af4c572286753dde`

Modified: Soundshed Guitar builds `dsp/ResamplingContainer/ResamplingContainer.h`
with local fixes to how `Reset()` pre-rolls and clears the resampler, to how its
filters detect and recover from invalid samples, and to where it reads its
debugging environment variables (in `Reset()` rather than on every audio block),
applied by `core/cmake/GuitarfxAudioDSPTools.cmake` and marked in the generated
source.

MIT License

Copyright (c) 2023 Steven Atkinson

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## LLVM libc++ (`<filesystem>` implementation)

Used only by iOS 12 (legacy) builds of the iOS apps, which compile libc++'s
`<filesystem>` implementation into the app because iOS 12's system library
does not provide it (see `ios/compat/libcxx-filesystem/README.md`).

Source: https://github.com/llvm/llvm-project, `libcxx/src/filesystem/`

Revision: `llvmorg-20.1.8`

Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

The full licence text is in `ios/compat/libcxx-filesystem/LICENSE.TXT`.

## alphaTab

Renders tablature and notation in the Practice Tool's tab view (`core/ui`, loaded
from `dist/vendor/alphatab/`, see `docs/practice-tool-tablature.md`). Used
unmodified.

Source: https://github.com/CoderLine/alphaTab, npm package `@coderline/alphatab`
(version pinned in `core/ui/package.json`).

Copyright © 2025, Daniel Kuschny and Contributors. Licensed under the Mozilla
Public License, v. 2.0 (https://mozilla.org/MPL/2.0/). The full licence text,
including the licences of the libraries alphaTab integrates, ships beside the
script as `dist/vendor/alphatab/LICENSE`. The "alphaTab.net" attribution in
rendered scores must stay visible (https://alphatab.net/docs/attribution).

## Bravura

The SMuFL music font alphaTab draws with, shipped as `dist/vendor/alphatab/font/`.

Copyright © 2015, Steinberg Media Technologies GmbH. Licensed under the SIL Open
Font License, Version 1.1; the full text ships beside the font as
`Bravura-OFL.txt`.
