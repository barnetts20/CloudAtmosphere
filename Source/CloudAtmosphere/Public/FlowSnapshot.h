#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GasGiantSnapshot.generated.h"

/** The profile a snapshot was captured under.
 *
 *  PROVENANCE IS RECORDED RATHER THAN ASSUMED because a snapshot's eddies sit on
 *  the jets that existed when it was taken. Restored under a different BandCount
 *  or JetStrength they sit on jets that are not there, and since the nudge
 *  re-registers the zonal mean over a few hundred steps the field quietly
 *  corrects itself -- looking wrong in the meantime, then settling subtly
 *  different from what was captured. Silent and slow is the worst combination,
 *  so this is reported at load instead. */
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
	float WidthBias = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float PlanetaryVorticity = 0.0f;

	/** True when this profile would draw the same jets at the same latitudes.
	 *
	 *  Only the parameters that place the jets are compared. PlanetaryVorticity is
	 *  recorded but NOT compared, along with DragRate, NudgeRate and the forcing,
	 *  which are not recorded: all of them change how the field EVOLVES rather
	 *  than where its structure sits. A snapshot restored under different rotation
	 *  or dissipation is still registered correctly on its jets, it just relaxes
	 *  toward a different equilibrium -- a legitimate thing to do deliberately,
	 *  and the recorded value is there to make it visible. */
	bool MatchesShape(const FGasGiantSnapshotProvenance& Other) const
	{
		const float Tol = 1e-3f;

		return FMath::IsNearlyEqual(BandCount, Other.BandCount, Tol)
			&& FMath::IsNearlyEqual(JetStrength, Other.JetStrength, Tol)
			&& FMath::IsNearlyEqual(EquatorialBoost, Other.EquatorialBoost, Tol)
			&& FMath::IsNearlyEqual(Asymmetry, Other.Asymmetry, Tol)
			&& FMath::IsNearlyEqual(WidthBias, Other.WidthBias, Tol);
	}
};

/** A captured simulation state: the whole thing, in two float arrays.
 *
 *  RAW FLOATS AND NOT A TEXTURE ASSET. A UTexture2DArray carries compression
 *  settings, an sRGB flag and mip generation, and any one applied to a physical
 *  field destroys it -- block compression on a vorticity field presents as the
 *  sim misbehaving rather than as an import setting, and nothing about a
 *  wrong-looking flow points at a texture group. A float array round-trips
 *  exactly, so the class of bug is unreachable rather than avoided. At 512x256x3
 *  the pair is about 3 MB uncompressed, small enough to ship a library.
 *
 *  BOTH FIELDS, NOT JUST VORTICITY. Psi is recoverable by inverting the
 *  Laplacian, but recovering it means the cold-start Poisson solve at load:
 *  slow, keeping InitPoissonIterations alive as a runtime concern, and
 *  reproducing the captured psi only to solver tolerance. Doubling the file
 *  removes all three. */
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