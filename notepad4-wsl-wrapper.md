# WSL Wrapper for Notepad4

## Purpose

Create a shell wrapper script so that `notepad4` can be called from WSL to open files in the Windows application `D:\ut\notepad4.exe`.

## Implementation

1. Create `~/bin` directory if it doesn't exist
2. Create a wrapper script `~/bin/notepad4` with the following content:

```sh
#!/bin/sh
exec /mnt/d/ut/notepad4.exe "$@"
```

3. Make the script executable: `chmod +x ~/bin/notepad4`

4. Create a short alias symlink: `ln -s ~/bin/notepad4 ~/bin/n4`

5. Add `~/bin` to PATH in `~/.bashrc` (if not already present):

```sh
export PATH="$HOME/bin:$PATH"
```

## Usage

After setup, run:
```sh
source ~/.bashrc
notepad4 myfile.txt
n4 myfile.txt  # short alias
```

## Notes

- The Windows path `D:\ut\notepad4.exe` maps to `/mnt/d/ut/notepad4.exe` in WSL
- This is the same pattern used by Git for Windows' `/usr/bin/notepad` wrapper
- WSL automatically translates Linux paths to Windows paths when calling Windows executables
- The symlink approach (`n4 -> notepad4`) is preferred over a separate script for easier maintenance
- To adapt for other Windows apps, just change the exe path in the wrapper script
