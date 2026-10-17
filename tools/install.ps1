# Install or update Notepad4 (MikeWise2718 fork) from its latest GitHub Release.
#
# One line, in PowerShell (no admin needed):
#
#   irm https://raw.githubusercontent.com/MikeWise2718/notepad4/main/tools/install.ps1 | iex
#
# Or download this file and run it with options:
#
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-InstallDir <dir>] [-Version 1.3.1]
#                                                         [-ZipPath <zip>] [-NoPath] [-NoShortcut]
#                                                         [-Uninstall]
#
# -ZipPath installs from a release zip already on disk (no network), checking
# it against a .sha256 file beside it if there is one.
#
# Installs per user, to %LOCALAPPDATA%\Programs\Notepad4 by default, adds a
# Start menu shortcut, and puts the folder on the user PATH so `notepad4 file`
# works from a new terminal. Running it again updates in place, even while
# Notepad4 is open: a running exe cannot be overwritten, but it can be renamed
# out of the way, and the open windows keep running the old version.
#
# Settings are never touched. They live in Notepad4.ini beside the exe, which
# Notepad4 creates on first run from the shipped Notepad4.ini-default; releases
# never contain Notepad4.ini itself.
#
# Written for Windows PowerShell 5.1, the version every Windows 10/11 machine
# has: no ternaries, no &&, TLS 1.2 forced for older defaults.
#
# See specs\release-and-install.md.

param(
	[string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\Notepad4'),
	[string]$Version = '',
	[string]$ZipPath = '',          # install from a local release zip instead of downloading
	[switch]$NoPath,
	[switch]$NoShortcut,
	[switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$Repo = 'MikeWise2718/notepad4'
$ShortcutPath = Join-Path ([Environment]::GetFolderPath('Programs')) 'Notepad4.lnk'
$LockableFiles = @('Notepad4.exe', 'matepath.exe', 'WebView2Loader.dll')   # held open while running

function Say([string]$message) { Write-Host "notepad4: $message" }

function Get-UserPath {
	$value = [Environment]::GetEnvironmentVariable('Path', 'User')
	if ($null -eq $value) { return '' }
	return $value
}

function Test-OnPath([string]$dir) {
	foreach ($entry in (Get-UserPath).Split(';')) {
		if ($entry.TrimEnd('\') -ieq $dir.TrimEnd('\')) { return $true }
	}
	return $false
}

# Move a file aside if something has it open. The rename succeeds on a running
# exe; the leftover is deleted on the next run once nothing uses it.
function Clear-Locked([string]$path) {
	if (-not (Test-Path $path)) { return }
	try {
		Remove-Item $path -Force
	} catch {
		$aside = "$path.old"
		if (Test-Path $aside) { Remove-Item $aside -Force -ErrorAction SilentlyContinue }
		if (Test-Path $aside) { $aside = "$path.$([DateTime]::Now.ToString('yyyyMMddHHmmss')).old" }
		Rename-Item $path (Split-Path $aside -Leaf)
	}
}

# --- Uninstall ---------------------------------------------------------------

if ($Uninstall) {
	if (Test-Path $ShortcutPath) {
		# Only our own shortcut: a Notepad4.lnk pointing anywhere else is not ours.
		$target = (New-Object -ComObject WScript.Shell).CreateShortcut($ShortcutPath).TargetPath
		if ($target -and ($target -like (Join-Path $InstallDir '*'))) {
			Remove-Item $ShortcutPath -Force
			Say 'removed Start menu shortcut'
		} else {
			Say "left Start menu shortcut alone (it points to $target)"
		}
	}
	if (Test-OnPath $InstallDir) {
		$kept = (Get-UserPath).Split(';') | Where-Object { $_ -and ($_.TrimEnd('\') -ine $InstallDir.TrimEnd('\')) }
		[Environment]::SetEnvironmentVariable('Path', ($kept -join ';'), 'User')
		Say 'removed from user PATH'
	}
	if (Test-Path $InstallDir) {
		$keep = @('Notepad4.ini', 'Notepad4 DarkTheme.ini', 'matepath.ini')
		Get-ChildItem $InstallDir -Force | Where-Object { $keep -notcontains $_.Name } | ForEach-Object {
			try { Remove-Item $_.FullName -Recurse -Force } catch { Say "could not remove $($_.Name) (in use?)" }
		}
		Say "removed program files; settings kept in $InstallDir (delete the folder to remove them too)"
	}
	return
}

# --- Find the release zip ----------------------------------------------------

function Get-HashFromText($text) {
	if ($text -is [byte[]]) { $text = [Text.Encoding]::ASCII.GetString($text) }
	return ([string]$text).Trim().Split(' ')[0].ToLower()
}

$work = Join-Path $env:TEMP "notepad4-install-$([Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force $work | Out-Null
try {
	$expected = $null
	if ($ZipPath) {
		# Local zip: no network at all.
		if (-not (Test-Path $ZipPath)) { throw "no such file: $ZipPath" }
		$zip = (Resolve-Path $ZipPath).ProviderPath
		$tagName = [IO.Path]::GetFileNameWithoutExtension($zip) -replace '^Notepad4-', '' -replace '-x64$', ''
		if (Test-Path "$zip.sha256") { $expected = Get-HashFromText (Get-Content "$zip.sha256" -Raw) }
	} else {
		[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
		$headers = @{ 'User-Agent' = 'notepad4-install'; 'Accept' = 'application/vnd.github+json' }
		if ($Version) {
			$api = "https://api.github.com/repos/$Repo/releases/tags/mw-$($Version.TrimStart('v').Replace('mw-', ''))"
		} else {
			$api = "https://api.github.com/repos/$Repo/releases/latest"
		}
		try {
			$release = Invoke-RestMethod -Uri $api -Headers $headers
		} catch {
			throw "cannot read $api : $($_.Exception.Message)"
		}
		$tagName = $release.tag_name
		$zipAsset = $release.assets | Where-Object { $_.name -like 'Notepad4-mw-*-x64.zip' } | Select-Object -First 1
		$hashAsset = $release.assets | Where-Object { $_.name -like 'Notepad4-mw-*-x64.zip.sha256' } | Select-Object -First 1
		if (-not $zipAsset) { throw "release $tagName has no x64 zip" }

		$zip = Join-Path $work $zipAsset.name
		$oldProgress = $ProgressPreference
		$ProgressPreference = 'SilentlyContinue'   # the 5.1 progress bar makes downloads crawl
		try {
			Invoke-WebRequest -Uri $zipAsset.browser_download_url -OutFile $zip -Headers $headers -UseBasicParsing
			if ($hashAsset) {
				# Served as application/octet-stream, so Content is a byte[];
				# never piped, which would enumerate it a byte at a time.
				$expected = Get-HashFromText (Invoke-WebRequest -Uri $hashAsset.browser_download_url -Headers $headers -UseBasicParsing).Content
			}
		} finally {
			$ProgressPreference = $oldProgress
		}
	}

	if ($expected) {
		$actual = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
		if ($expected -ne $actual) { throw "checksum mismatch: expected $expected, got $actual" }
		Say 'checksum verified'
	} else {
		Say 'no checksum available; skipping verification'
	}

	$versionFile = Join-Path $InstallDir 'VERSION.txt'
	if (Test-Path $versionFile) {
		$installed = (Get-Content $versionFile -TotalCount 1).Trim()
		if ($installed -eq $tagName) {
			Say "$installed is already installed in $InstallDir; reinstalling"
		} else {
			Say "updating $installed -> $tagName"
		}
	} else {
		Say "installing $tagName to $InstallDir"
	}

	$unpacked = Join-Path $work 'files'
	Expand-Archive -Path $zip -DestinationPath $unpacked -Force
	# Long form, so relative paths cut from FullName line up even if %TEMP%
	# is an 8.3 short path.
	$unpacked = (Get-Item $unpacked).FullName

	# --- Install ---------------------------------------------------------------

	New-Item -ItemType Directory -Force $InstallDir | Out-Null
	# Leftovers from an update made while Notepad4 was open.
	Get-ChildItem $InstallDir -Filter '*.old' -File -ErrorAction SilentlyContinue | ForEach-Object {
		Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
	}

	$running = Get-Process -Name Notepad4, matepath -ErrorAction SilentlyContinue |
		Where-Object { $_.Path -and ($_.Path -like (Join-Path $InstallDir '*')) }
	if ($running) { Say "$(@($running).Count) window(s) open; they keep the old version until closed" }

	foreach ($item in Get-ChildItem $unpacked -Recurse -File) {
		$relative = $item.FullName.Substring($unpacked.Length + 1)
		$target = Join-Path $InstallDir $relative
		New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
		if ($LockableFiles -contains $relative) { Clear-Locked $target }
		Copy-Item $item.FullName $target -Force
	}
} finally {
	Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

# --- Integrate ---------------------------------------------------------------

$exe = Join-Path $InstallDir 'Notepad4.exe'
if (-not $NoShortcut) {
	$shell = New-Object -ComObject WScript.Shell
	$link = $shell.CreateShortcut($ShortcutPath)
	$link.TargetPath = $exe
	$link.WorkingDirectory = $InstallDir
	$link.Description = 'Notepad4 text editor'
	$link.Save()
	Say 'Start menu shortcut: Notepad4'
}
if (-not $NoPath) {
	if (Test-OnPath $InstallDir) {
		Say 'already on user PATH'
	} else {
		$current = (Get-UserPath).TrimEnd(';')
		if ($current) { $current = "$current;$InstallDir" } else { $current = $InstallDir }
		[Environment]::SetEnvironmentVariable('Path', $current, 'User')
		Say 'added to user PATH (open a new terminal for `notepad4 <file>` to work)'
	}
}

# The Markdown preview needs the Edge WebView2 Runtime. Its documented
# registry entry, per-machine or per-user install.
$webView2 = '{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}'
$runtime = $false
foreach ($key in @("HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\$webView2",
		"HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$webView2",
		"HKCU:\Software\Microsoft\EdgeUpdate\Clients\$webView2")) {
	$pv = (Get-ItemProperty -Path $key -Name pv -ErrorAction SilentlyContinue).pv
	if ($pv -and $pv -ne '0.0.0.0') { $runtime = $true }
}
if (-not $runtime) {
	Say 'note: the WebView2 Runtime was not found, so the Markdown preview will stay off.'
	Say '      Install it from https://developer.microsoft.com/microsoft-edge/webview2/ if you want it.'
}

$fileVersion = (Get-Item $exe).VersionInfo.FileVersion
Say "done: $tagName (build $fileVersion) in $InstallDir"
