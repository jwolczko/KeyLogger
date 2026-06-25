# WZC - Cybersecurity Course Project

This repository contains a Windows desktop project prepared for the "Selected Topics in Cybersecurity" course. The solution demonstrates a visible WinAPI game application together with a background component that records keyboard input for educational analysis of Windows hooks, persistence mechanisms, buffering, and file output.

The project is intended only for controlled laboratory use, on systems where every user has given explicit consent.

## Technology Stack

- C++ with the Microsoft Visual C++ toolchain.
- WinAPI for window creation, message handling, drawing, timers, keyboard input, hidden windows, and low-level keyboard hooks.
- GDI for rendering the game board, pieces, score, and UI text.
- C++ Standard Library components such as `std::wstring`, `std::vector`, `std::mutex`, `std::condition_variable`, `std::atomic`, streams, and time utilities.
- Windows Shell APIs for resolving the Desktop directory and building output paths.
- MSBuild and Visual Studio solution/project files.
- WiX Toolset for building an MSI installer package.

## Solution Structure

```text
src/
  Surprise.sln
  Game/
    Game.cpp
    Game.vcxproj
    Game.rc
    Resource.h
    Game.ico
    small.ico
  Surprise/
    Surprise.cpp
    Surprise.vcxproj
  GameSetup/
    GameSetup.wixproj
    Package.wxs
    Package.en-us.wxl
```

## Components

### Game

`Game` is a graphical Win32 application that implements a simple Tetris-style game. It creates a desktop window, registers a custom window class, handles keyboard controls through the WinAPI message loop, and draws the board using GDI.

Main responsibilities:

- Maintains the game board state, current piece, score, and game-over state.
- Uses a timer to move pieces down at a fixed interval.
- Handles arrow keys and space for movement, rotation, faster drop, and hard drop.
- Clears completed lines and updates the score.
- Uses double buffering with a memory device context to reduce flickering during repainting.
- Stores application resources such as icons and resource definitions in the project.

### Surprise

`Surprise` is a background Win32 component used to demonstrate keyboard monitoring concepts in a cybersecurity lab context. It creates a hidden message-only style workflow through an invisible window, installs a low-level keyboard hook, converts virtual key codes to Unicode text, buffers captured characters, and periodically writes buffered data to a file on the user's Desktop.

Main responsibilities:

- Registers a hidden window class and runs a standard Windows message loop.
- Installs a `WH_KEYBOARD_LL` hook with `SetWindowsHookExW`.
- Converts keyboard input with layout-aware WinAPI functions such as `GetKeyboardState`, `GetKeyboardLayout`, `MapVirtualKeyEx`, and `ToUnicodeEx`.
- Tracks modifier keys including Shift, Caps Lock, and right Alt/AltGr.
- Uses worker threads for periodic file writes and idle spacing.
- Protects shared buffers with `std::mutex` and coordinates worker activity with `std::condition_variable`.
- Writes output to `suprise_log.txt` on the Desktop.
- Supports graceful shutdown with `Ctrl + Shift + Q`.

### GameSetup

`GameSetup` is a WiX installer project that packages the built executables and runtime files into an MSI package named `TheGame`.

Main responsibilities:

- Installs application files under `Program Files`.
- Creates a Start Menu shortcut for `Game.exe`.
- Includes `Surprise.exe` in the installation directory.
- Starts the background component after installation.
- Registers the background component in the current user's `Run` registry key for autostart.

## Build Requirements

- Windows 10 or newer.
- Visual Studio 2022 with the Desktop development with C++ workload.
- MSVC platform toolset `v143`.
- Windows 10 SDK.
- WiX Toolset v4 for the installer project.

## Building

Open `src/Surprise.sln` in Visual Studio, select the desired configuration and platform, then build the solution.

Common configurations:

- `Debug|x64`
- `Release|x64`
- `Debug|Win32`
- `Release|Win32`

The `Game` and `Surprise` projects are standard C++ application projects. The `GameSetup` project depends on WiX support being installed in the development environment.

## Runtime Behavior

- `Game.exe` opens a visible Tetris-style game window.
- `Surprise.exe` runs without showing a visible application window.
- Captured text is buffered in memory and periodically flushed to `suprise_log.txt` on the Desktop.
- The installer can configure `Surprise.exe` to start automatically for the current Windows user.

## Ethical and Legal Notice

This project demonstrates techniques that can be sensitive in real environments, especially keyboard hooks and autostart configuration. Use it only for coursework, research, or defensive education in an isolated and authorized environment. Do not deploy or run it on systems without clear permission from the owner and users of the system.
