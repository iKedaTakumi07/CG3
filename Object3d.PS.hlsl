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
struct PointLight
{
    float32_t4 color; // 色
    float32_t3 position; // 位置
    float intensity; // 輝度
    float radius; // ライトの届く最大距離
    float decay; // 減衰率
};


ConstantBuffer<Material> gMaterial : register(b0);
Texture2D<float32_t4> gTexture : register(t0);
SamplerState gSampler : register(s0);
ConstantBuffer<DirectionalLight> gDirectionalLight : register(b1);
ConstantBuffer<Camera> gCamera : register(b2);
ConstantBuffer<PointLight> gPointLight : register(b3);


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
        // ============================
        // DirectionalLight
        // ===========================
        
        // half lambert
        float NdotL = dot(normalize(input.normal), -gDirectionalLight.direction);
        float cos = pow(NdotL * 0.5f + 0.5f, 2.0f);
    
        // 拡散反射
        float32_t3 DirectionalLight_diffuse = gMaterial.color.rgb * textureColor.rgb * gDirectionalLight.color.rgb * cos * gDirectionalLight.intensity;
        
        // PixelShaderでCameraへの方向を算出
        float32_t3 toEye = normalize(gCamera.worldPosition - input.worldPosition);
        
        // blinn-phong
        float32_t3 halfVector = normalize(-gDirectionalLight.direction + toEye);
        float NDotH = dot(normalize(input.normal), halfVector);
        float specularPow = pow(saturate(NDotH), gMaterial.shininess);

        // 鏡面反射
        float32_t3 DirectionalLight_specular = gDirectionalLight.color.rgb * gDirectionalLight.intensity * specularPow * float32_t3(1.0f, 1.0f, 1.0f);
     
        
        // =======================
        // pointLight
        // =======================
        
        // 入射光 
        float32_t3 pointLightDirection = normalize(gPointLight.position - input.worldPosition);
        // ポイントライトへの距離
        float32_t distance = length(gPointLight.position - input.worldPosition);
        // 逆二乗則による減衰係数
        float32_t factor = pow(saturate(-distance / gPointLight.radius + 1.0), gPointLight.decay);
        
        float NdotL_pt = saturate(dot(input.normal, pointLightDirection));
        float32_t3 ptDiffuse = gMaterial.color.rgb * textureColor.rgb * gPointLight.color.rgb * gPointLight.intensity * factor * NdotL_pt;

        // --- 鏡面反射 (Blinn-Phong) ---
        float32_t3 halfVector_pt = normalize(pointLightDirection + toEye);
        float NDotH_pt = saturate(dot(input.normal, halfVector_pt));
        float specularPow_pt = pow(NDotH_pt, gMaterial.shininess);
        float32_t3 ptSpecular = gPointLight.color.rgb * gPointLight.intensity * factor * specularPow_pt;
     
        // 拡散反射 + 鏡面反射
        output.color.rgb = DirectionalLight_diffuse + ptDiffuse + DirectionalLight_specular + ptSpecular;
        // αはいつも通り
        output.color.a = gMaterial.color.a * textureColor.a;
    }
    else
    {
        output.color = gMaterial.color * textureColor;
    }

    // 最終的なαチェック
    if (output.color.a <= 0.0f)
    {
        discard;
    }
    
    return output;
}

