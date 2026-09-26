#include "FlowSimTypes.h"

#include "FlowSimShaders.h"
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

			/** Grid-scale damping authored per unit time. */
			DampingRate = 2,

			/** Storm cell inflow authored as a fraction of the speed root. */
			InflowSpeed = 3,

			/** Storm cell inflow authored as a fraction of the target wind. */
			InflowRatio = 4,

			Latest = InflowRatio
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

	/** SimRowStiffness at the equator: the Laplacian's diagonal there, the
	 *  eigenvalue of the grid-scale mode that divergence damping's fraction
	 *  refers to. */
	float EquatorStiffness(const UFlowSimConfig& Config)
	{
		const float DLon = 2.0f * UE_PI / FlowSimShader::GridLongitude(Config.GridLongitude);
		const float DMu = 2.0f / FlowSimShader::GridLatitude(Config.GridLatitude);

		return 2.0f / (DLon * DLon) + 2.0f / (DMu * DMu);
	}

	/** Most of the grid-scale divergence one step may remove: SIM_DAMPING_MAX. */
	constexpr float MaxDampingFraction = 0.45f;
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
	return FMath::Max(Config.DeformationRadius * FMath::Max(Config.PlanetaryVorticity, 0.1f) * 0.70710678f, 1e-3f);
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

// ---------------------------------------------------------------------------
// Grid damping
// ---------------------------------------------------------------------------

float UFlowSimConfig::GetImplicitWeight(float Step) const
{
	const float Authored = FMath::Clamp(ImplicitWeight, 0.5f, 1.0f);

	return (FMath::Clamp(LayerCount, 1, 8) > 1 && Step > FlowSimStep::StackLargeStep)
		? FMath::Max(Authored, FlowSimStep::StackWeight)
		: Authored;
}

float UFlowSimConfig::GetImplicitDampingRate(float Step) const
{
	if (Step <= 0.0f)
	{
		return 0.0f;
	}

	// Per step the off-centred scheme scales a gravity wave by |g|, with
	// |g|^2 = (1 + (1 - a)^2 A) / (1 + a^2 A) and A = (c dt)^2 k^2.
	const float C = FlowSimProfile::WaveSpeed(*this);
	const float A = C * C * Step * Step * EquatorStiffness(*this);
	const float Alpha = GetImplicitWeight(Step);

	return FMath::Loge((1.0f + Alpha * Alpha * A) / (1.0f + (1.0f - Alpha) * (1.0f - Alpha) * A)) / (2.0f * Step);
}

float UFlowSimConfig::GetDivergenceDamping(float Step) const
{
	const float Rate = FMath::Max(GridDamping - GetImplicitDampingRate(Step), 0.0f);

	return FMath::Min(1.0f - FMath::Exp(-Rate * FMath::Max(Step, 0.0f)), MaxDampingFraction);
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

namespace
{
	/** Winds from the values saved before the speed root. */
	void ConvertToSpeedRoot(UFlowSimConfig& Config)
	{
		// A ceiling of 0 turned the ceiling off; the root needs one, so it takes
		// the loosest.
		if (Config.FroudeCeiling <= 0.0f)
		{
			Config.FroudeCeiling = 1.0f;
		}

		// EVERY VALUE AT THE REGIME IT WAS SAVED WITH: the fractions reproduce the
		// same rates, so the converted config runs exactly as it did.
		const float Root = Config.GetSpeedRoot();
		const float Turnover = FMath::Max(Config.DeformationRadius, 0.01f) / Root;

		Config.JetSpeed = Config.JetStrength_DEPRECATED * JetPeak(Config) / Root;
		Config.ShearSpeed = Config.ThermalShear_DEPRECATED * ShapePeak(Config) / Root;

		// Layer 0's forcing-to-drag ratio becomes EddySpeed, and each layer's
		// ratio against it that layer's EddyScale.
		float Reference = 1.0f;

		if (Config.LayerProfiles.Num() > 0 && Config.LayerProfiles[0].ForcingScale_DEPRECATED > 0.0f)
		{
			Reference = Config.LayerProfiles[0].ForcingScale_DEPRECATED / FMath::Max(Config.LayerProfiles[0].DragScale, 1e-3f);
		}

		Config.EddySpeed = Config.ForcingAmplitude_DEPRECATED * Reference / Root;

		for (FFlowLayerProfile& Layer : Config.LayerProfiles)
		{
			Layer.EddyScale = Layer.ForcingScale_DEPRECATED / FMath::Max(Layer.DragScale, 1e-3f) / Reference;
		}

		Config.StormCellSpeed = Config.StormCellWind_DEPRECATED / Root;
		Config.StormCellDriftSpeed = Config.StormCellDrift_DEPRECATED / Root;
		Config.GenesisShearSpeed = Config.GenesisShear_DEPRECATED / Root;
		Config.WindEvaporationGain = Config.WindEvaporation_DEPRECATED * Root;

		const float JetRate = Config.JetStrength_DEPRECATED * (Config.LayerProfiles.Num() > 0 ? Config.LayerProfiles[0].JetScale : 1.0f);

		Config.NoiseDriftSpeed = Config.NoiseDrift_DEPRECATED * JetRate / Root;
		Config.NoiseResetTurnovers = FMath::Max(Config.NoiseResetPeriod_DEPRECATED, 0.05f) / FMath::Max(FMath::Abs(JetRate), 1e-3f) / Turnover;

		UE_LOG(LogFlowSim, Display,
			TEXT("Converted '%s' to the speed root (%.3f): jet %.3f, shear %.3f, eddies %.3f, storm cells %.3f of it. ")
			TEXT("Save the asset to keep the conversion."),
			*Config.GetName(), Root, Config.JetSpeed, Config.ShearSpeed, Config.EddySpeed, Config.StormCellSpeed);
	}
}

void UFlowSimConfig::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FFlowSimConfigVersion::Guid);

	Super::Serialize(Ar);
}

void UFlowSimConfig::PostLoad()
{
	Super::PostLoad();

	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		return;
	}

	const int32 Version = GetLinkerCustomVersion(FFlowSimConfigVersion::Guid);

	if (Version < FFlowSimConfigVersion::SpeedRoot)
	{
		ConvertToSpeedRoot(*this);
	}

	if (Version < FFlowSimConfigVersion::DampingRate)
	{
		// The per-step fraction as a rate at the step it was saved with, plus
		// the implicit scheme's share there, which it was adding to.
		const float Step = GetStepSize();
		const float Fraction = FMath::Clamp(DivergenceDamping_DEPRECATED, 0.0f, MaxDampingFraction);

		GridDamping = -FMath::Loge(1.0f - Fraction) / Step + GetImplicitDampingRate(Step);

		UE_LOG(LogFlowSim, Display,
			TEXT("Converted '%s' to GridDamping %.3f per unit time (step %g). Save the asset to keep the conversion."),
			*GetName(), GridDamping, Step);
	}

	// Earlier versions store the inflow as a fraction of the target wind
	// already; only the speed-root form needs converting.
	if (Version == FFlowSimConfigVersion::InflowSpeed)
	{
		StormCellInflow = FMath::Clamp(StormCellInflow / FMath::Max(StormCellSpeed, 1e-3f), 0.0f, 1.0f);

		UE_LOG(LogFlowSim, Display,
			TEXT("Converted '%s' to StormCellInflow %.3f of the target wind. Save the asset to keep the conversion."),
			*GetName(), StormCellInflow);
	}
}