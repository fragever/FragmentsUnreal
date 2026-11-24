# Fragments wire compatibility

Upstream: https://github.com/jairo-picott/engine_fragment
Pinned revision: `643d24e17d0842765744fe8f66b459a2ba734dbc`
Package version at this revision: `@thatopen/fragments` 3.6.1.
Upstream schema: `packages/fragments/flatbuffers/index.fbs`.
License: MIT; see LICENSE.md in this directory.

## Readers

The original `../index_generated.h` remains unchanged. A mechanically namespaced
copy is generated under Private/Index for independent legacy verification.
`fragments361.fbs` is the pinned upstream schema with only a namespace added.
`modern.fbs` describes the same wire layout before `profiles_face_ids` became
required. This intermediate exporter is used by existing models in the project;
its exact producer package version cannot be inferred from the layout.
Some intermediate exporters also retained the legacy root while adding shell
extensions. These pass the independent legacy verifier and the common shell
checks and are logged as `legacy schema with extended shells`.

`compatibility.fbs` is a read-only common wire view, not an export format:

* The shared root prefix and all shared struct layouts retain their field slots.
* Root slots 12 and 13 are deprecated in this view. Legacy alignments/geometries
  and modern unique_attributes/relation_names occupy those same slots with
  incompatible types. The importer does not consume those lookup arrays.
* Added shell fields are optional so legacy shells can be read. Required fields
  are still enforced by the selected independent wire verifier.
* Modern indexes are structurally verified but not used to change import behavior.

`FragmentSchema::Identify` checks the common view, geometry and instance
references, and the independent schema verifiers before exposing any model.
Normal shells use 16-bit indices; BIG shells use 32-bit indices. Out-of-range
indices and values outside Unreal's signed int32 range are rejected.
The shared view reads the original buffer without rebuilding it. Existing
FModelBufferPin ownership keeps its geometry pointers alive across worker tasks.

Both upstream versions use identifier `0001`. A buffer with only fields common
to both schemas can pass both verifiers; it is reported as shared/ambiguous.
Log labels identify wire schemas, not an unverified package version.

After raw-file reading or zlib inflation, the loader logs the detected schema.
Buffer-only loading logs the schema too. Identification requires full-buffer
verification, including a second identification at wrapper loading for callers
that bypass file decompression; this adds validation work but no model copy.

Selection, filters, cell partitioning, material handling and instancing continue
to consume the same model prefix, local IDs and sample indices. Both shell paths
use FShellSource; optional face IDs are retained there. This change does not
introduce new face-group shading behavior. Properties and relations are decoded
as JSON, preserving quoted commas, nested values and optional type entries.

## Regeneration

Use flatc 25.2.10, matching ThirdParty/FlatBuffers. From the plugin root:

```powershell
./Source/FragmentsUnreal/Public/Index/GenerateSchemas.ps1 -Flatc C:/Users/jairo.picott/engine_fragments/packages/fragments/flatbuffers/flatc.exe
```

Do not overwrite the original legacy header. Review wire-slot changes before
updating the pinned schema or the compatibility view.

## Validation

Build MyProjectEditor for Win64 Development with Unreal 5.7. Run the
`FragmentsUnreal` automation suite. `FragmentsUnreal.Importer.MultiVersion`
constructs modern fixtures that also pass the strict latest upstream verifier,
checks normal and BIG shells with high-index holes, imports sample/category
data through the existing importer, checks JSON attributes, pins buffers across
reload, and rejects corrupt geometry and truncated files. The existing suite
covers legacy loading, visibility, selection, filters, overrides and editor flows.
Rendered appearance and platform/engine versions other than this test target
require their usual representative-model checks.
