# Пакет winget

Здесь лежит копия манифеста, который уходит в [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs)
в каталог `manifests/r/rcktsci/WinWhisper/<версия>/`. Держим у себя, чтобы при новом релизе было
что обновлять, не вспоминая формат.

## Новый релиз

1. Взять SHA256 свежего ассета:
   ```powershell
   gh release download v1.2.3 --pattern WinWhisper.exe --dir $env:TEMP\ww --clobber
   (Get-FileHash $env:TEMP\ww\WinWhisper.exe -Algorithm SHA256).Hash.ToLower()
   ```
2. Скопировать прошлую папку в новую, поправить `PackageVersion` в трёх файлах,
   `InstallerUrl` и `InstallerSha256` в `*.installer.yaml`.
3. Проверить локально (нужна включённая настройка `LocalManifestFiles`):
   ```powershell
   winget validate --manifest .\packaging\winget\<версия>
   winget install --manifest .\packaging\winget\<версия>
   ```
4. Отправить в winget одним из двух способов:
   ```
   wingetcreate submit --path .\packaging\winget\<версия> --token <токен>
   ```
   или вручную: форк, ветка, файлы в `manifests/r/rcktsci/WinWhisper/<версия>/`, pull request.

Важно: после публикации в winget **нельзя** переписывать релиз и его ассеты — хеш в манифесте
привязан к файлу. Только новые версии.
