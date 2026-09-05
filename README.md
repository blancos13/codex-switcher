# Codex Account Switcher

**Your Codex accounts, one small window.**

Switch accounts, see how much usage you have left, and know when your limits reset. Built in C++ with Dear ImGui, with a familiar blue interface and a few extra themes to make it yours.

## 🔧 Installation

The macOS and Linux ports are new and awaiting native validation. Signed installers are not available yet; the instructions below build from source.

### Windows

Install [CMake](https://cmake.org/download/) and [Visual Studio 2022](https://visualstudio.microsoft.com/vs/) with **Desktop development with C++**. Open PowerShell in the extracted project folder:

```powershell
cmake --preset windows
cmake --build --preset windows --parallel
.\build\windows\bin\CodexSwitcher.exe
```

### macOS

Install the Xcode command-line tools and dependencies with [Homebrew](https://brew.sh/), then run these commands from the project folder:

```bash
xcode-select --install
brew install cmake ninja glfw openssl@3
cmake --preset macos
cmake --build --preset macos --parallel
open build/macos/bin/CodexSwitcher.app
```

### Linux

On Ubuntu or Debian, install the dependencies and build from the project folder:

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
  libglfw3-dev libgl1-mesa-dev libssl-dev libsecret-1-dev
cmake --preset linux
cmake --build --preset linux --parallel
./build/linux/bin/codex-switcher
```

Use a desktop session with an unlocked Secret Service keyring, such as GNOME Keyring or KWallet. On other distributions, install the equivalent packages.

## Getting started

Click **Add account**, give it a name, and sign in through your browser. Your account appears in the list and its usage is checked automatically.

Drag accounts into your preferred order. Hover a usage bar to see when that limit resets. When you're ready to change accounts, click **Switch** and confirm—save unfinished Codex work first.

Codex installations are detected automatically. If yours is in a custom location, set `CODEX_CLI_PATH`; Linux desktop installations can also use `CODEX_DESKTOP_PATH`. Account switching requires Codex's file-based authentication (`cli_auth_credentials_store = "file"` in its `config.toml`).

## How it works

The switcher talks to your installed Codex App Server over local stdin/stdout. It uses `account/login/start` for browser sign-in, `account/read` for account information, and `account/rateLimits/read` for usage. Codex handles the OpenAI network requests. This project has no account server or analytics.

Saved sessions are protected locally using Windows DPAPI, or encryption backed by macOS Keychain / Linux Secret Service. Switching backs up the current sign-in, changes the active account and reopens a detected Codex desktop app. With a CLI-only setup, reopen Codex from your terminal.

## ✨ Features

- Multiple accounts, drag-to-reorder, and one-click switching with confirmation.
- Usage bars with separate reset countdowns and automatic refresh every five minutes.
- Browser sign-in with a cancel button that actually stops the wait.
- Seven ImGui themes, three fonts, and matching icons.
- Local session storage. No subscription or project-owned backend.

Independent community project. Not affiliated with OpenAI. [MIT License](LICENSE).
