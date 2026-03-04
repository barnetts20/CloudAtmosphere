//PARAM STRUCTS SO WE DONT HAVE TO PASS IN A BILLION PARAMETERS TO THE MAIN FUNCTION CALLS
struct MainRayParams {
    float cosTheta;
    float3 cameraPos; 
    float3 cameraDir; 
    float3 planetPos; 
    float planetRadius; 
    float atmoRadius; 
    float cloudOuterRadius; 
    float cloudInnerRadius; 
    float sceneDepth; 
    int atmoSteps; 
    int cloudSteps; 
    int atmoLightSteps; 
    int cloudLightSteps; 
    float3 lightDir; 
    float3 lightCol; 
    float3 scaledRayBeta; 
    float3 scaledMieBeta; 
    float3 scaledAbsBeta; 
    float atmoAmbient; 
    float rayScaleH; 
    float mieScaleH; 
    float absHeight; 
    float absFalloff; 
    float3 cloudScatBeta; 
    float3 cloudAbsBeta; 
    float3 cloudAmb;
    float4 cloudPhaseParams;
    float phaseR; 
    float phaseM; 
    float blueNoiseFactor; 
    float stepScaleFactor; 
    float3 sceneCol; 
    float cloudScale; 
    float cloudCoverage; 
    float cloudDensityMult;
    float4 cloudNoiseWeights;
    float4 cloudNoiseInvert;
    float4 detailNoiseWeights;
    float4 detailNoiseInvert;
    float detailNoiseScale;
    float detailNoiseErode;
};

struct LightMarchParams {
    float3 samplePos; 
    float3 lightDir; 
    float3 planetPos; 
    float planetRadius; 
    float planetRadiusSq; 
    float atmoRadius; 
    float cloudOuterRadius; 
    float cloudInnerRadius; 
    float cloudInnerRSq; 
    float cloudOuterRSq; 
    int atmoSteps; 
    int cloudSteps; 
    float3 rayBeta; 
    float3 mieBetaVec; 
    float3 absBeta; 
    float rayScaleH; 
    float mieScaleH; 
    float absHeight; 
    float absFalloff; 
    float3 cloudAbsBeta; 
    float4 cloudPhaseParams;
    float blueFactor;  
    float cloudScale; 
    float cloudCoverage; 
    float cloudDensityMult;
    float4 cloudNoiseWeights;
    float4 cloudNoiseInvert;
    float4 detailNoiseWeights;
    float4 detailNoiseInvert;
    float detailNoiseScale;
    float detailNoiseErode;
    
    static LightMarchParams FromMainRayParams(MainRayParams input){
        LightMarchParams lmp;
        lmp.planetPos = input.planetPos;
        lmp.planetRadius = input.planetRadius;
        lmp.planetRadiusSq = input.planetRadius * input.planetRadius;
        lmp.atmoRadius = input.atmoRadius;
        lmp.cloudOuterRadius = input.cloudOuterRadius;
        lmp.cloudInnerRadius = input.cloudInnerRadius;
        lmp.cloudInnerRSq = input.cloudInnerRadius * input.cloudInnerRadius;
        lmp.cloudOuterRSq = input.cloudOuterRadius * input.cloudOuterRadius;
        lmp.atmoSteps = input.atmoLightSteps;
        lmp.cloudSteps = input.cloudLightSteps;
        lmp.rayBeta = input.scaledRayBeta;
        lmp.mieBetaVec = input.scaledMieBeta;
        lmp.absBeta = input.scaledAbsBeta;
        lmp.rayScaleH = input.rayScaleH;
        lmp.mieScaleH = input.mieScaleH;
        lmp.absHeight = input.absHeight;
        lmp.absFalloff = input.absFalloff;
        lmp.cloudAbsBeta = input.cloudAbsBeta;
        lmp.cloudPhaseParams = input.cloudPhaseParams;            
        lmp.cloudScale = input.cloudScale;
        lmp.cloudCoverage = input.cloudCoverage;
        lmp.cloudDensityMult = input.cloudDensityMult;
        lmp.cloudNoiseWeights = input.cloudNoiseWeights;
        lmp.cloudNoiseInvert = input.cloudNoiseInvert;
        lmp.detailNoiseWeights = input.detailNoiseWeights;
        lmp.detailNoiseInvert = input.detailNoiseInvert;
        lmp.detailNoiseScale = input.detailNoiseScale;
        lmp.detailNoiseErode = input.detailNoiseErode;
        return lmp;
    }
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

    float CloudDensity(float distFromCenter, float3 samplePos, LightMarchParams lmp) {
        float cloudNormH = (distFromCenter - lmp.cloudInnerRadius) / (lmp.cloudOuterRadius - lmp.cloudInnerRadius);
        //TODO: use height curve to round top/bottoms of clouds
        float heightGradient = saturate(1.0 - abs(cloudNormH * 2.0 - 1.0));
        heightGradient = heightGradient * heightGradient;

        float3 uvw = samplePos / (lmp.cloudOuterRadius * lmp.cloudScale);
        float4 cSample = cloudDensityTex.Sample(cloudDensitySampler, uvw);
        float4 icSample = float4(1,1,1,1) - cSample;
        float4 fcSample = lerp(cSample, icSample, lmp.cloudNoiseInvert);

        float baseNoise = fcSample.r;
        float detail = (fcSample.g * lmp.cloudNoiseWeights.r + fcSample.b * lmp.cloudNoiseWeights.g + fcSample.a * lmp.cloudNoiseWeights.b);
        float noise = saturate(baseNoise - detail * lmp.cloudNoiseWeights.a);

        float remapped = saturate((noise - (1.0 - lmp.cloudCoverage)) / lmp.cloudCoverage);

        if (remapped > 0) {
            float3 uvwDetail = uvw * lmp.detailNoiseScale;
            float4 dSample = cloudDensityTex.Sample(cloudDensitySampler, uvwDetail);
            float4 idSample = float4(1,1,1,1) - dSample;
            float4 fdSample = lerp(dSample, idSample, lmp.detailNoiseInvert);

            float fineDetail = fdSample.r * lmp.detailNoiseWeights.r + fdSample.g * lmp.detailNoiseWeights.g + fdSample.b * lmp.detailNoiseWeights.b + fdSample.a * lmp.detailNoiseWeights.a; //TODO: PARAMETERIZE multipliers as vector param
            float detailErode = lmp.detailNoiseErode;
            remapped = saturate(remapped - fineDetail * detailErode);
        }

        return remapped * heightGradient * lmp.cloudDensityMult;
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
    float3 rayOrigin;
    float3 rayDir;
    bool planetShadowed;

    void Plan(LightMarchParams input, SharedFunctions sf) {
        rayOrigin = input.samplePos;
        rayDir = input.lightDir;
        segmentCount = 0;
        planetShadowed = false;
        float2 planetHit = sf.RaySphere(input.samplePos, input.lightDir, input.planetPos, input.planetRadius);
        if (planetHit.x > 0.0) {
            planetShadowed = true;
            return;
        }
        float2 atmoHit = sf.RaySphere(input.samplePos, input.lightDir, input.planetPos, input.atmoRadius);
        if (atmoHit.y < 0.0) return;
        float2 cloudOHit = sf.RaySphere(input.samplePos, input.lightDir, input.planetPos, input.cloudOuterRadius);
        float2 cloudIHit = sf.RaySphere(input.samplePos, input.lightDir, input.planetPos, input.cloudInnerRadius);
        float atmoStart = max(atmoHit.x, 0.0);
        float atmoEnd = atmoHit.y;
        if (atmoStart >= atmoEnd) return;
        bool hasCloudOuter = cloudOHit.y > 0.0;
        bool hasCloudInner = cloudIHit.y > 0.0;
        if (!hasCloudOuter) {
            float stepSize = (atmoEnd - atmoStart) / float(input.atmoSteps);
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
            float3 midPos = input.samplePos + input.lightDir * midDist;
            float midRadius = length(midPos - input.planetPos);
            bool inCloudBand = (midRadius >= input.cloudInnerRadius && midRadius <= input.cloudOuterRadius);
            if (inCloudBand) {
                totalCloudDist += (segEnd - segStart);
            } else {
                totalAtmoDist += (segEnd - segStart);
            }
        }
        float coarseStep = totalAtmoDist > 0.0 ? totalAtmoDist / float(input.atmoSteps) : 1.0;
        float fineStep = totalCloudDist > 0.0 ? totalCloudDist / float(input.cloudSteps) : 1.0;
        for (int j = 0; j < bCount - 1; j++) {
            float segStart = boundaries[j];
            float segEnd = boundaries[j + 1];
            if (segEnd <= segStart) continue;
            float midDist = (segStart + segEnd) * 0.5;
            float3 midPos = input.samplePos + input.lightDir * midDist;
            float midRadius = length(midPos - input.planetPos);
            bool inCloudBand = (midRadius >= input.cloudInnerRadius && midRadius <= input.cloudOuterRadius);
            segments[segmentCount] = float4(segStart, segEnd, inCloudBand ? fineStep : coarseStep, inCloudBand ? 1.0 : 0.0);
            segmentCount++;
            if (segmentCount >= 5) break;
        }
    }

    float3 March(LightMarchParams input, SharedFunctions sf) {
        Plan(input, sf);
        if (planetShadowed) return float3(0.0, 0.0, 0.0);
        if (segmentCount == 0) return float3(1.0, 1.0, 1.0);
        float3 atmoOptDepth = float3(0.0, 0.0, 0.0);
        float cloudOptDepth = 0.0;
        for (int s = 0; s < segmentCount; s++) {
            float segStart = segments[s].x;
            float segEnd = segments[s].y;
            float stepSize = segments[s].z * (1.0 + input.blueFactor);
            float pos = segStart;
            int iter = 0;
            while (pos < segEnd && iter < 256) {
                iter++;
                float currentStep = min(stepSize, segEnd - pos);
                float3 p = rayOrigin + rayDir * (pos + currentStep * 0.5);
                float3 toCenter = p - input.planetPos;
                float distSq = dot(toCenter, toCenter);
                if (distSq < input.planetRadiusSq) return float3(0.0, 0.0, 0.0);
                float dist = sqrt(distSq);
                float h = dist - input.planetRadius;
                atmoOptDepth += sf.AtmoDensity(h, input.rayScaleH, input.mieScaleH, input.absHeight, input.absFalloff) * currentStep;
                if (distSq >= input.cloudInnerRSq && distSq <= input.cloudOuterRSq) {
                    float cDensity = sf.CloudDensity(dist, p, input);
                    if (cDensity > 0.0) {
                        cloudOptDepth += cDensity * currentStep;
                    }
                }
                pos += currentStep;
            }
        }
        return exp(-(input.rayBeta * atmoOptDepth.x + input.mieBetaVec * atmoOptDepth.y + input.absBeta * atmoOptDepth.z + input.cloudAbsBeta * cloudOptDepth));
    }
};

//MAIN MARCH
struct MainRayFunctions {
    float4 segments[5];
    int segmentCount;
    float3 rayOrigin;
    float3 rayDir;
    bool depthLimited; // add this

    void Plan(MainRayParams input, SharedFunctions sf) {
        rayOrigin = input.cameraPos;
        rayDir = input.cameraDir;
        segmentCount = 0;
        float2 atmoHit = sf.RaySphere(input.cameraPos, input.cameraDir, input.planetPos, input.atmoRadius);
        if (atmoHit.y < 0.0) return;
        float2 planetHit = sf.RaySphere(input.cameraPos, input.cameraDir, input.planetPos, input.planetRadius);
        float maxDist = input.sceneDepth;
        if (planetHit.x > 0.0) {
            maxDist = min(maxDist, planetHit.x);
        }
        float atmoStart = max(atmoHit.x, 0.0);
        float atmoEnd = min(atmoHit.y, maxDist);
        depthLimited = (maxDist < atmoHit.y); 
        if (atmoStart >= atmoEnd) return;
        float2 cloudOHit = sf.RaySphere(input.cameraPos, input.cameraDir, input.planetPos, input.cloudOuterRadius);
        float2 cloudIHit = sf.RaySphere(input.cameraPos, input.cameraDir, input.planetPos, input.cloudInnerRadius);
        bool hasCloudOuter = cloudOHit.y > 0.0 && cloudOHit.x < maxDist;
        bool hasCloudInner = cloudIHit.y > 0.0 && cloudIHit.x < maxDist;
        if (!hasCloudOuter) {
            float stepSize = (atmoEnd - atmoStart) / float(input.atmoSteps);
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
            float3 midPos = input.cameraPos + input.cameraDir * midDist;
            float midRadius = length(midPos - input.planetPos);
            bool inCloudBand = (midRadius >= input.cloudInnerRadius && midRadius <= input.cloudOuterRadius);
            if (inCloudBand) {
                totalCloudDist += (segEnd - segStart);
            } else {
                totalAtmoDist += (segEnd - segStart);
            }
        }
        float coarseStep = totalAtmoDist > 0.0 ? totalAtmoDist / float(input.atmoSteps) : 1.0;
        float fineStep = totalCloudDist > 0.0 ? totalCloudDist / float(input.cloudSteps) : 1.0;
        for (int j = 0; j < bCount - 1; j++) {
            float segStart = boundaries[j];
            float segEnd = boundaries[j + 1];
            if (segEnd <= segStart) continue;
            float midDist = (segStart + segEnd) * 0.5;
            float3 midPos = input.cameraPos + input.cameraDir * midDist;
            float midRadius = length(midPos - input.planetPos);
            bool inCloudBand = (midRadius >= input.cloudInnerRadius && midRadius <= input.cloudOuterRadius);
            segments[segmentCount] = float4(segStart, segEnd, inCloudBand ? fineStep : coarseStep, inCloudBand ? 1.0 : 0.0);
            segmentCount++;
            if (segmentCount >= 5) break;
        }
    }

    float4 March(MainRayParams input, SharedFunctions sf) {
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
        float worldSeed = frac(sin(dot(input.cameraDir, float3(12.9898, 78.233, 45.164))) * 43758.5453 + frameSeed);
        
        // Orthogonal basis for light direction jitter
        float3 sunRight = normalize(cross(abs(input.lightDir.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0), input.lightDir));
        float3 sunUp = cross(input.lightDir, sunRight);
        
        // Build light march params once
        LightMarchParams lmp = LightMarchParams::FromMainRayParams(input);
        
        for (int s = 0; s < segmentCount; s++) {
            float segStart = segments[s].x;
            float segEnd = segments[s].y;
            float baseStepSize = segments[s].z;
            bool isCloudBand = segments[s].w > 0.5;
            float pos = segStart;
            
            int iter = 0;
            while (pos < segEnd && iter < 256) {
                iter++;
                float currentStep = sf.AdaptiveStepSize(baseStepSize, pos, rayStart, rayLength, input.stepScaleFactor);
                
                // Per-step jitter using golden ratio sequence
                float stepSeed = frac(worldSeed + float(stepIndex) * 0.618033); //TODO: PASS IN BLUE NOISE TEX SO WE CAN SAMPLE IT FOR RANDOM VALUES
                float jitterAmount = input.blueNoiseFactor;
                float stepSizeMultiplier = 1.0 + (stepSeed - 0.5) * 2.0 * jitterAmount;
                float jitteredStep = currentStep * stepSizeMultiplier;
                jitteredStep = max(0.001, min(jitteredStep, segEnd - pos));
                
                float3 samplePos = rayOrigin + rayDir * (pos + jitteredStep * 0.5);
                float distFromCenter = length(samplePos - input.planetPos);
                float height = distFromCenter - input.planetRadius;
                
                // Jittered light direction
                float2 ditherUV = float2(frac(stepSeed * 12.98), frac(stepSeed * 78.23)) * 2.0 - 1.0;
                float3 jitteredLightDir = normalize(input.lightDir + (sunRight * ditherUV.x + sunUp * ditherUV.y) * (0.01 * jitterAmount));
                
                float3 sunTransmittance = float3(1.0, 1.0, 1.0);
                if (height > 0) {
                    lmp.samplePos = samplePos;
                    lmp.lightDir = jitteredLightDir;
                    lmp.blueFactor = 0.0;
                    LightMarchFunctions lightRay;
                    sunTransmittance = lightRay.March(lmp, sf);
                    
                    float3 atmoDens = sf.AtmoDensity(height, input.rayScaleH, input.mieScaleH, input.absHeight, input.absFalloff);
                    float3 rayleighScatter = input.scaledRayBeta * input.phaseR * atmoDens.x;
                    float3 mieScatter = depthLimited ? float3(0,0,0) :input.scaledMieBeta * input.phaseM * atmoDens.y;
                    float3 directScatter = (rayleighScatter + mieScatter) * sunTransmittance * input.lightCol;
                    float3 ambientScatter = input.scaledRayBeta * atmoDens.x * input.atmoAmbient;
                    float3 inscattered = (directScatter + ambientScatter) * jitteredStep;
                    float3 extinction = (input.scaledRayBeta * atmoDens.x + input.scaledMieBeta * atmoDens.y + input.scaledAbsBeta * atmoDens.z) * jitteredStep;
                    float3 atmoStepTransmittance = exp(-extinction);
                    AccumLight += Transmittance * inscattered;
                    Transmittance *= atmoStepTransmittance;
                    
                    if (isCloudBand) {
                        float cloudDensity = sf.CloudDensity(distFromCenter, samplePos, lmp);
                        if (cloudDensity > 0) {
                            float shadowFactor = max(sunTransmittance.x, max(sunTransmittance.y, sunTransmittance.z));
                            float3 incomingLight = input.lightCol * sunTransmittance;
                            float phaseDual = sf.PhaseDualLobe(input.cosTheta, input.cloudPhaseParams.r, input.cloudPhaseParams.g, input.cloudPhaseParams.b);
                            float phaseIsotropic = 1.0 / (4.0 * 3.14159265);
                            float3 multiScatter = incomingLight * phaseIsotropic * input.cloudPhaseParams.a;
                            float3 cloudLight = incomingLight * phaseDual + multiScatter + input.lightCol * input.cloudAmb;                        
                            float3 cloudExtinction = cloudDensity * input.cloudScatBeta * jitteredStep;
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
MainRayParams mrp;
mrp.cosTheta = cosTheta;
mrp.cameraPos = cameraPos; 
mrp.cameraDir = FinalRayDir; 
mrp.planetPos = atmoPos; 
mrp.planetRadius = planetRadius; 
mrp.atmoRadius = atmoRadius; 
mrp.cloudOuterRadius = cloudOuterRadius; 
mrp.cloudInnerRadius = cloudInnerRadius; 
mrp.sceneDepth = correctedDepth;
mrp.atmoSteps = mainAtmoSteps; 
mrp.cloudSteps = mainCloudSteps;
mrp.atmoLightSteps = atmoLightSteps; 
mrp.cloudLightSteps = cloudLightSteps; 
mrp.lightDir = lightDirection; 
mrp.lightCol = lightColor; 
mrp.scaledRayBeta = scaledRayleighBeta; 
mrp.scaledMieBeta = scaledMieBeta; 
mrp.scaledAbsBeta = scaledAbsBeta; 
mrp.atmoAmbient = atmoAmbient; 
mrp.stepScaleFactor = stepScaleFactor; 
mrp.rayScaleH = rayleighScaleHeight; 
mrp.mieScaleH = mieScaleHeight; 
mrp.absHeight = absHeight; 
mrp.absFalloff = absFalloff; 
mrp.cloudScatBeta = scaledCloudScatBeta; 
mrp.cloudAbsBeta = scaledCloudAbsBeta; 
mrp.cloudAmb = cloudAmbient;
mrp.cloudPhaseParams = cloudPhaseParams;
mrp.phaseR = phaseR; 
mrp.phaseM = phaseM; 
mrp.blueNoiseFactor = blueNoiseFactor; 
mrp.sceneCol = sceneColor;
mrp.cloudScale = cloudScale; 
mrp.cloudCoverage = cloudCoverage; 
mrp.cloudDensityMult = cloudDensityMult;
mrp.cloudNoiseWeights = cloudNoiseWeights;
mrp.cloudNoiseInvert = cloudNoiseInvert;
mrp.detailNoiseWeights = detailNoiseWeights;
mrp.detailNoiseInvert = detailNoiseInvert;
mrp.detailNoiseScale = detailNoiseScale;
mrp.detailNoiseErode = detailNoiseErode;

//RAYMARCH
MainRayFunctions mainRay;
return mainRay.March(mrp, sf);