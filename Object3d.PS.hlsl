#include "object3d.hlsli"

struct Material
{
    float32_t4 color;
    int32_t enableLighting;
    float32_t4x4 uvTransform;
    float32_t shininess;
};

struct DirectionalLight
{
    float32_t4 color;
    float32_t3 direction;
    float intensity;
};
struct Camera
{
    float32_t3 worldPosition;
};


ConstantBuffer<Material> gMaterial : register(b0);
Texture2D<float32_t4> gTexture : register(t0);
SamplerState gSampler : register(s0);
ConstantBuffer<DirectionalLight> gDirectionalLight : register(b1);
ConstantBuffer<Camera> gCamera : register(b2);


struct PixelShaderOutput
{
    float32_t4 color : SV_TARGET0;
};

PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;
    
    // UV
    float4 transformedUV = mul(float32_t4(input.texcoord, 0.0f, 1.0f), gMaterial.uvTransform);
    float32_t4 textureColor = gTexture.Sample(gSampler, transformedUV.xy);

    // 早期リターン
    if (textureColor.a == 0.0)
    {
        discard;
    }
    
  if (gMaterial.enableLighting != 0)
    {
        // 正規化：補間されたベクトルの再正規化は必須
        float32_t3 N = normalize(input.normal);
        float32_t3 L = normalize(gDirectionalLight.direction); // 光源方向
        float32_t3 V = normalize(gCamera.worldPosition - input.worldPosition); // 視線方向
        
        // Blinn-Phong: Half-Vector
        float32_t3 H = normalize(-L + V);
        
        // Specular
        float dotNH = dot(N, H);
        float specularPow = pow(saturate(dotNH), gMaterial.shininess);
        
        // Half-Lambert Diffuse
        float dotNL = dot(N, -L);
        float halfLambert = pow(dotNL * 0.5f + 0.5f, 2.0f);
        
        float32_t3 diffuse = gMaterial.color.rgb * textureColor.rgb * gDirectionalLight.color.rgb * halfLambert * gDirectionalLight.intensity;
        float32_t3 specular = gDirectionalLight.color.rgb * gDirectionalLight.intensity * specularPow;
        
        output.color.rgb = diffuse + specular;
        output.color.a = gMaterial.color.a * textureColor.a;
    }
    else
    {
        output.color = gMaterial.color * textureColor;
    }

    // 最終的なαチェック
    if (output.color.a <= 0.0f) { discard; }
    
    return output;
}

