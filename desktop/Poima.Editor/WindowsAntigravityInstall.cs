// SPDX-License-Identifier: Apache-2.0
using System.Security.Cryptography;

namespace Poima.Editor;

internal static class WindowsAntigravityInstall
{
    // Version, URL and checksum come from Google's windows_amd64 installer manifest.
    internal const string PackageUrl = "https://storage.googleapis.com/antigravity-public/antigravity-cli/1.5.13-5999302598852608/windows-x64/cli_windows_x64.exe";
    internal const long DownloadBytes = 236_040_344;
    internal const string Version = "1.5.13";
    internal const string GuideUrl = "https://antigravity.google/docs/getting-started?tab=cli";
    private const string Sha512 = "6b80a17a2bc34a969ed9ba31500a8201b0b4aad3e7fe061374022d161fec8bd7de49b6dc069a6ac67a90632e8671bbe860a7e94309bc5adbc536d27a75a86b58";

    public static WindowsAgentPackage Package { get; } = new("Antigravity", "antigravity", Version, PackageUrl,
        DownloadBytes, Sha512, ".poima-antigravity-installed", "agy.exe",
        new Dictionary<string, long> { ["agy.exe"] = DownloadBytes }, unpack: null, algorithm: HashAlgorithmName.SHA512);
}
