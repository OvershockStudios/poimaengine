// SPDX-License-Identifier: Apache-2.0
namespace Poima.Launcher;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();
        Application.SetColorMode(SystemColorMode.Dark);
        using var window = new LauncherForm();
        Application.Run(window);
    }
}
