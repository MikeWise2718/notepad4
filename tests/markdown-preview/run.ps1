# Build the marker test harness and check every sample in samples\.
#
#   powershell -File tests\markdown-preview\run.ps1
#
# Builds render_markers.exe with the same defines as Notepad4.vcxproj, renders
# each sample with and without line markers, and runs check_markers.py over the
# pair. Exit code is non-zero if anything fails.

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $here '..\..')
$src = Join-Path $root 'src'
$out = Join-Path $env:TEMP 'np4-marker-test'
New-Item -ItemType Directory -Force $out | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

# Mirrors Notepad4.vcxproj's ClCompile settings, minus /Gv: md4c is C with the
# default calling convention, and nothing here needs __vectorcall.
$defines = '/D_WINDOWS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DSTRICT_TYPED_ITEMIDS /DUNICODE /D_UNICODE ' +
	'/D_CRT_SECURE_NO_WARNINGS /DNDEBUG /D_WIN64 /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00'
$includes = "/I`"$src`" /I`"$root\scintilla\include`""
$sources = @(
	(Join-Path $here 'render_markers.cpp'),
	(Join-Path $src 'MarkdownPreview.cpp'),
	(Join-Path $src 'md4c\md4c.c'),
	(Join-Path $src 'md4c\md4c-html.c'),
	(Join-Path $src 'md4c\entity.c')
) | ForEach-Object { "`"$_`"" }
$libs = 'user32.lib shlwapi.lib ole32.lib shell32.lib advapi32.lib'

$build = "`"$vcvars`" >nul && cd /d `"$out`" && cl /nologo /std:c++latest /O1 /EHsc /W3 /Zc:__cplusplus " +
	"/source-charset:utf-8 /wd4201 /wd4204 /wd4221 $defines $includes $($sources -join ' ') " +
	"/Fe:render_markers.exe /link $libs"
cmd /c $build
if ($LASTEXITCODE -ne 0) { throw "harness build failed" }

$exe = Join-Path $out 'render_markers.exe'
$failed = 0
foreach ($sample in Get-ChildItem (Join-Path $here 'samples') -Filter *.md) {
	$plain = Join-Path $out ($sample.BaseName + '.plain.html')
	$marked = Join-Path $out ($sample.BaseName + '.marked.html')
	& $exe $sample.FullName $plain $marked
	if ($LASTEXITCODE -ne 0) { Write-Host "RENDER FAILED: $($sample.Name)"; $failed++; continue }
	python (Join-Path $here 'check_markers.py') $sample.FullName $plain $marked
	if ($LASTEXITCODE -ne 0) { $failed++ }
}
if ($failed -ne 0) { Write-Host "$failed sample(s) failed"; exit 1 }
Write-Host 'all samples passed'
