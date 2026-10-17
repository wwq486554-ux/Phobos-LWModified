![Phobos YR Engine Extension](logo.png)

[![license](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](https://www.gnu.org/licenses/gpl-3.0.html)
[![latest release](https://img.shields.io/github/v/release/wwq486554-ux/Phobos-LWModified?include_prereleases&label=release)](https://github.com/wwq486554-ux/Phobos-LWModified/releases)

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
> | **Based on / 基线** | Phobos `v0.5.0.0-alpha2` (`b1dc830570358315273ce3f958a8846c439c1d7e`) |
> | **Also integrates / 另整合** | upstream PR [#2060](https://github.com/Phobos-developers/Phobos/pull/2060) **"New trajectory system" — still an unmerged draft.** It reopens [#1582](https://github.com/Phobos-developers/Phobos/pull/1582) by [@CrimRecya](https://github.com/CrimRecya), reopened by [@TaranDahl](https://github.com/TaranDahl). |
> | **Current version / 当前版本** | `v0.5.0.0-alpha2-pbsnew6` |
> | **Last updated / 最后更新** | 2026-10-17 |
> | **Releases / 下载** | <https://github.com/wwq486554-ux/Phobos-LWModified/releases> |
>
> If a log, crash report or file property mentions `pbsnew`, you are running this
> fork and **not** official Phobos.
> **请注意不要把本版本的 bug 报到官方 Phobos 仓库。**
>
> - Upstream project: <https://github.com/Phobos-developers/Phobos>
> - Upstream docs: <https://phobos.readthedocs.io> · Phobos CN: <https://phoboscn.top>

## What this is / 这是什么

A personal, non-commercial fork of [Phobos](https://github.com/Phobos-developers/Phobos)
built on the official `v0.5.0.0-alpha2` baseline, adding a set of engine-level gameplay
extensions and fixes on top of the official feature set. Everything is driven by
`rulesmd.ini` / `artmd.ini` keys, keeps working together with Ares, and has its own INI
reference in [`docs/`](docs/). Features the vanilla Phobos project provides behave exactly
as upstream documents them — this fork only adds to them.

## What this fork adds / 本分支新增内容

The list below is a summary; the complete reference for every key is in
[`docs/`](docs/) and the full changelog is [`docs/Whats-New.md`](docs/Whats-New.md).

### Weapons, projectiles and combat / 武器、弹道与战斗

- **[SweepFire](docs/New-or-Enhanced-Logics.md#sweepfire)** — a weapon-level sweeping fire
  mode: one trigger fires a whole series of real, individually interceptable shots along a
  configurable line while `Ammo`, `ROF` and `Burst` keep their vanilla behaviour.
- **[ROF curve](docs/New-or-Enhanced-Logics.md#rof-curve-weapon-level-spin-up)** — a weapon
  can fire faster or slower the longer it keeps firing, without swapping weapons; the same
  curve also paces the gaps between SweepFire sweeps and Temporal freezes.
- **[Universal scatter shaping](docs/New-or-Enhanced-Logics.md#universal-scatter-shaping)**
  — `Sigma`, `Aspect` and `Divergence` give `BallisticScatter` the same meaning on every
  engine scatter path, extend it to vanilla `ROT`/`Arm` missiles (through their burst
  point) and keep `Trajectory=Missile` offsets alive through the pre-detonation snap.
- **[Spawned missile scatter](docs/New-or-Enhanced-Logics.md#spawned-missile-scatter)** —
  `MissileSpawn=yes` missiles can be made to fly to a point offset from the target centre
  (`Missile.Scatter`), so the engine's own landing payload explodes off-centre.
- **[Sudden death warhead](docs/New-or-Enhanced-Logics.md#sudden-death-warhead)** — a
  warhead whose detonation kills whatever it reaches; it bypasses damage, armour,
  `Verses`, `Immune=yes`, shields and the Iron Curtain, and clears the tiberium, breaks
  the bridges and removes the terrain objects in its radius.
- **[Body-aimed weapons](docs/New-or-Enhanced-Logics.md#body-aimed-weapons)** — vehicle
  weapon slots that are aimed by turning the hull instead of the turret.
- **[Locomotor weapon](docs/New-or-Enhanced-Logics.md#locomotor-weapon)** — a rework of
  `IsLocomotor=yes`: every `Locomotor=` GUID releases its victim properly, `DropPod` no
  longer crashes, and the behaviour is configurable through warhead keys.
- **[Per-weapon firing animation selection](docs/New-or-Enhanced-Logics.md#per-weapon-firing-animation-selection)**
  — each weapon slot chooses its own firing animation group in `artmd.ini`.
- **[Engrave `FiringAnim`](docs/New-or-Enhanced-Logics.md#projectile-trajectories)** — a
  continuous firing animation for the Engrave trajectory.

### Units and abilities / 单位与技能

- **[SpecialAction](docs/New-or-Enhanced-Logics.md#specialaction)** — a unified,
  player-triggered action entry point for any TechnoType, bound to the **Special Action**
  hotkey. It is a dispatcher into existing game systems rather than one scripted effect,
  so a single unit can expose several independent abilities:
  - **weapon firing** — arm the unit so its *next* firing cycle uses one of its own weapon
    slots, then attack normally; `Burst`, ammo, animations and targeting all stay vanilla;
  - **deployment and transformation** — the complete vanilla deploy package, or only its
    parts: the `DeploysInto` building transformation, a one-way `Convert` to another
    TechnoType, the deploy weapon fired at the unit's own position (`DeployFire`), and
    passenger release without deploying (`Unload`);
  - **super weapons** — fire a super weapon at the unit's own position through the normal
    launch queue, optionally from a super weapon the unit itself carries
    (`SpecialAction.SuperWeaponSource=unit`) instead of the owner's sidebar one;
  - **attached effects** — release and/or strip the listed Attach Effects on the unit
    itself, with duration overrides and group-based removal;
  - **aircraft return** — order the aircraft home immediately, sharing the ability and its
    cooldown with the airfield right-click.

  The deploy-family actions are deliberately **decoupled from the vanilla deploy hotkey**:
  the ability reads its own parameters instead of the type's `DeploysInto`/`DeployFire`
  keys, so one vehicle can keep vanilla deploy on the Deploy hotkey and still expose a
  second, independent deploy-family ability. Each action has its own cooldown
  (`SpecialActionROF`, with an elite override), its own voice and optional cooldown pips,
  all independent of weapon `ROF`, super weapon recharge and deploy time;
  `EliteSpecialAction` replaces the action at elite veterancy.
- **[Extra weapons](docs/New-or-Enhanced-Logics.md#extra-weapons)** — any TechnoType can
  mount additional weapons that fire at the current target alongside the engine-driven
  one, each on its own `ROF`, `Burst`, projectile, damage and range.
- **[Carryall](docs/New-or-Enhanced-Logics.md#carryall-jumpjet-vehicles-lifting-other-vehicles)**
  — a war-factory vehicle with the Jumpjet locomotor lifts another vehicle with a
  force-move click, carries it underneath and puts it down with
  `SpecialAction=DropPayload`.
- **[Tank makeup kit](docs/New-or-Enhanced-Logics.md#tank-makeup-kit-vehicle-disguise-as-the-unit-it-hits)**
  — a vehicle firing a `MakesDisguise=yes` warhead takes on the look of the unit it hits
  and keeps it.
- **[Armed engineers](docs/Fixed-or-Improved-Logics.md#armed-engineers-can-attack)** — an
  armed engineer that force-fires attacks like any other armed infantry instead of the
  click being rewritten back to capture.

### Aircraft and missiles / 空军与导弹

- **[Advanced Aircraft Missions](docs/Fixed-or-Improved-Logics.md#advanced-aircraft-missions)**
  — aircraft loiter over their destination instead of returning to base, with
  `circle`/`hover` modes, plus a manual return that speeds up the cruise home.
- **[Guided spawned missiles](docs/New-or-Enhanced-Logics.md#guided-spawned-missiles)**
  (`Missile.Homing`) — spawned missiles aim at the target's predicted intercept point
  instead of its current position, with a swept-segment proximity fuze and a per-type
  terminal-dive control.

### Temporal warheads / 超时空武器体系

- A rework of the `Temporal=yes` freeze system: it can keep its target damageable and open
  to warhead effects, freeze for a fixed duration, put the cooldown on the caster, decide
  what a freeze may do to the target's last hit point, and warp the whole `CellSpread`
  radius with per-victim timers or one pooled timer
  ([docs](docs/Fixed-or-Improved-Logics.md#freeze-a-unit-for-a-fixed-duration)).
- `Temporal=yes` warheads now also work from any weapon slot instead of only the unit's
  first one ([fix notes](docs/Fixed-or-Improved-Logics.md#bugfixes-and-miscellaneous)).

### User interface and engine fixes / 界面与引擎修复

- **[Tactical view zoom](docs/User-Interface.md#tactical-view-zoom)** — the tactical map is
  rendered at a resolution independent of the UI, with configurable zoom levels and
  hotkeys.
- **[Bugfixes](docs/Fixed-or-Improved-Logics.md#bugfixes-and-miscellaneous)** — a set of
  upstream and fork fixes, including the Syringe hook-size correction that removed the
  `faulty return 0 hook` instruction-rebuild warnings.

## Installation / 安装

Identical to official Phobos:

1. Phobos requires **[SyringeEx](https://github.com/Phobos-developers/SyringeEx)**
   (`v0.1.0.2` or newer); the release archives bundle `Syringe.exe`. Installing
   [Ares](https://launchpad.net/ares/+download) as well is recommended.
2. Download `Phobos.dll` (plus `Phobos.pdb` if you want crash reports) from the
   [releases page](https://github.com/wwq486554-ux/Phobos-LWModified/releases) and put it
   into the game folder next to `gamemd.exe`.
3. Launch the game through `Syringe.exe` (or `RunAres.bat` from Ares).

> ⚠️ Back up the previous `Phobos.dll` first, and never mix this file with an official
> `Phobos.dll`. 先备份原文件，不要把本 DLL 与官方 `Phobos.dll` 混放。

## Compatibility and known issues / 兼容性与已知问题

- **This is a personal test build.** The new features have been tested in game, but they
  are **not guaranteed to be 100% stable** — keep a backup of `Phobos.dll` and of your
  savegames before switching. Bug reports are welcome on this repository's issue tracker,
  but **do not report them to the official Phobos repository**.
- **Savegames are not compatible between different fork builds.** The new type-level INI
  keys and per-missile save fields change the extension layout, so a save written by an
  older build is refused cleanly. Start a new game after updating.
- **Multiplayer:** most additions are deterministic and sync-safe, but a few features have
  not been checked over a full network match yet.
- This fork tracks upstream `v0.5.0.0-alpha2` and is **not** synced with the current
  upstream `develop`.

## Building from source / 构建

- **Windows (upstream way):** `scripts\build_release.bat` with Visual Studio 2022
  (MSVC v143).
- **Linux (fork addition):** `scripts/linux/build_linux.sh Release --build-type RELEASE`
  runs the real MSVC toolchain under Wine; see `scripts/linux/README.md`.
- **CI:** publishing a GitHub Release triggers `.github/workflows/release.yml`, which
  builds with `BuildType=RELEASE` and attaches `Phobos.dll` / `Phobos.pdb` /
  `gamemd.edb` / `Syringe.exe` to the release.

> **Build type decides the version string.** Only a `RELEASE` build reports
> `v0.5.0.0-alpha2-pbsnewN`. A plain local build — `--build-type` omitted, or the
> upstream `build_release.bat`, which passes no `BuildType` — reports
> `v0.5.0.0 @ <commit> @ refs/heads/main` instead, which does **not** identify this fork.
> Ship release-family builds.

## Documentation / 文档

- This repository's [`docs/`](docs/) is the full INI reference for the features listed
  above, plus the complete changelog in [`docs/Whats-New.md`](docs/Whats-New.md). The same
  sources build the project's Sphinx documentation.
- [Official Phobos documentation](https://phobos.readthedocs.io) (also
  [in Chinese](https://phobos.readthedocs.io/zh_CN/latest)) — reference for the unmodified
  upstream features this fork is based on.

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
UIDescription=<none> ; CSF entry key
```

## Credits and license / 署名与许可

- This fork is maintained by **laowang**; the full contribution list is in
  [`CREDITS.md`](CREDITS.md).
- Based on [Phobos](https://github.com/Phobos-developers/Phobos), founded by
  [@Belonit](https://github.com/Belonit) and [@Metadorius](https://github.com/Metadorius).
  The upstream project's maintenance crew, community channels and contribution guidelines
  are described in the upstream repository.
- The integrated trajectory system (upstream PR #2060) is original work by
  [@CrimRecya](https://github.com/CrimRecya), reopened by
  [@TaranDahl](https://github.com/TaranDahl).
- GPL-3.0 — see [`LICENSE.md`](LICENSE.md). As a modification, the project also follows
  the [EA C&C modding guidelines](https://www.ea.com/games/command-and-conquer/command-and-conquer-remastered/news/modding-faq):
  **non-commercial only**, and no game assets are redistributed here. This project has no
  affiliation with Electronic Arts Inc. Command & Conquer, Command & Conquer Red Alert 2
  and Command & Conquer Yuri's Revenge are registered trademarks of Electronic Arts Inc.
  All Rights Reserved.

---

## About Phobos / 关于上游 Phobos

Phobos is a community engine extension project providing a set of new features and fixes
for Yuri's Revenge, based on [modified YRpp](https://github.com/Phobos-developers/YRpp) and
[SyringeEx](https://github.com/Phobos-developers/SyringeEx). It is meant to accompany
[Ares](https://github.com/Ares-Developers/Ares) rather than replace it. Phobos is
independent of Ares and does not require it specifically to function, but the two
complement each other.

- Upstream repository: <https://github.com/Phobos-developers/Phobos>
- Upstream releases: <https://github.com/Phobos-developers/Phobos/releases>
- Community: [C&C Mod Haven Discord](https://discord.gg/sZeMzz6qVg) ·
  [Phobos CN](https://phoboscn.top)
- If you promote a mod feature implemented with Phobos, please credit Phobos and its
  contributors; the upstream project's attribution and logo policy applies.

EA has not endorsed and does not support this product.
