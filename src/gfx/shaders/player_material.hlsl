sampler2D skin : register(s0);

float4 main(float2 uv : TEXCOORD0, float4 diffuse : COLOR0) : COLOR0
{
    float4 material = tex2D(skin, uv);
    // Preserve the skin texture in the reflection, including parts illuminated
    // only by ambient light on the underside of the animated model.
    float3 lighting = 0.56 + 0.44 * saturate(diffuse.rgb);
    return float4(material.rgb * lighting, material.a * diffuse.a);
}
