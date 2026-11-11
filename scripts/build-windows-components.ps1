param([switch]$TestTools)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path $root 'out/native'
$destination = Join-Path $root 'resources/native/win32-x64'
$source = Join-Path $root 'native'
New-Item -ItemType Directory -Force $work, $destination | Out-Null

function Get-Source($name, $url, $sha256) {
    $file = Join-Path $work $name
    if (!(Test-Path -LiteralPath $file)) {
        curl.exe --fail --silent --show-error --location --output $file $url
        if ($LASTEXITCODE -ne 0) { throw "Download failed for $name" }
    }
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $sha256) {
        throw "SHA-256 mismatch for $file"
    }
    return $file
}

$archive = Get-Source 'sqlite3mc.zip' 'https://github.com/utelle/SQLite3MultipleCiphers/releases/download/v2.5.1/sqlite3mc-2.5.1-sqlite-3.53.4-amalgamation.zip' '4125F8FF275EA953DABB3289331B20A0E76D4FC060F57148F4A5DF3BF3B0D5E0'
$sqlite = Join-Path $work 'sqlite3mc'
Expand-Archive -LiteralPath $archive -DestinationPath $sqlite -Force
Get-Source 'json.hpp' 'https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp' 'AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63' | Out-Null
$node = 'https://raw.githubusercontent.com/nodejs/node/v24.13.0/src'
Get-Source 'node_api.h' "$node/node_api.h" '2D4560831E525B47B060EC8A0864AB73993DF9E20215CE9F0FE7B24CD31AF32A' | Out-Null
Get-Source 'node_api_types.h' "$node/node_api_types.h" '088766B3CF1F0CD9B292C361AD30F7A04E7CC6DB35A20CA52C572C1ADF9635E7' | Out-Null
Get-Source 'js_native_api.h' "$node/js_native_api.h" 'FEC72E29A59EF5FA49129EFF141F3A0CE0F0CB3E628B0695F8E711DCCB4FDB9A' | Out-Null
Get-Source 'js_native_api_types.h' "$node/js_native_api_types.h" '4DBE2545FFF0B2EA19E48021EF2E146666A6CB9B2264E9CB720A96DE31FEEB9F' | Out-Null

if ((gcc -dumpmachine) -ne 'x86_64-w64-mingw32') { throw 'An x64 MinGW-w64 GCC toolchain is required.' }
$object = Join-Path $work 'sqlite3mc.o'
gcc -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -c (Join-Path $sqlite 'sqlite3mc_amalgamation.c') -o $object
if ($LASTEXITCODE -ne 0) { throw 'SQLite3 Multiple Ciphers compilation failed' }

$flags = @('-std=c++17', '-O2', '-static', '-I', $work, '-I', $sqlite)
g++ @flags -shared (Join-Path $source 'wcdb.cpp') $object -lbcrypt -o (Join-Path $destination 'wcdb.dll')
if ($LASTEXITCODE -ne 0) { throw 'WCDB DLL compilation failed' }
g++ @flags -shared (Join-Path $source 'image_addon.cpp') -lbcrypt -o (Join-Path $destination 'image-decrypt.node')
if ($LASTEXITCODE -ne 0) { throw 'Image addon compilation failed' }
g++ @flags (Join-Path $source 'welive.cpp') $object -lbcrypt -o (Join-Path $destination 'welive.exe')
if ($LASTEXITCODE -ne 0) { throw 'WeLive compilation failed' }

$licenses = @(
    @('SQLite3MultipleCiphers-LICENSE.txt', 'https://raw.githubusercontent.com/utelle/SQLite3MultipleCiphers/v2.5.1/LICENSE', '39205EC18E0F25F56C62D3BDA9768E7509B63A74BA58BEB7442903DFC81C247A'),
    @('nlohmann-json-LICENSE.txt', 'https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT', '46A65CFFD1EA955132D95A8DD921640714A8D6B537D2E4E482D31145AE95B603'),
    @('Node-LICENSE.txt', 'https://raw.githubusercontent.com/nodejs/node/v24.13.0/LICENSE', '537308465103A306D0E3EECF42632B4FF1B48AAAEC044E9FC10A78C81FD00B34')
)
foreach ($license in $licenses) {
    $file = Get-Source @license
    Copy-Item -LiteralPath $file -Destination $destination
}

if ($TestTools) {
    gcc -O1 -DSQLITE_OMIT_LOAD_EXTENSION -I $sqlite (Join-Path $sqlite 'shell3mc_amalgamation.c') $object -o (Join-Path $work 'sqlite3mc.exe')
    if ($LASTEXITCODE -ne 0) { throw 'SQLite fixture shell compilation failed' }
}
