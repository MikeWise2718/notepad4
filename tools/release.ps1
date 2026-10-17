# Build Notepad4 (this fork) and publish it as a GitHub Release.
#
#   powershell -File tools\release.ps1            # build, zip, publish
#   powershell -File tools\release.ps1 -DryRun    # build and zip only
#
# The release is tagged after the fork version in src\ForkVersion.h
# (mw-1.3.1, ...) and carries one zip, Notepad4-mw-<version>-x64.zip, plus its
# SHA-256. tools\install.ps1 installs or updates from the latest release.
#
# Refuses to publish unless the working tree is clean and HEAD is pushed, so a
# release always corresponds to a commit anyone can check out, and unless the
# fork version is new -- bump NP2_FORK_VERSION first.
#
# See specs\release-and-install.md.

param(
	[switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$Repo = 'MikeWise2718/notepad4'   # explicit: the local gh default may be upstream
$root = Resolve-Path (Join-Path $PSScriptRoot '..')

function Fail([string]$message) {
	Write-Host "release: $message" -ForegroundColor Red
	exit 1
}

# --- What is being released -------------------------------------------------

$forkHeader = Get-Content (Join-Path $root 'src\ForkVersion.h') -Raw
if ($forkHeader -notmatch 'NP2_FORK_VERSION\s+L"([0-9.]+)"') { Fail 'cannot read NP2_FORK_VERSION' }
$version = $Matches[1]
$tag = "mw-$version"
$zipName = "Notepad4-$tag-x64.zip"

# The user-visible change list, for the release notes.
$changes = [regex]::Matches($forkHeader, 'L"\\x2022 ([^"\\]+)') | ForEach-Object { "- " + $_.Groups[1].Value }

Push-Location $root
try {
	$head = (git rev-parse HEAD).Trim()
	if (-not $DryRun) {
		if (git status --porcelain) { Fail 'working tree is not clean; commit first' }
		git fetch -q origin
		$remoteHead = (git rev-parse origin/main).Trim()
		if ($head -ne $remoteHead) { Fail "HEAD $($head.Substring(0,8)) is not origin/main; push first" }
		# Listing rather than `gh release view`: the latter reports a missing
		# release on stderr, which Windows PowerShell 5.1 turns into an error.
		$existing = gh release list --repo $Repo --limit 200 --json tagName --jq '.[].tagName'
		if ($LASTEXITCODE -ne 0) { Fail 'cannot list releases (gh auth?)' }
		if (@($existing) -contains $tag) { Fail "release $tag already exists; bump NP2_FORK_VERSION in src\ForkVersion.h" }
	}
} finally {
	Pop-Location
}

# --- Build -------------------------------------------------------------------

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { Fail 'MSBuild not found' }

foreach ($project in 'Notepad4', 'matepath') {
	Write-Host "Building $project x64 Release..."
	& $msbuild (Join-Path $root "build\VisualStudio\$project.vcxproj") /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
	if ($LASTEXITCODE -ne 0) { Fail "$project build failed" }
}

# --- Package -----------------------------------------------------------------

$bin = Join-Path $root 'build\bin\Release\x64'
$stage = Join-Path $env:TEMP "notepad4-release-$tag"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'res') | Out-Null
# Long form, so entry names cut from Get-ChildItem's FullName line up even if
# %TEMP% is an 8.3 short path.
$stage = (Get-Item $stage).FullName

# Same layout as upstream's make_zip.bat, plus the Markdown preview's files.
# The .ini files ship as *.ini-default: Notepad4 copies them to Notepad4.ini
# only when that does not exist yet, so an update never touches settings.
$files = @(
	@{ From = "$bin\Notepad4.exe";                    To = 'Notepad4.exe' },
	@{ From = "$bin\matepath.exe";                    To = 'matepath.exe' },
	@{ From = "$bin\WebView2Loader.dll";              To = 'WebView2Loader.dll' },
	@{ From = "$bin\res\mermaid.min.js";              To = 'res\mermaid.min.js' },
	# Not License.txt: installs may go into a shared folder (D:\ut on syther
	# had another program's License.txt), and must not overwrite its files.
	@{ From = "$root\License.txt";                    To = 'Notepad4-License.txt' },
	@{ From = "$root\doc\Notepad4.ini";               To = 'Notepad4.ini-default' },
	@{ From = "$root\doc\Notepad4 DarkTheme.ini";     To = 'Notepad4 DarkTheme.ini-default' },
	@{ From = "$root\matepath\doc\matepath.ini";      To = 'matepath.ini-default' }
)
foreach ($file in $files) {
	if (-not (Test-Path $file.From)) { Fail "missing $($file.From)" }
	Copy-Item $file.From (Join-Path $stage $file.To)
}
Set-Content (Join-Path $stage 'VERSION.txt') "$tag`r`n$head`r`n" -Encoding ASCII

$outDir = Join-Path $root 'build\bin\release'   # gitignored with the rest of build\bin
New-Item -ItemType Directory -Force $outDir | Out-Null
$zip = Join-Path $outDir $zipName
if (Test-Path $zip) { Remove-Item $zip -Force }
# Not Compress-Archive: the version in Windows PowerShell 5.1 writes entry
# names with backslashes, which the zip format does not allow.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
try {
	foreach ($item in Get-ChildItem $stage -Recurse -File) {
		$entryName = $item.FullName.Substring($stage.Length + 1).Replace('\', '/')
		[void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $item.FullName, $entryName,
			[System.IO.Compression.CompressionLevel]::Optimal)
	}
} finally {
	$archive.Dispose()
}
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
$hashFile = "$zip.sha256"
Set-Content $hashFile "$hash  $zipName" -Encoding ASCII
Remove-Item $stage -Recurse -Force

$size = '{0:N1} MB' -f ((Get-Item $zip).Length / 1MB)
Write-Host "Packaged $zipName ($size), sha256 $hash"

if ($DryRun) {
	Write-Host "Dry run: not published. Zip is in $outDir"
	exit 0
}

# --- Publish -----------------------------------------------------------------

$fileVersion = (Get-Item "$bin\Notepad4.exe").VersionInfo.FileVersion
$notes = @"
Notepad4 with this fork's additions, built from $($head.Substring(0, 8)) (upstream revision $fileVersion).

$($changes -join "`n")

**Install or update** (Windows 10/11 x64, no admin needed), in PowerShell:

``````powershell
irm https://raw.githubusercontent.com/$Repo/main/tools/install.ps1 | iex
``````

The Markdown preview needs the Microsoft Edge WebView2 Runtime, which Windows 11 includes. Without it the editor works normally and the preview stays off.

SHA-256 of the zip: ``$hash``
"@

gh release create $tag $zip $hashFile --repo $Repo --target $head --title "Notepad4 $tag" --notes $notes
if ($LASTEXITCODE -ne 0) { Fail 'gh release create failed' }
Write-Host "Published https://github.com/$Repo/releases/tag/$tag"
