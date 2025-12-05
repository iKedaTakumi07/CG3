#include "externals/DirectXTex/d3dx12.h"

#include "externals/DirectXTex/DirectXTex.h"
#include "externals/imgui/imgui.h"
#include "externals/imgui/imgui_impl_dx12.h"
#include "externals/imgui/imgui_impl_win32.h"
#include <DbgHelp.h>
#include <Windows.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <d3d12.h>
#include <dxcapi.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <numbers>
#include <random>
#include <sstream>
#include <string>
#include <strsafe.h>
#include <vector>
#include <xaudio2.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Dbghelp.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "dxcompiler.lib")
#pragma comment(lib, "xaudio2.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct Vector2 {
    float x;
    float y;
};
struct Vector3 {
    float x;
    float y;
    float z;
};
struct Vector4 {
    float x;
    float y;
    float z;
    float w;
};
struct Matrix4x4 {
    float m[4][4];
};
struct Transform {
    Vector3 scale;
    Vector3 rotate;
    Vector3 translate;
};
struct VertexData {
    Vector4 position;
    Vector2 texcoord;
    Vector3 normal;
};
struct Material {
    Vector4 color;
    int32_t enableLighting;
    float padding[3];
    Matrix4x4 uvTransform;
    float shininess;
};
struct TransformationMatrix {
    Matrix4x4 WVP;
    Matrix4x4 world;
};
struct ParticleForGPU {
    Matrix4x4 WVP;
    Matrix4x4 world;
    Vector4 color;
};
struct DirectionalLight {
    Vector4 color;
    Vector3 direction;
    float intensity;
};
struct MaterialData {
    std::string textureFilePath;
};
struct ModelData {
    std::vector<VertexData> vertices;
    MaterialData material;
};
struct D3DResourceLeakChecker {
    ~D3DResourceLeakChecker()
    {
        // リソースチェック
        Microsoft::WRL::ComPtr<IDXGIDebug1> debug;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug)))) {
            debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
            debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
            debug->ReportLiveObjects(DXGI_DEBUG_D3D12, DXGI_DEBUG_RLO_ALL);
        }
    }
};
struct ChunkHeader {
    char id[4]; // チャンク毎のID
    int32_t size; // チャンクサイズ
};
struct RiffHeader {
    ChunkHeader chunk; // "RIFF"
    char type[4]; // "WAVE"
};
struct FormatChunk {
    ChunkHeader chunk; // "fmt"
    WAVEFORMATEX fmt; // 波型フォーマット
};
struct SoundData {
    // 波型フォーマット
    WAVEFORMATEX wfex;
    // バッフアの先頭アドレス
    BYTE* pBuffer;
    // バッフアのサイズ
    unsigned int bufferSize;
};
struct Particle {
    Transform transform;
    Vector3 velocity;
    Vector4 color;
    float lifeTime;
    float currentTime;
};
enum BlendMode {
    kBlendModeNone, // ブレンドなし
    kBlendModeNormal, // 通常αブレンド
    kBlendModeAdd, // 加算
    kBlendModeSubtract, // 減算
    kBlendModeMultily, // 乗算
    kBlendModeScreen, // スクリーン
};
struct Emitter {
    Transform transform;
    uint32_t count;
    float frequency;
    float ferquencyTime;
};
struct CameraForGPU {
    Vector3 worldPosition;
};

Particle MakeNewParticle(std::mt19937& randomEngine, const Vector3& translate)
{
    std::uniform_real_distribution<float> distribution(-1.0f, 1.0f);
    std::uniform_real_distribution<float> distColor(0.0f, 1.0f);
    std::uniform_real_distribution<float> distTime(1.0f, 3.0f);
    Particle particle;
    particle.transform.scale = { 1.0f, 1.0f, 1.0f };
    particle.transform.rotate = { 0.0f, 0.0f, 0.0f };
    Vector3 randomTranslate { distribution(randomEngine), distribution(randomEngine), distribution(randomEngine) };
    particle.transform.translate = { translate.x + randomTranslate.x, translate.y + randomTranslate.y, translate.z + randomTranslate.z };
    particle.velocity = { distribution(randomEngine), distribution(randomEngine), distribution(randomEngine) };
    particle.color = { distColor(randomEngine), distColor(randomEngine), distColor(randomEngine), 1.0f };
    particle.lifeTime = distTime(randomEngine);
    particle.currentTime = 0;
    return particle;
}

std::list<Particle> Emit(const Emitter& emitter, std::mt19937& randomEngine)
{
    std::list<Particle> particles;
    for (uint32_t count = 0; count < emitter.count; ++count) {
        particles.push_back(MakeNewParticle(randomEngine, emitter.transform.translate));
    }
    return particles;
}

D3D12_BLEND_DESC CreateBlendDesc(BlendMode mode)
{

    // レンダーターゲットのブレンド設定
    D3D12_BLEND_DESC desc {};
    desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; // RBGA全てのチャンネルを描画
    desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;

    switch (mode) {
    case kBlendModeNone:
        desc.RenderTarget[0].BlendEnable = false;
        break;
    case kBlendModeNormal:
        desc.RenderTarget[0].BlendEnable = true;
        desc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        desc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        break;
    case kBlendModeAdd:
        desc.RenderTarget[0].BlendEnable = true;
        desc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        desc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        break;
    case kBlendModeSubtract:
        desc.RenderTarget[0].BlendEnable = true;
        desc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_REV_SUBTRACT;
        desc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        break;
    case kBlendModeMultily:
        desc.RenderTarget[0].BlendEnable = true;
        desc.RenderTarget[0].SrcBlend = D3D12_BLEND_ZERO;
        desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        desc.RenderTarget[0].DestBlend = D3D12_BLEND_SRC_COLOR;
        break;
    case kBlendModeScreen:
        desc.RenderTarget[0].BlendEnable = true;
        desc.RenderTarget[0].SrcBlend = D3D12_BLEND_INV_DEST_COLOR;
        desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        desc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        break;
    default:
        break;
    }

    return desc;
}

Matrix4x4 MakeIdentity4x4()
{
    Matrix4x4 num;
    num = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
    return num;
}

Matrix4x4 MakeRotateXMatrix(float radian)
{
    Matrix4x4 num;
    num = { 1, 0, 0, 0,
        0, std::cos(radian), std::sin(radian), 0,
        0, std::sin(-radian), std::cos(radian), 0,
        0, 0, 0, 1 };
    return num;
}
Matrix4x4 MakeRotateYMatrix(float radian)
{
    Matrix4x4 num;
    num = { std::cos(radian), 0, std::sin(-radian), 0,
        0, 1, 0, 0,
        std::sin(radian), 0, std::cos(radian), 0,
        0, 0, 0, 1 };
    return num;
}
Matrix4x4 MakeRotateZMatrix(float radian)
{
    Matrix4x4 num;
    num = { std::cos(radian), std::sin(radian), 0, 0,
        std::sin(-radian), std::cos(radian), 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1 };
    return num;
}
Matrix4x4 MakeScaleMatrix(const Vector3& scale)
{

    Matrix4x4 result { scale.x, 0.0f, 0.0f, 0.0f, 0.0f, scale.y, 0.0f, 0.0f, 0.0f, 0.0f, scale.z, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };

    return result;
}
Matrix4x4 MakeTranslateMatrix(const Vector3& translate)
{
    Matrix4x4 result { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, translate.x, translate.y, translate.z, 1.0f };

    return result;
}
Matrix4x4 Multiply(const Matrix4x4& m1, const Matrix4x4& m2)
{
    Matrix4x4 num;
    num.m[0][0] = m1.m[0][0] * m2.m[0][0] + m1.m[0][1] * m2.m[1][0] + m1.m[0][2] * m2.m[2][0] + m1.m[0][3] * m2.m[3][0];
    num.m[0][1] = m1.m[0][0] * m2.m[0][1] + m1.m[0][1] * m2.m[1][1] + m1.m[0][2] * m2.m[2][1] + m1.m[0][3] * m2.m[3][1];
    num.m[0][2] = m1.m[0][0] * m2.m[0][2] + m1.m[0][1] * m2.m[1][2] + m1.m[0][2] * m2.m[2][2] + m1.m[0][3] * m2.m[3][2];
    num.m[0][3] = m1.m[0][0] * m2.m[0][3] + m1.m[0][1] * m2.m[1][3] + m1.m[0][2] * m2.m[2][3] + m1.m[0][3] * m2.m[3][3];

    num.m[1][0] = m1.m[1][0] * m2.m[0][0] + m1.m[1][1] * m2.m[1][0] + m1.m[1][2] * m2.m[2][0] + m1.m[1][3] * m2.m[3][0];
    num.m[1][1] = m1.m[1][0] * m2.m[0][1] + m1.m[1][1] * m2.m[1][1] + m1.m[1][2] * m2.m[2][1] + m1.m[1][3] * m2.m[3][1];
    num.m[1][2] = m1.m[1][0] * m2.m[0][2] + m1.m[1][1] * m2.m[1][2] + m1.m[1][2] * m2.m[2][2] + m1.m[1][3] * m2.m[3][2];
    num.m[1][3] = m1.m[1][0] * m2.m[0][3] + m1.m[1][1] * m2.m[1][3] + m1.m[1][2] * m2.m[2][3] + m1.m[1][3] * m2.m[3][3];

    num.m[2][0] = m1.m[2][0] * m2.m[0][0] + m1.m[2][1] * m2.m[1][0] + m1.m[2][2] * m2.m[2][0] + m1.m[2][3] * m2.m[3][0];
    num.m[2][1] = m1.m[2][0] * m2.m[0][1] + m1.m[2][1] * m2.m[1][1] + m1.m[2][2] * m2.m[2][1] + m1.m[2][3] * m2.m[3][1];
    num.m[2][2] = m1.m[2][0] * m2.m[0][2] + m1.m[2][1] * m2.m[1][2] + m1.m[2][2] * m2.m[2][2] + m1.m[2][3] * m2.m[3][2];
    num.m[2][3] = m1.m[2][0] * m2.m[0][3] + m1.m[2][1] * m2.m[1][3] + m1.m[2][2] * m2.m[2][3] + m1.m[2][3] * m2.m[3][3];

    num.m[3][0] = m1.m[3][0] * m2.m[0][0] + m1.m[3][1] * m2.m[1][0] + m1.m[3][2] * m2.m[2][0] + m1.m[3][3] * m2.m[3][0];
    num.m[3][1] = m1.m[3][0] * m2.m[0][1] + m1.m[3][1] * m2.m[1][1] + m1.m[3][2] * m2.m[2][1] + m1.m[3][3] * m2.m[3][1];
    num.m[3][2] = m1.m[3][0] * m2.m[0][2] + m1.m[3][1] * m2.m[1][2] + m1.m[3][2] * m2.m[2][2] + m1.m[3][3] * m2.m[3][2];
    num.m[3][3] = m1.m[3][0] * m2.m[0][3] + m1.m[3][1] * m2.m[1][3] + m1.m[3][2] * m2.m[2][3] + m1.m[3][3] * m2.m[3][3];

    return num;
}
Vector3 Multiply(const Vector3& m1, const float& m2)
{
    Vector3 num;
    num.x = m1.x * m2;
    num.y = m1.y * m2;
    num.z = m1.z * m2;

    return num;
}
Matrix4x4 Inverse(const Matrix4x4& m)
{
    float determinant;
    Matrix4x4 num;

    determinant = m.m[0][0] * m.m[1][1] * m.m[2][2] * m.m[3][3] + m.m[0][0] * m.m[1][2] * m.m[2][3] * m.m[3][1] + m.m[0][0] * m.m[1][3] * m.m[2][1] * m.m[3][2]
        - m.m[0][0] * m.m[1][3] * m.m[2][2] * m.m[3][1] - m.m[0][0] * m.m[1][2] * m.m[2][1] * m.m[3][3] - m.m[0][0] * m.m[1][1] * m.m[2][3] * m.m[3][2]
        - m.m[0][1] * m.m[1][0] * m.m[2][2] * m.m[3][3] - m.m[0][2] * m.m[1][0] * m.m[2][3] * m.m[3][1] - m.m[0][3] * m.m[1][0] * m.m[2][1] * m.m[3][2]
        + m.m[0][3] * m.m[1][0] * m.m[2][2] * m.m[3][1] + m.m[0][2] * m.m[1][0] * m.m[2][1] * m.m[3][3] + m.m[0][1] * m.m[1][0] * m.m[2][3] * m.m[3][2]
        + m.m[0][1] * m.m[1][2] * m.m[2][0] * m.m[3][3] + m.m[0][2] * m.m[1][3] * m.m[2][0] * m.m[3][1] + m.m[0][3] * m.m[1][1] * m.m[2][0] * m.m[3][2]
        - m.m[0][3] * m.m[1][2] * m.m[2][0] * m.m[3][1] - m.m[0][2] * m.m[1][1] * m.m[2][0] * m.m[3][3] - m.m[0][1] * m.m[1][3] * m.m[2][0] * m.m[3][2]
        - m.m[0][1] * m.m[1][2] * m.m[2][3] * m.m[3][0] - m.m[0][2] * m.m[1][3] * m.m[2][1] * m.m[3][0] - m.m[0][3] * m.m[1][1] * m.m[2][2] * m.m[3][0]
        + m.m[0][3] * m.m[1][2] * m.m[2][1] * m.m[3][0] + m.m[0][2] * m.m[1][1] * m.m[2][3] * m.m[3][0] + m.m[0][1] * m.m[1][3] * m.m[2][2] * m.m[3][0];

    if (determinant == 0.0f) {
        return m;
    };

    num.m[0][0] = (m.m[1][1] * m.m[2][2] * m.m[3][3] + m.m[1][2] * m.m[2][3] * m.m[3][1] + m.m[1][3] * m.m[2][1] * m.m[3][2] - m.m[1][3] * m.m[2][2] * m.m[3][1] - m.m[1][2] * m.m[2][1] * m.m[3][3] - m.m[1][1] * m.m[2][3] * m.m[3][2]) / determinant;
    num.m[0][1] = (-m.m[0][1] * m.m[2][2] * m.m[3][3] - m.m[0][2] * m.m[2][3] * m.m[3][1] - m.m[0][3] * m.m[2][1] * m.m[3][2] + m.m[0][3] * m.m[2][2] * m.m[3][1] + m.m[0][2] * m.m[2][1] * m.m[3][3] + m.m[0][1] * m.m[2][3] * m.m[3][2]) / determinant;
    num.m[0][2] = (m.m[0][1] * m.m[1][2] * m.m[3][3] + m.m[0][2] * m.m[1][3] * m.m[3][1] + m.m[0][3] * m.m[1][1] * m.m[3][2] - m.m[0][3] * m.m[1][2] * m.m[3][1] - m.m[0][2] * m.m[1][1] * m.m[3][3] - m.m[0][1] * m.m[1][3] * m.m[3][2]) / determinant;
    num.m[0][3] = (-m.m[0][1] * m.m[1][2] * m.m[2][3] - m.m[0][2] * m.m[1][3] * m.m[2][1] - m.m[0][3] * m.m[1][1] * m.m[2][2] + m.m[0][3] * m.m[1][2] * m.m[2][1] + m.m[0][2] * m.m[1][1] * m.m[2][3] + m.m[0][1] * m.m[1][3] * m.m[2][2]) / determinant;

    num.m[1][0] = (-m.m[1][0] * m.m[2][2] * m.m[3][3] - m.m[1][2] * m.m[2][3] * m.m[3][0] - m.m[1][3] * m.m[2][0] * m.m[3][2] + m.m[1][3] * m.m[2][2] * m.m[3][0] + m.m[1][2] * m.m[2][0] * m.m[3][3] + m.m[1][0] * m.m[2][3] * m.m[3][2]) / determinant;
    num.m[1][1] = (m.m[0][0] * m.m[2][2] * m.m[3][3] + m.m[0][2] * m.m[2][3] * m.m[3][0] + m.m[0][3] * m.m[2][0] * m.m[3][2] - m.m[0][3] * m.m[2][2] * m.m[3][0] - m.m[0][2] * m.m[2][0] * m.m[3][3] - m.m[0][0] * m.m[2][3] * m.m[3][2]) / determinant;
    num.m[1][2] = (-m.m[0][0] * m.m[1][2] * m.m[3][3] - m.m[0][2] * m.m[1][3] * m.m[3][0] - m.m[0][3] * m.m[1][0] * m.m[3][2] + m.m[0][3] * m.m[1][2] * m.m[3][0] + m.m[0][2] * m.m[1][0] * m.m[3][3] + m.m[0][0] * m.m[1][3] * m.m[3][2]) / determinant;
    num.m[1][3] = (m.m[0][0] * m.m[1][2] * m.m[2][3] + m.m[0][2] * m.m[1][3] * m.m[2][0] + m.m[0][3] * m.m[1][0] * m.m[2][2] - m.m[0][3] * m.m[1][2] * m.m[2][0] - m.m[0][2] * m.m[1][0] * m.m[2][3] - m.m[0][0] * m.m[1][3] * m.m[2][2]) / determinant;

    num.m[2][0] = (m.m[1][0] * m.m[2][1] * m.m[3][3] + m.m[1][1] * m.m[2][3] * m.m[3][0] + m.m[1][3] * m.m[2][0] * m.m[3][1] - m.m[1][3] * m.m[2][1] * m.m[3][0] - m.m[1][1] * m.m[2][0] * m.m[3][3] - m.m[1][0] * m.m[2][3] * m.m[3][1]) / determinant;
    num.m[2][1] = (-m.m[0][0] * m.m[2][1] * m.m[3][3] - m.m[0][1] * m.m[2][3] * m.m[3][0] - m.m[0][3] * m.m[2][0] * m.m[3][1] + m.m[0][3] * m.m[2][1] * m.m[3][0] + m.m[0][1] * m.m[2][0] * m.m[3][3] + m.m[0][0] * m.m[2][3] * m.m[3][1]) / determinant;
    num.m[2][2] = (m.m[0][0] * m.m[1][1] * m.m[3][3] + m.m[0][1] * m.m[1][3] * m.m[3][0] + m.m[0][3] * m.m[1][0] * m.m[3][1] - m.m[0][3] * m.m[1][1] * m.m[3][0] - m.m[0][1] * m.m[1][0] * m.m[3][3] - m.m[0][0] * m.m[1][3] * m.m[3][1]) / determinant;
    num.m[2][3] = (-m.m[0][0] * m.m[1][1] * m.m[2][3] - m.m[0][1] * m.m[1][3] * m.m[2][0] - m.m[0][3] * m.m[1][0] * m.m[2][1] + m.m[0][3] * m.m[1][1] * m.m[2][0] + m.m[0][1] * m.m[1][0] * m.m[2][3] + m.m[0][0] * m.m[1][3] * m.m[2][1]) / determinant;

    num.m[3][0] = (-m.m[1][0] * m.m[2][1] * m.m[3][2] - m.m[1][1] * m.m[2][2] * m.m[3][0] - m.m[1][2] * m.m[2][0] * m.m[3][1] + m.m[1][2] * m.m[2][1] * m.m[3][0] + m.m[1][1] * m.m[2][0] * m.m[3][2] + m.m[1][0] * m.m[2][2] * m.m[3][1]) / determinant;
    num.m[3][1] = (m.m[0][0] * m.m[2][1] * m.m[3][2] + m.m[0][1] * m.m[2][2] * m.m[3][0] + m.m[0][2] * m.m[2][0] * m.m[3][1] - m.m[0][2] * m.m[2][1] * m.m[3][0] - m.m[0][1] * m.m[2][0] * m.m[3][2] - m.m[0][0] * m.m[2][2] * m.m[3][1]) / determinant;
    num.m[3][2] = (-m.m[0][0] * m.m[1][1] * m.m[3][2] - m.m[0][1] * m.m[1][2] * m.m[3][0] - m.m[0][2] * m.m[1][0] * m.m[3][1] + m.m[0][2] * m.m[1][1] * m.m[3][0] + m.m[0][1] * m.m[1][0] * m.m[3][2] + m.m[0][0] * m.m[1][2] * m.m[3][1]) / determinant;
    num.m[3][3] = (m.m[0][0] * m.m[1][1] * m.m[2][2] + m.m[0][1] * m.m[1][2] * m.m[2][0] + m.m[0][2] * m.m[1][0] * m.m[2][1] - m.m[0][2] * m.m[1][1] * m.m[2][0] - m.m[0][1] * m.m[1][0] * m.m[2][2] - m.m[0][0] * m.m[1][2] * m.m[2][1]) / determinant;

    return num;
}
Vector3 Normalize(const Vector3& v)
{
    float Normalize;
    Vector3 num;
    Normalize = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    num.x = v.x / Normalize;
    num.y = v.y / Normalize;
    num.z = v.z / Normalize;
    return num;
}
Matrix4x4 MakeAffineMatrix(const Vector3& scale, const Vector3& rotate, const Vector3& translate)
{
    Matrix4x4 rotateX = MakeRotateXMatrix(rotate.x);
    Matrix4x4 rotateY = MakeRotateYMatrix(rotate.y);
    Matrix4x4 rotateZ = MakeRotateZMatrix(rotate.z);
    Matrix4x4 rotateXYZ = Multiply(rotateX, Multiply(rotateY, rotateZ));

    Matrix4x4 num;
    num.m[0][0] = scale.x * rotateXYZ.m[0][0];
    num.m[0][1] = scale.x * rotateXYZ.m[0][1];
    num.m[0][2] = scale.x * rotateXYZ.m[0][2];
    num.m[0][3] = 0.0f * 0.0f * 0.0f * 0.0f;
    num.m[1][0] = scale.y * rotateXYZ.m[1][0];
    num.m[1][1] = scale.y * rotateXYZ.m[1][1];
    num.m[1][2] = scale.y * rotateXYZ.m[1][2];
    num.m[1][3] = 0.0f * 0.0f * 0.0f * 0.0f;
    num.m[2][0] = scale.z * rotateXYZ.m[2][0];
    num.m[2][1] = scale.z * rotateXYZ.m[2][1];
    num.m[2][2] = scale.z * rotateXYZ.m[2][2];
    num.m[2][3] = 0.0f * 0.0f * 0.0f * 0.0f;
    num.m[3][0] = translate.x;
    num.m[3][1] = translate.y;
    num.m[3][2] = translate.z;
    num.m[3][3] = 1.0f;
    return num;
}

Matrix4x4 MakePrespectiveFovMatrix(float fovY, float aspectRatio, float nearClip, float farClip)
{
    Matrix4x4 num;
    num = { (1 / aspectRatio) * (1 / tanf(fovY / 2)), 0, 0, 0, 0, (1 / tanf(fovY / 2)), 0, 0, 0, 0, farClip / (farClip - nearClip), 1, 0, 0, (-nearClip * farClip) / (farClip - nearClip) };
    return num;
}
Matrix4x4 MakeOrthographicMatrix(float left, float top, float right, float bottom, float nearClip, float farClip)
{
    Matrix4x4 num;
    num = { 2 / (right - left), 0, 0, 0, 0, 2 / (top - bottom), 0, 0, 0, 0, 1 / (farClip - nearClip), 0, (left + right) / (left - right),
        (top + bottom) / (bottom - top),
        nearClip / (nearClip - farClip), 1 };
    return num;
}

Vector3 operator*(const Vector3& m1, const float& m2) { return Multiply(m1, m2); }
Vector3& operator+=(Vector3& lhv, const Vector3& rhv)
{
    lhv.x += rhv.x;
    lhv.y += rhv.y;
    lhv.z += rhv.z;
    return lhv;
}

Microsoft::WRL::ComPtr<ID3D12Resource> CreateBufferResource(const Microsoft::WRL::ComPtr<ID3D12Device>& device, size_t sizwInBytes)
{
    // 頂点リソース用のヒープを設定
    D3D12_HEAP_PROPERTIES uploadHeapProperties {};
    uploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    // 頂点リソースの設定
    D3D12_RESOURCE_DESC vertexResourceDesc {};
    // バッファリソース
    vertexResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vertexResourceDesc.Width = sizwInBytes;
    // バッファの場合はこれらは1にする決まり
    vertexResourceDesc.Height = 1;
    vertexResourceDesc.DepthOrArraySize = 1;
    vertexResourceDesc.MipLevels = 1;
    vertexResourceDesc.MipLevels = 1;
    vertexResourceDesc.SampleDesc.Count = 1;
    // バッファの場合はこれにする決まり
    vertexResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // 実際に頂点リソースを作る
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexResource = nullptr;
    HRESULT hr = device->CreateCommittedResource(&uploadHeapProperties, D3D12_HEAP_FLAG_NONE, &vertexResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexResource));
    assert(SUCCEEDED(hr));
    return vertexResource;
};
Microsoft::WRL::ComPtr<ID3D12PipelineState> CreateGraphicsPipelineState(
    ID3D12Device* device,
    ID3D12RootSignature* rootSignature,
    const D3D12_INPUT_LAYOUT_DESC& inputLayoutDesc,
    const D3D12_RASTERIZER_DESC& rasterizerDesc,
    const D3D12_DEPTH_STENCIL_DESC& depthStencilDesc,
    const D3D12_SHADER_BYTECODE& vs,
    const D3D12_SHADER_BYTECODE& ps,
    const D3D12_BLEND_DESC& blendDesc)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc {};
    desc.pRootSignature = rootSignature;
    desc.InputLayout = inputLayoutDesc;
    desc.VS = vs;
    desc.PS = ps;
    desc.BlendState = blendDesc;
    desc.RasterizerState = rasterizerDesc;
    desc.DepthStencilState = depthStencilDesc;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
    desc.SampleDesc.Count = 1;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelineState));
    assert(SUCCEEDED(hr));
    return pipelineState;
}

// CrashHandler

static LONG WINAPI ExportDump(EXCEPTION_POINTERS* excption)
{
    SYSTEMTIME time;
    GetLocalTime(&time);
    wchar_t filePath[MAX_PATH] = { 0 };
    CreateDirectory(L"./Dumps", nullptr);
    StringCchPrintf(filePath, MAX_PATH, L"./Dumps/%04d-%02d%02d-%02d%02d.dmp", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute);
    HANDLE dumpFileHandle = CreateFile(filePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, 0, CREATE_ALWAYS, 0, 0);
    // processId とクラッシュの発生したthreadidを取得
    DWORD processId = GetCurrentProcessId();
    DWORD threadId = GetCurrentThreadId();
    // 設定情報を入力
    MINIDUMP_EXCEPTION_INFORMATION minidumpInformation { 0 };
    minidumpInformation.ThreadId = threadId;
    minidumpInformation.ExceptionPointers = excption;
    minidumpInformation.ClientPointers = TRUE;

    // Dumpを出力
    MiniDumpWriteDump(GetCurrentProcess(), processId, dumpFileHandle, MiniDumpNormal, &minidumpInformation, nullptr, nullptr);

    return EXCEPTION_EXECUTE_HANDLER;
}

// string->wstring
std::wstring ConvertString(const std::string& str)
{
    if (str.empty()) {
        return std::wstring();
    }

    auto sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(&str[0]), static_cast<int>(str.size()), NULL, 0);
    if (sizeNeeded == 0) {
        return std::wstring();
    }
    std::wstring result(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(&str[0]), static_cast<int>(str.size()), &result[0], sizeNeeded);
    return result;
}

// wstring->string
std::string ConvertString(const std::wstring& str)
{
    if (str.empty()) {
        return std::string();
    }

    auto sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), NULL, 0, NULL, NULL);
    if (sizeNeeded == 0) {
        return std::string();
    }
    std::string result(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), result.data(), sizeNeeded, NULL, NULL);
    return result;
}

void Log(const std::string& message)
{
    OutputDebugStringA(message.c_str());
}

void Log(std::ostream& os, const std::string& message)
{
    os << message << std::endl;
    OutputDebugStringA(message.c_str());
}

MaterialData LoadMaterialTemplateFile(const std::string& directoryPath, const std::string& filename)
{
    MaterialData materialData; // 構築するMaterialData
    std::string line; // ファイルから読み込んだ1行を格納するもの
    std::ifstream file(directoryPath + "/" + filename); // ファイルを開く
    assert(file.is_open()); // 開けられないなら止める

    while (std::getline(file, line)) {
        std::string identifier;
        std::istringstream s(line);
        s >> identifier;

        // identfierに応じた処理
        if (identifier == "map_Kd") {
            std::string textureFilename;
            s >> textureFilename;
            // 連結してファイルパスにする
            materialData.textureFilePath = directoryPath + "/" + textureFilename;
        }
    }
    return materialData;
}

DirectX::ScratchImage LoadTexture(const std::string& filePath)
{
    // テクスチャファイルを読み込んでプログラムで使えるようにする
    DirectX::ScratchImage image {};
    std::wstring filePathW = ConvertString(filePath);
    HRESULT hr = DirectX::LoadFromWICFile(filePathW.c_str(), DirectX::WIC_FLAGS_FORCE_SRGB, nullptr, image);
    assert(SUCCEEDED(hr));

    // ミップマップの作成
    DirectX::ScratchImage mipImages {};
    hr = DirectX::GenerateMipMaps(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DirectX::TEX_FILTER_SRGB, 0, mipImages);

    // ミップマップ月のデータを返す
    return mipImages;
}

ModelData LoadObjFile(const std::string& directoryPath, const std::string& filename)
{
    ModelData modelData; // 構築するmodeldata
    std::vector<Vector4> positions; // 位置
    std::vector<Vector3> normals; // 法線
    std::vector<Vector2> texcoords; // テクスチャ座標
    std::string line; // ファイルから読んだ1行を格納するもの

    // ファイルを開く
    std::ifstream file(directoryPath + "/" + filename);
    assert(file.is_open()); // 開けられないなら止める

    while (std::getline(file, line)) {
        std::string identifier;
        std::istringstream s(line);
        s >> identifier; // 先頭の識別子を読む

        // identifierに応じた処理
        if (identifier == "v") {
            Vector4 position;
            s >> position.x >> position.y >> position.z;
            position.w = 1.0f;
            positions.push_back(position);
        } else if (identifier == "vt") {
            Vector2 texcoord;
            s >> texcoord.x >> texcoord.y;
            texcoords.push_back(texcoord);
        } else if (identifier == "vn") {
            Vector3 normal;
            s >> normal.x >> normal.y >> normal.z;
            normals.push_back(normal);
        } else if (identifier == "f") {
            // 面は三角形限定,その他未対応

            VertexData triangle[3];

            for (int32_t faceVertex = 0; faceVertex < 3; ++faceVertex) {
                std::string vertexDefinition;
                s >> vertexDefinition;
                // 頂点の要素へのindexは[位置/uv/法線]で格納されているので.分割してindexを取得する
                std::istringstream v(vertexDefinition);
                uint32_t elementIndeices[3];
                for (int32_t element = 0; element < 3; ++element) {
                    std::string index;
                    std::getline(v, index, '/'); // 区切りインデクスを読んでいく
                    elementIndeices[element] = std::stoi(index);
                }
                // 要素へのindexから,実際の要素の値を取得して,頂点を構築する
                Vector4 position = positions[elementIndeices[0] - 1];
                Vector2 texcoord = texcoords[elementIndeices[1] - 1];
                Vector3 normal = normals[elementIndeices[2] - 1];

                // 位置の反転&法線の反転&左下原点
                texcoord.y = 1.0f - texcoord.y;

                triangle[faceVertex] = { position, texcoord, normal };
            }
            // 頂点を逆順で登録することで、周り順を逆にする
            modelData.vertices.push_back(triangle[2]);
            modelData.vertices.push_back(triangle[1]);
            modelData.vertices.push_back(triangle[0]);
        } else if (identifier == "mtllib") {
            // materialTemplateLibraryファイルの名前を取得する
            std::string materialFilename;
            s >> materialFilename;
            // 基本的のobjファイルと同一階級にmtlは存在させるので,ディレクトリ名とファイル名を返す
            modelData.material = LoadMaterialTemplateFile(directoryPath, materialFilename);
        }
    }

    return modelData;
}

SoundData SoundLoadWave(const char* filename)
{

    // ファイル入力ストリームのインスタンス
    std::ifstream file;
    // .wavファイルをバイナリモードで開く
    file.open(filename, std::ios_base::binary);
    // ファイルオープン失敗を検出する
    assert(file.is_open());

    // RIFFヘッダーの読み込み
    RiffHeader riff;
    file.read((char*)&riff, sizeof(riff));
    // ファイルがRIFFかチェック
    if (strncmp(riff.chunk.id, "RIFF", 4) != 0) {
        assert(0);
    }
    // タイプがWAVEかチェック
    if (strncmp(riff.type, "WAVE", 4) != 0) {
        assert(0);
    }

    // Formatチャンクの読み込み
    FormatChunk format = {};
    // チャンクヘッダーの確認
    file.read((char*)&format, sizeof(ChunkHeader));
    if (strncmp(format.chunk.id, "fmt ", 4) != 0) {
        assert(0);
    }
    // チャンク本体の読み込み
    assert(format.chunk.size <= sizeof(format.fmt));
    file.read((char*)&format.fmt, format.chunk.size);

    // Dataチャンクの読み込み
    ChunkHeader data;
    file.read((char*)&data, sizeof(data));
    // JUNKチャンクを検出した場合
    if (strncmp(data.id, "JUNK", 4) == 0) {
        // 読み込み位置をJUNKチャンクの終わりまで進める
        file.seekg(data.size, std::ios_base::cur);
        // 再度読み込み
        file.read((char*)&data, sizeof(data));
    }

    if (strncmp(data.id, "data", 4) != 0) {
        assert(0);
    }

    // Dataチャンクのデータ部の読み込み
    char* pBuffer = new char[data.size];
    file.read(pBuffer, data.size);

    // waveファイルを閉じる
    file.close();

    // Returnするための音声データ
    SoundData soundData = {};

    soundData.wfex = format.fmt;
    soundData.pBuffer = reinterpret_cast<BYTE*>(pBuffer);
    soundData.bufferSize = data.size;

    return soundData;
}

void SoundUhload(SoundData* soundData)
{
    // バッフアのメモリ解放
    delete[] soundData->pBuffer;

    soundData->pBuffer = 0;
    soundData->bufferSize = 0;
    soundData->wfex = {};
}

void SoundPlayWave(IXAudio2* xAudio2, const SoundData& soundData)
{
    HRESULT result;

    // 波形フォーマットを元にSoundVoiceの生成
    IXAudio2SourceVoice* pSourceVoice = nullptr;
    result = xAudio2->CreateSourceVoice(&pSourceVoice, &soundData.wfex);
    assert(SUCCEEDED(result));

    // 再生する波形データの設定
    XAUDIO2_BUFFER buf {};
    buf.pAudioData = soundData.pBuffer;
    buf.AudioBytes = soundData.bufferSize;
    buf.Flags = XAUDIO2_END_OF_STREAM;

    // 波形データの再生
    result = pSourceVoice->SubmitSourceBuffer(&buf);
    result = pSourceVoice->Start();
}

// ウィンドウプロ―ジャ
LRESULT CALLBACK Windowproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        return true;
    }

    // メッセージに応じてゲーム固有の処理を行う
    switch (msg) {
        // ウィンドウが破壊された
    case WM_DESTROY:
        // OSに対してアプリの終了を伝える
        PostQuitMessage(0);
        return 0;
    }

    // 標準のメッセージ処理を行う
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> createDescriptorHeap(const Microsoft::WRL::ComPtr<ID3D12Device>& device, D3D12_DESCRIPTOR_HEAP_TYPE heapType, UINT numDescriptors, bool shaderVisible)
{

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DescripotrHeap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC DescriptorHeapDesc {};
    DescriptorHeapDesc.Type = heapType; // レンダーターゲットビュー用
    DescriptorHeapDesc.NumDescriptors = numDescriptors; // ダブルバッファ用に2つ。多くてもかまわない
    DescriptorHeapDesc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HRESULT hr = device->CreateDescriptorHeap(&DescriptorHeapDesc, IID_PPV_ARGS(&DescripotrHeap));
    // ディスクリプタヒープが作れなかったので起動できない
    assert(SUCCEEDED(hr));
    return DescripotrHeap;
}

Microsoft::WRL::ComPtr<IDxcBlob> CompileShader(
    // CompilerするShaderファイルへのパス
    const std::wstring& filePath,
    // Compilerに使用するProfile,
    const wchar_t* profile,
    // 初期化で生成したものを3つ
    const Microsoft::WRL::ComPtr<IDxcUtils>& dxcUtils,
    const Microsoft::WRL::ComPtr<IDxcCompiler3>& dxcCompiler,
    const Microsoft::WRL::ComPtr<IDxcIncludeHandler>& includeHandler,
    std::ostream& os)
{
    // これからシェーダーにコンパイルするログを出す
    Log(os, ConvertString(std::format(L"Bggin CompileShader,path:{}, profile:{}\n", filePath, profile)));
    // hlslファイルを読む
    Microsoft::WRL::ComPtr<IDxcBlobEncoding> shaderSourec = nullptr;
    HRESULT hr = dxcUtils->LoadFile(filePath.c_str(), nullptr, &shaderSourec);
    // 読めなかったら止める
    assert(SUCCEEDED(hr));
    // 読み込んだファイルの内容を設定する
    DxcBuffer shaderSourceBuffer;
    shaderSourceBuffer.Ptr = shaderSourec->GetBufferPointer();
    shaderSourceBuffer.Size = shaderSourec->GetBufferSize();
    shaderSourceBuffer.Encoding = DXC_CP_UTF8;

    LPCWSTR arguments[] = {
        filePath.c_str(),
        L"-E",
        L"main",
        L"-T",
        profile,
        L"-Zi",
        L"-Qembed_debug",
        L"-Od",
        L"-Zpr",
    };

    // 実際にShaderをコンパイルする
    Microsoft::WRL::ComPtr<IDxcResult> shaderResult = nullptr;
    hr = dxcCompiler->Compile(
        &shaderSourceBuffer,
        arguments,
        _countof(arguments),
        includeHandler.Get(),
        IID_PPV_ARGS(&shaderResult));

    // コンパイルエラーではなくdxcが起動できないなど致命的な状況
    assert(SUCCEEDED(hr));

    // 警告・エラーが出たらログに出して止める
    Microsoft::WRL::ComPtr<IDxcBlobUtf8> shaderError = nullptr;
    shaderResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&shaderError), nullptr);
    if (shaderError != nullptr && shaderError->GetStringLength() != 0) {
        Log(os, shaderError->GetStringPointer());
        // 警告・エラー絶対ダメ
        assert(false);
    }

    // コンパイル結果から実用性のバイナリ部分を取得
    Microsoft::WRL::ComPtr<IDxcBlob> shaderBlob = nullptr;
    hr = shaderResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBlob), nullptr);
    assert(SUCCEEDED(hr));
    // 成功したログを出す
    Log(os, ConvertString(std::format(L"Compile Succeeded,path:{},profile:{}\n", filePath, profile)));

    // 実験用のバイナリを返却
    return shaderBlob;
};

Microsoft::WRL::ComPtr<ID3D12Resource> CreateTextureResource(const Microsoft::WRL::ComPtr<ID3D12Device>& device, const DirectX::TexMetadata& metadata)
{
    // metadataを基にResourecの設定
    D3D12_RESOURCE_DESC resourceDesc {};
    resourceDesc.Width = UINT(metadata.width); // 幅
    resourceDesc.Height = UINT(metadata.height); // 高さ
    resourceDesc.MipLevels = UINT16(metadata.mipLevels); // mipmapの数
    resourceDesc.DepthOrArraySize = UINT16(metadata.arraySize); // 奥行きor配列textureの配列数
    resourceDesc.Format = metadata.format; // textureのformat
    resourceDesc.SampleDesc.Count = 1; // サンプリングカウント
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION(metadata.dimension); // Textureの次元数。普段使っているのは2次元

    // 利用するheapの設定非常に特殊な運用。02_04exで一般的なケース版がある
    D3D12_HEAP_PROPERTIES heapProperties {};
    heapProperties.Type = D3D12_HEAP_TYPE_CUSTOM; // 細かい設定をする
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK; // writeBackポリシーでCPUアクセス可能
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0; // プロセッサの近くに配置

    // resourceの生成
    Microsoft::WRL::ComPtr<ID3D12Resource> resource = nullptr;
    HRESULT hr = device->CreateCommittedResource(
        &heapProperties, // Heapの設定
        D3D12_HEAP_FLAG_NONE, // Heapの特殊な設定
        &resourceDesc, // Resourceの設定
        D3D12_RESOURCE_STATE_COPY_DEST, // 初回のResourcestate
        nullptr, // clear最適値
        IID_PPV_ARGS(&resource)); // 作成するResourceポインタへのポインタ
    assert(SUCCEEDED(hr));
    return resource;
}

[[nodiscard]]
Microsoft::WRL::ComPtr<ID3D12Resource> UploadTextureData(const Microsoft::WRL::ComPtr<ID3D12Resource>& texture, const DirectX::ScratchImage& mipImages, const Microsoft::WRL::ComPtr<ID3D12Device>& device, const Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>& commandList)
{
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
    DirectX::PrepareUpload(device.Get(), mipImages.GetImages(), mipImages.GetImageCount(), mipImages.GetMetadata(), subresources);
    uint64_t intermediateSize = GetRequiredIntermediateSize(texture.Get(), 0, UINT(subresources.size()));
    Microsoft::WRL::ComPtr<ID3D12Resource> intermediateResourec = CreateBufferResource(device, intermediateSize);
    UpdateSubresources(commandList.Get(), texture.Get(), intermediateResourec.Get(), 0, 0, UINT(subresources.size()), subresources.data());
    // teture
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
    commandList->ResourceBarrier(1, &barrier);
    return intermediateResourec.Get();
}

Microsoft::WRL::ComPtr<ID3D12Resource> CreateDepthSetencilTextureResource(const Microsoft::WRL::ComPtr<ID3D12Device>& device, int32_t width, int32_t height)
{
    D3D12_RESOURCE_DESC resourceDesc {};
    resourceDesc.Width = width;
    resourceDesc.Height = height;
    resourceDesc.MipLevels = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // 利用するheaoの設定
    D3D12_HEAP_PROPERTIES heapProperties {};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    // 震度値のクリア設定
    D3D12_CLEAR_VALUE depthClearValue {};
    depthClearValue.DepthStencil.Depth = 1.0f;
    depthClearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;

    // Resourceの設定
    Microsoft::WRL::ComPtr<ID3D12Resource> resource = nullptr;
    HRESULT hr = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depthClearValue, IID_PPV_ARGS(&resource));
    assert(SUCCEEDED(hr));
    return resource;
}

D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandle(ID3D12DescriptorHeap* descriptorHeap, uint32_t descriptorSize, uint32_t index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handleCPU = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    handleCPU.ptr += (descriptorSize * index);
    return handleCPU;
}

D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandle(ID3D12DescriptorHeap* descriptorHeap, uint32_t descriptorSize, uint32_t index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handleGPU = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    handleGPU.ptr += (descriptorSize * index);
    return handleGPU;
}

// windowsアプリでのエントリーポイント(main関数)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    D3DResourceLeakChecker leakChecl;

    CoInitializeEx(0, COINIT_MULTITHREADED);
    // 誰も捕捉しなかった場合に、捕捉する関数を登録
    SetUnhandledExceptionFilter(ExportDump);

    // ログのディレクトリを用意
    std::filesystem::create_directory("logs");
    // 現在時刻を取得(UTC)
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    // ログファイルの名前にコンマ何秒はいらぬから削る
    std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>
        nowSeconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    // 日本時間(pcの設定)に変更
    std::chrono::zoned_time localTime { std::chrono::current_zone(), nowSeconds };
    // formatを使って年月日_時分秒に変換
    std::string dateString = std::format("{:%Y%m%d_%H%M%S}", localTime);
    // 時刻を使ってファイル名を決定
    std::string logFilePath = std::string("logs/") + dateString + ".log";
    // ファイルを作って書き込み準備
    std::ofstream logStream(logFilePath);

    WNDCLASS wc {};
    // ウィンドウプロ―ジャ
    wc.lpfnWndProc = Windowproc;
    // ウィンドウクラス名(なんでもよし)
    wc.lpszClassName = L"LE2C_05_イケダ_タクミ";
    // インスタンスハンドル
    wc.hInstance = GetModuleHandle(nullptr);
    // カーソル
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // ウィンドウクラスを登録する
    RegisterClass(&wc);

    // クライアント領域のサイズ
    const int32_t KClientWidth = 1280;
    const int32_t KClientHeight = 720;

    // ウィンドウサイズを表す構造体にクライアント領域を入れる
    RECT wrc = { 0, 0, KClientWidth, KClientHeight };

    // クライアント領域を元に実際のサイズをwrcを変更してもらう
    AdjustWindowRect(&wrc, WS_OVERLAPPEDWINDOW, false);

    // ウィンドウの作成
    HWND hwnd = CreateWindow(
        wc.lpszClassName, // 利用するクラス名
        L"LE2C_05_イケダ_タクミ", // タイトルバーの文字
        WS_OVERLAPPEDWINDOW, // よく見るウィンドウスタイル
        CW_USEDEFAULT, // 座標X表示
        CW_USEDEFAULT, // 座標Y表示
        wrc.right - wrc.left, // ウィンドウ横幅
        wrc.bottom - wrc.top, // ウィンドウ縦幅
        nullptr, // メニュースタイル
        nullptr, wc.hInstance, // インスタンスハンドル
        nullptr); // オプション

#ifdef _DEBUG
    Microsoft::WRL::ComPtr<ID3D12Debug1> debugController = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        // デバックレイヤーを有効化
        debugController->EnableDebugLayer();
        // さらにGPU側でもチェックを行う
        debugController->SetEnableGPUBasedValidation(TRUE);
    }
#endif // _DEBUG

    // ウィンドウを表示する
    ShowWindow(hwnd, SW_SHOW);
    // 出力ウィンドウへの文字出力
    Log(logStream, "Hello,DirectX!\n");
    Log(logStream, ConvertString(std::format(L"clientSize{},{}\n", KClientWidth, KClientHeight)));

    // DXGIファクトリーの生成
    Microsoft::WRL::ComPtr<IDXGIFactory7> dxgiFactory = nullptr;
    // 関数が成功したかマクロ判定
    HRESULT hr = CreateDXGIFactory(IID_PPV_ARGS(&dxgiFactory));
    // assertにしておく
    assert(SUCCEEDED(hr));

    // 使用するアダプタ用の変数
    Microsoft::WRL::ComPtr<IDXGIAdapter4> useAdapter = nullptr;
    // 良い順にアダプタを頼む
    for (UINT i = 0; dxgiFactory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&useAdapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        // アダプターの情報を取得する
        DXGI_ADAPTER_DESC3 adapterDesc {};
        hr = useAdapter->GetDesc3(&adapterDesc);
        assert(SUCCEEDED(hr)); // 取得できないのは一大事
        // ソフトウェアアダプタでなければ採用
        if (!(adapterDesc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE)) {
            // ログに出力
            Log(logStream, ConvertString(std::format(L"Use Adapater:{}\n", adapterDesc.Description)));
            break;
        }
        useAdapter = nullptr;
    }
    // 適切なアダプタが見つからなかったので起動できない
    assert(useAdapter != nullptr);

    Microsoft::WRL::ComPtr<ID3D12Device> device = nullptr;
    // 昨日レベルログ出力に文字列
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_12_2,
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0
    };
    const char* featureLevelStrings[] = { "12.2", "12.1", "12.0" };
    // 高い順に生成する
    for (size_t i = 0; i < _countof(featureLevels); ++i) {
        // 指定したあだぷたーでデバイスを作成
        hr = D3D12CreateDevice(useAdapter.Get(), featureLevels[i], IID_PPV_ARGS(&device));
        // 　指定したレベルで生成できたか確認
        if (SUCCEEDED(hr)) {
            Log(logStream, std::format("FeatureLevel :{}\n", featureLevelStrings[i]));
            break;
        }
    }

    // デバイスの生成がうまくいかなかったので起動できない
    assert(device != nullptr);
    Log(logStream, "Complete create D3D12Device!\n"); // 初期化完了のログを出す
#ifdef _DEBUG
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
        // やばいエラー時に止まる
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
        // エラー時に泊まる
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
        // 警報時に泊まる
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);

        // 警告時に止まる
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);

        // 制御するメッセージのID
        D3D12_MESSAGE_ID denyids[] = {
            // windows11でのDXGIデバックレイヤーとDX12デバックれいやーの相互作用バグによるエラーメッセージ
            D3D12_MESSAGE_ID_RESOURCE_BARRIER_MISMATCHING_COMMAND_LIST_TYPE
        };
        // 抑制するレベル
        D3D12_MESSAGE_SEVERITY severities[] = { D3D12_MESSAGE_SEVERITY_INFO };
        D3D12_INFO_QUEUE_FILTER filter {};
        filter.DenyList.NumIDs = _countof(denyids);
        filter.DenyList.pIDList = denyids;
        filter.DenyList.NumSeverities = _countof(severities);
        filter.DenyList.pSeverityList = severities;
        // 措定したメッセージの表示を抑制する
        infoQueue->PushStorageFilter(&filter);
    }

#endif // _DEBUG
       // コマンドキュー
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue = nullptr;
    D3D12_COMMAND_QUEUE_DESC commandQueueDesc {};
    hr = device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&commandQueue));
    // コマンドキューの生成がうまくいかなかったから起動できない
    assert(SUCCEEDED(hr));
    // コマンドアロケータを生成する
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
    // コマンドアロケータの生成がうまくいかなかったから起動できない
    assert(SUCCEEDED(hr));
    // コマンドリストを生成する
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList = nullptr;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList));
    // コマンドリストの生成がうまくいかなかったから起動できない
    assert(SUCCEEDED(hr));
    // スワップチェーンを生成する
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swapChain = nullptr;
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc {};
    swapChainDesc.Width = KClientWidth; // 画面の幅
    swapChainDesc.Height = KClientHeight; // 画面の高さ
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // 色の形式
    swapChainDesc.SampleDesc.Count = 1; // マルチサンプルしない
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; // 画面のターゲットとして利用する
    swapChainDesc.BufferCount = 2; // ダブルバッファ
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // モニターに移したら、中身を破棄
    // コマンドキュー、ウィンドウハンドル、設定をして渡す
    hr = dxgiFactory->CreateSwapChainForHwnd(commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, reinterpret_cast<IDXGISwapChain1**>(swapChain.GetAddressOf()));

    // デスクリプタヒープの生成
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvDescripotrHeap = createDescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, false);

    // SRV用のヒープでディスクリプタの数128。SRVはShadre内で触るものなので、ShaderVisiblrはtrue
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvDescriptorHeap = createDescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 128, true);

    // SwapChainからResourceを引っ張てくる
    Microsoft::WRL::ComPtr<ID3D12Resource> swapChainResources[2] = { nullptr };
    hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&swapChainResources[0]));
    // うまく起動できなければ起動できない
    assert(SUCCEEDED(hr));
    hr = swapChain->GetBuffer(1, IID_PPV_ARGS(&swapChainResources[1]));
    assert(SUCCEEDED(hr));

    // RTVの設定
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc {};
    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // 出力結果をSRGBに変換して書き込む
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; // 2dテクスチャとして書き込む
    // 　ディスクリプタの先頭を取得する
    D3D12_CPU_DESCRIPTOR_HANDLE rtvStartHandle = rtvDescripotrHeap->GetCPUDescriptorHandleForHeapStart();
    // RTVを二つ作るのでディスクリプタを2用意
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[2];
    // まず1つ目
    rtvHandles[0] = rtvStartHandle;
    device->CreateRenderTargetView(swapChainResources[0].Get(), &rtvDesc, rtvHandles[0]);
    // 2つ目
    rtvHandles[1].ptr = rtvHandles[0].ptr + device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    // 作成
    device->CreateRenderTargetView(swapChainResources[1].Get(), &rtvDesc, rtvHandles[1]);

    // 初期値0でfenecを作る
    Microsoft::WRL::ComPtr<ID3D12Fence> fence = nullptr;
    uint64_t fenceValue = 0;
    hr = device->CreateFence(fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    assert(SUCCEEDED(hr));

    // FenceのSignalを待つためのイベントを作る
    HANDLE fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    assert(fenceEvent != nullptr);

    // dxcCompilerを初期化
    Microsoft::WRL::ComPtr<IDxcUtils> dxcUtils = nullptr;
    Microsoft::WRL::ComPtr<IDxcCompiler3> dxcCompiler = nullptr;
    hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxcUtils));
    assert(SUCCEEDED(hr));
    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxcCompiler));
    assert(SUCCEEDED(hr));

    // 現時点ではincludeはしないが、nicludeに対するための設定を行っておく
    Microsoft::WRL::ComPtr<IDxcIncludeHandler> includeHandler = nullptr;
    hr = dxcUtils->CreateDefaultIncludeHandler(&includeHandler);
    assert(SUCCEEDED(hr));

    // RootSignature作成
    D3D12_ROOT_SIGNATURE_DESC descriptionRootSignature {};
    descriptionRootSignature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    D3D12_DESCRIPTOR_RANGE descriptorRange[1] = {};
    descriptorRange[0].BaseShaderRegister = 0;
    descriptorRange[0].NumDescriptors = 1;
    descriptorRange[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // RootParameter作成
    D3D12_ROOT_PARAMETER rootParameters[5] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[0].Descriptor.ShaderRegister = 0;

    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rootParameters[1].Descriptor.ShaderRegister = 0;

    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[2].DescriptorTable.pDescriptorRanges = descriptorRange;
    rootParameters[2].DescriptorTable.NumDescriptorRanges = _countof(descriptorRange);

    rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[3].Descriptor.ShaderRegister = 1;

    rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[4].Descriptor.ShaderRegister = 2;

    D3D12_DESCRIPTOR_RANGE descriptorRangeForInstancing[1] = {};
    descriptorRangeForInstancing[0].BaseShaderRegister = 0;
    descriptorRangeForInstancing[0].NumDescriptors = 1;
    descriptorRangeForInstancing[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRangeForInstancing[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_STATIC_SAMPLER_DESC staticSamplers[1] = {};
    staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    staticSamplers[0].ShaderRegister = 0;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    descriptionRootSignature.pStaticSamplers = staticSamplers;
    descriptionRootSignature.NumStaticSamplers = _countof(staticSamplers);

    descriptionRootSignature.pParameters = rootParameters;
    descriptionRootSignature.NumParameters = _countof(rootParameters);

    // シリアスライズしてバイナリにする
    Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob = nullptr;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob = nullptr;
    hr = D3D12SerializeRootSignature(&descriptionRootSignature, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
    if (FAILED(hr)) {
        Log(logStream, reinterpret_cast<char*>(errorBlob->GetBufferPointer()));
        assert(false);
    }

    // バイナリを元に生成
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature = nullptr;
    hr = device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
    assert(SUCCEEDED(hr));

    // InputLayout
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[3] = {};
    inputElementDescs[0].SemanticName = "POSITION";
    inputElementDescs[0].SemanticIndex = 0;
    inputElementDescs[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    inputElementDescs[0].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
    inputElementDescs[1].SemanticName = "TEXCOORD";
    inputElementDescs[1].SemanticIndex = 0;
    inputElementDescs[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    inputElementDescs[1].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

    inputElementDescs[2].SemanticName = "NORMAL";
    inputElementDescs[2].SemanticIndex = 0;
    inputElementDescs[2].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    inputElementDescs[2].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
    
    /*inputElementDescs[3].SemanticName = "POSITION";
    inputElementDescs[3].SemanticIndex = 0;
    inputElementDescs[3].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    inputElementDescs[3].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;*/

    D3D12_INPUT_LAYOUT_DESC inputLayoutDesc {};
    inputLayoutDesc.pInputElementDescs = inputElementDescs;
    inputLayoutDesc.NumElements = _countof(inputElementDescs);

    // BlendStateの設定
    D3D12_BLEND_DESC blendDesc {};
    // 全ての色要素を書き込む
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blendDesc.RenderTarget[0].BlendEnable = true;
    blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;

    // RasiterzerStateの設定
    D3D12_RASTERIZER_DESC rasterizerDesc {};
    // 裏面(時計回り)を表示しない
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    // 三角形の中を塗りつぶす
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;

    // Shaderをコンパイルする
    Microsoft::WRL::ComPtr<IDxcBlob> vertexShaderBlob = CompileShader(L"Object3d.VS.hlsl", L"vs_6_0", dxcUtils, dxcCompiler, includeHandler, logStream);
    assert(vertexShaderBlob != nullptr);

    Microsoft::WRL::ComPtr<IDxcBlob> pixeShaderBlob = CompileShader(L"Object3d.PS.hlsl", L"ps_6_0", dxcUtils, dxcCompiler, includeHandler, logStream);
    assert(pixeShaderBlob != nullptr);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsPipelineStateDesc {};
    graphicsPipelineStateDesc.pRootSignature = rootSignature.Get();
    graphicsPipelineStateDesc.InputLayout = inputLayoutDesc;
    graphicsPipelineStateDesc.VS = { vertexShaderBlob->GetBufferPointer(), vertexShaderBlob->GetBufferSize() };
    graphicsPipelineStateDesc.PS = { pixeShaderBlob->GetBufferPointer(), pixeShaderBlob->GetBufferSize() };
    graphicsPipelineStateDesc.BlendState = blendDesc;
    graphicsPipelineStateDesc.RasterizerState = rasterizerDesc;
    // 書き込むRTVの情報
    graphicsPipelineStateDesc.NumRenderTargets = 1;
    graphicsPipelineStateDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    // 利用するとぽろじのタイプ
    graphicsPipelineStateDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // どのように画面に色を打ち込むかの設定
    graphicsPipelineStateDesc.SampleDesc.Count = 1;
    graphicsPipelineStateDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

    // depthStencilStateの設定
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc {};
    // depthを有効化
    depthStencilDesc.DepthEnable = true;
    // 書き込み
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    // 比較関数はLessEqual
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    // DepthStencilの設定
    graphicsPipelineStateDesc.DepthStencilState = depthStencilDesc;
    graphicsPipelineStateDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    // 実際に生成
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipelineState = nullptr;
    hr = device->CreateGraphicsPipelineState(&graphicsPipelineStateDesc, IID_PPV_ARGS(&graphicsPipelineState));
    assert(SUCCEEDED(hr));

    // Particle用RootParameter作成 ===========================================================================================================================

    // Particle用RootSignature作成
    D3D12_ROOT_SIGNATURE_DESC ParticledescriptionRootSignature {};
    ParticledescriptionRootSignature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    D3D12_ROOT_PARAMETER ParticlerootParameters[4] = {};
    ParticlerootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    ParticlerootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    ParticlerootParameters[0].Descriptor.ShaderRegister = 0;

    ParticlerootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    ParticlerootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    ParticlerootParameters[1].DescriptorTable.pDescriptorRanges = descriptorRangeForInstancing;
    ParticlerootParameters[1].DescriptorTable.NumDescriptorRanges = _countof(descriptorRangeForInstancing);

    ParticlerootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    ParticlerootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    ParticlerootParameters[2].DescriptorTable.pDescriptorRanges = descriptorRange;
    ParticlerootParameters[2].DescriptorTable.NumDescriptorRanges = _countof(descriptorRange);

    ParticlerootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    ParticlerootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    ParticlerootParameters[3].Descriptor.ShaderRegister = 1;

    D3D12_STATIC_SAMPLER_DESC ParticlestaticSamplers[1] = {};
    ParticlestaticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ParticlestaticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    ParticlestaticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    ParticlestaticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    ParticlestaticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    ParticlestaticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    ParticlestaticSamplers[0].ShaderRegister = 0;
    ParticlestaticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    ParticledescriptionRootSignature.pStaticSamplers = ParticlestaticSamplers;
    ParticledescriptionRootSignature.NumStaticSamplers = _countof(ParticlestaticSamplers);

    ParticledescriptionRootSignature.pParameters = ParticlerootParameters;
    ParticledescriptionRootSignature.NumParameters = _countof(ParticlerootParameters);

    // シリアスライズしてバイナリにする
    Microsoft::WRL::ComPtr<ID3DBlob> ParticlesignatureBlob = nullptr;
    Microsoft::WRL::ComPtr<ID3DBlob> ParticleerrorBlob = nullptr;
    hr = D3D12SerializeRootSignature(&ParticledescriptionRootSignature, D3D_ROOT_SIGNATURE_VERSION_1, &ParticlesignatureBlob, &ParticleerrorBlob);
    if (FAILED(hr)) {
        Log(logStream, reinterpret_cast<char*>(ParticleerrorBlob->GetBufferPointer()));
        assert(false);
    }

    // バイナリを元に生成
    Microsoft::WRL::ComPtr<ID3D12RootSignature> ParticlerootSignature = nullptr;
    hr = device->CreateRootSignature(0, ParticlesignatureBlob->GetBufferPointer(), ParticlesignatureBlob->GetBufferSize(), IID_PPV_ARGS(&ParticlerootSignature));
    assert(SUCCEEDED(hr));

    // InputLayout
    D3D12_INPUT_ELEMENT_DESC ParticleinputElementDescs[3] = {};
    ParticleinputElementDescs[0].SemanticName = "POSITION";
    ParticleinputElementDescs[0].SemanticIndex = 0;
    ParticleinputElementDescs[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    ParticleinputElementDescs[0].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
    ParticleinputElementDescs[1].SemanticName = "TEXCOORD";
    ParticleinputElementDescs[1].SemanticIndex = 0;
    ParticleinputElementDescs[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    ParticleinputElementDescs[1].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

    ParticleinputElementDescs[2].SemanticName = "COLOR";
    ParticleinputElementDescs[2].SemanticIndex = 0;
    ParticleinputElementDescs[2].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    ParticleinputElementDescs[2].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

    D3D12_INPUT_LAYOUT_DESC ParticleinputLayoutDesc {};
    ParticleinputLayoutDesc.pInputElementDescs = ParticleinputElementDescs;
    ParticleinputLayoutDesc.NumElements = _countof(ParticleinputElementDescs);

    // BlendStateの設定
    D3D12_BLEND_DESC ParticleblendDesc {};
    // 全ての色要素を書き込む
    ParticleblendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    ParticleblendDesc.RenderTarget[0].BlendEnable = true;
    ParticleblendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    ParticleblendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    ParticleblendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    ParticleblendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    ParticleblendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    ParticleblendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;

    // RasiterzerStateの設定
    D3D12_RASTERIZER_DESC ParticlerasterizerDesc {};
    // 裏面(時計回り)を表示しない
    ParticlerasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    // 三角形の中を塗りつぶす
    ParticlerasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;

    // Shaderをコンパイルする
    Microsoft::WRL::ComPtr<IDxcBlob> ParticlevertexShaderBlob = CompileShader(L"Particle.VS.hlsl", L"vs_6_0", dxcUtils, dxcCompiler, includeHandler, logStream);
    assert(ParticlevertexShaderBlob != nullptr);

    Microsoft::WRL::ComPtr<IDxcBlob> ParticlepixeShaderBlob = CompileShader(L"Particle.PS.hlsl", L"ps_6_0", dxcUtils, dxcCompiler, includeHandler, logStream);
    assert(ParticlepixeShaderBlob != nullptr);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC ParticlegraphicsPipelineStateDesc {};
    ParticlegraphicsPipelineStateDesc.pRootSignature = ParticlerootSignature.Get();
    ParticlegraphicsPipelineStateDesc.InputLayout = ParticleinputLayoutDesc;
    ParticlegraphicsPipelineStateDesc.VS = { ParticlevertexShaderBlob->GetBufferPointer(), ParticlevertexShaderBlob->GetBufferSize() };
    ParticlegraphicsPipelineStateDesc.PS = { ParticlepixeShaderBlob->GetBufferPointer(), ParticlepixeShaderBlob->GetBufferSize() };
    ParticlegraphicsPipelineStateDesc.BlendState = ParticleblendDesc;
    ParticlegraphicsPipelineStateDesc.RasterizerState = ParticlerasterizerDesc;
    // 書き込むRTVの情報
    ParticlegraphicsPipelineStateDesc.NumRenderTargets = 1;
    ParticlegraphicsPipelineStateDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    // 利用するとぽろじのタイプ
    ParticlegraphicsPipelineStateDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // どのように画面に色を打ち込むかの設定
    ParticlegraphicsPipelineStateDesc.SampleDesc.Count = 1;
    ParticlegraphicsPipelineStateDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

    // depthStencilStateの設定
    D3D12_DEPTH_STENCIL_DESC ParticledepthStencilDesc {};
    // depthを有効化
    ParticledepthStencilDesc.DepthEnable = true;
    // 書き込み
    ParticledepthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    // 比較関数はLessEqual
    ParticledepthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    // DepthStencilの設定
    ParticlegraphicsPipelineStateDesc.DepthStencilState = ParticledepthStencilDesc;
    ParticlegraphicsPipelineStateDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    // 実際に生成
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ParticlegraphicsPipelineState = nullptr;
    hr = device->CreateGraphicsPipelineState(&ParticlegraphicsPipelineStateDesc, IID_PPV_ARGS(&ParticlegraphicsPipelineState));
    assert(SUCCEEDED(hr));

    // ================================================================================================================

    // 頂点場合はびゅーを作成する
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexResource = CreateBufferResource(device, sizeof(VertexData) * 6);

    D3D12_VERTEX_BUFFER_VIEW vertexBufferView {};
    // リソースの先頭のアドレス使う
    vertexBufferView.BufferLocation = vertexResource->GetGPUVirtualAddress();
    // 使用するリソースのサイズは頂点3つ分のサイズ
    vertexBufferView.SizeInBytes = sizeof(VertexData) * 6;
    // 1頂点当たりのサイズ
    vertexBufferView.StrideInBytes = sizeof(VertexData);

    // 頂点リソースにデータを書き込む
    VertexData* vertexData = nullptr;
    // 書き込むためのアドレス獲得
    vertexResource->Map(0, nullptr, reinterpret_cast<void**>(&vertexData));

    // 左下
    vertexData[0].position = { -0.5f, -0.5f, 0.0f, 1.0f };
    vertexData[0].texcoord = { 0.0f, 1.0f };

    // 上
    vertexData[1].position = { 0.0f, 0.5f, 0.0f, 1.0f };
    vertexData[1].texcoord = { 0.5f, 0.0f };

    // 右下
    vertexData[2].position = { 0.5f, -0.5f, 0.0f, 1.0f };
    vertexData[2].texcoord = { 1.0f, 1.0f };

    // 左下2
    vertexData[3].position = { -0.5f, -0.5f, 0.5f, 1.0f };
    vertexData[3].texcoord = { 0.0f, 1.0f };

    // 上2
    vertexData[4].position = { 0.0f, 0.0f, 0.0f, 1.0f };
    vertexData[4].texcoord = { 0.5f, 0.0f };

    // 右下2
    vertexData[5].position = { 0.5f, -0.5f, -0.5f, 1.0f };
    vertexData[5].texcoord = { 1.0f, 1.0f };

    // マテリアル用のリソースを作る
    Microsoft::WRL::ComPtr<ID3D12Resource> materialResource = CreateBufferResource(device, sizeof(Material));
    // マテリアルにデータを書き込む
    Material* materialData = nullptr;
    // 書き込むためのアドレスを取得
    materialResource->Map(0, nullptr, reinterpret_cast<void**>(&materialData));

    // 今回は赤を書き込んでみる
    materialData->color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    materialData->enableLighting = false;
    materialData->uvTransform = MakeIdentity4x4();

    // wvp用のリソースを作る
    Microsoft::WRL::ComPtr<ID3D12Resource> wvpResource = CreateBufferResource(device, sizeof(TransformationMatrix));
    // データを書き込む
    TransformationMatrix* transformationMatrixData = nullptr;
    // 書き込んだアドレスを取得
    wvpResource->Map(0, nullptr, reinterpret_cast<void**>(&transformationMatrixData));
    // 行列単位を書き込む
    transformationMatrixData->WVP = MakeIdentity4x4();
    transformationMatrixData->world = MakeIdentity4x4();

    // 平行光源
    Microsoft::WRL::ComPtr<ID3D12Resource> directionalLightMatrixResource = CreateBufferResource(device, sizeof(DirectionalLight));
    // データを書き込み
    DirectionalLight* directionalLightData = nullptr;
    // アドレスを取得
    directionalLightMatrixResource->Map(0, nullptr, reinterpret_cast<void**>(&directionalLightData));
    // 書き込み
    directionalLightData->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    directionalLightData->direction = { 0.0f, -1.0f, 0.0f };
    directionalLightData->intensity = 1.0f;

    // びゅーポート
    D3D12_VIEWPORT viewport {};
    // クライアント領域のサイズと一緒にして画面全体に表示
    viewport.Width = KClientWidth;
    viewport.Height = KClientHeight;
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    // シザー矩形
    D3D12_RECT scissorRect {};
    // 基本的にビューポートと同じ矩形が構成されるようにする
    scissorRect.left = 0;
    scissorRect.right = KClientWidth;
    scissorRect.top = 0;
    scissorRect.bottom = KClientHeight;

    // Transform変数を作る
    Transform transform { { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
    Transform cameratransform { { 1.0f, 1.0f, 1.0f }, { 0.3f, 3.14f, 0.0f }, { 0.0f, 4.0f, 10.0f } };

    float kWindowWidth = 1280.0f;
    float kWindowHeight = 720.0f;

    // ImGui初期化
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_Init(device.Get(),
        swapChainDesc.BufferCount,
        rtvDesc.Format,
        srvDescriptorHeap.Get(),
        srvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
        srvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

    // Textureを読み込み
    DirectX::ScratchImage mipImages = LoadTexture("resources/uvChecker.png");
    const DirectX::TexMetadata& metadata = mipImages.GetMetadata();
    Microsoft::WRL::ComPtr<ID3D12Resource> textureResource = CreateTextureResource(device, metadata);
    /*UploadTextureData(textureResource, mipImages);*/
    Microsoft::WRL::ComPtr<ID3D12Resource> intermediateResource = UploadTextureData(textureResource, mipImages, device, commandList.Get());

    // 2枚目Textureを読み込み
    DirectX::ScratchImage mipImages2 = LoadTexture("resources/monsterBall.png");
    const DirectX::TexMetadata& metadata2 = mipImages2.GetMetadata();
    Microsoft::WRL::ComPtr<ID3D12Resource> textureResource2 = CreateTextureResource(device, metadata2);
    /*UploadTextureData(textureResource2, mipImages2);*/
    Microsoft::WRL::ComPtr<ID3D12Resource> intermediateResource2 = UploadTextureData(textureResource2, mipImages2, device, commandList.Get());

    // metaDataを基にSRVの設定
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
    srvDesc.Format = metadata.format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = UINT(metadata.mipLevels);

    // 2枚目metaDataを基にSRVの設定
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc2 {};
    srvDesc2.Format = metadata2.format;
    srvDesc2.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc2.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc2.Texture2D.MipLevels = UINT(metadata2.mipLevels);

    // descriptorSize
    const uint32_t desriptorSizeSRV = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint32_t desriptorSizeRTV = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const uint32_t desriptorSizeDSV = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // SRVを作成するdescriptorHeapの場所を決める
    D3D12_CPU_DESCRIPTOR_HANDLE textureSrvHandleCPU = GetCPUDescriptorHandle(srvDescriptorHeap.Get(), desriptorSizeSRV, 1);
    D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandleGPU = GetGPUDescriptorHandle(srvDescriptorHeap.Get(), desriptorSizeSRV, 1);

    D3D12_CPU_DESCRIPTOR_HANDLE textureSrvHandleCPU2 = GetCPUDescriptorHandle(srvDescriptorHeap.Get(), desriptorSizeSRV, 2);
    D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandleGPU2 = GetGPUDescriptorHandle(srvDescriptorHeap.Get(), desriptorSizeSRV, 2);

    D3D12_CPU_DESCRIPTOR_HANDLE textureSrvHandleCPU3 = GetCPUDescriptorHandle(srvDescriptorHeap.Get(), desriptorSizeSRV, 3);
    D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandleGPU3 = GetGPUDescriptorHandle(srvDescriptorHeap.Get(), desriptorSizeSRV, 3);

    D3D12_CPU_DESCRIPTOR_HANDLE textureSrvHandleCPU4 = GetCPUDescriptorHandle(srvDescriptorHeap.Get(), desriptorSizeSRV, 4);
    D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandleGPU4 = GetGPUDescriptorHandle(srvDescriptorHeap.Get(), desriptorSizeSRV, 4);

    // SRVの生成
    device->CreateShaderResourceView(textureResource.Get(), &srvDesc, textureSrvHandleCPU);

    device->CreateShaderResourceView(textureResource2.Get(), &srvDesc2, textureSrvHandleCPU2);

    // heapの設定
    D3D12_HEAP_PROPERTIES heapProperties {};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;

    // depthStencilTextureをウィンドウのサイズで作成
    Microsoft::WRL::ComPtr<ID3D12Resource> depthStencilResource = CreateDepthSetencilTextureResource(device, KClientWidth, KClientHeight);

    // DSV用のひーぷでディスクリプタの数は1
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvDescriptorHeap = createDescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);

    // DSV
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    // DSVheapの先頭にDSVを作る
    device->CreateDepthStencilView(depthStencilResource.Get(), &dsvDesc, dsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    // Sprite用の頂点リソースを作る
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexResourceSprite = CreateBufferResource(device, sizeof(VertexData) * 4);

    // 頂点バッファビューを作る
    D3D12_VERTEX_BUFFER_VIEW vertexBufferViewSprite {};
    // リソースの先端のアドレスから使う
    vertexBufferViewSprite.BufferLocation = vertexResourceSprite->GetGPUVirtualAddress();
    // 使用するサイズ
    vertexBufferViewSprite.SizeInBytes = sizeof(VertexData) * 4;
    // 1ツ当たりのサイズ
    vertexBufferViewSprite.StrideInBytes = sizeof(VertexData);

    VertexData* vertexDataSprite = nullptr;
    vertexResourceSprite->Map(0, nullptr, reinterpret_cast<void**>(&vertexDataSprite));
    // 1枚目
    vertexDataSprite[0].position = { 0.0f, 360.0f, 0.0f, 1.0f };
    vertexDataSprite[0].texcoord = { 0.0f, 1.0f };
    vertexDataSprite[0].normal = { 0.0f, 0.0f, 1.0f };
    vertexDataSprite[1].position = { 0.0f, 0.0f, 0.0f, 1.0f };
    vertexDataSprite[1].texcoord = { 0.0f, 0.0f };
    vertexDataSprite[1].normal = { 0.0f, 0.0f, 1.0f };
    vertexDataSprite[2].position = { 640.0f, 360.0f, 0.0f, 1.0f };
    vertexDataSprite[2].texcoord = { 1.0f, 1.0f };
    vertexDataSprite[2].normal = { 0.0f, 0.0f, 1.0f };
    vertexDataSprite[3].position = { 640.0f, 0.0f, 0.0f, 1.0f };
    vertexDataSprite[3].texcoord = { 1.0f, 0.0f };
    vertexDataSprite[3].normal = { 0.0f, 0.0f, 1.0f };

    // インデックスリソースにデータを書き込む
    Microsoft::WRL::ComPtr<ID3D12Resource> indexResourceSprite = CreateBufferResource(device, sizeof(uint32_t) * 6);

    D3D12_INDEX_BUFFER_VIEW indexBufferViewSprite {};
    // リソースの先頭のアドレスから使う
    indexBufferViewSprite.BufferLocation = indexResourceSprite->GetGPUVirtualAddress();
    // 使用するリソースのサイズはインデックス6つ分のサイズ
    indexBufferViewSprite.SizeInBytes = sizeof(uint32_t) * 6;
    // インデックスはuint32_Tとする
    indexBufferViewSprite.Format = DXGI_FORMAT_R32_UINT;

    uint32_t* indexDataSprite = nullptr;
    indexResourceSprite->Map(0, nullptr, reinterpret_cast<void**>(&indexDataSprite));
    indexDataSprite[0] = 0;
    indexDataSprite[1] = 1;
    indexDataSprite[2] = 2;
    indexDataSprite[3] = 1;
    indexDataSprite[4] = 3;
    indexDataSprite[5] = 2;

    // Sprite用のマテリアルリソースを作る
    Microsoft::WRL::ComPtr<ID3D12Resource> materialResourceSprite = CreateBufferResource(device, sizeof(Material));

    Material* materialDataSprite = nullptr;
    // mapして書き込み
    materialResourceSprite->Map(0, nullptr, reinterpret_cast<void**>(&materialDataSprite));

    // 今回は白を書き込んでみる
    materialDataSprite->color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);

    materialDataSprite->enableLighting = false;
    materialDataSprite->uvTransform = MakeIdentity4x4();

    Transform uvTransformSprite {
        { 1.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f },
    };

    // Sprite用のtransformmatrix用のリソースを作る
    Microsoft::WRL::ComPtr<ID3D12Resource> transformationMatrixResourceSprite = CreateBufferResource(device, sizeof(TransformationMatrix));
    // データを書き込む
    TransformationMatrix* transformationMatrixDataSprite = nullptr;
    // 書き込むためのアドレス取得
    transformationMatrixResourceSprite->Map(0, nullptr, reinterpret_cast<void**>(&transformationMatrixDataSprite));
    // 単位行列を書き込む
    transformationMatrixDataSprite->WVP = MakeIdentity4x4();
    transformationMatrixDataSprite->world = MakeIdentity4x4();

    // 平行光源
    Microsoft::WRL::ComPtr<ID3D12Resource> directionalLightMatrixResourceSprite = CreateBufferResource(device, sizeof(DirectionalLight));
    // データを書き込み
    DirectionalLight* directionalLightDataSprite = nullptr;
    // アドレスを取得
    directionalLightMatrixResourceSprite->Map(0, nullptr, reinterpret_cast<void**>(&directionalLightDataSprite));
    // 書き込み
    directionalLightDataSprite->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    directionalLightDataSprite->direction = { 0.0f, -1.0f, 0.0f };
    directionalLightDataSprite->intensity = 1.0f;

    // 動かす用のtransform
    Transform transformSprite { { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };

    // =============================================================================================
    // 弾
    // =============================================================================================

    const uint32_t kSubdivision = 16;
    // 球の頂点数
    const uint32_t sphervertexNum = (kSubdivision + 1) * (kSubdivision + 1);
    const uint32_t spherindexNum = kSubdivision * kSubdivision * 6;

    // 頂点場合はびゅーを作成する
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexResourcesphere = CreateBufferResource(device, sizeof(VertexData) * sphervertexNum);

    D3D12_VERTEX_BUFFER_VIEW vertexBufferViewsphere {};
    // リソースの先頭のアドレス使う
    vertexBufferViewsphere.BufferLocation = vertexResourcesphere->GetGPUVirtualAddress();
    // 使用するリソースのサイズ
    vertexBufferViewsphere.SizeInBytes = sizeof(VertexData) * sphervertexNum;
    // 1頂点当たりのサイズ
    vertexBufferViewsphere.StrideInBytes = sizeof(VertexData);

    // 頂点リソースにデータを書き込む
    VertexData* vertexDatasphere = nullptr;
    // 書き込むためのアドレス獲得
    vertexResourcesphere->Map(0, nullptr, reinterpret_cast<void**>(&vertexDatasphere));

    // インデックスリソースにデータを書き込む
    Microsoft::WRL::ComPtr<ID3D12Resource> indexResourcesphere = CreateBufferResource(device, sizeof(uint32_t) * spherindexNum);

    D3D12_INDEX_BUFFER_VIEW indexBufferViewsphere {};
    // リソースの先頭のアドレスから使う
    indexBufferViewsphere.BufferLocation = indexResourcesphere->GetGPUVirtualAddress();
    // 使用するリソースのサイズはインデックス6つ分のサイズ
    indexBufferViewsphere.SizeInBytes = sizeof(uint32_t) * spherindexNum;
    // インデックスはuint32_Tとする
    indexBufferViewsphere.Format = DXGI_FORMAT_R32_UINT;

    uint32_t* indexDatasphere = nullptr;
    indexResourcesphere->Map(0, nullptr, reinterpret_cast<void**>(&indexDatasphere));

    const float kLonEvery = 2.0f * std::numbers::pi_v<float> / (float)kSubdivision;
    const float KLatEvery = std::numbers::pi_v<float> / (float)kSubdivision;

    for (uint32_t latIndex = 0; latIndex < (kSubdivision + 1); ++latIndex) {
        float lat = -std::numbers::pi_v<float> / 2.0f + KLatEvery * float(latIndex);
        for (uint32_t lonIndex = 0; lonIndex < (kSubdivision + 1); ++lonIndex) {
            float lon = lonIndex * float(kLonEvery);

            //  計算
            VertexData a = {
                { std::cosf(lat) * std::cosf(lon),
                    std::sinf(lat),
                    std::cosf(lat) * std::sinf(lon),
                    1.0f },
                { float(lonIndex) / float(kSubdivision),
                    1.0f - float(latIndex) / float(kSubdivision) },
                {
                    std::cosf(lat) * std::cosf(lon),
                    std::sinf(lat),
                    std::cosf(lat) * std::sinf(lon),
                }
            };

            uint32_t start = (latIndex * (kSubdivision + 1) + lonIndex);
            vertexDatasphere[start] = a;
        }
    }

    for (uint32_t lat = 0; lat < kSubdivision; ++lat) {
        for (uint32_t lon = 0; lon < kSubdivision; ++lon) {

            uint32_t lt = lon + lat * (kSubdivision + 1);
            uint32_t rt = (lon + 1) + lat * (kSubdivision + 1);
            uint32_t lb = lon + (lat + 1) * (kSubdivision + 1);
            uint32_t rb = (lon + 1) + (lat + 1) * (kSubdivision + 1);

            uint32_t start = (lat * kSubdivision + lon) * 6;

            indexDatasphere[start + 0] = lb;
            indexDatasphere[start + 1] = lt;
            indexDatasphere[start + 2] = rb;
            indexDatasphere[start + 3] = lt;
            indexDatasphere[start + 4] = rt;
            indexDatasphere[start + 5] = rb;
        }
    }

    // sphere用のtransformmatrix用のリソースを作る
    Microsoft::WRL::ComPtr<ID3D12Resource> transformationMatrixResourcesphere = CreateBufferResource(device, sizeof(TransformationMatrix));
    // データを書き込む
    TransformationMatrix* transformationMatrixDatasphere = nullptr;
    // 書き込むためのアドレス取得
    transformationMatrixResourcesphere->Map(0, nullptr, reinterpret_cast<void**>(&transformationMatrixDatasphere));
    // 単位行列を書き込む
    transformationMatrixDatasphere->WVP = MakeIdentity4x4();
    transformationMatrixDatasphere->world = MakeIdentity4x4();
    // 動かす用のtransform
    Transform transformsphere { { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };

    // sphere用のマテリアルリソースを作る
    Microsoft::WRL::ComPtr<ID3D12Resource> materialResourcesphere = CreateBufferResource(device, sizeof(Material));

    Material* materialDatasphere = nullptr;

    // mapして書き込み
    materialResourcesphere->Map(0, nullptr, reinterpret_cast<void**>(&materialDatasphere));
    // 今回は白を書き込んでみる
    materialDatasphere->color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    materialDatasphere->enableLighting = true;
    materialDatasphere->uvTransform = MakeIdentity4x4();
    materialDatasphere->shininess = 10.0f;

    // 平行光源
    Microsoft::WRL::ComPtr<ID3D12Resource> directionalLightMatrixResourcesphere = CreateBufferResource(device, sizeof(DirectionalLight));
    // データを書き込み
    DirectionalLight* directionalLightDatasphere = nullptr;
    // アドレスを取得
    directionalLightMatrixResourcesphere->Map(0, nullptr, reinterpret_cast<void**>(&directionalLightDatasphere));
    // 書き込み
    directionalLightDatasphere->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    directionalLightDatasphere->direction = { 0.0f, -1.0f, 0.0f };
    directionalLightDatasphere->intensity = 1.0f;

    // sphere用のマテリアルリソースを作る
    Microsoft::WRL::ComPtr<ID3D12Resource> CameraDataResourcesphere = CreateBufferResource(device, sizeof(CameraForGPU));
    CameraForGPU* CameraForGPUDatasphere = nullptr;
    // mapして書き込み
    CameraDataResourcesphere->Map(0, nullptr, reinterpret_cast<void**>(&CameraForGPUDatasphere));
    // 今回は白を書き込んでみる
    CameraForGPUDatasphere->worldPosition = cameratransform.translate;

    // 切り替えフラグ
    bool useMonsterBall = true;

    /// ==============================================================================================================
    /// モデルデータ
    /// ==============================================================================================================

    // モデル読み込み
    ModelData model = LoadObjFile("resources", "plane.obj");

    // 画像読み込み
    DirectX::ScratchImage mip2 = LoadTexture("resources/circle.png");
    const DirectX::TexMetadata& metadata3 = mip2.GetMetadata();
    Microsoft::WRL::ComPtr<ID3D12Resource> textureResource3 = CreateTextureResource(device, metadata3);
    Microsoft::WRL::ComPtr<ID3D12Resource> intermediateResource3 = UploadTextureData(textureResource3, mip2, device, commandList.Get());

    // 3枚目metaDataを基にSRVの設定
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc3 {};
    srvDesc3.Format = metadata3.format;
    srvDesc3.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc3.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc3.Texture2D.MipLevels = UINT(metadata3.mipLevels);

    device->CreateShaderResourceView(textureResource3.Get(), &srvDesc3, textureSrvHandleCPU3);

    // 頂点リソースを作成
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexResourceModel = CreateBufferResource(device, sizeof(VertexData) * model.vertices.size());

    // 頂点バッファビューを作成
    D3D12_VERTEX_BUFFER_VIEW vertexBufferViewModel {};
    vertexBufferViewModel.BufferLocation = vertexResourceModel->GetGPUVirtualAddress(); // リソースの先頭のアドレスから使用
    vertexBufferViewModel.SizeInBytes = UINT(sizeof(VertexData) * model.vertices.size()); // 使用するリソースのサイズ
    vertexBufferViewModel.StrideInBytes = sizeof(VertexData); // 1頂点当たりのサイズ

    // 頂点リソースに書き込み
    VertexData* vertexDataModel = nullptr;
    vertexResourceModel->Map(0, nullptr, reinterpret_cast<void**>(&vertexDataModel)); // 書き込むためのアドレス取得
    std::memcpy(vertexDataModel, model.vertices.data(), sizeof(VertexData) * model.vertices.size()); // 頂点データをリソースにコピー

    // インデックスリソースにデータを書き込む
    Microsoft::WRL::ComPtr<ID3D12Resource> indexResourceModel = CreateBufferResource(device, sizeof(uint32_t) * model.vertices.size());

    D3D12_INDEX_BUFFER_VIEW indexBufferViewModel {};
    // リソースの先頭のアドレスから使う
    indexBufferViewModel.BufferLocation = indexResourceModel->GetGPUVirtualAddress();
    // 使用するリソースのサイズはインデックス6つ分のサイズ
    indexBufferViewModel.SizeInBytes = UINT(sizeof(uint32_t) * model.vertices.size());
    // インデックスはuint32_Tとする
    indexBufferViewModel.Format = DXGI_FORMAT_R32_UINT;

    uint32_t* indexDataModel = nullptr;
    indexResourceModel->Map(0, nullptr, reinterpret_cast<void**>(&indexDataModel));

    // sphere用のマテリアルリソースを作る
    Microsoft::WRL::ComPtr<ID3D12Resource> materialResourceModel = CreateBufferResource(device, sizeof(Material));

    Material* materialDataModel = nullptr;

    // mapして書き込み
    materialResourceModel->Map(0, nullptr, reinterpret_cast<void**>(&materialDataModel));
    // 今回は白を書き込んでみる
    materialDataModel->color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);

    materialDataModel->enableLighting = true;

    materialDataModel->uvTransform = MakeIdentity4x4();

    // sphere用のtransformmatrix用のリソースを作る
    Microsoft::WRL::ComPtr<ID3D12Resource> transformationMatrixResourceModel = CreateBufferResource(device, sizeof(TransformationMatrix));
    // データを書き込む
    TransformationMatrix* transformationMatrixDataModel = nullptr;
    // 書き込むためのアドレス取得
    transformationMatrixResourceModel->Map(0, nullptr, reinterpret_cast<void**>(&transformationMatrixDataModel));
    // 単位行列を書き込む
    transformationMatrixDataModel->WVP = MakeIdentity4x4();
    transformationMatrixDataModel->world = MakeIdentity4x4();

    Transform transformModel { { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };

    // 平行光源
    Microsoft::WRL::ComPtr<ID3D12Resource> directionalLightMatrixResourceModel = CreateBufferResource(device, sizeof(DirectionalLight));
    // データを書き込み
    DirectionalLight* directionalLightDataModel = nullptr;
    // アドレスを取得
    directionalLightMatrixResourceModel->Map(0, nullptr, reinterpret_cast<void**>(&directionalLightDataModel));
    // 書き込み
    directionalLightDataModel->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    directionalLightDataModel->direction = { 0.0f, -1.0f, 0.0f };
    directionalLightDataModel->intensity = 1.0f;

    // ===================================================================
    // いたポリ
    // ===================================================================

    const uint32_t kNumMaxInstance = 100;
    // instancing用のtransformmatrixリソースを作る
    Microsoft::WRL::ComPtr<ID3D12Resource> instancingResource = CreateBufferResource(device, sizeof(ParticleForGPU) * kNumMaxInstance);
    // 　書き込む溜めのアドレス取得
    ParticleForGPU* instancingData = nullptr;
    instancingResource->Map(0, nullptr, reinterpret_cast<void**>(&instancingData));
    // 単位行列を書き込んでおく
    for (uint32_t index = 0; index < kNumMaxInstance; ++index) {
        instancingData[index].WVP = MakeIdentity4x4();
        instancingData[index].world = MakeIdentity4x4();
        instancingData[index].color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC instancingSrvDesc {};
    instancingSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    instancingSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    instancingSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    instancingSrvDesc.Buffer.FirstElement = 0;
    instancingSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    instancingSrvDesc.Buffer.NumElements = kNumMaxInstance;
    instancingSrvDesc.Buffer.StructureByteStride = sizeof(ParticleForGPU);

    D3D12_CPU_DESCRIPTOR_HANDLE instancingSrvHandleCPU6 = GetCPUDescriptorHandle(srvDescriptorHeap.Get(), desriptorSizeSRV, 5);
    D3D12_GPU_DESCRIPTOR_HANDLE instancingSrvHandleGPU6 = GetGPUDescriptorHandle(srvDescriptorHeap.Get(), desriptorSizeSRV, 5);
    device->CreateShaderResourceView(instancingResource.Get(), &instancingSrvDesc, instancingSrvHandleCPU6);

    std::random_device seedGenerator;
    std::mt19937 randomEngine(seedGenerator());
    std::uniform_real_distribution<float> distribution(-1.0f, 1.0f);

    Emitter emitter {};
    emitter.count = 3;
    emitter.frequency = 0.5f;
    emitter.ferquencyTime = 0.0f;
    emitter.transform.translate = { 0.0f, 0.0f, 0.0f };
    emitter.transform.rotate = { 0.0f, 0.0f, 0.0f };
    emitter.transform.scale = { 1.0f, 1.0f, 1.0f };

    std::list<Particle> Particles;
    Particles.push_back(MakeNewParticle(randomEngine, emitter.transform.translate));
    Particles.push_back(MakeNewParticle(randomEngine, emitter.transform.translate));
    Particles.push_back(MakeNewParticle(randomEngine, emitter.transform.translate));

    const float kDeltaTime = 1.0f / 60.0f;

    // 頂点リソースを作成
    Microsoft::WRL::ComPtr<ID3D12Resource> instancingvertexResource = CreateBufferResource(device, sizeof(VertexData) * model.vertices.size());

    // 頂点バッファビューを作成
    D3D12_VERTEX_BUFFER_VIEW instancingvertexBufferView {};
    instancingvertexBufferView.BufferLocation = instancingvertexResource->GetGPUVirtualAddress(); // リソースの先頭のアドレスから使用
    instancingvertexBufferView.SizeInBytes = UINT(sizeof(VertexData) * model.vertices.size()); // 使用するリソースのサイズ
    instancingvertexBufferView.StrideInBytes = sizeof(VertexData); // 1頂点当たりのサイズ

    VertexData* instancingVertexData = nullptr;
    instancingvertexResource->Map(0, nullptr, reinterpret_cast<void**>(&instancingVertexData));
    instancingVertexData[0] = { { 1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
    instancingVertexData[1] = { { -1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
    instancingVertexData[2] = { { 1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f } };
    instancingVertexData[3] = { { 1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f } };
    instancingVertexData[4] = { { -1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
    instancingVertexData[5] = { { -1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f, 1.0f } };

    /*for (uint32_t index = 0; index < kNumMaxInstance; ++index) {
        instancingData[index].color = Particles[index].color;
    }*/

    /// ============================================================================================================
    /// 音声データ
    /// ============================================================================================================

    Microsoft::WRL::ComPtr<IXAudio2>
        xAudio2;
    IXAudio2MasteringVoice* masterVoice;

    // xAudioエンジンインスタンスを生成
    HRESULT result = XAudio2Create(&xAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    assert(SUCCEEDED(result));

    result = xAudio2->CreateMasteringVoice(&masterVoice);
    assert(SUCCEEDED(result));

    // 音声読み込み
    SoundData soundData1 = SoundLoadWave("resources/fanfare.wav");

    // 1度だけ→初期化の後
    SoundPlayWave(xAudio2.Get(), soundData1);

    ///
    /// その他
    ///

    const char* blendModeNames[] = { "None", "Normal", "Add", "Subtract", "Multiply", "Screen" };
    static BlendMode blendMode = kBlendModeNone;
    static BlendMode prevMode = blendMode;
    bool useBillboard = false;

    MSG msg {};
    // ウィンドウの×ボタンが押されるまでループ
    while (msg.message != WM_QUIT) {

        // windowにメッセージが来ていたら最優先で処理させる
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            ImGui::Begin("Settings");

            if (ImGui::Button("add particle")) {
                Particles.splice(Particles.end(), Emit(emitter, randomEngine));
            }

            ImGui::DragFloat3("EmitterTranslate", &emitter.transform.translate.x, 0.01f, -100.0f, 100.0f);

            prevMode = blendMode;
            ImGui::Combo("Mode", (int*)&blendMode, blendModeNames, IM_ARRAYSIZE(blendModeNames));
            ImGui::Checkbox("useBillboard", &useBillboard);
            if (ImGui::Button("add particle")) {
                Particles.push_back(MakeNewParticle(randomEngine, emitter.transform.translate));
                Particles.push_back(MakeNewParticle(randomEngine, emitter.transform.translate));
                Particles.push_back(MakeNewParticle(randomEngine, emitter.transform.translate));
            }

            if (blendMode != prevMode) {
                D3D12_BLEND_DESC blendDesc = CreateBlendDesc(blendMode);

                graphicsPipelineState = CreateGraphicsPipelineState(
                    device.Get(),
                    rootSignature.Get(),
                    inputLayoutDesc,
                    rasterizerDesc,
                    depthStencilDesc,
                    { vertexShaderBlob->GetBufferPointer(), vertexShaderBlob->GetBufferSize() },
                    { pixeShaderBlob->GetBufferPointer(), pixeShaderBlob->GetBufferSize() },
                    blendDesc);

                prevMode = blendMode;
            }

            if (ImGui::CollapsingHeader("Model##Model")) {
                ImGui::DragFloat3("Translate##Model", &transformModel.translate.x, 0.01f);
                ImGui::SliderAngle("RotateX##Model", &transformModel.rotate.x);
                ImGui::SliderAngle("RotateY##Model", &transformModel.rotate.y);
                ImGui::SliderAngle("RotateZ##Model", &transformModel.rotate.z);
                ImGui::ColorEdit4("Color##Model", &(materialDataModel->color).x);
                ImGui::SliderFloat3("direction##ModelLight", &directionalLightDataModel->direction.x, -1.0f, 1.0f);
                ImGui::DragFloat("intensity##ModelLight", &directionalLightDataModel->intensity, 0.01f);
                ImGui::ColorEdit4("Color##ModelLight", &(directionalLightDataModel->color).x);
            }

            ImGui::DragFloat3("cameratransform##", &cameratransform.translate.x, 0.01f);
            ImGui::DragFloat3("camerarotate##", &cameratransform.rotate.x, 0.01f);
            ImGui::End();

            ImGui::Begin("sphere");
            ImGui::DragFloat3("Translate##Sphere", &transformsphere.translate.x, 0.01f);
            ImGui::DragFloat3("Rotate##Sphere", &transformsphere.rotate.x, 0.01f);
            ImGui::DragFloat3("Scale##Sphere", &transformsphere.scale.x, 0.01f);
            ImGui::ColorEdit4("Color##sphere", &(materialDatasphere->color).x);
            ImGui::Checkbox("useMonsterBall", &useMonsterBall);
            ImGui::SliderFloat3("direction##SphereLight", &directionalLightDatasphere->direction.x, -1.0f, 1.0f);
            ImGui::DragFloat("intensity##SphereLight", &directionalLightDatasphere->intensity, 0.01f);
            ImGui::SliderFloat4("Color##SphereLight", &directionalLightDatasphere->color.x, -20.0f, 20.0f);
            ImGui::ColorEdit4("Color##SphereLight", &(directionalLightDatasphere->color).x);
            ImGui::End();

            // update/更新処理

            // imguiのUI
            /* ImGui::ShowDemoWindow();*/

            Matrix4x4 cameraMatrix = MakeAffineMatrix(cameratransform.scale, cameratransform.rotate, cameratransform.translate);
            Matrix4x4 viewMatrix = Inverse(cameraMatrix);

            // 球体
            Matrix4x4 worldMatrixsphere = MakeAffineMatrix(transformsphere.scale, transformsphere.rotate, transformsphere.translate);
            Matrix4x4 projectionMatrixsphere = MakePrespectiveFovMatrix(0.45f, float(kWindowWidth) / float(kWindowHeight), 0.1f, 100.0f);
            Matrix4x4 worldViewProjectionMatrixsphere = Multiply(worldMatrixsphere, Multiply(viewMatrix, projectionMatrixsphere));
            transformationMatrixDatasphere->WVP = worldViewProjectionMatrixsphere;
            transformationMatrixDatasphere->world = worldMatrixsphere;

            directionalLightDatasphere->direction = Normalize(directionalLightDatasphere->direction);
            CameraForGPUDatasphere->worldPosition = cameratransform.translate;

            // モデルデータ

            Matrix4x4 worldMatrixModel = MakeAffineMatrix(transformModel.scale, transformModel.rotate, transformModel.translate);
            Matrix4x4 projectionMatrixModel = MakePrespectiveFovMatrix(0.45f, float(kWindowWidth) / float(kWindowHeight), 0.1f, 100.0f);
            Matrix4x4 worldViewProjectionMatrixModel = Multiply(worldMatrixModel, Multiply(viewMatrix, projectionMatrixModel));
            transformationMatrixDataModel->WVP = worldViewProjectionMatrixModel;
            transformationMatrixDataModel->world = worldMatrixModel;

            directionalLightDataModel->direction = Normalize(directionalLightDataModel->direction);

            Matrix4x4 backToFrontMatrix = MakeRotateYMatrix(std::numbers::pi_v<float>);
            Matrix4x4 billboardMatrix = Multiply(backToFrontMatrix, cameraMatrix);
            billboardMatrix.m[3][0] = 0.0f;
            billboardMatrix.m[3][1] = 0.0f;
            billboardMatrix.m[3][2] = 0.0f;

            // 板ポリ
            uint32_t numInstance = 0;
            for (std::list<Particle>::iterator particleIterator = Particles.begin(); particleIterator != Particles.end();) {
                if ((*particleIterator).lifeTime <= (*particleIterator).currentTime) {
                    particleIterator = Particles.erase(particleIterator);
                    continue;
                }

                if (numInstance < kNumMaxInstance) {
                    Matrix4x4 worldMatrixpori = MakeAffineMatrix((*particleIterator).transform.scale, (*particleIterator).transform.rotate, (*particleIterator).transform.translate);
                    if (useBillboard) {
                        worldMatrixpori = Multiply(worldMatrixpori, billboardMatrix);
                    }
                    Matrix4x4 projectionMatrixpori = MakePrespectiveFovMatrix(0.45f, float(kWindowWidth) / float(kWindowHeight), 0.1f, 100.0f);
                    Matrix4x4 worldViewProjectionMatrixpori = Multiply(worldMatrixpori, Multiply(viewMatrix, projectionMatrixpori));

                    (*particleIterator).transform.translate += (*particleIterator).velocity * kDeltaTime;
                    (*particleIterator).currentTime += kDeltaTime;
                    float alpha = 1.0f - ((*particleIterator).currentTime / (*particleIterator).lifeTime);
                    instancingData[numInstance].WVP = worldViewProjectionMatrixpori;
                    instancingData[numInstance].world = worldMatrixpori;
                    instancingData[numInstance].color = (*particleIterator).color;
                    instancingData[numInstance].color.w = alpha;
                    ++numInstance;
                }

                ++particleIterator;
            }

            emitter.ferquencyTime += kDeltaTime;
            if (emitter.frequency <= emitter.ferquencyTime) {
                Particles.splice(Particles.end(), Emit(emitter, randomEngine));
                emitter.ferquencyTime -= emitter.frequency;
            }

            // draw
            ImGui::Render();
            // バックバッファのインデックス取得
            UINT backBufferIndex = swapChain->GetCurrentBackBufferIndex();

            // TransitionBarrierの設定
            D3D12_RESOURCE_BARRIER barrier {};
            // 今回のバリアはTransutuion
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            // noneにしておく
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            // バリアを貼る対象のリソース。現在のバックバッファに対して行う
            barrier.Transition.pResource = swapChainResources[backBufferIndex].Get();
            // 繊維前のResourceState
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            // 遷移後のResourceState
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            // TransitionBarrierを張る
            commandList->ResourceBarrier(1, &barrier);

            // 描画先のRTVを設定する
            D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
            commandList->OMSetRenderTargets(1, &rtvHandles[backBufferIndex], false, &dsvHandle);
            // 指定した色で画面全体をクリアにする
            float clearColor[] = { 0.1f, 0.25f, 0.5f, 1.0f }; // 青っぽい色。RGBAの順番
            commandList->ClearRenderTargetView(rtvHandles[backBufferIndex], clearColor, 0, nullptr);

            // 描画用のDescriptorHeapの設定
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHraps[] = { srvDescriptorHeap.Get() };
            commandList->SetDescriptorHeaps(1, descriptorHraps->GetAddressOf());

            commandList->RSSetViewports(1, &viewport);
            commandList->RSSetScissorRects(1, &scissorRect);
            // RootSignatureを設定。PSOに設定しているけど別途設定が必要
            commandList->SetGraphicsRootSignature(rootSignature.Get());
            commandList->SetPipelineState(graphicsPipelineState.Get());

            // 形状を設定
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            //
            // スフィア/球
            //
            // SRV
            commandList->SetGraphicsRootDescriptorTable(2, useMonsterBall ? textureSrvHandleGPU2 : textureSrvHandleGPU);
            commandList->IASetVertexBuffers(0, 1, &vertexBufferViewsphere);
            // transformationMatrixCBufferの場所を設置
            commandList->SetGraphicsRootConstantBufferView(0, materialResourcesphere->GetGPUVirtualAddress());
            commandList->SetGraphicsRootConstantBufferView(1, transformationMatrixResourcesphere->GetGPUVirtualAddress());
            commandList->SetGraphicsRootConstantBufferView(3, directionalLightMatrixResourcesphere->GetGPUVirtualAddress());
            commandList->SetGraphicsRootConstantBufferView(4, CameraDataResourcesphere->GetGPUVirtualAddress());
            commandList->IASetIndexBuffer(&indexBufferViewsphere);

            commandList->DrawIndexedInstanced(spherindexNum, 1, 0, 0, 0);

            //
            // モデルデータ
            //

            commandList->SetGraphicsRootDescriptorTable(2, textureSrvHandleGPU3);
            commandList->IASetVertexBuffers(0, 1, &vertexBufferViewModel);
            commandList->SetGraphicsRootConstantBufferView(0, materialResourceModel->GetGPUVirtualAddress());
            commandList->SetGraphicsRootConstantBufferView(1, transformationMatrixResourceModel->GetGPUVirtualAddress());
            commandList->SetGraphicsRootConstantBufferView(3, directionalLightMatrixResourceModel->GetGPUVirtualAddress());

            commandList->IASetIndexBuffer(&indexBufferViewModel);

            /*commandList->DrawInstanced(UINT(model.vertices.size()), 1, 0, 0);*/

            // 板ポリ
            commandList->SetGraphicsRootSignature(ParticlerootSignature.Get());
            commandList->SetPipelineState(ParticlegraphicsPipelineState.Get());
            commandList->IASetVertexBuffers(0, 1, &instancingvertexBufferView);

            // 描画
            commandList->SetGraphicsRootConstantBufferView(0, materialResource->GetGPUVirtualAddress());
            commandList->SetGraphicsRootDescriptorTable(1, instancingSrvHandleGPU6);
            commandList->SetGraphicsRootDescriptorTable(2, textureSrvHandleGPU3);
            /*if (numInstance > 0) {
                commandList->DrawInstanced(UINT(model.vertices.size()), numInstance, 0, 0);
            }*/

            // 実際のcommandListのImGuiの描画コマンドを詰む
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList.Get());

            // 画面に各処理は全て終わり、画面に移すので、状態を遷移
            // 今回hRenderTargetからPresentにする
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            // TransitionBarrierを張る
            commandList->ResourceBarrier(1, &barrier);

            // 指定した震度で画面全体をクリアする
            commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            // コマンドリストの内容を確定させる
            hr = commandList->Close();
            assert(SUCCEEDED(hr));

            // GPUにコマンドリストの実行を行わせる
            Microsoft::WRL::ComPtr<ID3D12CommandList> commandLists[] = { commandList };
            commandQueue->ExecuteCommandLists(1, commandLists->GetAddressOf());
            // GPUとOSに画面交換を行うように通知する
            swapChain->Present(1, 0);

            // Fenceの値を更新
            fenceValue++;
            // GPUがここまでたどり着いたときに、Fenceの値を指定した値に代入するようにSignalを送る
            commandQueue->Signal(fence.Get(), fenceValue);

            // Fenceの値が指定したSignal値にたどり着いているか確認する
            // GetcompletedValueの初期値はFence作成時に渡した初期値
            if (fence->GetCompletedValue() < fenceValue) {
                // 指定したSignalにたどり着いてないので、たどり着くまで待つようにイベントを設定する
                fence->SetEventOnCompletion(fenceValue, fenceEvent);
                // イベントを待つ
                WaitForSingleObject(fenceEvent, INFINITE);
            }

            // 次のフレーム用のコマンドリストを準備
            hr = commandAllocator->Reset();
            assert(SUCCEEDED(hr));
            hr = commandList->Reset(commandAllocator.Get(), nullptr);
            assert(SUCCEEDED(hr));

            // ゲームの処理
        }
    }

    CoUninitialize();

    // ImGuiの終了
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CloseHandle(fenceEvent);

    // XAuido2解放
    xAudio2.Reset();
    // 音声データ解放
    SoundUhload(&soundData1);

    CloseWindow(hwnd);

    return 0;
}
