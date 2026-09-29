#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

// What every shadow bake shares, whatever field it walks: the map's shape and
// the history blend.
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
	 *  count, one slice per cascade. The shader decides what each level covers.
	 *
	 *  PITFALL: MUST MATCH ATMO_SHADOW_CASCADES in AtmosphereShadowMap.ush, and is NOT
	 *  pushed as a define. The march reads that header too, through its own
	 *  shader class, so a define pushed by the bake alone would move the bake
	 *  without moving the march that reads it. Edit the pair together. */
	static constexpr int32 CascadeCount = 3;

	/** Ceiling on the share of a level's previous bake a rebake keeps. At 1 the
	 *  map would never change. */
	static constexpr float MaxHistoryWeight = 0.9f;

	/** Light turn, as a cosine, past which a level's previous bake is dropped
	 *  rather than reprojected. About 2 degrees. */
	static constexpr float HistoryLightCosine = 0.9994f;

	/** A one-slice copy of Level, taken before that level's pass writes it: the
	 *  pass reads its previous bake at reprojected texels, which other threads
	 *  of the same pass are overwriting. Render thread. */
	inline FRDGTextureRef AddHistoryCopy(FRDGBuilder& GraphBuilder, FRDGTextureRef Map, int32 Level)
	{
		const FRDGTextureDesc& MapDesc = Map->Desc;

		FRDGTextureRef History = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2DArray(MapDesc.Extent, MapDesc.Format,
				FClearValueBinding::None, TexCreate_ShaderResource, 1),
			TEXT("Atmosphere.ShadowHistory"));

		FRHICopyTextureInfo Copy;
		Copy.Size = FIntVector(MapDesc.Extent.X, MapDesc.Extent.Y, 1);
		Copy.SourceSliceIndex = Level;
		Copy.DestSliceIndex = 0;
		Copy.NumSlices = 1;

		AddCopyTexturePass(GraphBuilder, Map, History, Copy);

		return History;
	}
}

/** What a level's previous bake was made with, and how much of it the next bake
 *  keeps. Weight 0 discards it: first bake into a target, a light jump, or
 *  smoothing off. */
struct FAtmoShadowHistory
{
	FVector3f LightDir = FVector3f(0.0f, 0.0f, 1.0f);
	FVector3f CameraLocal = FVector3f::ZeroVector;
	float Weight = 0.0f;
};