#include "Particle.hlsli"

struct Material
{
    float32_t4 coler;
    int32_t enableLighting;
    float32_t4x4 uvTransform;
};
struct DirectionalLight
{
    float32_t4 color;
    float32_t3 direction;
    float intensity;
};


ConstantBuffer<Material> gMaterial : register(b0);
Texture2D<float32_t4> gTexture : register(t0);
SamplerState gSampler : register(s0);
ConstantBuffer<DirectionalLight> gDirectionalLight : register(b1);


struct PixelShaderOutput
{
    float32_t4 color : SV_TARGET0;
   
};

PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;
    float32_t4 transformedUV = mul(float32_t4(input.texcoord, 0.0f, 1.0f), gMaterial.uvTransform);
    float32_t4 textureColor = gTexture.Sample(gSampler, transformedUV.xy);
    output.color = gMaterial.coler * textureColor * input.color;
    if (output.color.a == 0.0)
    {
        discard;
    }
    
    return output;
}

