#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "FlowSimTypes.h"
#include "FlowSnapshot.generated.h"

/** The profile a snapshot was captured under.
 *
 *  RECORDED RATHER THAN ASSUMED because a snapshot's eddies sit on the jets that
 *  existed when it was taken. Restored under a different profile they sit on
 *  jets that are not there, and the nudge re-registers the zonal mean over a few
 *  hundred steps -- quietly, looking wrong in the meantime. So it is reported at
 *  load instead. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FFlowSnapshotProvenance
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

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float ThermalShear = 0.0f;

	// The rest of the skeleton, recorded from format 1: the profile's own jet
	// and thermal shape, the deformation radius, the layers and the perpetual
	// storms' latitudes. Authored values, as the config holds them.

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	EFlowZonalProfile ZonalProfile = EFlowZonalProfile::Banded;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float JetLatitude = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float TradeWindStrength = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float PolarEasterlyStrength = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float JetIrregularity = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float JetHarmonic = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float JetFlatness = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float EquatorialJetWidth = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	EFlowThermalShape ThermalShape = EFlowThermalShape::Midlatitude;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float BaroclinicLatitude = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float BaroclinicWidth = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	float DeformationRadius = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	TArray<FFlowLayerProfile> LayerProfiles;

	/** Each perpetual storm's requested latitude, in list order. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Provenance")
	TArray<float> PerpetualLatitudes;

	/** The profile Config runs under: what a capture records and a restore
	 *  compares against. */
	static FFlowSnapshotProvenance FromConfig(const UFlowSimConfig& Config);

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
			&& FMath::IsNearlyEqual(WidthBias, Other.WidthBias, Tol)
			&& FMath::IsNearlyEqual(ThermalShear, Other.ThermalShear, Tol);
	}

	/** The format 1 fields: the rotation, the deformation radius, the active
	 *  profile's jet shape, the midlatitude zone when it is the thermal shape,
	 *  the layers, and the perpetual storms' count and latitudes. Settings the
	 *  active profile does not read are not compared. */
	bool MatchesSkeleton(const FFlowSnapshotProvenance& Other) const;
};

/** A captured simulation state, as raw floats.
 *
 *  RAW FLOATS AND NOT A TEXTURE ASSET. A texture carries compression, sRGB and
 *  mip settings, and any one applied to a physical field destroys it in a way
 *  that presents as the sim misbehaving. A float array round-trips exactly.
 *
 *  THE LAYOUT IS THE SOLVER'S STATE, one plane per float: u faces, v faces,
 *  the layer thickness, cloud fraction, cloud times formation ascent, vapour,
 *  storm, noise phase A's displacement xyz and phase B's, then the low-passed
 *  ascent and the eye tracer, each plane layer-major, then row, then column;
 *  then the storm cells, eight floats a slot, then their seeds, four a slot.
 *  The layout is told by its size: one without the last two planes restores
 *  them as zero, and one without the seeds restores every hurricane at the
 *  trait ranges' midpoints. See FFlowSimulation::StateFloatsPerCell and
 *  MainCaptureCS.
 *
 *  PITFALL: THE TRACERS ARE STATE TOO. The clouds, moisture and noise the deck
 *  draws are carried by the sim, not derived from the flow; a snapshot without
 *  them restores the winds under an empty sky, which reads as a fresh seed. */
UCLASS(BlueprintType)
class CLOUDATMOSPHERE_API UFlowSnapshot : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Grid this was captured at, layers included. A restore onto a different
	 *  grid is refused rather than resampled. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snapshot")
	FIntVector Grid = FIntVector::ZeroValue;

	/** Length Grid.X * Grid.Y * Grid.Z * FloatsPerCell + TrailingFloats, or a
	 *  legacy layout (TrailingFloatsFor). */
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

	/** What the snapshot records: 0 the jet shape alone, 1 the whole skeleton
	 *  (MatchesSkeleton). The state's layout is still told by its size. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snapshot")
	int32 FormatVersion = 0;

	/** The format a capture writes. */
	static constexpr int32 CurrentFormatVersion = 1;

	/** Floats per cell the current solver stores, and the layout without the
	 *  noise phases' w that still restores. Match FFlowSimulation's; duplicated
	 *  so this header stays free of the render-side one. */
	static constexpr int32 FloatsPerCell = 15;
	static constexpr int32 LegacyFloatsPerCell = 13;

	/** First planes of noise phase A's and phase B's displacement, x then y
	 *  then z, in both layouts. Match MainCaptureCS. */
	static constexpr int32 NoisePlaneA = 7;
	static constexpr int32 NoisePlaneB = 10;

	/** A storm cell slot's state floats, position and intensity, then age and
	 *  the last move; and its seed's, x the seed its traits resolve from. Every
	 *  slot's state comes first, then every slot's seed. Match MainCaptureCS. */
	static constexpr int32 FloatsPerSlot = 8;
	static constexpr int32 SeedFloatsPerSlot = 4;

	/** The storm cells after the planes, and the trailer without the seeds
	 *  that still restores. Match FFlowSimulation's. */
	static constexpr int32 TrailingFloats = (FloatsPerSlot + SeedFloatsPerSlot) * 32;
	static constexpr int32 LegacyTrailingFloats = FloatsPerSlot * 32;

	/** The trailer's length for N cells a plane, or 0 for a layout this solver
	 *  does not restore. */
	int32 TrailingFloatsFor(int32 N) const
	{
		const int32 Num = State.Num();

		return (N <= 0) ? 0
			: (Num == N * FloatsPerCell + TrailingFloats) ? TrailingFloats
			: (Num == N * FloatsPerCell + LegacyTrailingFloats || Num == N * LegacyFloatsPerCell + LegacyTrailingFloats) ? LegacyTrailingFloats
			: 0;
	}

	bool IsValidFor(const FIntVector& InGrid) const
	{
		return Grid == InGrid && TrailingFloatsFor(InGrid.X * InGrid.Y * InGrid.Z) > 0;
	}

	/** Whether Live matches the profile this was captured under, as far as
	 *  FormatVersion records it. */
	bool MatchesProvenance(const FFlowSnapshotProvenance& Live) const;

	/** A transient copy turned east about the pole by Columns grid columns,
	 *  Columns * 360 / Grid.X degrees: every plane rolled, and the noise
	 *  displacements and the storm cells' positions and moves turned with it.
	 *  The sim is zonally symmetric, so the copy starts balanced. The clock and
	 *  provenance are kept. Null when the layout is not one this solver restores.
	 *  PITFALL: perpetual storms are placed from their config, not the state, so
	 *  their Longitude must turn by the same angle or they spin up beside their
	 *  own imprint. */
	UFlowSnapshot* MakeRolled(UObject* Outer, int32 Columns) const;
};
