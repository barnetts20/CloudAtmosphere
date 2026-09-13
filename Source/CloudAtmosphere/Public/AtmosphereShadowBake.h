#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

// What every shadow bake shares, whatever field it walks: the map's shape, and
// how a depth capture describes itself.
//
// A MODEL OWNS ITS OWN PARAMETER STRUCT, SHADER CLASS AND DISPATCH, because
// IMPLEMENT_GLOBAL_SHADER binds one virtual path and the uniforms describing a
// field differ. What must NOT be per model is anything the reader also depends
// on -- the cascade count above all, which already has to agree with a shader
// define and would have a third place to disagree from if each bake kept a copy.

namespace AtmoShadowBake
{
	/** Thread group edge. 8x8 = 64, matching the sim's 2D kernels. Pushed to the
	 *  shader as ATMO_BAKE_THREADS. */
	static constexpr int32 ThreadGroupSize = 8;

	/** Cascade count, sizing both the dispatch and the render target's slice
	 *  count. The shader decides what each level covers.
	 *
	 *  PITFALL: MUST MATCH ATMO_SHADOW_CASCADES in AtmosphereShadowMap.ush, and is NOT
	 *  pushed as a define. The material reads that header too and never passes
	 *  through ModifyCompilationEnvironment, so a define set here would move the
	 *  bake without moving the march that reads it. Edit the pair together. */
	static constexpr int32 CascadeCount = 3;

	/** Slices the target carries when geometry occlusion is on: the deck's
	 *  crossing depths, then one occluder term per cascade.
	 *
	 *  THE SLICE COUNT IS THE FEATURE SWITCH. Both the bake and the reader gate
	 *  on the target's own depth, so a target sized to CascadeCount skips the
	 *  occluder write and the reader's blur taps -- which triple its fetch count
	 *  and run once per light sample -- with nothing else to keep in step. */
	static constexpr int32 SliceCount = CascadeCount * 2;

	/** Slices a target needs for the current setting. */
	inline int32 SlicesFor(bool bOccluders)
	{
		return bOccluders ? SliceCount : CascadeCount;
	}

	/** How far a depth capture reaches past the disc, as a multiple of the
	 *  planet's outer shell. ONLY A LOWER BOUND: the capture has to be at least
	 *  as wide as the shader's ATMO_SHADOW_EXTENT_MARGIN slice, and wider costs
	 *  resolution rather than correctness, so this does not have to track that
	 *  define exactly -- it must simply never be smaller. */
	static constexpr float CaptureExtentMargin = 1.02f;
}


/** One depth capture's placement, planet-local, as the bake reads it.
 *
 *  THE CAPTURE DESCRIBES ITSELF. U and V are the capture image's right and up
 *  axes, taken off the component's own transform rather than rebuilt from the
 *  light, so the bake cannot disagree with the capture about where a texel is
 *  and a mirrored basis is not expressible. Alignment with a cascade is a sizing
 *  convenience that makes the resample an identity; a mismatch costs resolution,
 *  never placement.
 *
 *  Render-thread safe: plain data plus an RHI handle, no UObject. */
struct CLOUDATMOSPHERE_API FAtmoOccluderFrame
{
	FVector3f U = FVector3f(1.0f, 0.0f, 0.0f);
	FVector3f V = FVector3f(0.0f, 1.0f, 0.0f);

	/** Plane centre in (U, V), world units. */
	FVector2f Centre = FVector2f::ZeroVector;

	/** Half-width of the capture, world units. */
	float Extent = 0.0f;

	/** Capture plane's distance from the planet centre, along the light. */
	float PlaneDist = 0.0f;

	/** Far clip, world units from the plane. Background reads at or past it. */
	float Far = 0.0f;

	FTextureRHIRef DepthTexture;

	/** Set once the capture has actually rendered. A level that has never
	 *  captured must stay invalid: a cleared R32F target reads as depth zero,
	 *  which is an occluder sitting on the capture plane and shadows the whole
	 *  level. */
	bool bCaptured = false;

	bool IsUsable() const
	{
		return bCaptured && DepthTexture.IsValid() && Extent > 0.0f && Far > 0.0f;
	}

	// The three float4s the shader unpacks in AtmoOcc_MakeFrame. Changing a
	// layout here means changing it there; there is no binding that checks it.

	FVector4f PackU() const { return FVector4f(U.X, U.Y, U.Z, Extent); }

	FVector4f PackV() const { return FVector4f(V.X, V.Y, V.Z, PlaneDist); }

	FVector4f PackPlane() const
	{
		return FVector4f(Centre.X, Centre.Y, Far, IsUsable() ? 1.0f : 0.0f);
	}
};
