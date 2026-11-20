# Windows launcher

Open `Poima.Launcher.exe` on Windows x64. Choose an install location and a
release channel, then select **Check and install**. **Open Poima** takes you
to the installed creator workspace. The launcher includes its own runtime.
Projects, authoring tools and agent sign-in follow the
[Windows engine guide](WINDOWS_PREVIEW.md).

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

## Prepare release assets

Build the [engine ZIP](DESKTOP_EDITOR.md#build-and-launch), then
generate its companion inventory.

```powershell
python scripts/windows_release_inventory.py D:\Releases\Poima-0.0.298-Windows-x64.zip
```

Attach the ZIP and `Poima-0.0.298-Windows-x64.manifest.json` to the same
GitHub release under canonical tag `v0.0.298`. Mark preview releases as
prereleases. The launcher reads the public release list for
`OvershockStudios/poimaengine` and uses the highest matching version with
both assets uploaded. Both assets must carry their GitHub SHA-256 digests.
[GitHub Releases API](https://docs.github.com/en/rest/releases/releases) ·
[Release asset integrity](https://docs.github.com/en/rest/releases/assets)

Publish the independent launcher on Windows with the .NET 10 SDK.

```powershell
dotnet publish desktop/Poima.Launcher -c Release -p:Version=0.0.298 -o build/launcher
```

Distribute the resulting executable with the repository license, the
wordmark font notice in `docs/branding/Playball-OFL.txt`, and the license
and notices from the .NET runtime packs selected by the publish.

## Qualification

Run the installer regression with the original 0.0.296 and 0.0.297 ZIPs
and their matching inventories in one directory. Use an empty install root.

```powershell
dotnet run --project tests/windows_launcher -c Release -- D:\ReleaseAssets "D:\Empty Test Install"
```

The Windows test drives the production installer with a controlled HTTP feed
and starts each installed native engine to read its version. The standalone
launcher received a separate Windows UI smoke check. A tagged public
installation, a clean-machine creator session, and the toolchain and agent
setup on that machine remain to be qualified.
[0.0.298 results](evidence/m2-windows-launcher-298.json)
