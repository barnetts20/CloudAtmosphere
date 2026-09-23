#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "FlowSnapshot.generated.h"

/** The profile a snapshot was captured under.
 *
 *  RECORDED RATHER THAN ASSUMED because a snapshot's eddies sit on the jets that
 *  existed when it was taken. Restored under a different profile they sit on
 *  jets that are not there, and the nudge re-registers the zonal mean over a few
 *  hundred steps -- quietly, looking wrong in the meantime. So it is reported at
 *  load instead. */
USTRUCT(BlueprintType)
struct FFlowSnapshotProvenance
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float BandCount = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float JetStrength = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float EquatorialBoost = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float Asymmetry = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float WidthBias = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float PlanetaryVorticity = 0.0f;

	/** True when this profile would draw the same jets at the same latitudes.
	 *  Only the parameters that place the jets are compared; the rest change how
	 *  the field evolves, not where its structure sits. */
	bool MatchesShape(const FFlowSnapshotProvenance& Other) const
	{
		const float Tol = 1e-3f;

		return FMath::IsNearlyEqual(BandCount, Other.BandCount, Tol)
			&& FMath::IsNearlyEqual(JetStrength, Other.JetStrength, Tol)
			&& FMath::IsNearlyEqual(EquatorialBoost, Other.EquatorialBoost, Tol)
			&& FMath::IsNearlyEqual(Asymmetry, Other.Asymmetry, Tol)
			&& FMath::IsNearlyEqual(WidthBias, Other.WidthBias, Tol);
	}
};

/** A captured simulation state, as raw floats.
 *
 *  RAW FLOATS AND NOT A TEXTURE ASSET. A texture carries compression, sRGB and
 *  mip settings, and any one applied to a physical field destroys it in a way
 *  that presents as the sim misbehaving. A float array round-trips exactly.
 *
 *  THE LAYOUT IS THE SOLVER'S STATE: u faces, then v faces, then the
 *  geopotential, each layer-major, then row, then column. See
 *  FFlowSimulation::StateFloatsPerCell. */
UCLASS(BlueprintType)
class CLOUDATMOSPHERE_API UFlowSnapshot : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Grid this was captured at. A restore onto a different grid is refused
	 *  rather than resampled. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snapshot")
	FIntVector Grid = FIntVector::ZeroValue;

	/** Length Grid.X * Grid.Y * Grid.Z * 3. */
	UPROPERTY()
	TArray<float> State;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snapshot")
	FFlowSnapshotProvenance Provenance;

	/** Simulated time and step count when captured, so a restored run continues
	 *  the forcing drift rather than snapping it back to zero. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snapshot")
	float SimulatedTime = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snapshot")
	int32 StepsCompleted = 0;

	/** Floats per cell the current solver stores. Matches
	 *  FFlowSimulation::StateFloatsPerCell; duplicated so this header stays free
	 *  of the render-side one. */
	static constexpr int32 FloatsPerCell = 3;

	bool IsValidFor(const FIntVector& InGrid) const
	{
		const int32 N = InGrid.X * InGrid.Y * InGrid.Z;

		return Grid == InGrid && N > 0 && State.Num() == N * FloatsPerCell;
	}
};