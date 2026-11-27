# Third-Party Notices

GameHQ first-party core code is licensed under GNU GPL version 3 only. The
separately packaged Playnite integration and public integration protocol remain
under MIT. The Windows package also redistributes runtime components under
their own licenses.

## Qt

GameHQ dynamically links to Qt libraries and plugins. The open-source Qt build
is available under the GNU Lesser General Public License version 3, with
individual third-party components under the licenses identified by Qt.

- Qt licensing: https://doc.qt.io/qt-6/licensing.html
- Qt source code: https://code.qt.io/cgit/qt/
- LGPL version 3: https://www.gnu.org/licenses/lgpl-3.0.html

The shared libraries remain replaceable in the package's `app` directory.

## FFmpeg

The Qt Multimedia runtime includes dynamically loaded FFmpeg shared libraries
under LGPL 2.1-or-later. The Qt Multimedia SPDX document under
`licenses/qt-sbom/` contains the upstream version and source details supplied
with the selected Qt kit.

- FFmpeg legal information: https://ffmpeg.org/legal.html
- FFmpeg source code: https://ffmpeg.org/download.html#get-sources
- LGPL version 2.1: https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html

## MinGW-w64 and GCC runtime libraries

The Windows package contains runtime libraries supplied by the MinGW-w64/GCC
toolchain. These components remain under their upstream licenses and applicable
runtime library exceptions.

- GCC licenses: https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html
- MinGW-w64 licensing: https://www.mingw-w64.org/about/

## miniz 3.1.2

`GameHQUpdater.exe` statically links miniz for ZIP staging and validation.
miniz is distributed under the MIT License; the complete license is included
at `licenses/miniz.txt`.

- Source: https://github.com/richgel999/miniz/tree/3.1.2

## Monocypher 4.0.3

GameHQ and `GameHQUpdater.exe` statically link Monocypher's optional Ed25519
implementation to verify signed release manifests. GameHQ uses the 2-clause
BSD license option; the complete license is included at
`licenses/monocypher.txt`.

- Source: https://monocypher.org/download/

## Mesa llvmpipe

Qt's Windows deployment includes `opengl32sw.dll`, its Mesa llvmpipe software
OpenGL fallback. Mesa and the LLVM components in this binary use permissive
licenses recorded by Qt; upstream attribution is available at:

- https://doc.qt.io/qtcreator/qtcreator-binary-attribution-llvmpipe.html

## Microsoft D3DCompiler

The Windows package contains Microsoft's redistributable
`D3Dcompiler_47.dll` as an application-local Qt runtime dependency. It remains
Microsoft software and is not part of GameHQ.

- Redistribution guidance: https://learn.microsoft.com/windows/win32/directx-sdk--august-2009-

## Microsoft GameInput 3.5.262

GameHQ vendors the MIT-licensed public header and loader source from the
`Microsoft.GameInput` NuGet package (`third_party/gameinput/`); the complete
license is included at `licenses/GameInput-MIT.txt`. The GameInput runtime
redistributable (`GameInputRedist.msi` / `GameInputRedist.dll`) remains
Microsoft software under the Microsoft Software License Terms shipped inside
the package and is not part of GameHQ; when the Windows package includes or
installs it, it is distributed unmodified as a separately licensed component.
The runtime is loaded dynamically, is optional, and GameHQ degrades to its
legacy input backends when it is absent.

- Package and licenses: https://www.nuget.org/packages/Microsoft.GameInput/3.5.262
- GameInput documentation: https://aka.ms/gameinput

## Inno Setup 7.1.0

The Windows Setup executable and uninstaller are produced with Inno Setup.
Its complete permissive license is included at `licenses/Inno-Setup.txt`.

- Source and project: https://github.com/jrsoftware/issrc

## Playnite integration dependencies

The separately packaged Playnite integration dynamically uses Playnite's MIT
SDK supplied by the host and ships Bouncy Castle C# 2.6.2 under its MIT-style
license. Its complete notice is included in the integration package at
`LICENSES/BouncyCastle.Cryptography.txt`.

- Playnite license: https://github.com/JosefNemec/Playnite/blob/master/LICENSE.md
- Bouncy Castle license: https://www.bouncycastle.org/about/license/

No ownership of these third-party components is claimed by the GameHQ project.
