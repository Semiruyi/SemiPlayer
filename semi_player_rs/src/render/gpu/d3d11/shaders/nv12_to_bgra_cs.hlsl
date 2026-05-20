// NV12 → BGRA Compute Shader
// Reads NV12 (Y plane R8 + UV plane R8G8) and outputs BGRA (R8G8B8A8).
// Constant buffer layout must match Rust-side YuvToRgbMatrix exactly.

cbuffer ColorSpace : register(b0)
{
    row_major float3x3 yuv_to_rgb;
    float3 range_offset;
    float3 range_scale;
    uint2 output_size;
};

Texture2D<float>  y_plane  : register(t0);
Texture2D<float2> uv_plane : register(t1);
RWTexture2D<float4> output : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= output_size.x || tid.y >= output_size.y)
        return;

    float y  = y_plane[tid.xy];
    float2 uv = uv_plane[uint2(tid.x >> 1, tid.y >> 1)];

    float3 yuv = float3(y, uv.r, uv.g);
    yuv = (yuv + range_offset) * range_scale;
    float3 rgb = mul(yuv_to_rgb, yuv);

    output[tid.xy] = float4(rgb.b, rgb.g, rgb.r, 1.0);
}
