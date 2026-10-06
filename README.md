# OBS De-Echo

An OBS Studio plugin for Windows that captures all desktop audio except one application.

It adds an audio source called **Desktop Audio (Exclude App)**. By default it leaves out Discord, so viewers hear your game, music and everything else, but not your voice chat.

## Requirements

- Windows 10 build 20348 or newer, or Windows 11
- OBS Studio 31.1 or newer, 64-bit

## Install

1. Download the `windows-x64` zip from [Releases](https://github.com/adman234/obs-de-echo/releases), or from the latest run under [Actions](https://github.com/adman234/obs-de-echo/actions).
2. Close OBS.
3. Extract the zip into `C:\ProgramData\obs-studio\plugins\` so that you end up with `C:\ProgramData\obs-studio\plugins\obs-de-echo\bin\64bit\obs-de-echo.dll`.
4. Start OBS.

## Setup

1. Open **Settings > Audio** and set every **Desktop Audio** device to **Disabled**. If you skip this, the stock capture still sends Discord to the stream.
2. In your scene, add a source: **+ > Desktop Audio (Exclude App)**.
3. Leave **Application to exclude** on `Discord.exe`, or pick or type another executable name.

Add the source to each scene that needs desktop audio, or put it in one scene and nest that scene in the others.

## Behavior

- The excluded application and all of its child processes are left out, on every output device.
- While the application is not running, the source captures all desktop audio. It switches over within about half a second of the application starting or closing.
- While the application is not running, audio played by OBS itself (for example audio monitoring) is left out instead, which avoids a feedback loop.
- Output is stereo at the OBS sample rate.

## Limits

- One application per source. Windows excludes a single process tree per capture stream, so adding a second source to exclude a second application does not work: each source would still capture the other's application.
- A voice chat running in a browser tab cannot be separated from the rest of that browser.
- While the application is running, OBS audio monitoring on a desktop device is captured, the same as with the stock Desktop Audio source.

## Building

Builds run on GitHub Actions from the [OBS plugin template](https://github.com/obsproject/obs-plugintemplate). To build locally you need Visual Studio 2022 and CMake 3.28 or newer:

```
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
