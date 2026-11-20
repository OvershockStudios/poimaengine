// SPDX-License-Identifier: Apache-2.0
namespace Poima.Launcher;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        Application.SetColorMode(SystemColorMode.Dark);
        var portable = args is ["--portable"];
        if (args.Length > 0 && !portable)
        {
            MessageBox.Show("Use Poima.Launcher.exe with the optional --portable argument.", "Poima Launcher");
            return;
        }
        using var window = new LauncherForm(portable);
        Application.Run(window);
    }
}
