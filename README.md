![Phobos YR Engine Extension](logo.png)

[![license](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](https://www.gnu.org/licenses/gpl-3.0.html)

---

> # ⚠️ Unofficial fork — 非官方整合版
>
> **This repository is NOT official Phobos.** It is a personal, non-commercial
> modification of Phobos. It is neither affiliated with, endorsed by, nor
> supported by the Phobos development team or Electronic Arts.
>
> **本仓库不是官方 Phobos。** 它是 Phobos 的个人非商业修改版，与 Phobos 开发团队及
> Electronic Arts 均无隶属、认可或支持关系。
>
> |  |  |
> |---|---|
> | **Based on / 基线** | Phobos `v0.5.0.0-alpha1` (`5a72f20641ec9a23f14fcc3959f647c08c736cd2`) |
> | **Also integrates / 另整合** | upstream PR [#2060](https://github.com/Phobos-developers/Phobos/pull/2060) **"New trajectory system" — still an unmerged draft.** It reopens [#1582](https://github.com/Phobos-developers/Phobos/pull/1582) by [@CrimRecya](https://github.com/CrimRecya), reopened by [@TaranDahl](https://github.com/TaranDahl). |
> | **Fork additions / 本分支追加** | `SpecialAction` · `SweepFire` · `WeaponXFire` · `LocomotorWeapon` · `BodyWeapon` · `AdvancedAircraftMissions` · Scatter fixes · `Temporal` any-slot fix · engineer attack · Engrave `FiringAnim` |
> | **Version string / 版本串** | `v0.5.0.0-alpha1-pbsnew1` |
> | **Modification date / 修改日期** | 2026-09-26 |
>
> If a log, crash report or file property mentions `pbsnew`, you are running this
> fork and **not** official Phobos.
> **请注意不要把本版本的 bug 报到官方 Phobos 仓库。**
>
> - Upstream project: <https://github.com/Phobos-developers/Phobos>
> - Upstream docs: <https://phobos.readthedocs.io> · Phobos CN: <https://phoboscn.top>
> - Upstream Discord: <https://discord.gg/sZeMzz6qVg>

## What this fork changes / 本分支改了什么

### Integrated from upstream (not yet merged)

**PR [#2060](https://github.com/Phobos-developers/Phobos/pull/2060) "New trajectory system"** (original work by
[@CrimRecya](https://github.com/CrimRecya) in [#1582](https://github.com/Phobos-developers/Phobos/pull/1582), reopened
by [@TaranDahl](https://github.com/TaranDahl)) — merged here by hand on top of `v0.5.0.0-alpha1`:

- New trajectory framework with `Actual`/`Virtual` base classes, replacing the old flat
  `Straight`/`Bombard`/`Parabola` implementations.
- Actual trajectories: `Straight`, `Bombard`, `Missile`, `Parabola`. Virtual trajectories: `Engrave`, `Tracing`.
- Projectile life cycle, end conditions and retargeting logic.
- Projectiles that release warheads (`AdditionalWarheads`) and weapons (`AdditionalWeapons`) in flight.
- The `Trajectory-demo-*.gif` images used by the documentation.

> This is a **draft** PR. Upstream has not merged it, so nothing here is guaranteed to match
> what upstream eventually ships. See `docs/New-or-Enhanced-Logics.md` for the full INI reference.

### Added by this fork

| Feature | Summary |
|---|---|
| **SpecialAction** | A unit-triggered active ability (`SpecialAction=...`) with its own cooldown, 8 action kinds, an optional per-unit super weapon (`SpecialAction.SuperWeaponSource=unit`), cooldown pips and an `AttachEffect` release action. |
| **SweepFire** | Weapon-level sweeping fire: one trigger fires a whole series of real, individually interceptable shots along a configurable line while ammo, `ROF` and `Burst` stay vanilla. |
| **WeaponXFire** | `Weapon%dFire` in `artmd.ini` lets any weapon slot pick its own firing animation group, instead of only slot 0 getting `FireUp`. |
| **LocomotorWeapon** | Reworks `IsLocomotor=yes`: every `Locomotor=` GUID releases its victim properly instead of leaving it permanently unable to move, `DropPod` no longer crashes, and the behaviour is configurable through `LocomotorWeapon.*` warhead keys. |
| **BodyWeapon** | `BodyWeapon=0,1` lists vehicle weapon slots that may not use the turret — the turret is locked to the hull and the vehicle must turn its body to aim. |
| **AdvancedAircraftMissions** | Aircraft loiter over their destination instead of flying home, with `circle`/`hover` modes, plus a manual return that speeds up the cruise back to base. |
| **Scatter fixes** | Fixes scatter behaviour for laser, electric bolt and rad beam weapons. |
| **Temporal fix** | `Temporal=yes` warheads are no longer restricted to a unit's first weapon slot (and no longer crash when fired from another slot). |
| **Engineer attack** | Engineers can attack with their regular weapon. |
| **Engrave `FiringAnim`** | `Trajectory.Engrave.FiringAnim` / `FiringAnimInterval` for a continuous firing animation. |

## Installation / 安装

Identical to official Phobos: put `Phobos.dll` into your YR game directory and launch through
**SyringeEx**. `.pdb` is only needed for crash reports.

> ⚠️ Do not mix this DLL with an official `Phobos.dll`. Back up the previous file first.
> 不要把本 DLL 与官方 `Phobos.dll` 混放，先备份原文件。

## Building / 构建

- **Windows (upstream way):** `scripts\build_release.bat` with Visual Studio 2022 (MSVC v143).
- **Linux (fork addition):** `scripts/linux/build_linux.sh Release --build-type RELEASE` runs the real
  MSVC toolchain under Wine — see `scripts/linux/README.md`.
- **CI:** publishing a GitHub Release triggers `.github/workflows/release.yml`, which builds with
  `BuildType=RELEASE` and attaches `Phobos.dll` / `Phobos.pdb` to the release. This fork tags its
  releases `v0.5.0.0-alpha1-pbsnewN`, which upstream's changelog extractor does not recognise, so
  that step was made non-fatal here (`Fallback Release Notes`) — otherwise the run would abort
  before the assets are uploaded.

> **Build type decides the version string.** Only a `RELEASE` build reports
> `v0.5.0.0-alpha1-pbsnew1`. A plain local build — `--build-type` omitted, or the upstream
> `build_release.bat`, which passes no `BuildType` — reports
> `v0.5.0.0 @ <commit> @ refs/heads/main` instead, which does **not** identify this fork.
> Ship release-family builds.

## Known issues / 已知问题

- Multiplayer sync of `SpecialAction` (`Weapon` pre-load and per-unit super weapon) has **not** been verified.
- Some features were only compile-verified, not play-tested, at the time of this release.
- This fork tracks upstream `v0.5.0.0-alpha1` and is **not** synced with current upstream `develop`.

## License / 许可

GPL-3.0 — see [`LICENSE.md`](LICENSE.md). As a modification it also follows the
[EA C&C modding guidelines](https://www.ea.com/games/command-and-conquer/command-and-conquer-remastered/news/modding-faq):
**non-commercial only**, and no game assets are redistributed here.
This project has no affiliation with Electronic Arts Inc.

---

# Phobos

...is a community engine extension project providing a set of new features and fixes for Yuri's Revenge based on [modified YRpp](https://github.com/Phobos-developers/YRpp) and [SyringeEx](https://github.com/Phobos-developers/SyringeEx) to allow injecting code. It's meant to accompany [Ares](https://github.com/Ares-Developers/Ares) rather than replace it, thus it won't introduce incompatibilities.

While Phobos is independent of Ares and does NOT require Ares specifically to function, Phobos complements some of the features found in Ares and vice versa.

EA has not endorsed and does not support this product.

Community
---------

As the project is international and English is currently studied the most commonly as a second language, it's the main project language. We do not limit the creation of language-specific community groups though, because we understand that many may not know English as well as their native language and/or may have trouble accessing Discord.

- **[🌐 International Discord channel on C&C Mod Haven](https://discord.gg/sZeMzz6qVg)**
- **[🇨🇳 Chinese Phobos Communication Forum - Phobos CN](https://phoboscn.top)**

Downloads
---------

You can choose one of the following:
- [Latest stable branch build](https://github.com/Phobos-developers/Phobos/releases/latest) (most bug-free release but very slow on new features)
- [Latest pre-release builds](https://github.com/Phobos-developers/Phobos/releases) (a bit less bug-free releases, pre-releases get new features when they are finished)
- [Latest development branch nightly](https://nightly.link/Phobos-developers/Phobos/blob/develop/.github/workflows/nightly.yml) (added unreleased features that will be in the next pre-release)
- Individual new feature nightly builds for testing can be found in [pull requests](https://github.com/Phobos-developers/Phobos/pulls)

To learn how these build types relate to each other and how versioning works, see the [release model and version lifecycle](docs/Project-guidelines-and-policies.md#git-branching-model-version-lifecycle-and-release-strategy).

### Note on nightly builds

Last two listed versions are bleeding edge (don't redistribute them outside of testing!) and have build information (commit and branch/tag) in them which is displayed ingame and can't be turned off. You can get a build for development branch (link above) any up-to-date pull request via an automatic bot comment that would appear in it and would contain the most recent successfully compiled version of Phobos for that feature branch. Please note that the build is  produced *only if the PR has no merge conflicts*. Alternatively, you can get an artifact manually from GitHub Actions runs. You can get an artifact for a specific commit which is built automatically with a GitHub Actions workflow, just press on a green tick, open the workflow, find and download the build artifact. This is limited to authorized users only.

Installation and Usage
----------------------

0. Phobos requires [SyringeEx](https://github.com/Phobos-developers/SyringeEx) (v0.1.0.2 or newer) - an extended, open-source version of Syringe - to run; the game will show an error and quit on startup under older Syringe versions. Phobos packages and nightly builds come with the SyringeEx `Syringe.exe` bundled; it can also be downloaded separately from the [SyringeEx releases page](https://github.com/Phobos-developers/SyringeEx/releases). It's highly recommended to **install [Ares](https://launchpad.net/ares/+download)** too to get full Phobos feature set, just drop all the files from the archive except documentation folder and `Syringe.exe` into your game folder.
1. Obtain a Phobos "package" (official builds can be found on [releases page](https://github.com/Phobos-developers/Phobos/releases); read below to learn how to get nightly builds). You should end up with `Phobos.dll`, `Phobos.pdb` and the bundled SyringeEx `Syringe.exe`.
2. Place those files in the game folder (where your `gamemd.exe` is located), replacing any existing `Syringe.exe` (for example the one shipped with Ares).
3. To launch the game with Phobos (and all other installed Syringe-compatible engine extensions including Ares) you need to execute `Syringe.exe "gamemd.exe" [command line arguments for gamemd.exe]` in command line (omit arguments if you don't need any). `RunAres.bat` from Ares package does the same so you may use that as well.

Be sure to read [migration and breaking changes](docs/Whats-New.md#migration-breaking-changes) to know if you need to adjust something in your mod after Phobos installation (or update).

If you already use Ares in your mod, you just need to drop Phobos files mentioned above in your game folder, Syringe will load Phobos automatically. This also applies to mods using XNA client with Syringe; if your mod doesn't use Syringe and Ares (or you just haven't set up the client) yet we recommend to use [CnCNet client mod base by Starkku](https://github.com/Starkku/cncnet-client-mod-base) which is compatible with Ares and Phobos out of the box.

Additional files and tools that you may need are located at [Phobos supplementaries repo](https://github.com/Phobos-developers/PhobosSupplementaries).

By default Phobos doesn't do any very noticeable changes except a few bugfixes. To learn how to use Phobos features head over to official documentation.

Documentation
-------------

- [Official docs](https://phobos.readthedocs.io) (also available in [Chinese](https://phobos.readthedocs.io/zh_CN/latest))
- [Community Chinese docs](https://docs.qq.com/doc/p/dc3da1ce39a6e787b6e133f7d33d6aebef581cb4)
  - Because the Chinese translation of the official docs is currently underdeveloped, at the time it is recommended to use the community docs for Chinese users.

You can switch between versions (displays latest develop nightly version by default) in the bottom right corner, as well as download a PDF version.

The documentation is split by a few major categories, each represented with a page on the sidebar. Each page has its contents grouped into multiple subcategories, be it buildings, technotypes, infantry, superweapons or something else.

### How to read code snippets

```ini
; which section the entries should be in
; can be a freeform name - in this case the comment would explain what it is
; if no comment to be found - then it's a precise name
[SOMENAME]           ; BuildingType
; KeyName=DefaultValue ; accepted type with optional explanation
; if there's nothing to the right of equals sign - the default value is empty/absent
; if these keys have had their value set, they can only be set to their default
; unset state again by setting the value to <default>, <none> or none
; for list of values only <default> clears the entire list
; if the default value is not static - it's written and explained in a comment
UIDescription=<none> ; CSF entry key
```

Building manually
-----------------

0. Install **Visual Studio** (2022 is minimum) with the dependencies listed in `.vsconfig` (it will prompt you to install missing dependences when you open the project, or you can run VS installer and import the config). If you prefer to use **Visual Studio Code** you may install **VS Build Tools** with the dependencies from `.vsconfig` instead. Not using a code editor or IDE and building via **command line scripts** included with the project is also an option.
1. Clone this repo recursively via your favorite git client (that will also clone YRpp).
2. To build the extension:
   - in Visual Studio: open the solution file in VS and build it (`Debug` build config is recommended);
   - in VSCode: open the project folder and hit `Run Build Task...` (`Ctrl + Shift + B`);
   - barebones: run `scripts/build_debug.bat`.
3. Upon build completion the resulting `Phobos.dll` and `Phobos.pdb` would be placed in the subfolder identical to the name of the buildconfig executed.

Credits
-------

This project was founded by [@Belonit](https://github.com/Belonit) (Gluk-v48) and [@Metadorius](https://github.com/Metadorius) (Kerbiter) in 2020, with the first public stable release in 2021. Since then it has grown into a large community project with many contributors and maintainers.

### Interoperability

Phobos has opened the external interfaces of some key components. If you are also developing your own engine extension and wish to use Phobos at the same time, please check out [Interoperability](docs/Interoperability.md).

### Maintenance crew

Maintenance crew consists of experienced Phobos contributors who are recognized and given the permission to maintain and shape the project to the extent of their permissions.

Every maintenance crew member is welcome to put a donation link to their entry in the list below.

- **Kerbiter ([@Metadorius](https://github.com/Metadorius))** - T3 maintainer (lead)
  - [Patreon](https://www.patreon.com/kerbiter) · PayPal (preferable because of no fees) on request
- **[@Starkku](https://github.com/Starkku)** - T3 maintainer (co-lead)
  - [Patreon](https://www.patreon.com/Starkku)
- **[@CrimRecya](https://github.com/CrimRecya) (绯红热茶)** - T2 maintainer
  - [Alipay](https://www.phoboscn.top/t/topic/45#crimrecya)
- **[@ZivDero](https://github.com/ZivDero)** - T2 maintainer
  - [Patreon](https://www.patreon.com/c/ZivDero)
- **Ollerus ([@Coronia](https://github.com/Coronia))** - T1 maintainer
  - [Alipay](https://www.phoboscn.top/t/topic/45#ollerus)
- **[@NetsuNegi](https://github.com/NetsuNegi)** - T1 maintainer
  - [Alipay](https://www.phoboscn.top/t/topic/45#netsunegi)
- **[@TaranDahl](https://github.com/TaranDahl) (航味麻酱)** - T1 maintainer
  - [WeChatPay](https://www.phoboscn.top/t/topic/45#tarandahl)
- **Noble_Fish ([@DeathFishAtEase](https://github.com/DeathFishAtEase))** - triage, doc maintainer
  - [Alipay](https://www.phoboscn.top/t/topic/45#noble_fish)
- **FlyStar ([@Fly-Star-him](https://github.com/Fly-Star-him))** - triage
- **[@Fryone](https://github.com/Fryone)** - triage

#### Inactive

*Please note that being put here just means that you seem to be currently inactive as a part of maintenance crew. You are always welcome to return to the active crew if you want to help out again!*

- **Gluk-v48 ([@Belonit](https://github.com/Belonit))** - lead in the past
- **Uranusian ([@Thrifinesma](https://github.com/Thrifinesma))** - T2 maintainer, CN community ambassador, doc maintainer
- **[@secsome](https://github.com/secsome)** - maintainer
- **[@Otamaa](https://github.com/Otamaa) (Fahroni, BoredEXE)** - maintainer
- **[@FS-21](https://github.com/FS-21)** - inactive as a maintainer specifically
- **Morton ([@MortonPL](https://github.com/FS-21))** - T2 maintainer
- **Trsdy ([@chaserli](https://github.com/chaserli))** - T2 maintainer

The project is so big that listing here all the help we receive or received will make the size of the readme explode, so please see the [full credits list](CREDITS.md) for that. We appreciate your help, contributions and support regardless!

Attribution
-----

You can show your appreciation and help project's publicity by displaying the logo (monochrome version can be found [here](https://github.com/Phobos-developers/Phobos/blob/develop/logo-mono.png)) in your client/launcher (make it a button that opens Phobos GitHub page for extra fanciness). To fit with the mod styling, you are allowed to stylize the monochrome logo in a non-intrusive way (for example, recolor it to match the mod theme) without otherwise changing it. If unsure - ask us first.

When promoting features of your mod that you implemented using Phobos, please give credit to Phobos and it's contributors. A good promotion for Phobos is concise and specific to features that are being showcased, for example: "This feature is made possible by Phobos", "Implemented using XYZ from Phobos", etc. This helps end users understand the nature of Phobos and its role in the modding community, and gives the needed recognition to the project and its contributors.

Legal and License
-----

[![GPL v3](https://www.gnu.org/graphics/gplv3-127x51.png)](https://opensource.org/license/GPL-3.0)

The Phobos project is an unofficial open-source community collaboration project to extend the Red Alert 2 Yuri's Revenge engine for modding and compatibility purposes.

As a modification, the project complies with [EA C&C modding guidelines](https://www.ea.com/games/command-and-conquer/command-and-conquer-remastered/news/modding-faq); should there be conflict between the project's license and modding guidelines - the rules imposed by guidelines shall take precedence (for example, the project should not be commercial or used to make money).

This project has no direct affiliation with Electronic Arts Inc. Command & Conquer, Command & Conquer Red Alert 2, Command & Conquer Yuri's Revenge are registered trademarks of Electronic Arts Inc. All Rights Reserved.
