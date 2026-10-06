#include "FlowSnapshot.h"

#include "UObject/Package.h"

namespace
{
	constexpr float ProvenanceTolerance = 1e-3f;

	bool Near(float A, float B)
	{
		return FMath::IsNearlyEqual(A, B, ProvenanceTolerance);
	}

	bool SameLayers(const FFlowLayerProfile& A, const FFlowLayerProfile& B)
	{
		return Near(A.JetScale, B.JetScale) && Near(A.BoostScale, B.BoostScale) && Near(A.EddyScale, B.EddyScale)
			&& Near(A.DragScale, B.DragScale) && Near(A.DepthScale, B.DepthScale);
	}

	/** Turns the vector (X, Y, z) about the pole, z unchanged. */
	void TurnAboutPole(float& X, float& Y, double Cos, double Sin)
	{
		const double X0 = X;
		const double Y0 = Y;

		X = (float)(X0 * Cos - Y0 * Sin);
		Y = (float)(X0 * Sin + Y0 * Cos);
	}
}

FFlowSnapshotProvenance FFlowSnapshotProvenance::FromConfig(const UFlowSimConfig& Config)
{
	const FFlowSimScales Speeds = Config.ResolveScales();

	FFlowSnapshotProvenance Out;
	Out.BandCount = Config.BandCount;
	Out.JetStrength = Speeds.JetStrength;
	Out.EquatorialBoost = Config.EquatorialBoost;
	Out.Asymmetry = Config.Asymmetry;
	Out.WidthBias = Config.WidthBias;
	Out.PlanetaryVorticity = Config.PlanetaryVorticity;
	Out.ThermalShear = Speeds.ThermalShear;

	Out.ZonalProfile = Config.ZonalProfile;
	Out.JetLatitude = Config.JetLatitude;
	Out.TradeWindStrength = Config.TradeWindStrength;
	Out.PolarEasterlyStrength = Config.PolarEasterlyStrength;
	Out.JetIrregularity = Config.JetIrregularity;
	Out.JetHarmonic = Config.JetHarmonic;
	Out.JetFlatness = Config.JetFlatness;
	Out.EquatorialJetWidth = Config.EquatorialJetWidth;
	Out.ThermalShape = Config.ThermalShape;
	Out.BaroclinicLatitude = Config.BaroclinicLatitude;
	Out.BaroclinicWidth = Config.BaroclinicWidth;
	Out.DeformationRadius = Config.DeformationRadius;
	Out.LayerProfiles = Config.LayerProfiles;

	for (const FFlowPerpetualStorm& Storm : Config.PerpetualStorms)
	{
		Out.PerpetualLatitudes.Add(Storm.Latitude);
	}

	return Out;
}

bool FFlowSnapshotProvenance::MatchesSkeleton(const FFlowSnapshotProvenance& Other) const
{
	if (ZonalProfile != Other.ZonalProfile || ThermalShape != Other.ThermalShape
		|| !Near(PlanetaryVorticity, Other.PlanetaryVorticity) || !Near(DeformationRadius, Other.DeformationRadius))
	{
		return false;
	}

	const bool bJets = (ZonalProfile == EFlowZonalProfile::ThreeCell)
		? Near(JetLatitude, Other.JetLatitude) && Near(TradeWindStrength, Other.TradeWindStrength)
			&& Near(PolarEasterlyStrength, Other.PolarEasterlyStrength)
		: Near(JetIrregularity, Other.JetIrregularity) && Near(JetHarmonic, Other.JetHarmonic)
			&& Near(JetFlatness, Other.JetFlatness) && Near(EquatorialJetWidth, Other.EquatorialJetWidth);

	const bool bZone = (ThermalShape != EFlowThermalShape::Midlatitude)
		|| (Near(BaroclinicLatitude, Other.BaroclinicLatitude) && Near(BaroclinicWidth, Other.BaroclinicWidth));

	if (!bJets || !bZone
		|| LayerProfiles.Num() != Other.LayerProfiles.Num()
		|| PerpetualLatitudes.Num() != Other.PerpetualLatitudes.Num())
	{
		return false;
	}

	for (int32 i = 0; i < LayerProfiles.Num(); ++i)
	{
		if (!SameLayers(LayerProfiles[i], Other.LayerProfiles[i]))
		{
			return false;
		}
	}

	for (int32 i = 0; i < PerpetualLatitudes.Num(); ++i)
	{
		if (!Near(PerpetualLatitudes[i], Other.PerpetualLatitudes[i]))
		{
			return false;
		}
	}

	return true;
}

bool UFlowSnapshot::MatchesProvenance(const FFlowSnapshotProvenance& Live) const
{
	return Provenance.MatchesShape(Live) && (FormatVersion < 1 || Provenance.MatchesSkeleton(Live));
}

UFlowSnapshot* UFlowSnapshot::MakeRolled(UObject* Outer, int32 Columns) const
{
	if (!IsValidFor(Grid))
	{
		return nullptr;
	}

	const int64 Width = Grid.X;
	const int64 Cells = Width * Grid.Y * Grid.Z;
	const int32 Trailing = TrailingFloatsFor((int32)Cells);
	const int64 Planes = (State.Num() - Trailing) / Cells;
	const int64 Shift = ((Columns % Width) + Width) % Width;

	// East is +Z by the right hand: SimDirection puts column I at longitude
	// (I + 0.5) * 2 pi / Width.
	const double Angle = UE_DOUBLE_TWO_PI * (double)Shift / (double)Width;
	const double Cos = FMath::Cos(Angle);
	const double Sin = FMath::Sin(Angle);

	UFlowSnapshot* Out = NewObject<UFlowSnapshot>(Outer ? Outer : GetTransientPackage(), NAME_None, RF_Transient);
	Out->Grid = Grid;
	Out->Provenance = Provenance;
	Out->SimulatedTime = SimulatedTime;
	Out->StepsCompleted = StepsCompleted;
	Out->FormatVersion = FormatVersion;
	Out->State.SetNumUninitialized(State.Num());

	const float* Src = State.GetData();
	float* Dst = Out->State.GetData();

	// Every row of every plane, layer and latitude: column C lands on C + Shift.
	for (int64 Row = 0; Row < Planes * Cells / Width; ++Row)
	{
		const float* From = Src + Row * Width;
		float* To = Dst + Row * Width;

		FMemory::Memcpy(To + Shift, From, (Width - Shift) * sizeof(float));
		FMemory::Memcpy(To, From + (Width - Shift), Shift * sizeof(float));
	}

	for (const int32 Plane : { NoisePlaneA, NoisePlaneB })
	{
		float* X = Dst + Plane * Cells;
		float* Y = X + Cells;

		for (int64 i = 0; i < Cells; ++i)
		{
			TurnAboutPole(X[i], Y[i], Cos, Sin);
		}
	}

	// The seeds, after the slots' state, carry over as they are.
	const int64 Trailer = Planes * Cells;
	FMemory::Memcpy(Dst + Trailer, Src + Trailer, Trailing * sizeof(float));

	// Position xyz and intensity, then age and the move xyz; a free slot is zero.
	for (int32 Slot = 0; Slot < LegacyTrailingFloats / FloatsPerSlot; ++Slot)
	{
		float* S = Dst + Trailer + Slot * FloatsPerSlot;

		TurnAboutPole(S[0], S[1], Cos, Sin);
		TurnAboutPole(S[5], S[6], Cos, Sin);
	}

	return Out;
}
