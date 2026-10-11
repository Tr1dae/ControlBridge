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

### Still open (Phase 0b)

- Resonant player transform in the ECS (not just an emitter): find the `coregame::component::Player`
  entity's transform. Start from what `views` and the emitter give us: scan for the emitter position as
  doubles, then use RTTI on the owning objects.
- Which Control camera is the gameplay camera (auto-pick = most updated in the last 0.25 s; check it).
- Does Control pause or stop rendering when it loses focus? (Phase 1 fake focus.)
- Resonant's FOV, and how the projection is set up with Streamline in the loop.
