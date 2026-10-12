# DesktopDisplay - GitHub Setup Script
# Run this once from the project folder (the one containing DesktopDisplay.ino)
# to create the GitHub repo and push everything.
# Requirements: Git (https://git-scm.com) and GitHub CLI (https://cli.github.com)
#
# If Windows blocks the script, run it like this instead:
#   powershell -ExecutionPolicy Bypass -File .\setup_github.ps1

# NOTE: "Continue" on purpose. With "Stop", Windows PowerShell 5.1 treats
# anything git/gh print to stderr (even harmless status messages) as a fatal
# error. Exit codes are checked manually below instead.
$ErrorActionPreference = "Continue"

# Keep the repo name the same as the .ino file: the Arduino IDE requires the
# sketch folder name to match, and a cloned repo's folder gets the repo's name.
$repoName   = "DesktopDisplay"
$repoDesc   = "ESP32 weather station for the 3.5-inch E32R35T touch display: three cities, OpenWeatherMap / Open-Meteo / KNMI, rain forecast, moon phase, SD-card history, optional BME280 indoor sensor and a live web dashboard."
$visibility = "public"   # or "private"
$sketchFile = "DesktopDisplay.ino"
$defaultMsg = "Initial release: DesktopDisplay weather station"

function Fail($text) {
    Write-Host ""
    Write-Host $text -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "  DesktopDisplay - GitHub Setup" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host ""

# -- Right folder? ------------------------------------------------
if (-not (Test-Path $sketchFile)) {
    Fail "$sketchFile not found. Run this script from inside the DesktopDisplay project folder."
}

# -- Check Git ----------------------------------------------------
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail "Git not found. Download it from https://git-scm.com and re-run this script."
}

# -- Git needs a name and email for commits ------------------------
$gitName  = git config user.name
$gitEmail = git config user.email
if ([string]::IsNullOrWhiteSpace($gitName) -or [string]::IsNullOrWhiteSpace($gitEmail)) {
    Write-Host "Git doesn't know who you are yet (needed for commits)." -ForegroundColor Yellow
    Write-Host "Tip: use your GitHub 'noreply' email to keep your real email private." -ForegroundColor Yellow
    Write-Host "     (github.com > Settings > Emails)" -ForegroundColor Yellow
    if ([string]::IsNullOrWhiteSpace($gitName))  { git config --global user.name  (Read-Host "Your name or GitHub username") }
    if ([string]::IsNullOrWhiteSpace($gitEmail)) { git config --global user.email (Read-Host "Your email") }
}

# -- Check GitHub CLI ---------------------------------------------
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Write-Host "GitHub CLI (gh) not found." -ForegroundColor Yellow
    Write-Host "Install it from https://cli.github.com, then run: gh auth login" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Alternatively, create the repo manually on github.com and run:" -ForegroundColor Yellow
    Write-Host "  git init -b main" -ForegroundColor White
    Write-Host "  git add ." -ForegroundColor White
    Write-Host "  git commit -m `"$defaultMsg`"" -ForegroundColor White
    Write-Host "  git remote add origin https://github.com/YOUR_USERNAME/$repoName.git" -ForegroundColor White
    Write-Host "  git push -u origin main" -ForegroundColor White
    exit 1
}

# -- Check gh auth ------------------------------------------------
gh auth status *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Not logged in to GitHub CLI. Running: gh auth login" -ForegroundColor Yellow
    gh auth login
    gh auth status *> $null
    if ($LASTEXITCODE -ne 0) { Fail "GitHub login failed. Run 'gh auth login' manually and try again." }
}

# -- Fill in your GitHub username in LICENSE / README --------------
# The docs ship with the placeholder YOUR_GITHUB_USERNAME so no real name
# ends up in them; it's replaced with your GitHub username here.
$username = gh api user --jq .login
if (-not [string]::IsNullOrWhiteSpace($username)) {
    foreach ($doc in @("LICENSE", "README.md", "CHANGELOG.md", "SECURITY.md")) {
        if ((Test-Path $doc) -and (Select-String -Path $doc -SimpleMatch "YOUR_GITHUB_USERNAME" -Quiet)) {
            $text = [System.IO.File]::ReadAllText((Resolve-Path $doc))
            $text = $text.Replace("YOUR_GITHUB_USERNAME", $username)
            [System.IO.File]::WriteAllText((Resolve-Path $doc), $text, (New-Object System.Text.UTF8Encoding $false))
            Write-Host "Filled in your GitHub username in $doc" -ForegroundColor Green
        }
    }
}

# -- Safety check: no API keys in the code -------------------------
# OpenWeatherMap keys are 32 hex characters. Anything like that inside quotes
# in the code is almost certainly a real key that shouldn't go public.
Write-Host "Checking code for API keys..." -ForegroundColor Green
$leaks = Get-ChildItem -Recurse -Include *.ino,*.h,*.cpp,*.c -File |
         Select-String -Pattern '"[0-9a-fA-F]{32}"'
if ($leaks) {
    Write-Host ""
    Write-Host "STOP: something that looks like an API key was found:" -ForegroundColor Red
    $leaks | ForEach-Object { Write-Host ("  {0}:{1}  {2}" -f $_.Filename, $_.LineNumber, $_.Line.Trim()) -ForegroundColor White }
    Fail "Remove it (enter the key on the device's web settings page instead) and run this script again."
}
Write-Host "No API keys found." -ForegroundColor Green

# -- .gitignore ---------------------------------------------------
if (-not (Test-Path ".gitignore")) {
    Write-Host "Creating .gitignore..." -ForegroundColor Green
    @(
        "# Build output",
        "build/",
        "*.bin",
        "*.elf",
        "*.map",
        "",
        "# Archives / backups",
        "*.zip",
        "*.bak",
        "",
        "# Local secrets (if you ever add one)",
        "secrets.h",
        "",
        "# Editor / OS junk",
        ".vscode/",
        "desktop.ini",
        "Thumbs.db",
        ".DS_Store"
    ) | Set-Content -Path ".gitignore" -Encoding ASCII
}

# -- Missing docs? (just a reminder, doesn't stop the script) -------
foreach ($doc in @("README.md", "LICENSE", "CHANGELOG.md", "SECURITY.md")) {
    if (-not (Test-Path $doc)) { Write-Host "Note: no $doc yet - you can add it later." -ForegroundColor Yellow }
}

# -- Init git repo ------------------------------------------------
if (-not (Test-Path ".git")) {
    Write-Host "Initialising git repository..." -ForegroundColor Green
    git init | Out-Null
    git symbolic-ref HEAD refs/heads/main   # name the first branch 'main' (works on any Git version)
} else {
    Write-Host "Git repository already exists." -ForegroundColor Green
}

# -- Stage and commit ---------------------------------------------
Write-Host "Staging files..." -ForegroundColor Green
git add .
Write-Host ""
Write-Host "Files to be committed:" -ForegroundColor Cyan
git status --short
Write-Host ""

$confirm = Read-Host "Upload these files? (y/n)"
if ($confirm -notmatch '^[Yy]') { Fail "Cancelled. Nothing was uploaded." }

git diff --cached --quiet
if ($LASTEXITCODE -ne 0) {
    $msg = Read-Host "Commit message [$defaultMsg]"
    if ([string]::IsNullOrWhiteSpace($msg)) { $msg = $defaultMsg }
    git commit -m $msg
    if ($LASTEXITCODE -ne 0) { Fail "Commit failed - see the message above." }
} else {
    Write-Host "Nothing new to commit, continuing with the push." -ForegroundColor Yellow
}

# -- Create GitHub repo and push ----------------------------------
Write-Host ""
$hasRemote = git remote | Select-String -SimpleMatch "origin"
if ($hasRemote) {
    Write-Host "Remote 'origin' already set, pushing..." -ForegroundColor Green
    git push -u origin main
} else {
    Write-Host "Creating GitHub repository '$repoName'..." -ForegroundColor Green
    gh repo create $repoName --description $repoDesc --$visibility --source=. --remote=origin --push
}
if ($LASTEXITCODE -ne 0) {
    Fail "Upload failed. If a repo called '$repoName' already exists on your account, delete or rename it, or change `$repoName at the top of this script."
}

Write-Host ""
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "  Done!" -ForegroundColor Green
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Your repo: https://github.com/$username/$repoName" -ForegroundColor White
Write-Host ""
Write-Host "Recommended next steps:" -ForegroundColor Yellow
Write-Host "  1. Add a photo of the display to the README (drag a .png into it on github.com)" -ForegroundColor White
Write-Host "  2. Add topics on GitHub: esp32, arduino, weather-station, tft-espi, openweathermap, open-meteo" -ForegroundColor White
Write-Host "  3. Enable private vulnerability reporting: Settings > Security (mentioned in SECURITY.md)" -ForegroundColor White
