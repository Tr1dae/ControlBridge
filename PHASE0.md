# Control Bridge Phase 0: recon

Started 2026-10-11. Static analysis of both installs, plus passive probe DLLs. RVAs are relative to the
named module.

## Builds this is pinned to

| | Control (2019), guest | CONTROL Resonant (2026), host |
|---|---|---|
| Steam app / build | 870780 (Ultimate Edition) / **17833993** | 3669870 / **25767184** (exe updated 2026-10-08) |
| Exe | `Control.exe` launcher, then `Control_DX12.exe` or `Control_DX11.exe` (0.0.518.2177, linked 2025-03-23) | `CONTROLResonant.exe` 0.564.629.5, linked 2026-10-05, 99 MB |
| Engine layout | Split DLLs: `coregame_`, `physics_`, `renderer_`, `rl_`, `snd_`, ... `rmdwin10_f.dll` (DX12) / `rmdwin7_f.dll` (DX11) | Monolithic exe |
| Graphics | D3D12 (`d3d_rmdwin10_f.dll` imports d3d12/dxgi) or D3D11 | **D3D12 only**, through NVIDIA Streamline (`sl.interposer.dll` wraps `D3D12CreateDevice` / `CreateDXGIFactory1`), DLSS/DLSS-G, FSR 3 frame generation |
| Physics | **PhysX 3.4.2** + APEX 1.4.1 + PhysX CCT (`physx3characterkinematic_x64.dll`) | **PhysX 5.6.1** (`PhysX_64.dll`, exports `PxGetPhysics`), NvCloth |
| Audio | Wwise, `AK::SoundEngine` exported from `snd_rmdwin*_f.dll` (float `AkTransform`) | Wwise, `AK::SoundEngine` exported from the exe (double `AkWorldTransform`) |
| Input | `rl_rmdwin*_f.dll` loads `xinput1_4` / `xinput1_3` / `XInput9_1_0` dynamically and resolves `XInputGetState` by name; raw input for keyboard/mouse | Static import `XINPUT1_4.dll` (ordinal 2 = `XInputGetState`), HID for Sony pads |
| Type info | **~17k exported C++ symbols** across the engine DLLs (decorated names, so `GetProcAddress` works) + RTTI (2.5k types in the exe, 1k in coregame) | **8,790 RTTI types** (`contentgame`, `heron` gameplay ECS, `coregame`, `rend`, `physics`, `physx`, `m`, ...). 650 exports, all Wwise / Lua timeline |
| Packing / anti-tamper | None: normal sections, no packer, not signed | No packer (normal sections); Authenticode-signed. f2g reports the only integrity check covers `steam_api64.dll`, and there's no mod check for achievements |
| Mod loading | `winmm.dll` is a static import of `navigation_rmdwin*_f.dll`, and winmm is not a KnownDLL here, so a winmm proxy in the game folder loads at startup. reg2k's Control Plugin Loader uses `xinput1_4.dll` the same way (loads `plugins\*.dll`); it isn't installed | f2g crloader (`winmm.dll` proxy) is installed and loads `crmods\**\*.dll`. Installed mods: CRModMenu, MapFusion, WeaponWheel, EasyWeaponWheel |
| Saves | `C:\Program Files (x86)\Steam\userdata\84163595\870780\remote` | `...\userdata\84163595\3669870\remote` |

- Both Steam manifests have `AutoUpdateBehavior 0` (always keep updated). Steam can't pin a build. To limit
  surprises, set each game's update option to "only update when I launch it" and launch through Steam
  in offline mode when a phase works.
- **Resonant changed under the existing mods.** MapFusion, WeaponWheel and CRModMenu last logged
  "known build 25472515" (SHA-256 `2c6575be...`). The current exe (build 25767184) is newer, so any
  RVA-pinned mod needs re-checking. That includes the EasyWeaponWheel RVAs in memory (IAT `0x3a79618`,
  resource vtable `0x5110c30`).

## Control 1: what the exports give us (Jesse, camera)

| Need | Export (coregame unless noted) | RVA (rmdwin10) | Notes |
|---|---|---|---|
| Player controller -> avatar | `ClientPlayerController::getAvatarObject` | `1119f0` | Returns `EntityState*` (Jesse). Imported by `Control_DX12.exe`, so the game calls it from game code. Server twin: `ServerPlayerController::getAvatarObject`. Avatar handle at controller `+0x60` |
| Session -> controller | `ClientGameSession::getPlayerController` | `42840` | `mov rax,[rcx+0x6d0]` |
| Entity transform | `EntityState::getTransform` (virtual, hidden return in rdx) | `127bc0` | Looks up the `TransformComponentState` and returns a `Matrix4x3<float>` |
| Transform component | `TransformComponentState::getWorldRotation` / `getWorldPosition` | `1bda80` / `1bda70` | Rotation quat at **+0xC0**, position (SIMD) at **+0xD0** |
| Camera | `CameraComponentState::getView` | `bb960` | `PerspectiveView` at **+0xC0** |
| | `CameraComponentState::updateViewFromTransform` | `bd300` | Per-camera update, hooked by the probe |
| View matrix | `m::View::getViewToWorld` (rl) | `112750` | **+0x70**, `Matrix4x3<double>` (Northlight keeps world positions in doubles) |
| FOV | `m::PerspectiveView::getFov` (rl) | `11ea40` | **+0x2E0** float; `getVerticalFov` derives from it |
| Character controller | `CharacterControllerComponentState::getCapsuleSize`, `setKeyframed` | `c1ca0`, `c1cd0` | Phase 1/2: Jesse's PhysX CCT |
| Physics scene | `EntityComponentState::getPhysicsScene` | | Phase 2 |
| Other singletons | `GameObjectManager::getInstance`, `SceneGraph::getInstance`, `input::InputManager::getInstance`, `app::Gamepad::*` (app) | | |

## Resonant: what we have to work with

- **ECS gameplay layer** (`heron` namespace, `ComponentStub<...>` RTTI): `coregame::component::Player`,
  `CameraView`, `CameraManView`, `CharacterController`, `CharacterControllerVoxelCache`,
  `LocomotionOutputs`, `heron::player_locomotion::Locomotion`, `tailcamera` (third-person camera),
  `player_input::{Input, Movement, CameraTransform}`, `photo_mode_camera`.
- **Still there from Control 1:** `m::PerspectiveView` (with RTTI, so live instances can be found by
  vtable), `content::TransformComponent`, `content::PerspectiveViewComponent`, `physics::PhysicsScene`,
  `physics::CharacterController`.
- **The character controller is new.** It sweeps against a voxel cache
  (`coregame::charactercontroller::voxelSweeperEarlyAsyncUpdate`, `CharacterControllerVoxelCache`) on top
  of PhysX 5. Whether the player's puppet can be driven like ArkWeb's Batman is a Phase 1 question.
- **Camera / player via Wwise.** `RenderAudio` is the frame tick. The default listener is normally the
  camera, and the player's emitters move with Dylan. The probe confirms both live.

## ArkWeb: what transfers

| ArkWeb piece | Transfers? | Here |
|---|---|---|
| Overall guest/host split, shared memory (seqlock slots + SPSC rings), heartbeats, live command files, Python streamer | **Yes, as a design** | Same layout idea. ArkWeb has no license file, so Control Bridge reimplements rather than copies |
| Proxy-DLL loading (winmm/dinput8) | Yes | Control: own `winmm.dll` proxy. Resonant: crloader mod, so no proxy of our own |
| Virtual pad (XInput through `GetProcAddress`), registry/WGI tricks | Mostly | Control resolves `XInputGetState` dynamically, the same shape as Spider-Man. Neither game contains "Windows.Gaming.Input", so ArkWeb's WGI workaround isn't needed |
| Fake focus (foreground/focus hooks, swallowing deactivation, cursor clip) | Pattern yes, details no | Northlight `app::` / `coregame::GameWindow` replace Nixxes' `NxGameWindowImpl` flags |
| Coordinate conversion module + link test | Yes (structure) | Both are Northlight, so units and handedness probably match: maybe an offset plus double precision instead of a basis change. The CSVs confirm it |
| Host puppet driving via UE3 natives (`SetLocation`, `SetHidden`, `ProcessEvent`) | **No** | Resonant is a Northlight ECS; the player components must be found first (Phase 0b) |
| Collision export from host PhysX 3.3 (vtable slots via reflection) | **Concept yes, much easier** | Resonant is PhysX 5.6.1 and Control is PhysX 3.4.2. Both are open source with public headers, so the vtable layouts are known instead of reverse-engineered |
| Collision injection into guest Havok (hknp compressed meshes, query filter) | **No** | The guest is PhysX 3.4 (`PxCreateRigidStatic`, `PxCooking` exported). Gotham-in-Havok becomes Manhattan-in-PhysX: a separate scene or a filter group for the streamed tiles |
| Frame capture D3D12 (guest) -> D3D11 overlay (host) | Half | Guest capture D3D12: yes (`Control_DX12`). The host overlay must be **D3D12** and coexist with Streamline/DLSS-G; frame generation off |
| Tools: PE/RTTI/strings/disasm/xrefs | Yes, in spirit | `tools/` has smaller equivalents (pefile + capstone). Control's export table replaces most of ArkWeb's RTTI hunting |
| UE3 SDK dumper, Havok/Spider-Man/Arkham specifics (swing hints, grapple points, pose retarget) | No | Game-specific |
| Combat | No (ArkWeb hands combat to the host) | Phase 4 needs Resonant hit/damage events, which nothing in ArkWeb covers |

## Phase 0 probes (built 2026-10-11, not yet run in either game)

- `bin\control\winmm.dll` (Control proxy) and `bin\resonant\cb_probe_resonant.dll` (crloader mod).
- Shared core in `src/common/`. The offline self-test passes.
- Every hook target is resolved by **exported name**, never a byte pattern:
  - Control: `getAvatarObject` x2, `updateViewFromTransform`, plus Wwise
  - Resonant: Wwise
- `tools/check_probe_targets.py` confirms the names in the installed builds:
  - Control: DX12 and DX11 sets both complete
  - Resonant: all 8 Wwise functions present (`SetMultiplePositions` has 2 overloads)

### First in-game test plan (one game at a time)

1. **Control alone.** Load a save, walk in a circle, jump, look around with the right stick for about 30 s,
   then quit. Check `logs\control_probe.log`:
   - every `hook ...: MH_OK`
   - `avatar client: ... ok 1` with a sensible matrix
   - cams listed with one marked `*`
   - the Wwise listener and emitters
   - wwise/s rates around the frame rate

   In `control_frames_*.csv`, check that p0..p15 move with Jesse (which entries are translation?), that
   c0..c11 follow the camera, and that the listener matches the camera.
2. **Resonant alone.** Same walk. Check `logs\resonant_probe.log`: hooks MH_OK, `RenderAudio` at
   about the frame rate, listener present, and nearest emitters (Dylan should be 2 to 4 m from the
   listener). Then write `views` into `logs\resonant_cmd.txt`: one `m::PerspectiveView` should contain
   the listener's position. Track it with `view <addr>` and check CSV c0..c11.
3. From the CSVs: units, up axis and handedness for both games (forward stick vs position delta), and
   whether listener = camera in each.

## Resonant ECS: system map (2026-10-11, static)

- The exe keeps `ecs::system::getName<&fn>` `__FUNCSIG__` strings for **2,414 ECS systems**.
  `tools/ecs_systems.py` follows each one to its registration and records the code pointers the
  registration stores in the system object:
  - `+0x140` = **run thunk** (iterates the matching entities)
  - `+0x148` = auxiliary; `exe+3f1d80` is a shared default
- 2,247 systems resolve; 2,033 run thunks are exact `.pdata` function starts. Output:
  `dumps/resonant_ecs_systems.json` (not committed: it's derived from the game binary). Re-run it after a
  game update.
- A run thunk receives the ECS world in `rcx`. The thunk for `applyControllerToTransform` walks an
  entity bitset from `[world]+0x58480` (array, `<< 10` per index taken from `[r8+0xC8]`) and
  `[world]+0x58478`.

| System (build 25767184) | Run thunk | Why it matters |
|---|---|---|
| `coregame::charactercontroller::applyControllerToTransform` | `1bc6500` | Writes the character controller's result into the entity transform. **Puppet driving**: run the original, then overwrite the player's transform with Jesse's |
| `coregame::charactercontroller::prePhysics` / `voxelSweeperEarlyAsyncUpdate` | `1bc5d90` / `1bc5a00` | Controller simulation; may need suppressing for the puppet |
| `coregame::charactermovement::applyMovement` | `1ba1390` | Animation/locomotion movement into the controller |
| `coregame::transform::clearTeleported` | `18402a0` | The `Teleported` tag is cleared once per frame, so teleports = write transform + tag Teleported |
| `heron::fall_respawn::*` | `25f...` | The game's own safe-position and respawn logic. Its cached safe position is a candidate for the safe-teleport command |
| `heron::cameramixer::camera_update::system` | `2825130` | Final gameplay camera blend: the point where Jesse's camera can replace the host camera (Phase 3) |
| `coregame::freecamera::update`, `coregame::fpscamera::update` | `1b04680`, `1b0ae70` | Alternative camera override paths (debug/photo cameras) |
| `heron::player_locomotion::*` | `22bb...` | Player locomotion state, primary/secondary drivers, driver override |

- Components seen in signatures: `WorldTransformReadOnly`, `PreviousWorldTransformReadOnly`,
  `LocalTransformReadOnly`, `Teleported`, `ActiveAvatar`, `Player`, `CameraView`, `FreeCameraTransform`,
  `InterpolatedTransform`.

## In-game test 1 (2026-10-11): passive probes in both games, no crashes

Controller session in each game: stand, stick forward, stick right, jump, camera turn. Control also had
an alt-tab. Analysis: `tools/analyze_frames.py` plus the camera-turn check.

**Coordinate systems: both games are left-handed, Y up, meters.**
- Up: Control jump +0.83 m on Y; Resonant camera rises 1.8 to 1.95 m on Y when Dylan jumps.
- Units: Jesse jogs about 4.4 m/s; Dylan runs about 5.8 m/s.
- Handedness:
  - Control: stick right moved Jesse along camera-right = (forward × up) · right = −0.99.
  - Both games: turning the camera right swings forward toward up × forward, in every sample
    (5 in Control, 7 in Resonant). So right = up × forward: X right, Y up, Z forward, as in D3D.
- The cross-game mapping is therefore a translation (plus a yaw if we re-anchor). No basis change.

**Control (2019)**
- Jesse's transform: `EntityState::getTransform` returns 4 rows of 3 floats. Rows 0-2 are rotation;
  translation is `p9..p11`. `getAvatarObject` is called about 160 times/s on thread 14432 (not the
  main thread). The entity is a `GenericEntityState`.
- `CameraComponentState::updateViewFromTransform` is **not** the gameplay camera: 19+ camera states,
  each updated only about 3 times, at positions far from Jesse. The gameplay camera must come from
  elsewhere; `views` (m::PerspectiveView matched to the listener) is the next try.
- Wwise:
  - listener id 4294967299 = the camera: 4 m behind and 2 m above Jesse; front matches the view
  - emitter 4294967297 rides on the listener
  - Jesse's emitters about 3 m from the camera
- Frame tick (RenderAudio) runs on the main thread at about 120 to 190 fps.
- **Alt-tab:** only `GameWindow::hasFocus` changed (1 -> 0 -> 1). No `setGamePaused`, no
  `InputManager::setFocus` call, and **the game kept running at about 152 frames/s in the
  background**. Faking focus may only be needed for input.

**CONTROL Resonant**
- Wwise: same listener id 4294967299 (shared Remedy audio code). Layout 3:
  `{front, top, double position}`. Emitter 4294967297 rides on the camera.
- Dylan's body emitters form a cluster about 4.5 m from the camera (ids like 252/253/254, stacked
  0.2 to 0.5 m apart in Y).
- RenderAudio about 130 to 175 fps on one thread. SetPosition peaks at about 1,600/s in busy areas.
- Dylan's own transform and the camera view are still unknown. The probe's `autofind` (delta-matching
  memory search seeded from his emitter) and `views` now run automatically (`AutoRecon=1`).

## In-game test 2 (2026-10-11): auto recon

- **Resonant: Dylan's position found** by delta-matching (2 moves of about 18 to 20 m):
  - 702 survivors. The first is a **double triple at feet height** (y 0.145 while the camera
    emitter is at 1.79): `575.917 0.145 -80.889`.
  - A second double triple 0x58 bytes later lags slightly (`576.025 ... -80.882`): previous frame or
    controller.
  - The rest are float copies (render/animation).
  - The first snapshot hit the caps (4M doubles + 4M floats with a ±2 m cube), so the box is now
    ±1 m in X/Z and ±2.5 m in Y, with the float cap at 1M.
- **Resonant camera:** `m::PerspectiveView` instances hold the camera position as doubles at
  **+0xB0**, so Resonant's `m::View` is 8 bytes shorter before the matrix than Control's (view-to-world
  at **+0x68**). It sits about 1.7 m from the Wwise listener: Resonant's listener is not exactly on the
  camera (it's pulled toward Dylan), which is why the 0.5 m match failed.
- **Control camera: `coreshared::g_pRenderView`** (exported `const m::View*`). m::View getters:
  - view-to-world `+0x70` (rows right/up/forward/position; `getViewDirection` +0xA0,
    `getViewPoint` +0xB8)
  - view-to-clip `+0xD0`

  The probe now reads it every frame. In test 2 the 64-instance cap was hit before the gameplay view
  turned up.
- Control's views scan took 19 s (copying every heap page). Scans now run in place under SEH, with
  results in a private block the scan skips. Control's autofind never ran: the tester walked during
  the views scan and quit during the snapshot. The probe now chimes (Windows sounds) at "walk now",
  "walk again" and "done".

## In-game test 3 (2026-10-11)

- **Control camera solved:** `g_pRenderView` points at a static `m::View` inside `Control_DX12.exe`.
  It gives position, axes and fovY 43 deg every frame.
- **Resonant ECS observers:** `applyControllerToTransform` runs about 30 times/s on varying worker
  threads (frames about 130/s), so it looks like a fixed 30 Hz simulation step. Its first argument is a
  stack context, not the world. `cameramixer::camera_update` runs 17 to 100/s, varying.
- **Both games: the first heap scan never finished.** Cause, reproduced offline: scanning in place
  reads GPU upload/readback heaps, which are write-combined, and CPU reads of write-combined memory are
  about 200x slower (256 MB: 4.2 s vs 0.02 s). Heap walks now skip `PAGE_WRITECOMBINE` /
  `PAGE_NOCACHE`, log progress, stop after 30 s, and auto recon runs on its own thread (snapshot
  first, then views). The chimes never played because the scan never ended; they now also sound an
  explicit `Beep()` tone.

## In-game test 4 (2026-10-11): Dylan's position and Resonant's camera found

- Scans now take under 1 s: about 4 GB scanned, about 10.6 GB of write-combined GPU memory skipped
  (Control: 2.3 GB / 6 GB).
- **Resonant camera:** the `m::PerspectiveView` instance 1.5 m from the listener has sane axes and
  fovY 55.6 deg (from the view-to-clip that follows view-to-world). Only heap and stack pointers lead
  to it (no module global found yet), so it is re-found each session with `views` (auto recon, no
  walking needed).
- **Dylan = Remedy's `physics::CharacterController`.** The delta-matched double triple sits at
  `ControllerHitReport + 0x38`:
  - vtable `exe+4d93290`, RTTI `.?AUControllerHitReport@physics@@`, a member object
  - a `createSweeper` lambda just before it
  - a second copy at +0x58 (previous step)

  The values are floats stored as doubles (the low mantissa bits are zero), as in PhysX CCT's
  `PxExtendedVec3`. The probe now finds the ControllerHitReport vtable by RTTI name, scans its
  instances and tracks the one nearest the player's emitters (`src/probe_resonant/player.h`).
- Other movers near Dylan: an AABB plus root pose block (min/max corners about 3.7 m tall, a quaternion,
  float position), no vtable: render/bounds data.
- **Control camera** cross-check: the m::PerspectiveView 0.00 m from the listener equals g_pRenderView's
  position.
- Control's autofind picked emitter 4294967346 (a camera-relative ambient emitter, not Jesse). Not
  needed there: Jesse comes from `getTransform`.

## Phase 1 started: passive link

Both probes open `Local\ControlBridge_v1` and publish every frame:
- Resonant: `HostState` (Dylan's controller position, the tracked camera view) and `PadState` (the
  real controller while Resonant is in front)
- Control: `GuestState` (Jesse's transform, the g_pRenderView camera)

`tools/cb_monitor.py` shows both sides live, plus the Dylan - Jesse offset. It uses
`bin/link_test.exe --layout` for offsets and is tested with `link_test --fake`. Nothing is driven yet.

## In-game test 5 (2026-10-11): both games at once, passive link

- Both games ran together without crashes. Frame rate was fine: Resonant 120 to 170/s, Control
  70 to 90/s in the background. Your controller reached the link (`PadState` active).
- **Link bug:** at about 10:57:26 Resonant moved RenderAudio to another thread, and two threads
  published at once. The seqlock writer derived its counter from the old value, so one overlap left
  it odd for good and every reader gave up. Fixed: writers are serialized (`TryWrite` / a busy flag,
  skip on contention), and the writer forces parity, so a stuck counter heals on the next write.
  `link_test` covers it (two writers, stuck-odd start).
- **Jesse stayed 0:** Control only calls `getAvatarObject` while it has focus, so the cached transform
  went stale behind Resonant. Now the frame thread samples `getTransform` from the cached entity, after
  checking its vtable is unchanged.
- **Dylan not tracked:** 62 controllers, none within 3 m of the emitter chosen as reference. The
  selection is now the controller nearest the Wwise listener (within 10 m), logging the three
  nearest.
- **Resonant camera not searched:** the "camera still" trigger (5 cm) never fired. It is now 25 cm,
  with a 30 s fallback and a re-search whenever the tracked view dies.
- After alt-tabbing back into Control, your controller stopped working there; keyboard was fine,
  and Resonant always kept the controller. Likely Steam Input routing the pad to the focused Steam
  game. Control's input will come from the virtual pad (next).

## Phase 1: virtual pad + fake focus (built 2026-10-11)

- `src/probe_control/vpad.h`: hooks `XInputGetState` in Control's xinput module (`xinput1_4` /
  `1_3` / `9_1_0`). While the host is alive and its pad active, index 0 returns Resonant's controller;
  otherwise every call passes through. The probe's own pad sampler is routed to the trampoline.
- Fake focus: `GameWindow::hasFocus` returns true, and `isMinimized` false, while the host's pad drives
  Jesse. The log still records the real values.
- Switched by `VirtualPad=1` and `FakeFocus=1` in Control's `controlbridge.ini`.
- `tests/vpad_test.cpp` checks this against the real system xinput1_4 and a private mapping: served
  while the host is alive with an active pad; passed through when inactive, when the host is gone, or
  when switched off.

## In-game test 6 (2026-10-11): virtual pad live, but Control paused

- **Virtual pad works:** once Resonant was up, Control's XInputGetState served the host pad (about
  2,000 calls/s) and fake focus switched on.
- **Control was already paused.** The tester alt-tabbed away from Control at 11:12:14 to start
  Resonant, 18 s before the host joined the link. Control saw the focus loss and opened its pause menu.
- **Resonant:** Dylan tracked (controller nearest the listener, 5.4 m, moving with him); camera view
  found (fovY 51.6).
- **How Control learns about focus:** coregame `GameWindow::dispatchAppEvent(app::Event*)` forwards
  window events. `app::Event` starts with an int type, and `app::EventHandler::dispatchAppEvent`
  switches on it (24 types, jump table at app+0x735c). Matched against `app::App`'s handleAppEvent
  overloads (vtable app+0x14e58 via RTTI; slots 25/24/23/21):
  - 0 = CloseEvent
  - 1 = FocusEvent
  - 2 = MinimizeEvent
  - 4 = CharEvent

  The event classes have no RTTI.
- **Fix (`src/probe_control/focus.h`, FakeFocus=1, always on, not only when the host is up):**
  - Focus and Minimize events are swallowed.
  - `hasFocus` / `isMinimized` report true / false, and the window's bytes +0x7C / +0x7D are held there.
  - `ClipCursor` / `SetCursorPos` are ignored while Control isn't really the foreground window, so it
    can't take the mouse from Resonant.

## In-game test 7 (2026-10-11): no pause, but the controller ignored

- Control no longer pauses on alt-tab, and mouse/keyboard work in both games.
- **The controller stopped working in Control** right after the first menu press, and Jesse didn't
  follow the virtual pad (the monitor saw the host pad active in all 421 samples, while Jesse's
  position changed 4 times). Cause: the startup Focus event (type 1, state int at +4 = 1, "gained") was
  swallowed along with the losses, so Control never thought it had focus and ignored gamepad input.
  Now only losses are swallowed (Focus state 0, Minimize state 1).
- A game restart overwrote the log of the session that mattered. The probe now keeps the previous one
  as `<tag>_probe.prev.log`.

### Still open (Phase 0b)

- Resonant player transform in the ECS (not just an emitter): find the `coregame::component::Player`
  entity's transform. Start from what `views` and the emitter give us: scan for the emitter position as
  doubles, then use RTTI on the owning objects.
- Which Control camera is the gameplay camera (auto-pick = most updated in the last 0.25 s; check it).
- Does Control pause or stop rendering when it loses focus? (Phase 1 fake focus.)
- Resonant's FOV, and how the projection is set up with Streamline in the loop.
