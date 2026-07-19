# <img width="64" height="64" alt="64x64" src="https://github.com/user-attachments/assets/bfa0f32f-ecb4-4e21-975f-7ab867ed68be" /> HSMAdvisor Plugin for Fusion 360

An unofficial, third-party Fusion 360 plugin that brings
[HSMAdvisor](https://hsmadvisor.com/)'s feeds and speeds calculator into the
Manufacture workspace. Pick a CAM operation, open HSMAdvisor pre-filled with that
tool, and apply the result back to the operation. Similar in spirit to
*HSMAdvisor for Mastercam*.

> **Unofficial plugin.** Made by UnperfektLab. Not supported by HSMAdvisor / Eldar Gerfanov. 
> It only drives your own separately installed, licensed copy of HSMAdvisor through its official DLL SDK.
>
> **Support and feedback:** report a bug in
> [Issues](https://github.com/UnperfektLab/HSMAdvisor-Plugin-for-Fusion-360/issues),
> ask a question or share an idea in
> [Discussions](https://github.com/UnperfektLab/HSMAdvisor-Plugin-for-Fusion-360/discussions),
> or [send an email](mailto:unperfektlab+hsmforfusion@gmail.com).

<p align="center">
  <a href="https://github.com/UnperfektLab/HSMAdvisor-Plugin-for-Fusion-360/releases/latest/download/HSMAdvisor-Plugin-for-Fusion360-Setup.exe">
    <img src="https://img.shields.io/badge/Download-install%20latest-2ea44f?style=for-the-badge" alt="Download the latest installer">
  </a>
  &nbsp;&nbsp;
  <a href="https://github.com/UnperfektLab/HSMAdvisor-Plugin-for-Fusion-360/releases/latest">
    <img src="https://img.shields.io/github/v/release/UnperfektLab/HSMAdvisor-Plugin-for-Fusion-360?style=for-the-badge&label=version&color=blue" alt="Latest release">
  </a>
</p>

<p align="center"><b>Download → run the installer → restart Fusion 360.</b></p>

<img width="700" height="394" alt="HSM" src="https://github.com/user-attachments/assets/8b188a5c-61db-42d0-a51f-7cbe7e6f5a48" />

## Requirements

- Windows 10 (1903+) or Windows 11, 64-bit.
- Autodesk Fusion 360.
- HSMAdvisor, installed and licensed (<https://hsmadvisor.com/download>). Run it once
  so its database is set up.
- .NET Framework 4.8 (ships with Windows 10 1903+ and Windows 11).

## Install

Click the button above to download the installer and run it. It checks for HSMAdvisor,
copies the add-in into Fusion's add-ins folder, and enables it. Restart Fusion 360; the
**HSMAdvisor Plugin** button appears in the Manufacture workspace, Manage panel
(Milling and Turning tabs).

SmartScreen may warn about an unknown publisher (the installer is not code-signed).
Click **More info → Run anyway**.

Manual install: copy the `HSMAdvisor Plugin` folder into

```
%APPDATA%\Autodesk\Autodesk Fusion 360\API\AddIns\
```

then in Fusion press **Shift+S → Add-Ins → HSMAdvisor Plugin → Run** (tick *Run on Startup*).

## Usage

1. In the Manufacture workspace, select a CAM operation.
2. Click **HSMAdvisor Plugin** in the Manage panel.
3. Choose material and machine in the HSMAdvisor window, click **OK**.
4. Tick which values to apply, click **OK**, then regenerate the toolpath.

You can apply spindle RPM, feedrate, depth of cut (ad), width of cut (ae), or all. If
the tool is found in your HSMAdvisor database (by description or product id), its saved
data is loaded. The last material, machine, and options are remembered in the plugin's
own settings file. Fusion stays responsive while the window is open. The first open in
a session loads the database once (a few seconds); later opens are instant.

## Troubleshooting

- **"HSMAdvisor could not be started."** HSMAdvisor is not installed or licensed.
  Install it, run it once, then retry.
- **No button.** Enable the add-in: **Shift+S → Add-Ins → HSMAdvisor Plugin → Run**.
  It lives in the Manage panel of the Milling and Turning tabs.
- **"Could not read a valid tool diameter."** Select a milling or drilling operation
  with a tool assigned. Turning tools are not supported.
- **No change on the toolpath.** Regenerate the operation after applying.
- Depth of cut writes the value but never toggles *Multiple Depths*; that stays your choice.

## Build from source

Requires Visual Studio 2022/2026 with the **Desktop development with C++** workload
(MSVC, Windows SDK, and the bundled Roslyn C# compiler), plus HSMAdvisor and Fusion 360
installed. From a PowerShell prompt in the repo root:

```powershell
.\build.ps1              # Release build to "dist\HSMAdvisor Plugin"
.\build.ps1 -Package     # also build the Inno Setup installer
```

`build.ps1` locates Visual Studio and your HSMAdvisor installation automatically.

## How it works

The native add-in (`HSMAdvisor Plugin.dll`) launches a separate host process
(`HSMAdvisorPluginHost.exe`) that shows the HSMAdvisor dialog. Running it out of process
keeps the controls from overlapping (a DPI issue when the dialog runs inside Fusion) and
keeps Fusion responsive; the two talk over a named pipe and the result is applied on the
main thread. The host loads HSMAdvisor's engine DLLs from your installation, so your
HSMAdvisor stays independently updatable and is never modified. Settings live in the
plugin's own `settings.xml`.

## License

MIT (see `LICENSE`). This is an unofficial, independent add-in, not affiliated with or
supported by HSMAdvisor / Eldar Gerfanov, it requires a separately installed, licensed
copy of HSMAdvisor.

## Credits

- HSMAdvisor © Eldar Gerfanov, HSMAdvisor Inc., <https://hsmadvisor.com/>.
- HSMAdvisor DLL SDK and demo: <https://github.com/swindex/HSMAdvisor-DLL-Test>.
