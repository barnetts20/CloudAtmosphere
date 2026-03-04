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
    float4 cloudPhaseParams;
    float3 cloudScatBeta;
    float3 cloudAbsBeta;
    float3 cloudAmb;
};

struct PlanetParams{
    float3 planetPos;
    float planetRadius;
    float planetRadiusSq; //If we are going to calculate this should we just use it everywhere? does it decrease precision to be squaring these large numbers?
    float atmoRadius;
    float atmoRadiusSq;
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
    float3 cameraPos; 
    float3 cameraDir; 
    float3 lightCol;  
    float3 sceneCol;
    float sceneDepth; 
    int atmoSteps; 
    int cloudSteps;
    float jitterFactor; 
    float stepScaleFactor; 
};

struct LightRayParams {
    float3 samplePos; 
    float3 lightDir; //Post jitter light direction... guess we could jitter inside the light march itself and remove from main params
    float3 sunRight;
    float3 sunUp;
    int atmoLightSteps; 
    int cloudLightSteps; 
    float jitterFactor;  
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

    float2 RaySphere(float3 origin, float3 dir, float3 center, float radius) {
        float3 oc = origin - center;
        float b = dot(oc, dir);
        float c = dot(oc, oc) - radius * radius;
        float disc = b * b - c;
        if (disc < 0.0) return float2(-1.0, -1.0);
        float sq = sqrt(disc);
        return float2(-b - sq, -b + sq);
    }

    float PhaseRayleigh(float cosTheta) {
        return 3.0 / (16.0 * 3.14159265) * (1.0 + cosTheta * cosTheta);
    }

    float PhaseHG(float cosTheta, float g) {
        float g2 = g * g;
        return (1.0 - g2) / (4.0 * 3.14159265 * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5));
    }

    float3 AtmoDensity(float height, float rayScaleH, float mieScaleH, float absHeight, float absFalloff) {
        float rayDensity = exp(-height / rayScaleH);
        float mieDensity = exp(-height / mieScaleH);
        float safeFalloff = max(absFalloff, 0.0001);
        float denom = (absHeight - height) / safeFalloff;
        float absDensity = (1.0 / (denom * denom + 1.0)) * rayDensity;
        return float3(rayDensity, mieDensity, absDensity);
    }

    float PhaseDualLobe(float cosTheta, float gForward, float gBack, float forwardWeight) {
        float forward = PhaseHG(cosTheta, gForward);
        float back = PhaseHG(cosTheta, -gBack);
        return max(lerp(back, forward, forwardWeight), .05);
    }

    float CloudDensity(float distFromCenter, float3 samplePos, CompositeParams input) {
        float cloudNormH = (distFromCenter - input.planetParams.cloudInnerRadius) / (input.planetParams.cloudOuterRadius - input.planetParams.cloudInnerRadius);
        float bottomFade = saturate(smoothstep(0.0, input.cloudParams.cloudHeightCurve.x, cloudNormH));
        float topFade = saturate(smoothstep(1.0, input.cloudParams.cloudHeightCurve.y, cloudNormH));
        float heightGradient = bottomFade * topFade;

        float3 uvw = samplePos / (input.planetParams.cloudOuterRadius * input.cloudParams.cloudScale);
        float4 cSample = cloudDensityTex.Sample(cloudDensitySampler, uvw);
        float4 icSample = float4(1,1,1,1) - cSample;
        float4 fcSample = lerp(cSample, icSample, input.cloudParams.cloudNoiseInvert);

        float baseNoise = fcSample.r;
        float detail = (fcSample.g * input.cloudParams.cloudNoiseWeights.r + fcSample.b * input.cloudParams.cloudNoiseWeights.g + fcSample.a * input.cloudParams.cloudNoiseWeights.b);
        float noise = saturate(baseNoise - detail * input.cloudParams.cloudNoiseWeights.a);

        float remapped = saturate(heightGradient * (noise - (1.0 - input.cloudParams.cloudCoverage)) / input.cloudParams.cloudCoverage);

        if (remapped > 0) {
            float3 uvwDetail = uvw * input.cloudParams.detailNoiseScale;
            float4 dSample = cloudDensityTex.Sample(cloudDensitySampler, uvwDetail);
            float4 idSample = float4(1,1,1,1) - dSample;
            float4 fdSample = lerp(dSample, idSample, input.cloudParams.detailNoiseInvert);

            float fineDetail = fdSample.r * input.cloudParams.detailNoiseWeights.r + fdSample.g * input.cloudParams.detailNoiseWeights.g + fdSample.b * input.cloudParams.detailNoiseWeights.b + fdSample.a * input.cloudParams.detailNoiseWeights.a; //TODO: PARAMETERIZE multipliers as vector param
            float detailErode = input.cloudParams.detailNoiseErode;
            remapped = saturate(remapped - fineDetail * detailErode);
        }

        return remapped * input.cloudParams.cloudDensityMult;
    }

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

    float3 March(CompositeParams input, SharedFunctions sf) {
        float seed = frac(sin(dot(input.lightRayParams.samplePos, float3(12.9898, 78.233, 45.164))) * 43758.5453);
        float2 ditherUV = float2(frac(seed * 12.98), frac(seed * 78.23)) * 2.0 - 1.0;
        float3 jitteredLightDir = normalize(input.lightRayParams.lightDir + (input.lightRayParams.sunRight * ditherUV.x + input.lightRayParams.sunUp * ditherUV.y) * (0.01 * input.lightRayParams.jitterFactor));
        Plan(input, sf);
        if (planetShadowed) return float3(0.0, 0.0, 0.0);
        if (segmentCount == 0) return float3(1.0, 1.0, 1.0);
        float3 atmoOptDepth = float3(0.0, 0.0, 0.0);
        float cloudOptDepth = 0.0;
        for (int s = 0; s < segmentCount; s++) {
            float segStart = segments[s].x;
            float segEnd = segments[s].y;
            float stepSize = segments[s].z * (1.0 + input.lightRayParams.jitterFactor);
            float pos = segStart;
            int iter = 0;
            while (pos < segEnd && iter < 256) {
                iter++;
                float currentStep = min(stepSize, segEnd - pos);
                float3 p = input.lightRayParams.samplePos + jitteredLightDir * (pos + currentStep * 0.5);
                float3 toCenter = p - input.planetParams.planetPos;
                float distSq = dot(toCenter, toCenter);
                if (distSq < input.planetParams.planetRadiusSq) return float3(0.0, 0.0, 0.0); //TODO: Again, idk if its worth using sq values.. faster yes but what about precision? if the same should we not switch everywhere to sq values?
                float dist = sqrt(distSq);
                float h = dist - input.planetParams.planetRadius;
                atmoOptDepth += sf.AtmoDensity(h, input.scatteringParams.rayScaleH, input.scatteringParams.mieScaleH, input.scatteringParams.absHeight, input.scatteringParams.absFalloff) * currentStep;
                if (distSq >= input.planetParams.cloudInnerRadiusSq && distSq <= input.planetParams.cloudOuterRadiusSq) { //TODO: Again, idk if its worth using sq values.. faster yes but what about precision? if the same should we not switch everywhere to sq values?
                    float cDensity = sf.CloudDensity(dist, p, input);
                    if (cDensity > 0.0) {
                        cloudOptDepth += cDensity * currentStep;
                    }
                }
                pos += currentStep;
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
    bool depthLimited; // add this

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

    float4 March(CompositeParams input, SharedFunctions sf) {
        Plan(input, sf);
        if (segmentCount == 0) return float4(0, 0, 0, 1.0);
        
        float3 AccumLight = float3(0, 0, 0);
        float3 Transmittance = float3(1.0, 1.0, 1.0);
        int stepIndex = 0;
        
        float rayStart = segments[0].x;
        float rayEnd = segments[segmentCount - 1].y;
        float rayLength = max(rayEnd - rayStart, 0.0001);
        
        // Stable per-pixel seed for jitter
        float frameSeed = frac(float(View.StateFrameIndex) * 0.618033); //0; //Set to zero to get rid of temporal jitter
        float worldSeed = frac(sin(dot(input.mainRayParams.cameraDir, float3(12.9898, 78.233, 45.164))) * 43758.5453 + frameSeed);
        
        for (int s = 0; s < segmentCount; s++) {
            float segStart = segments[s].x;
            float segEnd = segments[s].y;
            float baseStepSize = segments[s].z;
            bool isCloudBand = segments[s].w > 0.5;
            float pos = segStart;
            
            int iter = 0;
            while (pos < segEnd && iter < 256) {
                iter++;
                float currentStep = sf.AdaptiveStepSize(baseStepSize, pos, rayStart, rayLength, input.mainRayParams.stepScaleFactor);
                
                // Per-step jitter using golden ratio sequence
                float stepSeed = frac(worldSeed + float(stepIndex) * 0.618033); //TODO: PASS IN BLUE NOISE TEX SO WE CAN SAMPLE IT FOR RANDOM VALUES
                float jitterAmount = input.mainRayParams.jitterFactor;
                float stepSizeMultiplier = 1.0 + (stepSeed - 0.5) * 2.0 * jitterAmount;
                float jitteredStep = currentStep * stepSizeMultiplier;
                jitteredStep = max(0.001, min(jitteredStep, segEnd - pos));
                
                float3 samplePos = rayOrigin + rayDir * (pos + jitteredStep * 0.5);
                float distFromCenter = length(samplePos - input.planetParams.planetPos);
                float height = distFromCenter - input.planetParams.planetRadius;
                
                float3 sunTransmittance = float3(1.0, 1.0, 1.0);
                if (height > 0) {
                    input.lightRayParams.samplePos = samplePos;
                    LightMarchFunctions lightRay;
                    sunTransmittance = lightRay.March(input, sf);
                    
                    float3 atmoDens = sf.AtmoDensity(height, input.scatteringParams.rayScaleH, input.scatteringParams.mieScaleH, input.scatteringParams.absHeight, input.scatteringParams.absFalloff);
                    float3 rayleighScatter = input.scatteringParams.rayBeta * input.scatteringParams.phaseR * atmoDens.x;
                    float3 mieScatter = depthLimited ? float3(0,0,0) :input.scatteringParams.mieBeta * input.scatteringParams.phaseM * atmoDens.y;
                    float3 directScatter = (rayleighScatter + mieScatter) * sunTransmittance * input.mainRayParams.lightCol;
                    float3 ambientScatter = input.scatteringParams.rayBeta * atmoDens.x * input.scatteringParams.atmoAmbient;
                    float3 inscattered = (directScatter + ambientScatter) * jitteredStep;
                    float3 extinction = (input.scatteringParams.rayBeta * atmoDens.x + input.scatteringParams.mieBeta * atmoDens.y + input.scatteringParams.absBeta * atmoDens.z) * jitteredStep;
                    float3 atmoStepTransmittance = exp(-extinction);
                    AccumLight += Transmittance * inscattered;
                    Transmittance *= atmoStepTransmittance;
                    
                    if (isCloudBand) {
                        float cloudDensity = sf.CloudDensity(distFromCenter, samplePos, input);
                        if (cloudDensity > 0) {
                            float3 incomingLight = input.mainRayParams.lightCol * sunTransmittance;
                            float phaseDual = sf.PhaseDualLobe(input.scatteringParams.cosTheta, input.cloudParams.cloudPhaseParams.r, input.cloudParams.cloudPhaseParams.g, input.cloudParams.cloudPhaseParams.b);
                            float phaseIsotropic = 1.0 / (4.0 * 3.14159265);
                            float3 multiScatter = incomingLight * phaseIsotropic * input.cloudParams.cloudPhaseParams.a;
                            float3 cloudLight = incomingLight * phaseDual + multiScatter + input.mainRayParams.lightCol * input.cloudParams.cloudAmb;                        
                            float3 cloudExtinction = cloudDensity * input.cloudParams.cloudScatBeta * jitteredStep;
                            float3 cloudStepTransmittance = exp(-cloudExtinction);
                            float3 cloudStepLight = cloudLight * (1.0 - cloudStepTransmittance);
                            AccumLight += Transmittance * cloudStepLight;
                            Transmittance *= cloudStepTransmittance;
                        }
                    }
                }
                
                if (max(Transmittance.x, max(Transmittance.y, Transmittance.z)) < 0.001) {
                    Transmittance = float3(0, 0, 0);
                    break;
                }
                
                pos += jitteredStep;
                stepIndex++;
            }
            
            if (max(Transmittance.x, max(Transmittance.y, Transmittance.z)) < 0.001) {
                Transmittance = float3(0, 0, 0);
                break;
            }
        }
        
        float minTrans = min(Transmittance.x, min(Transmittance.y, Transmittance.z));
        return float4(AccumLight, minTrans);
    } 

};

//INIT PARAMS
SharedFunctions sf;
sf.cloudDensityTex = cloudDensityTexture;
sf.cloudDensitySampler = cloudDensityTextureSampler;

//TODO: SET BLUE NOISE TEX AND SWITCH TO IT FOR RANDOM VALUES INSTEAD OF FRACS

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
float cosTheta = dot(cameraDir, lightDirection);
float phaseR = sf.PhaseRayleigh(cosTheta);
float phaseM = sf.PhaseHG(cosTheta, mieG);

//HAVE TO JUMP THROUGH A LOT OF HOOPS TO HAVE THE DOWNSAMPLED USER TEXTURE GENERATE WITH THE PROPER DATA
//WE ARE BASICALLY RENDERING THE ENTIRE MARCH IN THE TOP LEFT 1/4TH OF THE SCREEN WITH DIMENSIONS RTSIZE 
float2 screenUV = Parameters.SvPosition.xy / rtSize;
float2 ClipXY = screenUV * float2(2, -2) + float2(-1, 1);
float4 WorldDir = mul(float4(ClipXY, 0.5, 1.0), View.ClipToTranslatedWorld);
float3 FinalRayDir = normalize(WorldDir.xyz / WorldDir.w);
float deviceZ = LookupDeviceZ(screenUV);
float2 fullResSvPos = screenUV * View.ViewSizeAndInvSize.xy + View.ViewRectMin.xy;
float4 virtualSvPos = float4(fullResSvPos, deviceZ, 1.0);
float3 TranslatedWorldPos = SvPositionToTranslatedWorld(virtualSvPos);
float reconstructedDepth = length(TranslatedWorldPos);

//SWITCHES BETWEEN DEPTH MODELS, OUR MAIN METHOD BREAKS DOWN AT LARGE DISTANCES - SO OUTSIDE THE ATMOSPHERE WE JUST IGNORE DEPTH TRACE
float camDist = length(cameraPos - atmoPos);
float outerBlend = saturate((atmoRadius - camDist) / (atmoRadius - cloudOuterRadius));
float correctedDepth = lerp(camDist * 2, sceneDepth, outerBlend);

//INIT MAIN PARAMS
CompositeParams params;

//Planet params
params.planetParams.planetPos = atmoPos; 
params.planetParams.planetRadius = planetRadius;
params.planetParams.atmoRadius = atmoRadius; 
params.planetParams.cloudOuterRadius = cloudOuterRadius; 
params.planetParams.cloudInnerRadius = cloudInnerRadius;
params.planetParams.planetRadiusSq = planetRadius * planetRadius;
params.planetParams.atmoRadiusSq = atmoRadius * atmoRadius; 
params.planetParams.cloudOuterRadiusSq = cloudOuterRadius * cloudOuterRadius; 
params.planetParams.cloudInnerRadiusSq = cloudInnerRadius * cloudInnerRadius; 

//Scattering Params
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

//Cloud params
params.cloudParams.cloudScatBeta = scaledCloudScatBeta; 
params.cloudParams.cloudAbsBeta = scaledCloudAbsBeta; 
params.cloudParams.cloudAmb = cloudAmbient;
params.cloudParams.cloudPhaseParams = cloudPhaseParams;
params.cloudParams.cloudScale = cloudScale; 
params.cloudParams.cloudHeightCurve = float2(.3,.7);
params.cloudParams.cloudCoverage = cloudCoverage; 
params.cloudParams.cloudDensityMult = cloudDensityMult;
params.cloudParams.cloudNoiseWeights = cloudNoiseWeights;
params.cloudParams.cloudNoiseInvert = cloudNoiseInvert;
params.cloudParams.detailNoiseWeights = detailNoiseWeights;
params.cloudParams.detailNoiseInvert = detailNoiseInvert;
params.cloudParams.detailNoiseScale = detailNoiseScale;
params.cloudParams.detailNoiseErode = detailNoiseErode;

//Main Ray Params
params.mainRayParams.cameraPos = cameraPos; 
params.mainRayParams.cameraDir = FinalRayDir; 
params.mainRayParams.lightCol = lightColor; 
params.mainRayParams.sceneCol = sceneColor;
params.mainRayParams.sceneDepth = correctedDepth;
params.mainRayParams.atmoSteps = mainAtmoSteps; 
params.mainRayParams.cloudSteps = mainCloudSteps;
params.mainRayParams.jitterFactor = jitterFactor;
params.mainRayParams.stepScaleFactor = stepScaleFactor; 

//Light Ray Params
params.lightRayParams.lightDir = lightDirection; 
params.lightRayParams.sunRight = normalize(cross(abs(params.lightRayParams.lightDir.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0), params.lightRayParams.lightDir));
params.lightRayParams.sunUp = cross(params.lightRayParams.lightDir, params.lightRayParams.sunRight);
params.lightRayParams.atmoLightSteps = atmoLightSteps; 
params.lightRayParams.cloudLightSteps = cloudLightSteps;
params.lightRayParams.jitterFactor = jitterFactor;

//RAYMARCH
MainRayFunctions mainRay;
return mainRay.March(params, sf);