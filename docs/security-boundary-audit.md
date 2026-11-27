# Security boundary and dependency audit

Reviewed 2026-11-27 against the Windows Release source at `9c20444`. This
audit covers dependency pins, runtime inclusion, and defensive regressions.
It does not qualify a release package.

## Runtime and toolchain inventory

| Component | Pin and distribution | Check |
| --- | --- | --- |
| Qt and Qt Multimedia | 6.8.3 MinGW binary kit. Shared Qt DLLs, plugins and QML are deployed with LGPL/GPL texts and SPDX SBOMs. | The audited build uses the repository pin. See the release qualification finding below. [Qt security and SBOMs](https://doc.qt.io/qt-6/security.html) |
| FFmpeg | Qt Multimedia's LGPL-2.1-or-later shared DLLs. The recorded ABI is `avcodec-61` at 61.19.100. | Several 7.1 patch releases share that ABI. Identify the deployed patch by vendor provenance and artifact hashes. [FFmpeg releases](https://ffmpeg.org/download.html#get-sources) |
| miniz | 3.1.2, MIT, linked into the updater. CMake pins the archive URL and SHA-256. | The pinned upstream release contains ZIP integer-bound and decompression fixes. [miniz 3.1.2](https://github.com/richgel999/miniz/releases/tag/3.1.2) |
| Monocypher | 4.0.3, BSD-2-Clause selected from the dual license. Linked statically with a SHA-512-checked archive. | Includes the upstream EdDSA timing fix. [Monocypher changelog](https://monocypher.org/changelog) |
| Microsoft GameInput | 3.5.262 headers and import library from Microsoft's NuGet package, with MIT-licensed headers. The separately licensed runtime is resolved from Windows or Microsoft's redistributable. | The pinned package and available system runtime are distinct. The desktop-safe audit did not exercise physical devices. [Microsoft package](https://www.nuget.org/packages/Microsoft.GameInput/3.5.262) |
| TDLib | Optional 1.8.67 source commit `bc9c263e2bfee06aaab41e82db51a103376030bc`, Boost Software License 1.0. | The default build disables Telegram Integrated and the runtime SHA-256 field is empty, so the audited build loads no TDLib. The optional build's transitive dependency and artifact review remains separate. [Pinned source license](https://github.com/tdlib/td/blob/bc9c263e2bfee06aaab41e82db51a103376030bc/LICENSE_1_0.txt) |
| Playnite crypto and runtime | Bouncy Castle C# 2.6.2, MIT-style, shipped with the plugin. Playnite SDK 6.16.0 and .NET Framework reference assemblies are build inputs. | NuGet content is locked. The host supplies the .NET Framework runtime. The compatibility target is net462 and Microsoft lists an end of support in January 2027 for that runtime version. [Bouncy Castle package](https://www.nuget.org/packages/BouncyCastle.Cryptography/2.6.2), [Microsoft Framework lifecycle](https://learn.microsoft.com/en-us/lifecycle/products/microsoft-net-framework) |
| Build and packaging | GCC/MinGW 13.1.0, CMake 4.3.3 and Ninja 1.13.2 in the audited native receipt. Ninja is pinned by SHA-256 and Inno Setup 7.1.0 x64 by hashes and expected publisher. Qt and the GCC runtime are distributed under their recorded terms. | CMake and Ninja are build tools. Inno Setup contributes the Setup and uninstaller binaries. [Inno Setup history](https://jrsoftware.org/files/is7-whatsnew.htm), [Distributed license audit](dependency-licenses.md) |

## Release qualification finding

The pinned Qt 6.8.3 binary kit is within the range Qt lists for
CVE-2026-8461 in bundled FFmpeg. GameHQ deploys Qt Multimedia with this
backend. Qt identifies Qt 6.8.9, 6.11.2 or later as including the fix and
also identifies a rebuilt FFmpeg 7.1.5 runtime as a media-specific mitigation.
The current pin cannot qualify for release on the strength of the local
regressions alone. Qualify a patched deployment and verify its runtime
provenance, bundled notices, media behavior and release trust before release.
[Qt Multimedia advisory](https://www.qt.io/blog/security-advisory-cve-2026-8461)

The Qt Network pin is also in the affected range for CVE-2026-105946 where
NTLM authentication is used, including through a proxy. Qt lists fixes in
6.8.10 and 6.11.2. The application includes Qt Network. The audit checked the
published dependency range and did not establish GameHQ-specific
exploitability or exercise an NTLM proxy. A media-only update would leave
this separate dependency review open. [Qt Network advisory](https://www.qt.io/blog/security-advisory-cve-2026-105946)

## Trust boundaries

- **Updater.** Public release metadata can select an offer. The application
  and helper authenticate the exact bytes of the detached manifest with a
  compiled trust table before accepting a package. Release sequence and
  accepted manifest identity enforce the anti-rollback rules. Size, digest
  and Windows destination checks run before replacing files. The transaction
  and rollback use app-owned locations within the package and profile.
  [Release manifest review](release-manifest-security-review.md)
- **Windows libraries.** The launcher and app restrict DLL search. Bundled
  runtime binaries are part of the package provenance. System-only dynamic
  loads and explicit runtime paths have different trust sources.
  [Packaging](packaging.md)
- **Local IPC.** Playnite and external Share pipes use same-user access.
  Framing, buffering and processing limits constrain accepted input.
  A peer's declared client/provider ID supplies protocol context.
  This boundary grants no OS sandbox or independent process authentication.
  External Share requires opt-in and a user-selected file. Job tokens bind
  callbacks to the matching connection/job, while the provider process keeps
  its ordinary Windows user access.
  [Integration protocol](integration-protocol.md), [Share API](share-provider-api-v1.md)
- **Capture library.** Deletion preserves both imported media and its library
  row. GameHQ-owned media use the Recycle Bin path. Decoder limits, path policies and app
  ownership checks are complementary controls.
  [Security and privacy](security-and-privacy.md)

## Verification scope

The local checkpoint covers manifest semantics and byte bounds, production
trust and test-key rejection, transaction and rollback fixtures, portable path
normalization, bounded IPC, provider manifest and job handling, and capture
deletion policy. Fixtures run against temporary files and owned test
processes. Live packaging, OS-level reparse and cross-account ACL tests,
network proxy behavior, and physical input remain separate checks. System DLL/driver patch
level and the exact FFmpeg patch identity of a future release package must
come from that package's own evidence.
