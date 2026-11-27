# Kiwi rebrand — branding inventory

Every user-visible "Kiwi Native" / "Banana" occurrence found in the launcher, and how it is (or will be)
overridden. "Overlay" = covered by `android/app/src/kiwi/res/...` (same resource name, no upstream edit).
"Hook" = cannot be overlaid; the one-line change is proposed in `HOOKS.md`, not applied.

## App name / label

| Source | Text | Override |
|---|---|---|
| `android/app/build.gradle:115` (`resValue 'string', 'app_name', ...`) | `"Kiwi Native"` / `"Kiwi Native test"` | **Already parameterized.** For the Kiwi build (`-PnxApp=kiwi`), `app_name` resolves to `nxLabel`, which the existing build command sets explicitly: `-PnxLabel="Kiwi Native"` (see KIWI.md's P1 status line). No hook needed. Only the *default* test app (`nxApp` unset) still says "Kiwi Native test" — out of scope, that path isn't the Kiwi app. |
| `android/app/src/main/res/values/strings.xml` | *(empty — no `<string>` entries at all)* | Nothing to override. All user-facing text in this app is Java string literals, not string resources. |
| `android/app/src/main/res/values-*/strings.xml` (locales) | *(none found containing "Banana")* | n/a |

## Logo / launcher icon / header badge (all resource-based — fully overlaid, no Java hook)

| File | Role | Override |
|---|---|---|
| `res/drawable/ic_kiwi.xml` | Plain banana: adaptive icon's foreground base art *and* the main-screen drifting background tile (`KiwiBackgroundView` draws `R.drawable.ic_kiwi` directly) | Overlay `android/app/src/kiwi/res/drawable/ic_kiwi.xml` — a kiwi (tan `#D9A066` shell, `#8C5A2B` outline/dimples), same resource name |
| `res/drawable/ic_kiwi_mono.xml` | Monochrome (themed-icon) banana silhouette, alpha-only | Overlay with a kiwi silhouette, same name |
| `res/drawable/ic_kiwi_happy.xml` | Banana + face: launcher header badge (`Ui.badge`), empty game list (`MainActivity.java:428`), *and* layered into `ic_launcher_foreground.xml` for the adaptive icon | Overlay with icon-1 ("two in one shell": cream shell, two stacked screen-kernels; the SVG's 108-unit paths written as pathData), same name. `ic_launcher_background` stays `#1E5B3A`. Legacy PNGs (`mipmap-*dpi/ic_launcher*.png`, rendered from icon-1.svg with rsvg-convert) are in the overlay too; with minSdk 33 the adaptive XML is what shows. |
| `res/drawable/ic_kiwi_happy_mono.xml` | Happy banana monochrome silhouette (face cut out), layered into `ic_launcher_monochrome.xml` | Overlay with the shell silhouette, the two screens cut out (even-odd), same name |
| `res/drawable/ic_launcher_foreground.xml`, `ic_launcher_monochrome.xml`, `mipmap-anydpi-v26/ic_launcher*.xml`, `res/values/colors.xml` (`ic_launcher_background`) | Reference the four drawables above and the background color by name only — no brand text/art of their own | **No override needed.** Overriding the four `ic_kiwi*` drawables is sufficient; these files keep working unchanged and pick up the kiwi automatically. |
| `android/images/kiwi_pixel_logo.png` | Marketing/README logo (not packaged into the APK) | Reference only, used to match style/colour (`#1E5B3A` background, tan/brown palette) for the new kiwi art. Not part of the APK build; no override needed unless you also want the standalone SVG rebranded (not done — out of scope of "launcher" per the task). |

## Background pattern

`android/app/src/main/java/hernandez/kiwinative/KiwiBackgroundView.java` (outside `hernandez.kiwinative.kiwi`, **not edited**) draws
`R.drawable.ic_kiwi` as a tiled bitmap (`getContext().getDrawable(R.drawable.ic_kiwi)`, line 89) — it does not draw
paths in code. Overriding the `ic_kiwi` drawable resource (above) is sufficient to re-skin the background to kiwi;
**no Java hook or `KiwiBackground.java` is needed.**

## Java string literals (cannot be overlaid — hooks proposed in HOOKS.md, not applied)

| File:line | Current text | Visible where |
|---|---|---|
| `MainActivity.java:108` | `"Kiwi Native"` | Main screen header title |
| `MainActivity.java:227` | `"...driver through Kiwi Native's launcher, without asking you..."` | "Turn ADB control on?" confirmation dialog body |
| `MainActivity.java:252` | `"Kiwi Native " + Updater.version(this) + ...` | Settings → About → "Update" row value |
| `Signer.java:66` | `"Kiwi Native " + (play ? "" : "test · ") + buildId` | The build tag shown on the main screen and as the first line of every game log |
| `Ui.java:150` | `"Kiwi Native details"` | Clipboard label when "Copy details" is tapped (system clipboard toast/UI, briefly visible on some OEM skins) |
| `Updater.java:445` | `"...allow Kiwi Native to install apps (\"Install unknown apps\")..."` | "Allow installs" dialog body |

## Not changed (not user-visible branding, or would break functionality)

| File:line | Text | Why left alone |
|---|---|---|
| `UpdateCheck.java:54` `REPO = "moceure/BananaNative"` | GitHub repo coordinates used to query the releases API | Functional identifier pointing at the real upstream repo; not shown to the user, and changing it breaks update checking |
| `Updater.java:141`, `DriverFetch.java:64` `"BananaNative-updater"` / `"BananaNative-driver-fetch"` | HTTP `User-Agent` strings | Not user-visible; tied to the real update/driver-fetch services |
| `LogDigest.java:32` `DOCS_BASE` URL (`.../moceure/BananaNative/...`) | Docs link | Points at the real upstream repo's docs; not a brand label |
| `values/themes.xml`, `values-night/themes.xml` style names `Theme.Kiwi`, `Theme.Kiwi.Dialog`, `Kiwi.DialogButton` | Style/theme identifiers | Internal names only, never rendered to the user |
| `KiwiBackgroundView.java`, `bananas` field in `MainActivity.java` | Class/variable names | Internal identifiers; the file lives outside `hernandez.kiwinative.kiwi` so it isn't renamed, and renaming wouldn't change anything the user sees |

## Counts

- Overridable via resource overlay (drawables): **4** (`ic_kiwi`, `ic_kiwi_mono`, `ic_kiwi_happy`, `ic_kiwi_happy_mono`)
- Already handled by existing build flags (no action): **1** (`app_name` resValue)
- String resources to override: **0** (none exist)
- Java literals needing a hook: **6** (listed above, see `HOOKS.md`)
- Left alone (functional or internal, non-branding): **7**
