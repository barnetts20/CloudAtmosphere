//PARAM STRUCTS SO WE DONT HAVE TO PASS IN A BILLION PARAMETERS TO THE MAIN FUNCTION CALLS
struct CloudParams {
    float cloudScale;
    float2 cloudHeightCurve;
    float cloudCoverage;
    float cloudDensityMult;
    float4 cloudNoiseWeights;
    float4 cloudNoiseInvert;
    float4 detailNoiseWeights;
    float4 detailNoiseInvert;
    float detailNoiseScale;
    float detailNoiseErode;
    float4 animationWeights;
    float4 cloudPhaseParams;
    float3 cloudScatBeta;
    float3 cloudAbsBeta;
    float3 cloudAmb;
};

struct PlanetParams{
    float3 planetPos;
    float planetRadius;
    float planetRadiusSq;
    float atmoRadius;
    float cloudOuterRadius;
    float cloudInnerRadius;
    float cloudOuterRadiusSq; 
    float cloudInnerRadiusSq; 
};

struct ScatterParams {
    float cosTheta;
    float3 rayBeta; 
    float3 mieBeta; 
    float3 absBeta; 
    float atmoAmbient; 
    float rayScaleH; 
    float mieScaleH; 
    float absHeight; 
    float absFalloff; 
    float phaseR; 
    float phaseM; 
};

struct MainRayParams {
    float2 svxy;
    float3 originalCameraPos;
    float3 cameraPos; 
    float3 cameraDir; 
    float3 lightCol;  
    float sceneDepth; 
    int atmoSteps; 
    int cloudSteps;
    float jitterFactor; 
    float stepScaleFactor; 
};

struct LightRayParams {
    float3 samplePos; 
    float3 lightDir;
    int atmoLightSteps; 
    int cloudLightSteps; 
    float jitterFactor;
    float stepSizeMult;
    int parentStepIndex;
};

struct CompositeParams{
    PlanetParams planetParams;
    ScatterParams scatteringParams;
    CloudParams cloudParams;
    MainRayParams mainRayParams;
    LightRayParams lightRayParams;
};

//HELPER METHODS
struct SharedFunctions {
    Texture3D cloudDensityTex;
    SamplerState cloudDensitySampler;

    //Ray sphere intersect used to compose our marching plans
    float2 RaySphere(float3 origin, float3 dir, float3 center, float radius) {
        float3 oc = origin - center;
        float b = dot(oc, dir);
        float c = dot(oc, oc) - radius * radius;
        float disc = b * b - c;
        if (disc < 0.0) return float2(-1.0, -1.0);
        float sq = sqrt(disc);
        return float2(-b - sq, -b + sq);
    }

    //Rayleigh phase function
    float PhaseRayleigh(float cosTheta) {
        return 3.0 / (16.0 * 3.14159265) * (1.0 + cosTheta * cosTheta);
    }

    //HG phase function for mie and also used inside the dual lobe function
    float PhaseHG(float cosTheta, float g) {
        float g2 = g * g;
        return (1.0 - g2) / (4.0 * 3.14159265 * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5));
    }

    //Dual lobe phase function used for cloud phase
    float PhaseDualLobe(float cosTheta, float gForward, float gBack, float forwardWeight) {
        float forward = PhaseHG(cosTheta, gForward);
        float backward = PhaseHG(cosTheta, -gBack);
        return max(lerp(backward, forward, forwardWeight), .05);
    }

    //Sample atmosphere density
    float3 AtmoDensity(float height, float rayScaleH, float mieScaleH, float absHeight, float absFalloff) {
        float rayDensity = exp(-height / rayScaleH);
        float mieDensity = exp(-height / mieScaleH);
        float safeFalloff = max(absFalloff, 0.0001);
        float denom = (absHeight - height) / safeFalloff;
        float absDensity = (1.0 / (denom * denom + 1.0)) * rayDensity;
        return float3(rayDensity, mieDensity, absDensity);
    }

    //Sample cloud density
    float CloudDensity(float distFromCenter, float3 samplePos, CompositeParams input) {
        float cloudNormH = (distFromCenter - input.planetParams.cloudInnerRadius) / (input.planetParams.cloudOuterRadius - input.planetParams.cloudInnerRadius);
        
        // Flat-top height curve: full density in middle, smooth fade at bottom (0 to curve.x) and top (curve.y to 1)
        float bottomFade = saturate(smoothstep(0.0, input.cloudParams.cloudHeightCurve.x, cloudNormH));
        float topFade = saturate(smoothstep(1.0, input.cloudParams.cloudHeightCurve.y, cloudNormH));
        float heightGradient = bottomFade * topFade;

        // Reconstruct world pos, we do not want to sample the noise in camera relative space
        float3 worldPos = samplePos + input.mainRayParams.originalCameraPos;

        // Modify uvw sample scale by our cloud scaling factor
        float3 uvw = worldPos / (input.planetParams.cloudOuterRadius * input.cloudParams.cloudScale);

        // Apply animation offsets to first sample
        uvw += input.cloudParams.animationWeights.rgb * View.RealTime;

        // Sample first layer of noise, calculate inverted value, blend between the two based on cloudNoiseInvert channel values
        float4 cSample = cloudDensityTex.Sample(cloudDensitySampler, uvw);
        float4 icSample = 1 - cSample;
        float4 fcSample = lerp(cSample, icSample, input.cloudParams.cloudNoiseInvert);

        // R channel is the largest octave, so we use it for blocking out the overall cloud shapes
        float baseNoise = fcSample.r;
        
        // Modulate by the more detailed GB channels
        float detail = (fcSample.g * input.cloudParams.cloudNoiseWeights.r + fcSample.b * input.cloudParams.cloudNoiseWeights.g + fcSample.a * input.cloudParams.cloudNoiseWeights.b);
        
        // Multiply by the A channel, controls erosion amount
        float noise = saturate(baseNoise - detail * input.cloudParams.cloudNoiseWeights.a);

        // Apply height gradient before coverage threshold so clouds round at edges without shrinking
        float remapped = saturate(heightGradient * (noise - (1.0 - input.cloudParams.cloudCoverage)) / input.cloudParams.cloudCoverage);

        // Threshold first, this avoids additional fine detail texture samples if we are not "inside" a cloud
        if (remapped > 0) {

            // Modify uvw sample scale by our detail scaling factor
            float3 uvwDetail = uvw * input.cloudParams.detailNoiseScale;
            // Apply another layer of animation offsets, scaled by a factor. 
            // This will cause the detail to scroll at different speeds from the larger noise, creating a "churning" effect
            uvwDetail += input.cloudParams.animationWeights.rgb * View.RealTime * input.cloudParams.animationWeights.a; //TODO: PARAMETERIZE MULTIPLIER AND EXPOSE

            // Sample second layer of noise, calculate inverted value, blend between the two based on detailNoiseInvert channel values
            float4 dSample = cloudDensityTex.Sample(cloudDensitySampler, uvwDetail);
            float4 idSample = 1 - dSample;
            float4 fdSample = lerp(dSample, idSample, input.cloudParams.detailNoiseInvert);
            // Incorporate the noise weights for each channel
            float fineDetail = dot(fdSample, input.cloudParams.detailNoiseWeights);
            // Erode from first layer magnified by the detailNoiseErode multiplier
            remapped = saturate(remapped - fineDetail * input.cloudParams.detailNoiseErode);
        }

        //Apply the final couldDensityMult and return
        return remapped * input.cloudParams.cloudDensityMult;
    }

    //Computes adaptive step sizing based on position along the total rayLength, growthRate increases step sizes as it gets further along the ray
    float AdaptiveStepSize(float baseStep, float pos, float rayStart, float rayLength, float growthRate) {
        float t = saturate((pos - rayStart) / rayLength);
        return baseStep * (1.0 + t * growthRate);
    }
};

//LIGHT MARCH
struct LightMarchFunctions {
    float4 segments[5];
    int segmentCount;
    bool planetShadowed;

    //Pre-plans the march for a light ray
    void Plan(CompositeParams input, SharedFunctions sf) {
        segmentCount = 0;
        planetShadowed = false;
        float2 planetHit = sf.RaySphere(input.lightRayParams.samplePos, input.lightRayParams.lightDir, input.planetParams.planetPos, input.planetParams.planetRadius);
        if (planetHit.x > 0.0) {
            planetShadowed = true;
            return;
        }
        float2 atmoHit = sf.RaySphere(input.lightRayParams.samplePos, input.lightRayParams.lightDir, input.planetParams.planetPos, input.planetParams.atmoRadius);
        if (atmoHit.y < 0.0) return;
        float2 cloudOHit = sf.RaySphere(input.lightRayParams.samplePos, input.lightRayParams.lightDir, input.planetParams.planetPos, input.planetParams.cloudOuterRadius);
        float2 cloudIHit = sf.RaySphere(input.lightRayParams.samplePos, input.lightRayParams.lightDir, input.planetParams.planetPos, input.planetParams.cloudInnerRadius);
        float atmoStart = max(atmoHit.x, 0.0);
        float atmoEnd = atmoHit.y;
        if (atmoStart >= atmoEnd) return;
        bool hasCloudOuter = cloudOHit.y > 0.0;
        bool hasCloudInner = cloudIHit.y > 0.0;
        if (!hasCloudOuter) {
            float stepSize = (atmoEnd - atmoStart) / float(input.lightRayParams.atmoLightSteps);
            segments[0] = float4(atmoStart, atmoEnd, stepSize, 0.0);
            segmentCount = 1;
            return;
        }
        float co_near = max(cloudOHit.x, 0.0);
        float co_far = min(cloudOHit.y, atmoEnd);
        float ci_near = hasCloudInner ? max(cloudIHit.x, 0.0) : -1.0;
        float ci_far = hasCloudInner ? min(cloudIHit.y, atmoEnd) : -1.0;
        float boundaries[6];
        int bCount = 0;
        boundaries[bCount++] = atmoStart;
        if (co_near > atmoStart && co_near < atmoEnd) boundaries[bCount++] = co_near;
        if (hasCloudInner && ci_near > atmoStart && ci_near < atmoEnd && ci_near > co_near) boundaries[bCount++] = ci_near;
        if (hasCloudInner && ci_far > atmoStart && ci_far < atmoEnd && ci_far > ci_near) boundaries[bCount++] = ci_far;
        if (co_far > atmoStart && co_far < atmoEnd) boundaries[bCount++] = co_far;
        boundaries[bCount++] = atmoEnd;
        float totalCloudDist = 0.0;
        float totalAtmoDist = 0.0;
        for (int i = 0; i < bCount - 1; i++) {
            float segStart = boundaries[i];
            float segEnd = boundaries[i + 1];
            if (segEnd <= segStart) continue;
            float midDist = (segStart + segEnd) * 0.5;
            float3 midPos = input.lightRayParams.samplePos + input.lightRayParams.lightDir * midDist;
            float midRadius = length(midPos - input.planetParams.planetPos);
            bool inCloudBand = (midRadius >= input.planetParams.cloudInnerRadius && midRadius <= input.planetParams.cloudOuterRadius);
            if (inCloudBand) {
                totalCloudDist += (segEnd - segStart);
            } else {
                totalAtmoDist += (segEnd - segStart);
            }
        }
        float coarseStep = totalAtmoDist > 0.0 ? totalAtmoDist / float(input.lightRayParams.atmoLightSteps) : 1.0;
        float fineStep = totalCloudDist > 0.0 ? totalCloudDist / float(input.lightRayParams.cloudLightSteps) : 1.0;
        
        for (int j = 0; j < bCount - 1; j++) {
            float segStart = boundaries[j];
            float segEnd = boundaries[j + 1];
            if (segEnd <= segStart) continue;
            float midDist = (segStart + segEnd) * 0.5;
            float3 midPos = input.lightRayParams.samplePos + input.lightRayParams.lightDir * midDist;
            float midRadius = length(midPos - input.planetParams.planetPos);
            bool inCloudBand = (midRadius >= input.planetParams.cloudInnerRadius && midRadius <= input.planetParams.cloudOuterRadius);
            segments[segmentCount] = float4(segStart, segEnd, inCloudBand ? fineStep : coarseStep, inCloudBand ? 1.0 : 0.0);
            segmentCount++;
            if (segmentCount >= 5) break;
        }
    }

    //Marches a light ray
    float3 March(CompositeParams input, SharedFunctions sf) {
        Plan(input, sf);
        
        //Early outs
        if (planetShadowed) return float3(0.0, 0.0, 0.0);
        if (segmentCount == 0) return float3(1.0, 1.0, 1.0);

        //Accumulation values
        float3 atmoOptDepth = float3(0.0, 0.0, 0.0);
        float cloudOptDepth = 0.0;

        //uint2 pixelCoord, used in our hash function
        uint2 pixelCoord = uint2(input.mainRayParams.svxy);
        
        float rayStart = segments[0].x;
        float rayEnd = segments[segmentCount - 1].y;
        float rayLength = max(rayEnd - rayStart, 0.0001);

        for (int s = 0; s < segmentCount; s++) {
            //Extract segment values for this segment
            float segStart = segments[s].x;
            float segEnd = segments[s].y;
            float stepSize = segments[s].z;
            float pos = segStart;

            // Iteration cap safegaurd against infinite loops
            int iter = 0;
            while (pos < segEnd && iter < 256) {
                float currentStep = min(sf.AdaptiveStepSize(stepSize, pos, rayStart, rayLength, input.mainRayParams.stepScaleFactor) * input.lightRayParams.stepSizeMult, segEnd - pos);

                // Pseudo random hash
                uint seed = pixelCoord.x * 1664525u + pixelCoord.y * 1013904223u + uint(input.lightRayParams.parentStepIndex) * 214013u + uint(pos * 100.0) * 2531011u;
                seed = (seed ^ (seed >> 16)) * 2654435769u;
                seed = (seed ^ (seed >> 16));
                float stepSeed = float(seed) / 4294967295.0;

                // Finally we use this random value to offset our sample position within the step, baseline is the center of the step
                float sampleOffset = (stepSeed - 0.5) * currentStep * input.lightRayParams.jitterFactor;
                float3 p = input.lightRayParams.samplePos + input.lightRayParams.lightDir * (pos + currentStep * 0.5 + sampleOffset);
                
                // Early out if the sample is inside the planet radius
                float3 toCenter = p - input.planetParams.planetPos;
                float distSq = dot(toCenter, toCenter);
                if (distSq < input.planetParams.planetRadiusSq) return float3(0.0, 0.0, 0.0);

                // Accumulate atmosphere density (always)
                float dist = sqrt(distSq);
                float h = dist - input.planetParams.planetRadius;
                atmoOptDepth += sf.AtmoDensity(h, input.scatteringParams.rayScaleH, input.scatteringParams.mieScaleH, input.scatteringParams.absHeight, input.scatteringParams.absFalloff) * currentStep;
                
                // If the sample is within the cloud band, accumulate cloud density
                if (distSq >= input.planetParams.cloudInnerRadiusSq && distSq <= input.planetParams.cloudOuterRadiusSq) {
                    float cDensity = sf.CloudDensity(dist, p, input);
                    if (cDensity > 0.0) {
                        cloudOptDepth += cDensity * currentStep;
                    }
                }

                // Set up for next iteration
                pos += currentStep;
                iter++;
            }
        }
        return exp(-(input.scatteringParams.rayBeta * atmoOptDepth.x + input.scatteringParams.mieBeta * atmoOptDepth.y + input.scatteringParams.absBeta * atmoOptDepth.z + input.cloudParams.cloudAbsBeta * cloudOptDepth));
    }
};

//MAIN MARCH
struct MainRayFunctions {
    float4 segments[5];
    int segmentCount;
    float3 rayOrigin;
    float3 rayDir;
    bool depthLimited;

    //Pre-plans the march for a main ray
    void Plan(CompositeParams input, SharedFunctions sf) {
        rayOrigin = input.mainRayParams.cameraPos;
        rayDir = input.mainRayParams.cameraDir;
        segmentCount = 0;
        float2 atmoHit = sf.RaySphere(input.mainRayParams.cameraPos, input.mainRayParams.cameraDir, input.planetParams.planetPos, input.planetParams.atmoRadius);
        if (atmoHit.y < 0.0) return;
        float2 planetHit = sf.RaySphere(input.mainRayParams.cameraPos, input.mainRayParams.cameraDir, input.planetParams.planetPos, input.planetParams.planetRadius);
        float maxDist = input.mainRayParams.sceneDepth;
        if (planetHit.x > 0.0) {
            maxDist = min(maxDist, planetHit.x);
        }
        float atmoStart = max(atmoHit.x, 0.0);
        float atmoEnd = min(atmoHit.y, maxDist);
        depthLimited = (maxDist < atmoHit.y); 
        if (atmoStart >= atmoEnd) return;
        float2 cloudOHit = sf.RaySphere(input.mainRayParams.cameraPos, input.mainRayParams.cameraDir, input.planetParams.planetPos, input.planetParams.cloudOuterRadius);
        float2 cloudIHit = sf.RaySphere(input.mainRayParams.cameraPos, input.mainRayParams.cameraDir, input.planetParams.planetPos, input.planetParams.cloudInnerRadius);
        bool hasCloudOuter = cloudOHit.y > 0.0 && cloudOHit.x < maxDist;
        bool hasCloudInner = cloudIHit.y > 0.0 && cloudIHit.x < maxDist;
        if (!hasCloudOuter) {
            float stepSize = (atmoEnd - atmoStart) / float(input.mainRayParams.atmoSteps);
            segments[0] = float4(atmoStart, atmoEnd, stepSize, 0.0);
            segmentCount = 1;
            return;
        }
        float co_near = max(cloudOHit.x, 0.0);
        float co_far  = min(cloudOHit.y, maxDist);
        float ci_near = hasCloudInner ? max(cloudIHit.x, 0.0) : -1.0;
        float ci_far  = hasCloudInner ? min(cloudIHit.y, maxDist) : -1.0;
        float boundaries[6];
        int bCount = 0;
        boundaries[bCount++] = atmoStart;
        if (co_near > atmoStart && co_near < atmoEnd) boundaries[bCount++] = co_near;
        if (hasCloudInner && ci_near > atmoStart && ci_near < atmoEnd && ci_near > co_near) boundaries[bCount++] = ci_near;
        if (hasCloudInner && ci_far > atmoStart && ci_far < atmoEnd && ci_far > ci_near) boundaries[bCount++] = ci_far;
        if (co_far > atmoStart && co_far < atmoEnd) boundaries[bCount++] = co_far;
        boundaries[bCount++] = atmoEnd;
        float totalCloudDist = 0.0;
        float totalAtmoDist = 0.0;
        for (int i = 0; i < bCount - 1; i++) {
            float segStart = boundaries[i];
            float segEnd = boundaries[i + 1];
            if (segEnd <= segStart) continue;
            float midDist = (segStart + segEnd) * 0.5;
            float3 midPos = input.mainRayParams.cameraPos + input.mainRayParams.cameraDir * midDist;
            float midRadius = length(midPos - input.planetParams.planetPos);
            bool inCloudBand = (midRadius >= input.planetParams.cloudInnerRadius && midRadius <= input.planetParams.cloudOuterRadius);
            if (inCloudBand) {
                totalCloudDist += (segEnd - segStart);
            } else {
                totalAtmoDist += (segEnd - segStart);
            }
        }
        float coarseStep = totalAtmoDist > 0.0 ? totalAtmoDist / float(input.mainRayParams.atmoSteps) : 1.0;
        float fineStep = totalCloudDist > 0.0 ? totalCloudDist / float(input.mainRayParams.cloudSteps) : 1.0;
        for (int j = 0; j < bCount - 1; j++) {
            float segStart = boundaries[j];
            float segEnd = boundaries[j + 1];
            if (segEnd <= segStart) continue;
            float midDist = (segStart + segEnd) * 0.5;
            float3 midPos = input.mainRayParams.cameraPos + input.mainRayParams.cameraDir * midDist;
            float midRadius = length(midPos - input.planetParams.planetPos);
            bool inCloudBand = (midRadius >= input.planetParams.cloudInnerRadius && midRadius <= input.planetParams.cloudOuterRadius);
            segments[segmentCount] = float4(segStart, segEnd, inCloudBand ? fineStep : coarseStep, inCloudBand ? 1.0 : 0.0);
            segmentCount++;
            if (segmentCount >= 5) break;
        }
    }

    // Performs the main ray march
    float4 March(CompositeParams input, SharedFunctions sf) {
        Plan(input, sf);
        if (segmentCount == 0) return float4(0, 0, 0, 1.0);
        
        // Accumulators
        float3 AccumLight = float3(0, 0, 0);
        float3 Transmittance = float3(1.0, 1.0, 1.0);
        
        // Extract full composite ray data from segment (Full plan start distance, end distance, and length)
        float rayStart = segments[0].x;
        float rayEnd = segments[segmentCount - 1].y;
        float rayLength = max(rayEnd - rayStart, 0.0001);

        // Compute phase data
        float phaseDual = sf.PhaseDualLobe(input.scatteringParams.cosTheta, input.cloudParams.cloudPhaseParams.r, input.cloudParams.cloudPhaseParams.g, input.cloudParams.cloudPhaseParams.b);
        float phaseIsotropic = 1.0 / (4.0 * 3.14159265);

        // Hash a base seed for the ray
        uint2 pixelCoord = uint2(input.mainRayParams.svxy);
        uint pixSeed = pixelCoord.x * 1664525u + pixelCoord.y * 1013904223u;
        pixSeed = (pixSeed ^ (pixSeed >> 16)) * 2654435769u;
        pixSeed = (pixSeed ^ (pixSeed >> 16));
        float worldSeed = float(pixSeed) / 4294967295.0;
        
        int stepIndex = 0;
        
        for (int s = 0; s < segmentCount; s++) {
            
            // Extract values for segment start distance, end distance, baseStepSize
            float segStart = segments[s].x;
            float segEnd = segments[s].y;
            float baseStepSize = segments[s].z;
            // Flag true to track if this is a cloud layer segment
            bool isCloudBand = segments[s].w > 0.5;
            float pos = segStart;
            
            // Early exit 
            int iter = 0;
            while (pos < segEnd && iter < 256) {
                iter++;
                // Get adaptive step size
                float currentStep = min(sf.AdaptiveStepSize(baseStepSize, pos, rayStart, rayLength, input.mainRayParams.stepScaleFactor), segEnd - pos);
                float stepSizeMult = currentStep / baseStepSize;
                // Randomize sample position within the ray
                float stepSeed = frac(worldSeed + float(stepIndex) * 0.618033);
                float sampleOffset = (stepSeed - 0.5) * currentStep * input.mainRayParams.jitterFactor; //1 is kinda the only value that makes sense now, we could get rid of both jitter factors
                float3 samplePos = rayOrigin + rayDir * (pos + currentStep * 0.5 + sampleOffset);

                // Find height relative to inner radius
                float distFromCenter = length(samplePos - input.planetParams.planetPos);
                float height = distFromCenter - input.planetParams.planetRadius;
                
                // Transmittance Accumulator
                float3 sunTransmittance = float3(1.0, 1.0, 1.0);
                if (height > 0) {
                    // Set step specific input.lightRayParams values
                    input.lightRayParams.samplePos = samplePos;
                    input.lightRayParams.parentStepIndex = stepIndex;
                    input.lightRayParams.stepSizeMult = stepSizeMult;
                    // Do light march
                    LightMarchFunctions lightRay;
                    sunTransmittance = lightRay.March(input, sf);
                    
                    // Sample and accumulate atmosphere scattering
                    float3 atmoDens = sf.AtmoDensity(height, input.scatteringParams.rayScaleH, input.scatteringParams.mieScaleH, input.scatteringParams.absHeight, input.scatteringParams.absFalloff);
                    float3 rayleighScatter = input.scatteringParams.rayBeta * input.scatteringParams.phaseR * atmoDens.x;
                    float3 mieScatter = depthLimited ? float3(0,0,0) : input.scatteringParams.mieBeta * input.scatteringParams.phaseM * atmoDens.y;
                    float3 directScatter = (rayleighScatter + mieScatter) * sunTransmittance * input.mainRayParams.lightCol;
                    float3 ambientScatter = input.scatteringParams.rayBeta * atmoDens.x * input.scatteringParams.atmoAmbient;
                    float3 inscattered = (directScatter + ambientScatter) * currentStep;
                    float3 extinction = (input.scatteringParams.rayBeta * atmoDens.x + input.scatteringParams.mieBeta * atmoDens.y + input.scatteringParams.absBeta * atmoDens.z) * currentStep;
                    float3 atmoStepTransmittance = exp(-extinction);
                    AccumLight += Transmittance * inscattered;
                    Transmittance *= atmoStepTransmittance;
                    
                    // If we are in the cloud band we will sample the CloudDensity function
                    if (isCloudBand) {
                        float cloudDensity = sf.CloudDensity(distFromCenter, samplePos, input);

                        // If density is greater than 0, accumulate cloud scattering
                        if (cloudDensity > 0) {
                            float3 incomingLight = input.mainRayParams.lightCol * sunTransmittance;
                            float3 multiScatter = incomingLight * phaseIsotropic * input.cloudParams.cloudPhaseParams.a;
                            float3 cloudLight = incomingLight * phaseDual + multiScatter + input.mainRayParams.lightCol * input.cloudParams.cloudAmb;                        
                            float3 cloudExtinction = cloudDensity * input.cloudParams.cloudScatBeta * currentStep;
                            float3 cloudStepTransmittance = exp(-cloudExtinction);
                            float3 cloudStepLight = cloudLight * (1.0 - cloudStepTransmittance);
                            AccumLight += Transmittance * cloudStepLight;
                            Transmittance *= cloudStepTransmittance;
                        }
                    }
                }
                
                // Transmittance segment early out
                if (max(Transmittance.x, max(Transmittance.y, Transmittance.z)) < 0.001) {
                    Transmittance = float3(0, 0, 0);
                    break;
                }
                
                pos += currentStep;
                stepIndex++;
            }
            
            // Transmittance ray early out
            if (max(Transmittance.x, max(Transmittance.y, Transmittance.z)) < 0.001) {
                Transmittance = float3(0, 0, 0);
                break;
            }
        }
        
        // Find the most opaque value in all 3 channels
        float minTrans = min(Transmittance.x, min(Transmittance.y, Transmittance.z));
        
        // Return accumulated light and transmittance
        return float4(AccumLight, minTrans);
    }

};

//INIT PARAMS
SharedFunctions sf;
sf.cloudDensityTex = cloudDensityTexture;
sf.cloudDensitySampler = cloudDensityTextureSampler;

//HAVE TO JUMP THROUGH A LOT OF HOOPS TO HAVE THE DOWNSAMPLED USER TEXTURE GENERATE WITH THE PROPER DATA
//WE ARE BASICALLY RENDERING THE ENTIRE MARCH IN THE TOP LEFT 1/4TH OF THE SCREEN WITH DIMENSIONS RTSIZE 
float2 screenUV = Parameters.SvPosition.xy / rtSize;
float2 ClipXY = screenUV * float2(2, -2) + float2(-1, 1);
float4 WorldDir = mul(float4(ClipXY, 0.5, 1.0), View.ClipToTranslatedWorld);
float3 FinalRayDir = normalize(WorldDir.xyz / WorldDir.w);

float atmoThickness = atmoRadius - planetRadius;
float rayleighScaleHeight = atmoThickness * rayleighHeight;
float mieScaleHeight = atmoThickness * mieHeight;
float3 scaledRayleighBeta = rayleighBeta / atmoThickness;
float3 scaledMieBeta = mieBeta / atmoThickness;
float3 scaledAbsBeta = absorptionBeta / atmoThickness;
float absHeight = atmoThickness * absorptionHeight;
float absFalloff = atmoThickness * absorptionFalloff;
float3 scaledCloudScatBeta = cloudScatterBeta / atmoThickness;
float3 scaledCloudAbsBeta = cloudAbsorptionBeta / atmoThickness;
float cosTheta = dot(FinalRayDir, lightDirection);
float phaseR = sf.PhaseRayleigh(cosTheta);
float phaseM = sf.PhaseHG(cosTheta, mieG);

float3 crPlanetPos = atmoPos - cameraPos;

//Blend between ignoring depth (outside atmo) and using scene depth (inside atmo) to avoid sceneDepth precision breakdown at large distances
float camDist = length(crPlanetPos);
float outerBlend = saturate((atmoRadius - camDist) / (atmoRadius - cloudOuterRadius));
float correctedDepth = lerp(camDist * 2, sceneDepth, outerBlend);

//INIT MAIN PARAMS
CompositeParams params;

//PLANET PARAMS
params.planetParams.planetPos = crPlanetPos;
params.planetParams.planetRadius = planetRadius;
params.planetParams.atmoRadius = atmoRadius; 
params.planetParams.cloudOuterRadius = cloudOuterRadius; 
params.planetParams.cloudInnerRadius = cloudInnerRadius;
params.planetParams.planetRadiusSq = planetRadius * planetRadius;
params.planetParams.cloudOuterRadiusSq = cloudOuterRadius * cloudOuterRadius; 
params.planetParams.cloudInnerRadiusSq = cloudInnerRadius * cloudInnerRadius; 

//SCATTERING PARAMS
params.scatteringParams.cosTheta = cosTheta;
params.scatteringParams.rayBeta = scaledRayleighBeta; 
params.scatteringParams.mieBeta = scaledMieBeta; 
params.scatteringParams.absBeta = scaledAbsBeta; 
params.scatteringParams.atmoAmbient = atmoAmbient; 
params.scatteringParams.rayScaleH = rayleighScaleHeight; 
params.scatteringParams.mieScaleH = mieScaleHeight; 
params.scatteringParams.absHeight = absHeight; 
params.scatteringParams.absFalloff = absFalloff;
params.scatteringParams.phaseR = phaseR; 
params.scatteringParams.phaseM = phaseM; 

//CLOUD PARAMS
params.cloudParams.cloudScatBeta = scaledCloudScatBeta; 
params.cloudParams.cloudAbsBeta = scaledCloudAbsBeta; 
params.cloudParams.cloudAmb = cloudAmbient;
params.cloudParams.cloudPhaseParams = cloudPhaseParams;
params.cloudParams.cloudScale = cloudScale; 
params.cloudParams.cloudHeightCurve = cloudHeightCurve;
params.cloudParams.cloudCoverage = cloudCoverage; 
params.cloudParams.cloudDensityMult = cloudDensityMult;
params.cloudParams.cloudNoiseWeights = cloudNoiseWeights;
params.cloudParams.cloudNoiseInvert = cloudNoiseInvert;
params.cloudParams.detailNoiseWeights = detailNoiseWeights;
params.cloudParams.detailNoiseInvert = detailNoiseInvert;
params.cloudParams.detailNoiseScale = detailNoiseScale;
params.cloudParams.detailNoiseErode = detailNoiseErode;
params.cloudParams.animationWeights = animationWeights;

//MAIN RAY PARAMS
params.mainRayParams.svxy = Parameters.SvPosition.xy;
//Camera is treated as the origin for all math, this increases our precision thresholds signifigantly
params.mainRayParams.cameraPos = float3(0,0,0);
params.mainRayParams.originalCameraPos = cameraPos;
params.mainRayParams.cameraDir = FinalRayDir; 
params.mainRayParams.lightCol = lightColor; 
params.mainRayParams.sceneDepth = correctedDepth;
params.mainRayParams.atmoSteps = mainAtmoSteps; 
params.mainRayParams.cloudSteps = mainCloudSteps;
params.mainRayParams.jitterFactor = jitterFactor;
params.mainRayParams.stepScaleFactor = stepScaleFactor; 

//LIGHT RAY PARAMS
params.lightRayParams.lightDir = lightDirection; 
params.lightRayParams.atmoLightSteps = atmoLightSteps; 
params.lightRayParams.cloudLightSteps = cloudLightSteps;
params.lightRayParams.jitterFactor = lightJitterFactor;

//RAYMARCH
MainRayFunctions mainRay;
return mainRay.March(params, sf);