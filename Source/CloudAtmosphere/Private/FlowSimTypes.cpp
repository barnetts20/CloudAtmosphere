#include "FlowSimTypes.h"

#include "FlowSimulation.h"
#include "Serialization/CustomVersion.h"

namespace
{
	/** Versions of UFlowSimConfig's saved data. */
	struct FFlowSimConfigVersion
	{
		enum Type : int32
		{
			Initial = 0,

			/** Winds authored as fractions of the speed root. */
			SpeedRoot = 1,

			Latest = SpeedRoot
		};

		static const FGuid Guid;
	};

	const FGuid FFlowSimConfigVersion::Guid(0x6A1F3C52, 0x9B4E4D17, 0xA2C85E31, 0x7D90B4F6);

	FCustomVersionRegistration GRegisterFlowSimConfigVersion(
		FFlowSimConfigVersion::Guid, FFlowSimConfigVersion::Latest, TEXT("FlowSimConfig"));

	/** Largest |rate(mu) cos(latitude)| over the sphere: the peak eastward wind
	 *  of an angular-rate profile, sampled at row centres in mu. */
	template<typename TRate>
	float PeakWind(TRate&& Rate)
	{
		constexpr int32 Samples = 512;

		float Peak = 0.0f;

		for (int32 i = 0; i < Samples; ++i)
		{
			const float Mu = -1.0f + (i + 0.5f) * (2.0f / Samples);

			Peak = FMath::Max(Peak, FMath::Abs(Rate(Mu)) * FMath::Sqrt(1.0f - Mu * Mu));
		}

		return FMath::Max(Peak, 1e-3f);
	}

	float JetPeak(const UFlowSimConfig& Config)
	{
		return PeakWind([&Config](float Mu) { return FlowSimProfile::JetRate(Config, Mu, 1.0f, Config.EquatorialBoost); });
	}

	float ShapePeak(const UFlowSimConfig& Config)
	{
		return PeakWind([&Config](float Mu) { return FlowSimProfile::ThermalShape(Config, Mu); });
	}
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------

float FlowSimProfile::JetRate(const UFlowSimConfig& Config, float Mu, float Strength, float Boost)
{
	if (Config.ZonalProfile == EFlowZonalProfile::ThreeCell)
	{
		const float A = FMath::Abs(Mu);
		const float Jet = (A - 0.71f) / 0.17f;
		const float Trades = Mu / 0.33f;
		const float Polar = (A - 0.97f) / 0.09f;

		return Strength * (FMath::Exp(-Jet * Jet) - 0.35f * FMath::Exp(-Trades * Trades) - 0.5f * FMath::Exp(-Polar * Polar));
	}

	const float K = Config.BandCount * UE_PI;
	const float Raw = (FMath::Cos(K * Mu) + 0.45f * FMath::Cos(K * 1.7f * Mu) + Config.Asymmetry * FMath::Sin(K * 0.6f * Mu))
		* 0.6897f - Config.WidthBias;

	float Saturated = Raw;
	const float Abs = FMath::Abs(Raw);

	if (Abs > 0.5f)
	{
		const float U = FMath::Clamp(Abs - 0.5f, 0.0f, 1.0f);
		const float U4 = U * U * U * U;
		Saturated = FMath::Sign(Raw) * (0.5f + U - (U4 * U * U - 3.0f * U4 * U + 2.5f * U4));
	}

	return Strength * (Saturated + Boost * FMath::Exp(-Mu * Mu * 12.0f));
}

float FlowSimProfile::ThermalShape(const UFlowSimConfig& Config, float Mu)
{
	if (Config.ThermalShape == EFlowThermalShape::FollowJets)
	{
		return JetRate(Config, Mu, 1.0f, Config.EquatorialBoost);
	}

	const float Offset = (FMath::Abs(FMath::Asin(FMath::Clamp(Mu, -1.0f, 1.0f))) - FMath::DegreesToRadians(Config.BaroclinicLatitude))
		/ FMath::Max(FMath::DegreesToRadians(Config.BaroclinicWidth), 1e-3f);

	return FMath::Exp(-Offset * Offset);
}

float FlowSimProfile::WaveSpeed(const UFlowSimConfig& Config)
{
	return FMath::Max(Config.DeformationRadius * Config.PlanetaryVorticity * 0.70710678f, 1e-3f);
}

// ---------------------------------------------------------------------------
// Speed root
// ---------------------------------------------------------------------------

float UFlowSimConfig::GetSpeedRoot() const
{
	return FMath::Max(FroudeCeiling, 0.1f) * FlowSimProfile::WaveSpeed(*this);
}

float UFlowSimConfig::GetNoiseDriftRate() const
{
	return NoiseDriftSpeed * GetSpeedRoot();
}

float UFlowSimConfig::GetNoiseResetTime() const
{
	return FMath::Max(NoiseResetTurnovers, 0.05f) * FMath::Max(DeformationRadius, 0.01f) / GetSpeedRoot();
}

FFlowSimSpeeds UFlowSimConfig::ResolveSpeeds() const
{
	FFlowSimSpeeds S;

	S.WaveSpeed = FlowSimProfile::WaveSpeed(*this);
	S.Root = GetSpeedRoot();
	S.Turnover = FMath::Max(DeformationRadius, 0.01f) / S.Root;

	S.JetStrength = JetSpeed * S.Root / JetPeak(*this);
	S.ThermalShear = ShearSpeed * S.Root / ShapePeak(*this);

	S.EddySpeed = FMath::Max(EddySpeed, 0.0f) * S.Root;
	S.CellWind = FMath::Max(StormCellSpeed, 0.0f) * S.Root;
	S.CellDrift = FMath::Max(StormCellDriftSpeed, 0.0f) * S.Root;
	S.GenesisShear = FMath::Max(GenesisShearSpeed * S.Root, 0.01f);

	S.WindEvaporation = FMath::Max(WindEvaporationGain, 0.0f) / S.Root;

	return S;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void UFlowSimConfig::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FFlowSimConfigVersion::Guid);

	Super::Serialize(Ar);
}

void UFlowSimConfig::PostLoad()
{
	Super::PostLoad();

	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
		|| GetLinkerCustomVersion(FFlowSimConfigVersion::Guid) >= FFlowSimConfigVersion::SpeedRoot)
	{
		return;
	}

	// A ceiling of 0 turned the ceiling off; the root needs one, so it takes
	// the loosest.
	if (FroudeCeiling <= 0.0f)
	{
		FroudeCeiling = 1.0f;
	}

	// EVERY VALUE AT THE REGIME IT WAS SAVED WITH: the fractions reproduce the
	// same rates, so the converted config runs exactly as it did.
	const float Root = GetSpeedRoot();
	const float Turnover = FMath::Max(DeformationRadius, 0.01f) / Root;

	JetSpeed = JetStrength_DEPRECATED * JetPeak(*this) / Root;
	ShearSpeed = ThermalShear_DEPRECATED * ShapePeak(*this) / Root;

	// Layer 0's forcing-to-drag ratio becomes EddySpeed, and each layer's ratio
	// against it that layer's EddyScale.
	float Reference = 1.0f;

	if (LayerProfiles.Num() > 0 && LayerProfiles[0].ForcingScale_DEPRECATED > 0.0f)
	{
		Reference = LayerProfiles[0].ForcingScale_DEPRECATED / FMath::Max(LayerProfiles[0].DragScale, 1e-3f);
	}

	EddySpeed = ForcingAmplitude_DEPRECATED * Reference / Root;

	for (FFlowLayerProfile& Layer : LayerProfiles)
	{
		Layer.EddyScale = Layer.ForcingScale_DEPRECATED / FMath::Max(Layer.DragScale, 1e-3f) / Reference;
	}

	StormCellSpeed = StormCellWind_DEPRECATED / Root;
	StormCellDriftSpeed = StormCellDrift_DEPRECATED / Root;
	GenesisShearSpeed = GenesisShear_DEPRECATED / Root;
	WindEvaporationGain = WindEvaporation_DEPRECATED * Root;

	const float JetRate = JetStrength_DEPRECATED * (LayerProfiles.Num() > 0 ? LayerProfiles[0].JetScale : 1.0f);

	NoiseDriftSpeed = NoiseDrift_DEPRECATED * JetRate / Root;
	NoiseResetTurnovers = FMath::Max(NoiseResetPeriod_DEPRECATED, 0.05f) / FMath::Max(FMath::Abs(JetRate), 1e-3f) / Turnover;

	UE_LOG(LogFlowSim, Display,
		TEXT("Converted '%s' to the speed root (%.3f): jet %.3f, shear %.3f, eddies %.3f, storm cells %.3f of it. ")
		TEXT("Save the asset to keep the conversion."),
		*GetName(), Root, JetSpeed, ShearSpeed, EddySpeed, StormCellSpeed);
}
