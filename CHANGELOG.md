# Changelog

## [1.0.0] - 2026-10-23

Initial open-source release.

- Fragment 2.0 `.frag` import for OpenBIM/IFC workflows.
- Unreal Engine 5.4 and later runtime and editor modules.
- Asynchronous loading, chunked processing, shell-first streaming, HISM
  registration, cell merging, proxy tiers, and metadata queries.
- Continuous, view-driven LOD: each model moves between its LOD2 proxy, LOD1
  merged cells and LOD0 instances by screen-space error, with no click needed.
- Blueprint-facing lifecycle, selection, visibility, and diagnostics, including
  `ResetViewerSession`, `AbortAllProcessing`, `SetModelUserVisible` /
  `IsModelUserVisible`, `AFragment::GetWorldBoundsByLocalId` and
  `AFragment::TryPromoteModelFromHit`.
- Editor: imported models stream through the same LOD path as runtime, driven by
  the editor viewport cameras (`r.Fragments.EditorStreaming`); a model shows LOD0
  while its materials are being edited; undo/redo for material and placement
  edits; non-modal import progress with failure notifications; material
  drag-and-drop and live placement fields in the Fragments panel.

### Removed

The click-driven activation API from the pre-release `main` branch is gone;
continuous LOD replaces it. To force a model to full detail at any distance, use
`PromoteAllCellsForModel`.

- `SetActiveModel`, `DeactivateActiveModel`, `GetActiveModelGuid`, `OnActiveModelChanged`
- `SetModelActive`, `IsModelActive`, `OnModelActiveStateChanged`, `OnModelActivationReady`
- `TryActivateProxyFromHit`, `TryActivateProxyFromHitAdditive`, `TryDeactivateActiveModel`
- Apache License 2.0 for original project code and documentation.
