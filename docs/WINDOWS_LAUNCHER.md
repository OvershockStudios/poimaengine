# Windows launcher

Open `Poima.Launcher.exe` on Windows x64. Choose an install location and a
release channel, then select **Check and install**. If you already have the
engine ZIP and its matching `.manifest.json` from Poima, keep them side by
side and choose **Install from ZIP…**. The launcher checks the ZIP and every
extracted file before activating it. **Open Poima** takes you to the
installed creator workspace. The launcher includes its own runtime.
Projects, authoring tools and agent sign-in follow the [Windows engine guide](WINDOWS_PREVIEW.md).

**Preview** selects tagged `0.x.x`, alpha, beta and release candidate builds.
**Stable** selects final builds from `1.0.0` onward. After the first install,
the launcher checks the selected channel on startup. Checking an installed
version requires the release list only. Installing a new version verifies
the archive and every file against the release inventory before switching
the active version. The previous engine remains available if an update
fails or is cancelled.

**Use previous version** switches back to the prior install. An automatic
check skips the version you just rolled back from. Use **Check and install**
to reselect it. **Open project…** remembers the engine version used with
that project manifest path. The matching install must remain on disk to
open it through the launcher.

Run `Poima.Launcher.exe --portable` from a writable folder to put launcher
settings, default engine installs, and the new-project folder in
`PoimaData` beside the executable. This can be used for a separate preview
installation.

## Prepare release assets

From the configured WSL checkout, use the pinned WSL .NET SDK, Windows x64
.NET SDK and C++ build tools. Supply the official embedded Python archive
used by the [Windows preview](DESKTOP_EDITOR.md#build-and-launch).

```sh
python3 scripts/build_windows_release.py \
  --windows-dotnet '/mnt/c/Program Files/dotnet/dotnet.exe' \
  --python-embed /path/to/python-3.13.17-embed-amd64.zip \
  --work build/windows-release-work --output build/windows-release-assets
```

The command builds the engine and authoring tools, compiles Collection Room
from its tracked source, then assembles the engine ZIP, its companion inventory
and the self-contained launcher ZIP. The local `release.json` identifies
the tag, archive hashes and sample build. Use new work and output directories.
During an incremental build, `--skip-native` uses the current native targets
from the configured Windows build. ZIP assembly gives the same bytes for the
same inputs and ZIP tooling, independent of file timestamps.

Attach the three named assets to the same GitHub release under the tag in
`release.json`. Mark preview releases as prereleases. The launcher reads
the public release list for `OvershockStudios/poimaengine` and uses the
highest matching version with the engine ZIP and inventory uploaded. The
launcher checks the GitHub SHA-256 digests for that pair.
[GitHub Releases API](https://docs.github.com/en/rest/releases/releases) ·
[Release asset integrity](https://docs.github.com/en/rest/releases/assets)

The launcher ZIP includes Poima's license, the wordmark font notice and
notices from the actual .NET runtime packs selected for the launcher.

## Qualification

Run the installer regression with the original 0.0.296 and 0.0.297 ZIPs
and their matching inventories in one directory. Use an empty install root.

```powershell
dotnet run --project tests/windows_launcher -c Release -- D:\ReleaseAssets "D:\Empty Test Install"
```

The Windows test drives the production installer with a controlled HTTP feed
and starts each installed native engine to read its version. To test the
local route, pass `--local` followed by two original Windows engine ZIPs
with their companion inventories, then a new install root. To use the
actual launcher and create a project from a blank portable installation,
run the Windows UI Automation check in an interactive session.

```powershell
dotnet run --project tests/windows_launcher -c Release -- --local D:\Releases\Poima-0.0.306-Windows-x64.zip D:\Releases\Poima-0.0.307-Windows-x64.zip "D:\Empty Local Test"
.\tests\windows_launcher_workspace.ps1 -Launcher D:\Releases\Poima.Launcher.exe -EngineZip D:\Releases\Poima-0.0.307-Windows-x64.zip -Output "D:\Launcher Workspace Test"
```

Qualification with a public release, a fresh Windows machine and a first-time
creator remains open.
[0.0.298 HTTP installation](evidence/m2-windows-launcher-298.json),
[0.0.308 local installation and creator tests](evidence/m2-launcher-local-install-308.json),
[0.0.309 release build and packaged sample](evidence/m2-windows-release-set-309.json).
