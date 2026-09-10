#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

class FRDGBuilder;

/** Everything the transmittance bake reads.
 *
 *  THE PINS, NOT THE DERIVED PROFILE. AtmoT_Profile turns the authored alphas
 *  into scale heights on both sides; converting here would be a third copy of
 *  that derivation. The radii are the same values the material builds its
 *  planetRadius and atmoRadius pins from. */
struct CLOUDATMOSPHERE_API FAtmosphereTransmittanceParams
{
	float PlanetRadius = 0.0f;
	float AtmosphereRadius = 0.0f;

	/** (RayleighBeta.A, MieBeta.A, AbsorptionBeta.A, AbsorptionFalloff). */
	FVector4f ProfilePins = FVector4f::Zero();

	/** The bake's destination. Its identity is part of what a bake is keyed
	 *  on: a recreated resource comes back cleared. */
	FTextureRHIRef Table;

	bool IsUsable() const
	{
		return Table.IsValid()
			&& PlanetRadius > 0.0f
			&& AtmosphereRadius > PlanetRadius;
	}

	/** Whether a table baked from Other is valid for these inputs. */
	bool Matches(const FAtmosphereTransmittanceParams& Other) const
	{
		return Table == Other.Table
			&& PlanetRadius == Other.PlanetRadius
			&& AtmosphereRadius == Other.AtmosphereRadius
			&& ProfilePins == Other.ProfilePins;
	}
};

/** Names must match AtmosphereTransmittance.usf exactly. */
BEGIN_SHADER_PARAMETER_STRUCT(FAtmosphereTransmittanceParameters, )

SHADER_PARAMETER(float, TransmittancePlanetRadius)
SHADER_PARAMETER(float, TransmittanceAtmosphereRadius)
SHADER_PARAMETER(FVector4f, TransmittanceProfilePins)

SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, TransmittanceUAV)

END_SHADER_PARAMETER_STRUCT()

class FAtmosphereTransmittanceCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FAtmosphereTransmittanceCS);

public:
	using FParameters = FAtmosphereTransmittanceParameters;
	SHADER_USE_PARAMETER_STRUCT(FAtmosphereTransmittanceCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

namespace AtmosphereTransmittance
{
	/** Table size. MUST MATCH ATMO_TRANSMITTANCE_WIDTH and _HEIGHT in
	 *  AtmosphereTransmittance.ush, which the lookup's texel-centre remap reads.
	 *  Width is the cosine axis, height the altitude axis. */
	static constexpr int32 Width = 256;
	static constexpr int32 Height = 64;

	static constexpr int32 ThreadGroupSize = 8;

	/** Pixel format of the table. Float32: it is 256 KB, and the horizon rows
	 *  hold the largest integrals in the table. */
	static constexpr EPixelFormat Format = PF_A32B32G32R32F;

	/** Enqueues the bake. Game thread. Params are validated here. */
	CLOUDATMOSPHERE_API void RequestBake(const FAtmosphereTransmittanceParams& Params);
}
