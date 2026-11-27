# Porting a game to Kiwi Native

Kiwi Native is not an emulator. A "port" is a **game profile** plus a **pack** made from your own dump: the profile says what the game's files and builds are, the pack is the rewritten code and translated shaders the phone runs. This page is the whole path, in two cases:

- **[A. The game already has a profile](#a-the-game-already-has-a-profile)** (it is in the [games table](../dm/games/README.md#games)): nothing to port, you build it and play.
- **[B. The game has no profile](#b-the-game-has-no-profile)**: you write the profile from the game's files, then bring the game up.

Only Switch 1 games are carried (`kiwi/upstream-dropped.txt`). A 32-bit game cannot run. Keep every game file, and everything made from it, outside the repository, in a work folder beside the dump (`<work dir>` below). Never commit or share a game file, a pack, a `.nup`, a key or a log with game data. Kiwi Native only runs dumps made from your own console and never asks for keys or says where to get games.

## What you need

| For | You need |
|---|---|
| Playing a game that has a profile | The APK, your own `prod.keys`, your own firmware dump, your own game dump (NSP, XCI, NSZ, XCZ, a dump folder or a `.nup`) with its update |
| Writing a profile | The above, plus a computer: Python 3.14 with `lz4`, `capstone`, `fonttools` (`android/environment.yml`), .NET 10, `spirv-val`, `adb` |
| Building the APK yourself | Nothing local: run the **apk** workflow in the Actions tab (engine `real`), or follow [android/README.md](../android/README.md#build-the-apk) |

## A. The game already has a profile

1. **Install the APK** and allow *All files access* when asked. (Built by the **apk** workflow: Actions, *apk*, *Run workflow*, engine `real`, then download the artifact.)
2. **Add your files** on the first screen: **Keys** (`prod.keys`), **Firmware** (a folder or `.zip`) and the **Game folder**. Both keys and firmware are needed before the first build.
3. **Open the game** in the library. If your update differs from the one in the table, the game's page says so and the app asks before building.
4. **Tap Build.** The app imports the game, rewrites its code, translates its shaders and installs its companion. Most games take 5 to 40 minutes on an AYN Thor.
5. **Play.** On a dual-screen handheld the companion starts on the bottom screen. Hold the cover for the game's page: picture and speed settings, mods and cheats, saves, storage.
6. After an app update, rebuild: hold the cover, **Storage**, **Build again**, **Rebuild**.

To build on a computer instead and push to the phone: `kiwi/scripts/kiwi-build.command|sh|bat "<game>" --keys ~/prod.keys --push` ([BUILDER.md](../kiwi/docs/BUILDER.md)).

If something fails, read the game's note (`dm/games/<title ID>/README.md`) and [docs/agents/symptoms.md](agents/symptoms.md), and share the log with **Share build log**.

## B. The game has no profile

The app stops at "no profile for `<title ID>`". A profile has to be in the APK (`assets/profiles/<title ID>.profile.bin`), so a new game takes a new APK. The long form of this is [kiwi/docs/NEW-GAME.md](../kiwi/docs/NEW-GAME.md); here are the steps in order.

### 1. Check that it is worth trying

- `ls dm/games/<title ID>` : if the folder exists, you are in case A.
- A 32-bit game stops at the survey (step 3). A game whose shaders the tools cannot read (a closed archive format, shaders compiled while it runs, none in the files) needs extra work first: [the three kinds](../kiwi/docs/NEW-GAME.md#games-the-pc-split-cannot-read).
- Compare with a game of the same engine or SDK: `grep` the notes in `dm/games/*/README.md`. A game on an engine Kiwi Native already runs is mostly automatic; a new engine can take days.

### 2. Make a `.nup` of the game

The `.nup` is the single file the phone plays and the input of the pack step.

```sh
kiwi/.build/nximport/nximport_cli --keys <prod.keys> nup "<work dir>/<Name> [<TITLE ID>][v<version>].nup" <title id in lower case> <base .nsp> [<update .nsp> ...]
kiwi/.build/nximport/nximport_cli --keys <prod.keys> icon <base .nsp> <title id in lower case> "<work dir>/<Name> [<TITLE ID>][v<version>].png"
```

`kiwi/scripts/kiwi-build.* "<files>" --dry-run` builds `nximport_cli` first if it is missing.

### 3. Survey the game

```sh
python kiwi/scripts/nup-extract.py "<work dir>/<Name>.nup" "<work dir>/dump"
python scripts/build_ipa_from_nsp.py "<work dir>/dump" --survey-only
```

Read `<work dir>/dump/<TITLE ID>_survey/survey.md`: modules and build IDs, SDK, engine, graphics API, shader containers, the rewrite-site census and any `[RED]` line. If the dump does not fit on the disk, extract only the heads (`--heads`) as [NEW-GAME.md](../kiwi/docs/NEW-GAME.md#2-a-dump-folder-and-the-survey) describes.

### 4. Write the provisional profile and split the shaders

```sh
python scripts/build_ipa_from_nsp.py "<work dir>/dump"          # add --platform mac on a Mac
P="<work dir>/dump/<TITLE ID>_build/profile/<TITLE ID>"
DM_GAMES_DIR="<work dir>/dump/<TITLE ID>_build/profile" DM_GAME=<TITLE ID> \
python dm/gfx/tools/shader_split.py "<work dir>/dump/base/romfs" "<work dir>/dump/base/exefs/main" "<work dir>/split/pkg" \
    "$P/shader_map.tsv" "$P/shader_facts.tsv" "$P/shader_pairs.tsv" "$P/pso_pairs.tsv"
```

- `dm/games/profile_guess.py` fills `game.tsv` from the files (identity, build rows, modules, memory layout, shader groups), each key under the reason for it. Nothing is decided by a short run; `render_cond`, `tex_flush_upload`, `gpu_timestamps`, `present_interval` and the rest keep their defaults.
- The script then stops at the first thing a phone build does not need and your computer lacks (on a Mac without Metal, "the Metal compiler does not run"): that is fine. Run the split by hand as above, or `shaders: 0` and an empty `shader_map.tsv` follow.
- The split's last line counts the shaders. **Zero means the files do not give them in a form the tool reads**; stop and read [04-graphics.md](agents/04-graphics.md).
- A game with updates takes comma-separated `<romfs>` and `<main>` lists, one per version.

### 5. Put the profile in the repository

```sh
mkdir dm/games/<TITLE ID> && cp "$P"/*.tsv dm/games/<TITLE ID>/
```

Then, in this order:

1. In `game.tsv` set `name`, `ipa`, the `build` row's display version and `shader_note`; replace the header comments with what the profile is and add `# KIWI: ...`. Leave the shader keys and tables as the tools wrote them (their bytes are in the shader library's stamp).
2. Write the game's note `dm/games/<TITLE ID>/README.md` (status, builds, shaders, memory, found on this game, open), starting with `<!-- KIWI: ... -->`, and add its row to the games table in [dm/games/README.md](../dm/games/README.md#games).
3. Add the title ID to `kiwi/release-titles.txt` (the games the APK builds hosts for by default; `-PnxGames=<id>` builds just this one) and give it a row in `kiwi/games.tsv` if its name is in capitals.
4. List the generated tables in `kiwi/upstream-unmarked.txt` and add a row to `kiwi/UPSTREAM-EDITS.md`.
5. Check: `python dm/tools/tests/test_docs.py` and `bash kiwi/scripts/check-profiles.sh`.

### 6. Pack the game

```sh
python scripts/pack_android.py "<work dir>/<Name>.nup" --shader-pkg "<work dir>/split/pkg"
```

Pack from the `.nup`, not from the extracted dump. The first run translates every shader (tens of thousands take about 10 minutes); a later run with the same inputs takes seconds. The end of the log must show `every one valid for spirv-val`, no `[RED]` line, and `copying the shader library into the pack: <n> of <n> MB` with `n` above 0.

### 7. Build a test app that carries the profile

Build under another app id so it installs beside your real app and leaves its saves alone:

```sh
cd android/app
./gradlew apk -PnxApp=kiwi -PnxAppId=kiwitest -PnxLabel="Kiwi test" -PnxGames=<TITLE ID> -PnxPackEngine=fake
adb install -r out/nxnative-android-kiwi.apk      # hernandez.kiwinative.kiwitest
```

`-PnxPackEngine=fake` leaves out the on-device packer (the pack comes from step 6). To carry the real packer, run `kiwi/scripts/ondevice-stage.sh <work dir>` and pass `-PnxPackStage=<work dir>/stage`, or let the **apk** workflow do both on GitHub. Put the packer's Python first on `PATH` for Gradle, or `:nxGenericProfiles` fails with a Python syntax error.

### 8. Test on the device

```sh
python kiwi/scripts/push-pack.py "<work dir>/<TITLE ID>-work/android-pack" --game "<work dir>/<Name>.nup" --dest "/sdcard/Kiwi Roms/<Name>"
```

In the test app: set **Keys**, **Firmware** (the test app is a separate app: nothing carries over, and without the firmware's shared fonts some games crash at startup) and the game folder. Start the game **from the library**, not with an `am start` intent, so its environment lines (`DM_SHARED_FONT`, `PN_BUILD_ID`) are written. Play about 20 seconds past the title on the system driver, then on a Turnip driver, then force-stop the app. Never delete or overwrite a save folder, and never install a local build over your real app.

### 9. Bring-up: when the game does not reach its frame loop

The loop is: patch, build the host, run, read the log, fix one cause, repeat ([03-bringup.md](agents/03-bringup.md)).

```sh
python3 dm/tools/patch_nso.py <ExeFS>/main <build dir>/patched main --target android
make -C dm/host GAME=<TITLE ID> dmhost
```

- Read the log by stage ([android/TESTING.md](../android/TESTING.md#what-the-log-says-at-each-stage)): `>>> nnosInitialize` and `>>> nnMain` are boot stages, `[RED]` lines are something unimplemented or wrong (`grep -aF "[RED]" log | sort | uniq -c`), `CRASH sig=...` starts a crash report. Start with `python3 scripts/issue_digest.py` or `log_digest.py` on the log, then search [docs/agents/symptoms.md](agents/symptoms.md) for the text you see.
- **Find the root cause and fix it there**: the faulting instruction, the value that went wrong, the Horizon / NVN / Vulkan behaviour the host gets wrong. A stub, a skipped call, a clamp or a longer timeout hides the symptom; use one only when the cause lies where it cannot be fixed, and say so in the code comment and the game note.
- Prefer a general fix (one that holds for every game) to a game rule. A game-specific rule (a profile key, a rewriter cut, a build-ID gate) is the last resort, and is keyed by the NSO build ID (`patches.py` in the profile, `dm/tools/sdk_patches.py` for Nintendo's modules); fixed addresses never run on a build they were not found in.
- Rendering wrong or missing: [android/SHADERS.md](../android/SHADERS.md), [04-graphics.md](agents/04-graphics.md), the [rendering](reference/rendering.md), [shaders](reference/shaders.md) and [NVN objects](reference/nvn-objects.md) references.
- A change to the shader chain tools (`dm/gfx/tools/dmsh_*`, `vk_android_*`) needs the chain header regenerated and every profile's libraries rebuilt (`dmsh_chain.py --header`, `vk_android_build.py --header`), as AGENTS.md describes.

Where a finding goes, once: a fact about one game in its note; a general mechanism in `docs/reference/<subsystem>.md` (Symptom, Cause, Fix, Found on), then `python3 dm/tools/docs_symptoms.py` to update the symptom index.

### 10. Ship it

- Revert any local build flag. Bring source only: no pack, APK, `.nup`, log with game data or key.
- Run `dm/tools/tests/test_docs.py`, `test_android_games.py`, `test_android_gamefonts.py`, `test_android_gamelist.py`, `kiwi/tests/*.py`, `dm/kiwi/profile/gen_runtime_profile.py check-all` and `kiwi/scripts/check-profiles.sh`.
- Do not edit the Supported games table of the README by hand: it is written from `kiwi/games.tsv`, `kiwi/release-titles.txt` and the profiles by `kiwi/scripts/games_catalog.py`.
- Say what ran, on which device and driver, the build ID from the log, and what was not tested.
