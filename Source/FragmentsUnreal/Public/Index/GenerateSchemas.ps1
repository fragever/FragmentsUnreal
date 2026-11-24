param([Parameter(Mandatory=$true)][string]$Flatc)
$ErrorActionPreference = 'Stop'
if ((& $Flatc --version) -ne 'flatc version 25.2.10') { throw 'Use flatc 25.2.10 to match ThirdParty/FlatBuffers.' }
$indexDir = $PSScriptRoot
$privateDir = [IO.Path]::GetFullPath((Join-Path $indexDir '../../Private/Index'))
& $Flatc --cpp -o $indexDir (Join-Path $indexDir 'Schemas/compatibility.fbs')
if ($LASTEXITCODE) { throw 'Compatibility schema generation failed' }
& $Flatc --cpp -o $privateDir (Join-Path $indexDir 'Schemas/fragments361.fbs')
if ($LASTEXITCODE) { throw 'Upstream schema generation failed' }
& $Flatc --cpp -o $privateDir (Join-Path $indexDir 'Schemas/modern.fbs')
if ($LASTEXITCODE) { throw 'Pre-face-ID schema generation failed' }
# Original legacy header is preserved byte-for-byte. Scope its declarations in a separate copy.
$legacy = Get-Content (Join-Path $indexDir 'index_generated.h') -Raw
$legacy = $legacy.Replace('FLATBUFFERS_GENERATED_INDEX_H_', 'FLATBUFFERS_GENERATED_LEGACY_INDEX_H_')
$legacy = $legacy.Replace('struct FloatVector;', "namespace FragmentsLegacy {`nstruct FloatVector;")
$legacy = $legacy.Replace('#endif  // FLATBUFFERS_GENERATED_LEGACY_INDEX_H_', "} // namespace FragmentsLegacy`n#endif  // FLATBUFFERS_GENERATED_LEGACY_INDEX_H_")
Set-Content (Join-Path $privateDir 'legacy_generated.h') $legacy -Encoding utf8
