$menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'Naruto Shippuden Ultimate Ninja 5.lnk'
$desktop = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Naruto Shippuden Ultimate Ninja 5.lnk'
foreach ($p in @($menu, $desktop)) { if (Test-Path $p) { Remove-Item $p -Force } }
Write-Host 'Atalhos removidos. A pasta de instalacao e os saves nao foram apagados.'
