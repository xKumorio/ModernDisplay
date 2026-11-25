cbuffer Reprojection : register(b0) {
    row_major float4x4 currToPrevClip;
    float2 invSize;
    float prevNear, prevFar;
    float mip;
    uint history;
    float blendWeight;
    uint phase;
};
Texture2D<float> prevAo : register(t0);
Texture2D<float> deviceDepth : register(t1);
Texture2D<float> linearDepth : register(t2);
Texture2D<float> prevDepth : register(t3);
SamplerState pointSampler : register(s0);

float main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    uint2 tile = uint2(pos.xy) / 8u;
    if (((tile.x + tile.y + phase) & 1u) != 0u) discard;
    float z = deviceDepth.SampleLevel(pointSampler, uv, 0);
    if (z >= 1.0) discard;
    float4 prev = mul(float4(uv * float2(2,-2) + float2(-1,1), z, 1), currToPrevClip);
    if (prev.w <= 0.0) discard;
    float3 ndc = prev.xyz / prev.w;
    float2 prevUV = ndc.xy * float2(0.5,-0.5) + 0.5;
    if (any(prevUV < 2*invSize) || any(prevUV > 1-2*invSize) ||
        any(abs(prevUV-uv) > 3*invSize) || ndc.z <= 0 || ndc.z >= 1) discard;
    float predicted = prevNear * prevFar / (prevFar - ndc.z * (prevFar-prevNear));
    float tolerance = min(0.09, max(0.009, predicted * 0.0015));
    float4 oldZ = prevDepth.GatherRed(pointSampler, prevUV);
    float4 newZ = linearDepth.GatherRed(pointSampler, uv);
    float center = linearDepth.SampleLevel(pointSampler, uv, 0);
    if (any(abs(oldZ-predicted) > tolerance) || any(abs(newZ-center) > tolerance)) discard;
    return prevAo.SampleLevel(pointSampler, prevUV, 0);
}
