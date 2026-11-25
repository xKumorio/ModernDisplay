cbuffer SsrParams : register(b0)
{
    row_major float4x4 invViewProj;
    row_major float4x4 viewProj;
    float3 cameraPosition;
    float nearZ;
    float2 invSize;
    float farZ;
    float wetness;
    float3 skyTop;
    uint quality;
    float3 skyBottom;
    float timeSeconds;
    float2 roadGridOrigin;
    float roadGridSpacing;
    float unknownMaterialOffset;
    float4 actorCrop;
    float4 actorGround;
};

Texture2D<float4> worldColor : register(t0);
Texture2D<float> linearDepth : register(t1);
Texture2D<float> sceneDepth : register(t2);
Texture2D<float> roadHeights : register(t3);
Texture2D<float4> classicReflection : register(t4);
Texture2D<float4> actorReflection : register(t5);
SamplerState pointSampler : register(s0);
SamplerState linearSampler : register(s1);

float Linearize(float depth)
{
    return nearZ * farZ / max(farZ - depth * (farZ - nearZ), 1e-4);
}

float3 WorldPosition(float2 uv, float depth)
{
    float2 ndc = uv * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, depth, 1), invViewProj);
    return world.xyz / max(world.w, 1e-6);
}

float2 Hash(float2 cell)
{
    float3 p = frac(cell.xyx * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yzx + 33.33);
    return frac((p.xx + p.yz) * p.zy);
}

float BasinNoise(float2 p)
{
    float2 cell = floor(p);
    float2 f = frac(p);
    f *= f * (3.0 - 2.0 * f);
    return lerp(lerp(Hash(cell).x, Hash(cell + float2(1, 0)).x, f.x),
                lerp(Hash(cell + float2(0, 1)).x, Hash(cell + 1).x, f.x), f.y);
}

float BasinDepth(float2 xy)
{
    float2 p = float2(dot(xy, float2(0.22, 0.11)), dot(xy, float2(-0.16, 0.32)));
    float basin = 0.72 * BasinNoise(p) + 0.22 * BasinNoise(p * 2.67 + 13.7)
                + 0.06 * BasinNoise(xy * 2.35);
    return lerp(0.27, 0.49, wetness) - basin;
}

float3 Ripple(float2 worldXY)
{
    const float spacing = 0.46;
    float2 grid = worldXY / spacing;
    float2 cell = floor(grid);
    float2 random = Hash(cell);
    float cycle = timeSeconds * (0.76 + random.y * 0.45) + random.x;
    float phase = frac(cycle);
    float2 center = Hash(cell + frac(floor(cycle) * 0.618034) * 27.1);
    float2 offset = (frac(grid) - (0.24 + 0.52 * center)) * spacing;
    float radius = length(offset);
    float wave = radius - phase * lerp(0.115, 0.22, center.y);
    float lifetime = smoothstep(0.04, 0.18, phase) * (1.0 - smoothstep(0.62, 1.0, phase));
    float envelope = saturate(1.0 - abs(wave) * 26.0) * lifetime * smoothstep(0.06, 0.68, center.x);
    float cellFade = saturate((0.5 - max(abs(frac(grid).x - 0.5), abs(frac(grid).y - 0.5))) * 10.0);
    float2 slope = offset / max(radius, 0.012) * sin(wave * 148.0) * envelope * cellFade;
    float crest = dot(slope, float2(-0.48, 0.88));
    return float3(slope, crest);
}

float RoadCoverage(float3 world, float3 sceneColor)
{
    float2 cell = world.xy / roadGridSpacing - roadGridOrigin;
    if (any(cell < 0) || any(cell >= 31.0)) return 0;
    int2 first = int2(floor(cell));
    float2 blend = frac(cell);
    float4 heights = float4(roadHeights.Load(int3(first, 0)),
                            roadHeights.Load(int3(first + int2(1, 0), 0)),
                            roadHeights.Load(int3(first + int2(0, 1), 0)),
                            roadHeights.Load(int3(first + int2(1, 1), 0)));
    float4 weights = float4((1.0 - blend.x) * (1.0 - blend.y), blend.x * (1.0 - blend.y),
                            (1.0 - blend.x) * blend.y, blend.x * blend.y);
    float4 unknown = step(unknownMaterialOffset * 0.5, heights);
    heights -= unknown * unknownMaterialOffset;
    float highest = max(max(heights.x, heights.y), max(heights.z, heights.w));
    float ground = 1.0 - smoothstep(0.055, 0.13, world.z - highest);
    float grass = smoothstep(0.012, 0.085, sceneColor.g - max(sceneColor.r, sceneColor.b));
    float4 nearSurface = (1.0 - smoothstep(0.12, 0.34, abs(heights - world.z))) * (1.0 - unknown * grass);
    return ground * smoothstep(0.25, 0.75, dot(nearSurface, weights));
}

float2 ScreenUV(float4 clip)
{
    return clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
}

float2 MirroredActorUV(float3 pos)
{
    pos.z = 2 * actorGround.w - pos.z;
    float4 clip = mul(float4(pos, 1), viewProj);
    if (clip.w <= nearZ) return -1;
    return (ScreenUV(clip) - actorCrop.xy) / actorCrop.zw;
}

float4 PlayerSample(float3 surface, float3 ray)
{
    if (actorCrop.z <= 0 || abs(surface.z - actorGround.w) > 0.45) return 0;
    float t = dot(actorGround.xyz - surface, ray);
    if (t <= 0) return 0;
    float3 probe = surface + t * ray;
    if (distance(probe, actorGround.xyz) > 2.2) return 0;
    float2 uv = MirroredActorUV(probe);
    if (any(uv <= 0) || any(uv >= 1)) return 0;
    return actorReflection.SampleLevel(linearSampler, uv, 0);
}

float4 TraceReflection(float3 surface, float3 floorNormal, float3 direction)
{
    float3 origin = surface + 0.0025 * floorNormal;
    float4 start = mul(float4(origin, 1), viewProj);
    float4 along = mul(float4(direction, 0), viewProj);
    uint width, height;
    sceneDepth.GetDimensions(width, height);
    float2 extent = float2(width, height);
    float lastT = 0;
    float lastDelta = -0.0025;
    uint steps = (quality & 0xff) >= 2 ? 26 : 17;
    [loop]
    for (uint step = 1; step <= steps; ++step) {
        // Start within a shoe tread of the receiver. The first few samples
        // must cover the contact before the ray advances to a distant facade.
        float t = (step - 0.6) / (steps - 0.6);
        float dist = 0.005 + t * t * (5.0 + t * (57.0 + t * 54.0));
        float4 clip = start + along * dist;
        if (clip.w <= nearZ) break;
        float2 uv = ScreenUV(clip);
        if (any(uv <= 0) || any(uv >= 1)) break;
        float delta = clip.w - linearDepth.SampleLevel(pointSampler, uv, 0);
        if (dist < 1.0 && delta >= 0) {
            float2 center = (floor(uv * extent) + 0.5) / extent;
            float3 sampleWorld = WorldPosition(center, sceneDepth.SampleLevel(pointSampler, center, 0));
            // A nearest-depth tap can still belong to the receiver's texel.
            // Keep searching instead of accepting the pavement as a shoe.
            if (dot(sampleWorld - surface, floorNormal) <= 0.002) delta = -0.0025;
        }
        if (lastDelta < 0 && delta >= 0) {
            float front = lastT, back = dist;
            float frontDelta = lastDelta, backDelta = delta;
            // On a facade the signed depth gap is nearly linear in ray distance.
            float hitT = lerp(front, back, -frontDelta / max(backDelta - frontDelta, 1e-5));
            float4 hit = start + along * hitT;
            float2 hitUV = ScreenUV(hit);
            float gap = hit.w - linearDepth.SampleLevel(pointSampler, hitUV, 0);
            if (abs(gap) > 0.025) {
                if (gap >= 0) { back = hitT; backDelta = gap; }
                else { front = hitT; frontDelta = gap; }
                [unroll]
                for (int refine = 0; refine < 3; ++refine) {
                    float mid = (front + back) * 0.5;
                    float4 test = start + along * mid;
                    float midGap = test.w - linearDepth.SampleLevel(pointSampler, ScreenUV(test), 0);
                    if (midGap >= 0) { back = mid; backDelta = midGap; }
                    else { front = mid; frontDelta = midGap; }
                }
                hitT = lerp(front, back, -frontDelta / max(backDelta - frontDelta, 1e-5));
                hit = start + along * hitT;
                hitUV = ScreenUV(hit);
                gap = hit.w - linearDepth.SampleLevel(pointSampler, hitUV, 0);
            }
            // At a silhouette the secant may land on background. Use the
            // foreground endpoint only within a nearby object's thickness.
            float closeObject = (1.0 - smoothstep(3.0, 8.0, hitT)) * (1.0 - smoothstep(8.0, 14.0, hit.w));
            float thickness = 0.08 + min(hit.w * 0.009, 0.24) + 0.30 * closeObject;
            if (gap < -0.004 && backDelta < thickness * 1.6 && closeObject > 0.01) {
                hitT = back;
                hit = start + along * hitT;
                hitUV = ScreenUV(hit);
                gap = backDelta;
            }
            float error = abs(gap);
            float confidence = 1.0 - smoothstep(thickness, 2.0 * thickness, error);
            float edge = saturate(min(min(hitUV.x, 1.0 - hitUV.x), min(hitUV.y, 1.0 - hitUV.y)) * 14.0);
            float3 fromCamera = origin - cameraPosition + direction * hitT;
            float3 visibleOffset = (origin - surface) + direction * hitT - fromCamera * (gap / hit.w);
            float heightAboveRoad = dot(visibleOffset, floorNormal);
            float influence = edge * smoothstep(0.002, 0.012, heightAboveRoad) * confidence;
            // The dedicated model pass owns the player silhouette. Reject old
            // screen-depth hits on the same body to avoid streaks beside it.
            float3 source = surface + visibleOffset;
            if (influence > 0.001 && actorCrop.z > 0 && abs(surface.z - actorGround.w) < 0.45 &&
                all(abs(source - actorGround.xyz) < float3(1,1,1.35))) {
                float2 actorUV = MirroredActorUV(source);
                if (all(actorUV > 0) && all(actorUV < 1) && actorReflection.SampleLevel(pointSampler, actorUV, 0).a > 0.5)
                    influence = 0;
            }
            if (influence > 0.001)
                return float4(worldColor.SampleLevel(linearSampler, hitUV, 0).rgb, influence);
        }
        lastDelta = delta;
        lastT = dist;
    }
    return 0;
}

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    float4 classic = classicReflection.SampleLevel(linearSampler, uv, 0);
    float sceneZ = sceneDepth.SampleLevel(pointSampler, uv, 0);
    float distance = Linearize(sceneZ);
    float3 world = WorldPosition(uv, sceneZ);
    float3 horizontal = ddx(world);
    float3 vertical = ddy(world);
    float3 surfaceNormal = cross(horizontal, vertical);
    float planar = abs(surfaceNormal.z) * rsqrt(max(dot(surfaceNormal, surfaceNormal), 1e-12));
    if (sceneZ >= 0.9998 || distance > 54.0 || planar < 0.75) return classic;
    float3 asphalt = worldColor.SampleLevel(linearSampler, uv, 0).rgb;
    float roadCoverage = RoadCoverage(world, asphalt);
    if (roadCoverage < 0.02) return classic;

    float depth = BasinDepth(world.xy);
    float coverage = smoothstep(-0.012, 0.022, depth) * smoothstep(0.75, 0.96, planar);
    float shore = smoothstep(-0.055, 0.0, depth);
    float3 wetAsphalt = asphalt * (1.0 - wetness * (0.08 + 0.16 * shore));
    float3 result = lerp(wetAsphalt, classic.rgb, classic.a * 0.52);

    if (coverage > 0.02) {
        float3 ripple = Ripple(world.xy) * (wetness * (1.0 - smoothstep(8.0, 24.0, distance)));
        float windPhase = dot(world.xy, float2(2.24, 1.57)) + timeSeconds * 1.25;
        float2 slope = ripple.xy * 0.020 + float2(0.0018, 0.0014) * sin(windPhase)
                     + float2(-0.0014, 0.0018) * sin(dot(world.xy, float2(-3.9, 5.1)) - timeSeconds * 1.65);
        float3 normal = normalize(float3(slope, 1));
        float3 view = normalize(cameraPosition - world);
        float3 ray = reflect(-view, normal);
        float grazing = 1.0 - saturate(dot(view, normal));
        float fresnel = 0.025 + 0.975 * pow(grazing, 5.0);
        float elevation = saturate(ray.z * 1.6);
        float3 sky = lerp(skyBottom, skyTop, elevation);
        float3 reflectedSky = max(sky, float3(0.25, 0.265, 0.285)) * lerp(1.08, 0.94, elevation);
        // The sky actually drawn by weather mods can differ from the timecycle colours.
        float4 farClip = mul(float4(ray, 0), viewProj);
        if (farClip.w > 0.001) {
            float2 farUV = farClip.xy / farClip.w * float2(0.5, -0.5) + 0.5;
            float border = saturate(min(min(farUV.x, farUV.y), min(1.0 - farUV.x, 1.0 - farUV.y)) * 18.0);
            float skyWeight = border * smoothstep(farZ * 0.8, farZ * 0.98, linearDepth.SampleLevel(pointSampler, farUV, 0));
            reflectedSky = lerp(reflectedSky, worldColor.SampleLevel(linearSampler, farUV, 0).rgb, skyWeight);
        }
        float skyLuma = dot(reflectedSky, float3(0.2126, 0.7152, 0.0722));
        reflectedSky = lerp(reflectedSky, skyLuma.xxx, 0.30 * wetness);
        float3 floorNormal = normalize(surfaceNormal) * sign(surfaceNormal.z);
        float4 actor = PlayerSample(world, ray);
        float4 hit = 0;
        if (actor.a < 0.995) hit = TraceReflection(world, floorNormal, ray);
        float3 reflected = lerp(reflectedSky, hit.rgb, hit.a);
        reflected = lerp(reflected, actor.rgb, actor.a);
        float waterDepth = smoothstep(0.0, 0.095, depth);
        float3 bed = lerp(wetAsphalt, asphalt * float3(0.63, 0.66, 0.68), waterDepth);
        float reflectionWeight = lerp(0.28 + 0.30 * waterDepth, 0.96, fresnel);
        // Opaque body details should survive the asphalt showing through shallower water.
        reflectionWeight = lerp(reflectionWeight, max(reflectionWeight, 0.80), actor.a * waterDepth);
        float3 puddle = sqrt(lerp(bed * bed, reflected * reflected, reflectionWeight));
        puddle += reflectedSky * ripple.z * (0.08 + 0.11 * fresnel);
        result = lerp(result, puddle, coverage);
    }

    float opacity = wetness * roadCoverage * (1.0 - smoothstep(35.0, 54.0, distance)) * lerp(0.55, 1.0, coverage);
    float4 road = float4(result, opacity);
    float blend = smoothstep(0.03, 0.22, opacity);
    float alpha = lerp(classic.a, road.a, blend);
    float3 premultiplied = lerp(classic.rgb * classic.a, road.rgb * road.a, blend);
    return float4(premultiplied / max(alpha, 0.001), alpha);
}
