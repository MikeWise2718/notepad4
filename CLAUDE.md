# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Notepad4 is a lightweight Scintilla-based text editor for Windows with syntax highlighting, code folding, and auto-completion. It's a modern C++ rewrite of Notepad2/Notepad2-mod. matepath is a companion file browser plugin.

## Build Commands

### Visual Studio (MSVC) - Primary Method
```batch
# From repository root, builds all architectures in Release
build\VisualStudio\build.bat

# Specific architecture and configuration
build\VisualStudio\build.bat Build x64 Release
build\VisualStudio\build.bat Build Win32 Release
build\VisualStudio\build.bat Build AVX2 Release
build\VisualStudio\build.bat Build ARM64 Release

# With LLVM/Clang toolset
build\VisualStudio\build.bat Build x64 LLVMRelease

# Clean build
build\VisualStudio\build.bat Rebuild x64 Release
```

### MinGW/GCC
```batch
# From build\mingw directory
build.bat x86_64           # 64-bit GCC
build.bat i686             # 32-bit GCC
build.bat x86_64 Clang     # 64-bit Clang
build.bat llvm aarch64     # ARM64 with llvm-mingw
```

### Direct Makefile (from build\mingw directory)
```bash
mingw32-make TRIPLET=x86_64-w64-mingw32 LTO=1  # GCC 64-bit with LTO
mingw32-make CLANG=1 LTO=1                      # Clang with LTO
mingw32-make clean                              # Clean build artifacts
```

### Building Localized Versions
```batch
locale\build.bat Build x64 Release
```

## Project Architecture

### Directory Structure
- `src/` - Notepad4 main application source
- `matepath/src/` - matepath file browser source
- `scintilla/` - Embedded Scintilla editing component (customized)
  - `lexers/` - Syntax highlighting lexers (Lex*.cxx)
  - `lexlib/` - Lexer support library
  - `src/` - Core Scintilla source
- `build/` - Build scripts and project files
  - `VisualStudio/` - VS solution and vcxproj files
  - `mingw/` - Makefile-based build
- `locale/` - Localization resources
- `tools/` - Python utilities for code generation
- `res/` - Application resources (icons, bitmaps)

### Key Source Files
- `src/Notepad4.cpp` - Main application entry and window procedure
- `src/Edit.cpp` - Core editing functionality and Scintilla integration
- `src/EditAutoC.cpp` - Auto-completion logic
- `src/Styles.cpp` - Syntax highlighting style management
- `src/Dialogs.cpp` - Dialog implementations
- `src/Helpers.cpp` - Utility functions
- `src/SciCall.h` - Scintilla API wrapper macros
- `src/EditLexers/` - Language-specific style definitions (stl*.cpp)

### Lexer System
Each language has two parts:
1. **Lexer** in `scintilla/lexers/Lex*.cxx` - Tokenizes and highlights syntax
2. **Style definition** in `src/EditLexers/stl*.cpp` - Defines colors/fonts for token types

The `src/EditLexer.h` defines `EDITLEXER` and `EDITSTYLE` structures used by all lexers.

### Code Generation Tools
Python scripts in `tools/` generate code from language definitions:
- `KeywordCore.py` - Generates keyword lists from `tools/lang/` files
- `LexerConfig.py` - Generates lexer configuration code
- `KeywordUpdate.py` - Updates keyword definitions

## Code Style

- Uses tabs for indentation (configured in `.clang-format` and `.editorconfig`)
- C++20 standard (`/std:c++20` or `-std=gnu++20`)
- C17 standard for C code (`-std=gnu17`)
- Windows Unicode APIs (UNICODE and _UNICODE defined)
- No RTTI in release builds
- Column limit: none (ColumnLimit: 0)

## Compiler Support

- MSVC 2019, 2022, 2026
- Clang/LLVM (via Visual Studio or llvm-mingw)
- GCC (via MSYS2 mingw-w64)

## Target Platforms

- Windows Vista through Windows 11 (32-bit and 64-bit)
- Windows 10/11 on ARM64
- AVX2 and AVX512 optimized builds available
