# Naruto 5 Recomp - instalador grafico para Windows.
# Sem downloads: Zig, Python, SDL2, wgpu-native e eerecomp/vurecomp ja estao
# em toolchain/. A GUI usa apenas Windows Forms (nativo).
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$defaultDir = Join-Path $env:USERPROFILE 'Games\Naruto5'
$script:installed = ''

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Naruto Shippuden: Ultimate Ninja 5 - Instalador'
$form.Size = New-Object System.Drawing.Size(840, 660)
$form.StartPosition = 'CenterScreen'
$form.MinimumSize = New-Object System.Drawing.Size(760, 620)
$form.BackColor = [System.Drawing.Color]::FromArgb(28, 28, 32)
$form.ForeColor = [System.Drawing.Color]::White

$title = New-Object System.Windows.Forms.Label
$title.Text = 'Naruto Shippuden: Ultimate Ninja 5'
$title.Font = New-Object System.Drawing.Font('Segoe UI', 18, [System.Drawing.FontStyle]::Bold)
$title.ForeColor = [System.Drawing.Color]::FromArgb(255, 204, 51)
$title.AutoSize = $true
$title.Location = New-Object System.Drawing.Point(20, 15)
$form.Controls.Add($title)

$sub = New-Object System.Windows.Forms.Label
$sub.Text = 'Instalador do recomp nativo - nada e baixado; o toolchain ja esta no pacote.'
$sub.AutoSize = $true
$sub.ForeColor = [System.Drawing.Color]::FromArgb(190, 190, 190)
$sub.Location = New-Object System.Drawing.Point(22, 55)
$form.Controls.Add($sub)

$lblIso = New-Object System.Windows.Forms.Label
$lblIso.Text = 'ISO do jogo:'
$lblIso.Location = New-Object System.Drawing.Point(22, 95)
$lblIso.AutoSize = $true
$form.Controls.Add($lblIso)
$txtIso = New-Object System.Windows.Forms.TextBox
$txtIso.Location = New-Object System.Drawing.Point(150, 92)
$txtIso.Size = New-Object System.Drawing.Size(540, 26)
$form.Controls.Add($txtIso)
$btnIso = New-Object System.Windows.Forms.Button
$btnIso.Text = 'Escolher ISO...'
$btnIso.Location = New-Object System.Drawing.Point(700, 90)
$btnIso.Size = New-Object System.Drawing.Size(110, 30)
$form.Controls.Add($btnIso)

$lblDir = New-Object System.Windows.Forms.Label
$lblDir.Text = 'Pasta de instalacao:'
$lblDir.Location = New-Object System.Drawing.Point(22, 135)
$lblDir.AutoSize = $true
$form.Controls.Add($lblDir)
$txtDir = New-Object System.Windows.Forms.TextBox
$txtDir.Location = New-Object System.Drawing.Point(150, 132)
$txtDir.Size = New-Object System.Drawing.Size(540, 26)
$txtDir.Text = $defaultDir
$form.Controls.Add($txtDir)
$btnDir = New-Object System.Windows.Forms.Button
$btnDir.Text = 'Escolher pasta...'
$btnDir.Location = New-Object System.Drawing.Point(700, 130)
$btnDir.Size = New-Object System.Drawing.Size(110, 30)
$form.Controls.Add($btnDir)

$chkNtsc = New-Object System.Windows.Forms.CheckBox
$chkNtsc.Text = 'Aplicar patch NTSC/60 Hz (recomendado para a versao PAL)'
$chkNtsc.Location = New-Object System.Drawing.Point(22, 175)
$chkNtsc.Size = New-Object System.Drawing.Size(500, 24)
$chkNtsc.Checked = $true
$form.Controls.Add($chkNtsc)
$chkShortcut = New-Object System.Windows.Forms.CheckBox
$chkShortcut.Text = 'Criar atalhos no menu Iniciar e na area de trabalho'
$chkShortcut.Location = New-Object System.Drawing.Point(22, 202)
$chkShortcut.Size = New-Object System.Drawing.Size(500, 24)
$chkShortcut.Checked = $true
$form.Controls.Add($chkShortcut)

$btnInstall = New-Object System.Windows.Forms.Button
$btnInstall.Text = 'Instalar'
$btnInstall.Location = New-Object System.Drawing.Point(22, 238)
$btnInstall.Size = New-Object System.Drawing.Size(120, 36)
$btnInstall.BackColor = [System.Drawing.Color]::FromArgb(255, 204, 51)
$btnInstall.ForeColor = [System.Drawing.Color]::Black
$btnInstall.Font = New-Object System.Drawing.Font('Segoe UI', 10, [System.Drawing.FontStyle]::Bold)
$form.Controls.Add($btnInstall)
$btnPlay = New-Object System.Windows.Forms.Button
$btnPlay.Text = 'Jogar'
$btnPlay.Location = New-Object System.Drawing.Point(150, 238)
$btnPlay.Size = New-Object System.Drawing.Size(120, 36)
$btnPlay.Enabled = $false
$form.Controls.Add($btnPlay)
$btnClose = New-Object System.Windows.Forms.Button
$btnClose.Text = 'Fechar'
$btnClose.Location = New-Object System.Drawing.Point(700, 238)
$btnClose.Size = New-Object System.Drawing.Size(110, 36)
$form.Controls.Add($btnClose)

$status = New-Object System.Windows.Forms.Label
$status.Text = 'Escolha a ISO e a pasta de instalacao.'
$status.Location = New-Object System.Drawing.Point(22, 286)
$status.Size = New-Object System.Drawing.Size(790, 24)
$status.ForeColor = [System.Drawing.Color]::FromArgb(190, 190, 190)
$form.Controls.Add($status)
$progress = New-Object System.Windows.Forms.ProgressBar
$progress.Location = New-Object System.Drawing.Point(22, 312)
$progress.Size = New-Object System.Drawing.Size(790, 12)
$progress.Style = 'Marquee'
$progress.MarqueeAnimationSpeed = 0
$form.Controls.Add($progress)

$log = New-Object System.Windows.Forms.TextBox
$log.Location = New-Object System.Drawing.Point(22, 335)
$log.Size = New-Object System.Drawing.Size(790, 270)
$log.Multiline = $true
$log.ScrollBars = 'Vertical'
$log.ReadOnly = $true
$log.BackColor = [System.Drawing.Color]::FromArgb(16, 16, 20)
$log.ForeColor = [System.Drawing.Color]::FromArgb(232, 232, 232)
$log.Font = New-Object System.Drawing.Font('Consolas', 9)
$form.Controls.Add($log)

try {
    $icon = Join-Path $root 'icons\naruto5.ico'
    if (Test-Path $icon) { $form.Icon = New-Object System.Drawing.Icon($icon) }
} catch { }

$btnIso.Add_Click({
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = 'Escolha a ISO do Naruto Shippuden: Ultimate Ninja 5'
    $dlg.Filter = 'Imagem ISO (*.iso;*.zip)|*.iso;*.zip|Todos os arquivos (*.*)|*.*'
    if ($dlg.ShowDialog() -eq 'OK') { $txtIso.Text = $dlg.FileName }
})
$btnDir.Add_Click({
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = 'Escolha a pasta de instalacao'
    $dlg.SelectedPath = $txtDir.Text
    if ($dlg.ShowDialog() -eq 'OK') {
        if ($dlg.SelectedPath -eq [Environment]::GetFolderPath('UserProfile')) {
            $txtDir.Text = Join-Path $dlg.SelectedPath 'Games\Naruto5'
        } else { $txtDir.Text = $dlg.SelectedPath }
    }
})
$btnClose.Add_Click({ $form.Close() })
$btnPlay.Add_Click({
    if ($script:installed -ne '' -and (Test-Path (Join-Path $script:installed 'jogar.bat'))) {
        Start-Process -FilePath (Join-Path $script:installed 'jogar.bat') -WorkingDirectory $script:installed
    }
})

$btnInstall.Add_Click({
    $iso = $txtIso.Text.Trim('"')
    $dest = $txtDir.Text.Trim('"')
    if (-not (Test-Path $iso)) {
        [System.Windows.Forms.MessageBox]::Show('Escolha uma ISO valida.', 'ISO nao encontrada') | Out-Null
        return
    }
    if ($dest.Trim() -eq '' -or $dest -eq [Environment]::GetFolderPath('UserProfile') -or $dest -eq 'C:\') {
        [System.Windows.Forms.MessageBox]::Show('Escolha uma pasta dedicada, por exemplo C:\Games\Naruto5.', 'Pasta invalida') | Out-Null
        return
    }
    $btnInstall.Enabled = $false
    $btnPlay.Enabled = $false
    $progress.MarqueeAnimationSpeed = 30
    $status.Text = 'Instalando... isso pode levar varios minutos (compilando o jogo).'
    $form.Refresh()
    try {
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        $destFull = (Resolve-Path $dest).Path
        $rootFull = (Resolve-Path $root).Path
        if ($destFull -ne $rootFull) {
            if ($destFull.StartsWith($rootFull + [IO.Path]::DirectorySeparatorChar)) {
                throw 'Nao instale dentro da propria pasta do pacote.'
            }
            $log.Text = 'Copiando a ferramenta para ' + $destFull + "...`r`n"
            $form.Refresh()
            & robocopy $rootFull $destFull /E /XD "$rootFull\games" "$rootFull\generated" "$rootFull\build" "$rootFull\states" "$rootFull\.git" /XF iso_path.txt *.log /NFL /NDL /NJH /NJS /NP | Out-Null
            if ($LASTEXITCODE -ge 8) { throw ('robocopy falhou (' + $LASTEXITCODE + ')') }
        }
        Set-Content -Path (Join-Path $destFull 'iso_path.txt') -Value $iso -Encoding ASCII
        $py = Join-Path $destFull 'toolchain\python\python.exe'
        if (-not (Test-Path $py)) { $py = 'python' }
        $argLine = 'scripts\port_win.py naruto "' + $iso + '" --frames 12000 --rounds 4'
        if ($chkNtsc.Checked) {
            $argLine += ' --pnach "patches\PALtoNTSC_fixed.pnach" --pnach-section "NTSC Video Mode"'
        }
        $outLog = Join-Path $destFull 'install.log'
        $errLog = Join-Path $destFull 'install.err.log'
        $proc = Start-Process -FilePath $py -ArgumentList $argLine -WorkingDirectory $destFull -Wait -PassThru -NoNewWindow -RedirectStandardOutput $outLog -RedirectStandardError $errLog
        if (Test-Path $outLog) { $log.Text = Get-Content $outLog -Raw }
        if ($proc.ExitCode -ne 0) {
            if (Test-Path $errLog) { $log.AppendText("`r`n" + (Get-Content $errLog -Raw)) }
            throw 'port_win.py falhou. Veja install.log.'
        }
        if ($chkShortcut.Checked) {
            $ws = New-Object -ComObject WScript.Shell
            $target = Join-Path $destFull 'jogar.bat'
            $icon = Join-Path $destFull 'icons\naruto5.ico'
            $menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'Naruto Shippuden Ultimate Ninja 5.lnk'
            $lnk = $ws.CreateShortcut($menu)
            $lnk.TargetPath = $target
            $lnk.WorkingDirectory = $destFull
            $lnk.IconLocation = $icon
            $lnk.Description = 'Naruto 5 Recomp'
            $lnk.Save()
            $desktop = [Environment]::GetFolderPath('Desktop')
            if (Test-Path $desktop) {
                $lnk2 = $ws.CreateShortcut((Join-Path $desktop 'Naruto Shippuden Ultimate Ninja 5.lnk'))
                $lnk2.TargetPath = $target
                $lnk2.WorkingDirectory = $destFull
                $lnk2.IconLocation = $icon
                $lnk2.Description = 'Naruto 5 Recomp'
                $lnk2.Save()
            }
        }
        $script:installed = $destFull
        $btnPlay.Enabled = $true
        $status.Text = 'Instalacao concluida! O jogo esta pronto para jogar.'
        $progress.MarqueeAnimationSpeed = 0
        [System.Windows.Forms.MessageBox]::Show('Instalacao concluida! Use o botao Jogar.', 'Naruto 5 Recomp') | Out-Null
    } catch {
        $status.Text = 'Falhou: ' + $_.Exception.Message
        $progress.MarqueeAnimationSpeed = 0
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Falha na instalacao') | Out-Null
    } finally {
        $btnInstall.Enabled = $true
    }
})

[void]$form.ShowDialog()
