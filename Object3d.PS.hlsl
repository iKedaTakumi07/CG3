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
struct SpotLigth
{
    float32_t4 color; // ライトの色
    float32_t3 position; // ライトの位置
    float32_t intensity; // 輝度
    float32_t3 direction; // スポットライトの方向
    float32_t distance; // ライトの届く最大距離
    float32_t decay; // 減衰率
    float32_t cosAngle; // スポットライトの余弦
    float32_t cosFalloffStart;
};


ConstantBuffer<Material> gMaterial : register(b0);
Texture2D<float32_t4> gTexture : register(t0);
SamplerState gSampler : register(s0);
ConstantBuffer<DirectionalLight> gDirectionalLight : register(b1);
ConstantBuffer<Camera> gCamera : register(b2);
ConstantBuffer<PointLight> gPointLight : register(b3);
ConstantBuffer<SpotLigth> gSpotLigth : register(b4);


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
     
        // =========================
        // spotLigth
        // =========================
        
      // 1. 光源への方向ベクトルと距離 (Surface -> Light)
        float32_t3 spotLightDir = normalize(gSpotLigth.position - input.worldPosition);
        float32_t spotLightDistance = length(gSpotLigth.position - input.worldPosition);

// 2. 距離による減衰 (PointLightと同じ)
// 距離がgSpotLigth.distanceに達すると0になるように計算
        float32_t distanceAttenuation = pow(saturate(-spotLightDistance / gSpotLigth.distance + 1.0f), gSpotLigth.decay);

// 3. 角度による減衰 (SpotLight特有)
// 光源からサーフェスへの方向 (Light -> Surface) を使って計算
        float32_t3 spotLigthDirectionOnSurface = normalize(input.worldPosition - gSpotLigth.position);
        float32_t currentCosAngle = dot(spotLigthDirectionOnSurface, gSpotLigth.direction);

// 角度が内側(falloffStart)から外側(cosAngle)に向かって滑らかに0になるようにする
        float32_t falloffFactor = saturate((currentCosAngle - gSpotLigth.cosAngle) / (gSpotLigth.cosFalloffStart - gSpotLigth.cosAngle));

// 全体の減衰率 = 距離減衰 × 角度減衰
        float32_t spotLightAttenuation = distanceAttenuation * falloffFactor;

// --- 拡散反射 (Diffuse / Lambert) ---
        float32_t spotNDotL = saturate(dot(normalize(input.normal), spotLightDir));
        float32_t3 spDiffuse = gMaterial.color.rgb * textureColor.rgb * gSpotLigth.color.rgb * spotNDotL * gSpotLigth.intensity;

// --- 鏡面反射 (Specular / Blinn-Phong) ---
// カメラへの方向ベクトル
        float32_t3 viewDir = normalize(gCamera.worldPosition - input.worldPosition);
// ハーフベクトル (光源への方向とカメラへの方向の中間)
        float32_t3 spotHalfVector = normalize(spotLightDir + viewDir);
        float32_t spotNDotH = saturate(dot(normalize(input.normal), spotHalfVector));

// 鏡面反射の強さ（shininess はマテリアルの光沢度。通常10.0〜100.0程度）
        float32_t spotSpecularPow = pow(spotNDotH, gMaterial.shininess);
        float32_t3 spSpecular = gSpotLigth.color.rgb * gSpotLigth.intensity * spotSpecularPow;

// --- 減衰の適用 ---
        spDiffuse *= spotLightAttenuation;
        spSpecular *= spotLightAttenuation;
        
        // 拡散反射 + 鏡面反射
        output.color.rgb = DirectionalLight_diffuse + ptDiffuse + spDiffuse + DirectionalLight_specular + ptSpecular + spSpecular;
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

