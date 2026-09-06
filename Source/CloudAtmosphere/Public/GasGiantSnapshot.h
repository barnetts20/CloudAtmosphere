#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GasGiantSnapshot.generated.h"

/** The profile a snapshot was captured under.
 *
 *  WHY PROVENANCE IS RECORDED RATHER THAN ASSUMED.
 *
 *  A snapshot's eddies sit on the jets that existed when it was taken. Restore
 *  it under a different BandCount or JetStrength and they are sitting on jets
 *  that are no longer there -- and because the nudge re-registers the zonal
 *  mean over a few hundred steps, the field will quietly correct itself while
 *  looking wrong in the meantime, then look subtly different from the state
 *  that was captured.
 *
 *  That failure is silent and slow, which is the worst combination. Recording
 *  what the state was baked under lets it be reported at load instead. */
USTRUCT(BlueprintType)
struct FGasGiantSnapshotProvenance
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
	FVector BandShape = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float PlanetaryVorticity = 0.0f;

	/** True when this profile would draw the same jets at the same latitudes.
	 *
	 *  Only the shape parameters are compared. DragRate, NudgeRate and the
	 *  forcing do not appear, because they change how the field EVOLVES rather
	 *  than where its structure sits -- a snapshot restored under different
	 *  dissipation is still registered correctly, it just relaxes to a
	 *  different equilibrium from where it starts. That is a legitimate thing
	 *  to do deliberately. */
	bool MatchesShape(const FGasGiantSnapshotProvenance& Other) const
	{
		const float Tol = 1e-3f;

		return FMath::IsNearlyEqual(BandCount, Other.BandCount, Tol)
			&& FMath::IsNearlyEqual(JetStrength, Other.JetStrength, Tol)
			&& FMath::IsNearlyEqual(EquatorialBoost, Other.EquatorialBoost, Tol)
			&& FMath::IsNearlyEqual(Asymmetry, Other.Asymmetry, Tol)
			&& BandShape.Equals(Other.BandShape, Tol);
	}
};

/** A captured simulation state: the whole thing, in two float arrays.
 *
 *  WHY RAW FLOATS AND NOT A TEXTURE ASSET.
 *
 *  A UTexture2DArray carries compression settings, an sRGB flag and mip
 *  generation, and any one of them applied to a physical field destroys it.
 *  Block compression on a vorticity field would be catastrophic and would
 *  present as the sim misbehaving rather than as an import setting -- there is
 *  nothing about a wrong-looking flow that points at a texture group. A float
 *  array has none of that surface: it round-trips exactly, always, and the
 *  entire class of bug is unreachable rather than merely avoided.
 *
 *  It also serialises and compresses perfectly well. At 512x256x3 the pair is
 *  about 3 MB uncompressed, which is small enough to ship a library of them.
 *
 *  WHY BOTH FIELDS AND NOT JUST VORTICITY.
 *
 *  Vorticity alone is the complete state in principle -- psi is recoverable by
 *  inverting the Laplacian. But recovering it means running the cold-start
 *  Poisson solve at load, which is slow, keeps InitPoissonIterations alive as a
 *  runtime concern, and reproduces the captured psi only to solver tolerance
 *  rather than exactly. Doubling the file removes all three. */
UCLASS(BlueprintType)
class CLOUDATMOSPHERE_API UGasGiantSnapshot : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Grid this was captured at. A restore onto a different grid is refused
	 *  rather than resampled: there is no way to resample a vorticity field
	 *  that is cheaper or more faithful than re-running the spin-up. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snapshot")
	FIntVector Grid = FIntVector::ZeroValue;

	/** Layer-major, then row, then column. Length Grid.X * Grid.Y * Grid.Z. */
	UPROPERTY()
	TArray<float> Vorticity;

	UPROPERTY()
	TArray<float> Psi;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snapshot")
	FGasGiantSnapshotProvenance Provenance;

	/** Simulated time and step count when captured. Carried so a restored run
	 *  continues the forcing drift from where it left off rather than jumping
	 *  back to zero, which would otherwise snap the forcing pattern. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snapshot")
	float SimulatedTime = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snapshot")
	int32 StepsCompleted = 0;

	int32 ExpectedCount() const { return Grid.X * Grid.Y * Grid.Z; }

	bool IsValidFor(const FIntVector& InGrid) const
	{
		const int32 N = InGrid.X * InGrid.Y * InGrid.Z;

		return Grid == InGrid && N > 0 && Vorticity.Num() == N && Psi.Num() == N;
	}
};
