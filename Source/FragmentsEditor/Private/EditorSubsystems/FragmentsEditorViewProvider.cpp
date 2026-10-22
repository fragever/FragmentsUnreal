// Copyright 2026 Jairo Andres Basto Picott and Arkoun Merchant
#include "EditorSubsystems/FragmentsEditorViewProvider.h"

#include "Editor.h"
#include "LevelEditorViewport.h"
#include "SceneView.h"
#include "ConvexVolume.h"
#include "Spatial/FragmentScreenSpaceError.h"

namespace FragmentsEditorViewProvider
{
    namespace
    {
        /** Last-known view state for one level viewport, plus the frustum/SseCoeff CalcSceneView derived from it. */
        struct FCachedViewportView
        {
            FVector       Location = FVector::ZeroVector;
            FRotator      Rotation = FRotator::ZeroRotator;
            FIntPoint     Size = FIntPoint::ZeroValue;
            float         ViewFOV = 0.0f;
            FConvexVolume Frustum;
            bool          bHaveFrustum = false;
            float         SseCoeff = 0.0f;
        };

        /** Module-local: Gather is called repeatedly from the same importer tick timer, so the cache must survive across calls. */
        TMap<FLevelEditorViewportClient*, FCachedViewportView> GViewCache;
    }

    void GatherFromLevelViewports(UWorld* World, TArray<FFragmentViewInfo>& OutViews)
    {
        if (!GEditor || !World) { return; }

        TSet<FLevelEditorViewportClient*> SeenThisCall;

        for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
        {
            if (!Client) { continue; }
            if (Client->GetWorld() != World) { continue; }
            if (!Client->IsPerspective()) { continue; }
            if (!Client->IsVisible()) { continue; }

            FViewport* Viewport = Client->Viewport;
            if (!Viewport) { continue; }

            SeenThisCall.Add(Client);

            // Cheap, side-effect-free reads used only to decide whether CalcSceneView is needed.
            const FVector CurLocation = Client->GetViewLocation();
            const FRotator CurRotation = Client->GetViewRotation();
            const FIntPoint CurSize = Viewport->GetSizeXY();
            // Include ViewFOV in the cache key so FOV changes refresh the frustum.
            const float CurViewFOV = Client->ViewFOV;

            FFragmentViewInfo View;
            View.CameraLocation = CurLocation;
            // Rotation feeds the still-camera early-out.
            View.CameraRotation = CurRotation;

            const FCachedViewportView* Existing = GViewCache.Find(Client);
            const bool bReuseCache = Existing
                && Existing->Location.Equals(CurLocation)
                && Existing->Rotation.Equals(CurRotation)
                && Existing->Size == CurSize
                && FMath::IsNearlyEqual(Existing->ViewFOV, CurViewFOV);

            if (bReuseCache)
            {
                View.ViewFrustum = Existing->Frustum;
                View.bHaveFrustum = Existing->bHaveFrustum;
                View.SseCoeff = Existing->SseCoeff;
            }
            else
            {
                // Same fallback the runtime producer uses: ~1080p at 90 degrees.
                View.SseCoeff = FragmentSSE::SseCoeff(1080.0f, 1.0f);

                FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
                    Viewport, Client->GetScene(), Client->EngineShowFlags)
                    .SetRealtimeUpdate(Client->IsRealtime())
                    // Set view time so CalcSceneView does not trigger a per-tick ensure in empty viewports.
                    .SetTime(FGameTime::GetTimeSinceAppStart()));

                if (FSceneView* SceneView = Client->CalcSceneView(&ViewFamily))
                {
                    GetViewFrustumBounds(View.ViewFrustum,
                        SceneView->ViewMatrices.GetViewProjectionMatrix(), /*bUseNearPlane=*/false);
                    View.bHaveFrustum = true;

                    const float HeightPx = static_cast<float>(CurSize.Y);
                    const float ProjM11 = static_cast<float>(
                        SceneView->ViewMatrices.GetProjectionMatrix().M[1][1]);
                    if (HeightPx > 0.0f && ProjM11 > 0.0f)
                    {
                        View.SseCoeff = FragmentSSE::SseCoeff(HeightPx, ProjM11);
                    }
                }

                FCachedViewportView& NewEntry = GViewCache.FindOrAdd(Client);
                NewEntry.Location = CurLocation;
                NewEntry.Rotation = CurRotation;
                NewEntry.Size = CurSize;
                NewEntry.ViewFOV = CurViewFOV;
                NewEntry.Frustum = View.ViewFrustum;
                NewEntry.bHaveFrustum = View.bHaveFrustum;
                NewEntry.SseCoeff = View.SseCoeff;
            }

            // Always emit, cache hit or miss: UpdateRegistration owns the still-camera decision (and honours bForceNextUpdate, set by RegisterModel so a newly-loaded model gets one full pass).
            OutViews.Add(MoveTemp(View));
        }

        // Remove closed viewports so recycled pointers cannot reuse stale view data.
        for (auto It = GViewCache.CreateIterator(); It; ++It)
        {
            if (!SeenThisCall.Contains(It->Key))
            {
                It.RemoveCurrent();
            }
        }
    }
}
