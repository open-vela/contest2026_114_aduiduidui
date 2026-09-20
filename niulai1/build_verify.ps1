Remove-Item Env:MSYSTEM,Env:MSYS,Env:MINGW_PREFIX,Env:MINGW_CHOST -ErrorAction SilentlyContinue
Set-Location 'C:\Users\21595\Desktop\zhitong_submit\firmware-esp32'
& 'C:\Users\21595\esp-idf\export.ps1' *> $null
idf.py fullclean
idf.py build
exit $LASTEXITCODE
