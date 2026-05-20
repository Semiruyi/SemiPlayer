use crate::render::core::frame::{VideoColorInfo, VideoColorRange, VideoMatrixCoefficients};

// --- YUV → RGB color space conversion ---

const BT_709_MATRIX: [[f32; 3]; 3] = [
    [1.0, 0.0, 1.5748],
    [1.0, -0.1873, -0.4681],
    [1.0, 1.8556, 0.0],
];

const BT_601_MATRIX: [[f32; 3]; 3] = [
    [1.0, 0.0, 1.402],
    [1.0, -0.3441, -0.7141],
    [1.0, 1.772, 0.0],
];

const BT_2020_MATRIX: [[f32; 3]; 3] = [
    [1.0, 0.0, 1.4746],
    [1.0, -0.1646, -0.5714],
    [1.0, 1.8814, 0.0],
];

const LIMITED_RANGE_OFFSET: [f32; 3] = [-16.0 / 255.0, -128.0 / 255.0, -128.0 / 255.0];
const LIMITED_RANGE_SCALE: [f32; 3] = [255.0 / 219.0, 255.0 / 224.0, 255.0 / 224.0];
const FULL_RANGE_OFFSET: [f32; 3] = [0.0, -128.0 / 255.0, -128.0 / 255.0];
const FULL_RANGE_SCALE: [f32; 3] = [1.0, 255.0 / 254.0, 255.0 / 254.0];

/// YUV→RGB conversion parameters, layout matches HLSL cbuffer packing rules.
/// HLSL cbuffer: each float3 occupies a 16-byte row, float3x3 is 3 float4 columns.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct YuvToRgbMatrix {
    // HLSL: float3x3 yuv_to_rgb — 3 columns, each float4 (row-major = 3 rows of float4)
    pub matrix_row0: [f32; 4],
    pub matrix_row1: [f32; 4],
    pub matrix_row2: [f32; 4],
    // HLSL: float3 range_offset — 16-byte row
    pub range_offset: [f32; 3],
    _pad0: f32,
    // HLSL: float3 range_scale — 16-byte row
    pub range_scale: [f32; 3],
    _pad1: f32,
    // HLSL: uint2 output_size — 8 bytes
    pub output_width: u32,
    pub output_height: u32,
}

impl YuvToRgbMatrix {
    pub fn from_color_info(info: VideoColorInfo, width: u32, height: u32) -> Self {
        let m = match info.matrix {
            VideoMatrixCoefficients::Bt709 => BT_709_MATRIX,
            VideoMatrixCoefficients::Bt601 => BT_601_MATRIX,
            VideoMatrixCoefficients::Bt2020Ncl => BT_2020_MATRIX,
            _ => BT_709_MATRIX,
        };
        let (range_offset, range_scale) = match info.range {
            VideoColorRange::Full => (FULL_RANGE_OFFSET, FULL_RANGE_SCALE),
            _ => (LIMITED_RANGE_OFFSET, LIMITED_RANGE_SCALE),
        };
        Self {
            matrix_row0: [m[0][0], m[0][1], m[0][2], 0.0],
            matrix_row1: [m[1][0], m[1][1], m[1][2], 0.0],
            matrix_row2: [m[2][0], m[2][1], m[2][2], 0.0],
            range_offset,
            _pad0: 0.0,
            range_scale,
            _pad1: 0.0,
            output_width: width,
            output_height: height,
        }
    }
}

// --- Pixel format conversion algorithms ---

pub fn convert_rgba8_to_bgra8(data: &[u8]) -> Vec<u8> {
    let mut output = data.to_vec();
    for pixel in output.chunks_exact_mut(4) {
        pixel.swap(0, 2);
    }
    output
}

pub fn convert_gray8_to_bgra8(data: &[u8], width: usize, height: usize, stride: usize) -> Vec<u8> {
    let mut output = vec![0u8; width.saturating_mul(height).saturating_mul(4)];

    for y in 0..height {
        let src_row_start = y.saturating_mul(stride);
        let src_row_end = src_row_start.saturating_add(width);
        let dst_row_start = y.saturating_mul(width).saturating_mul(4);
        let src_row = &data[src_row_start..src_row_end];

        for (x, gray) in src_row.iter().copied().enumerate() {
            let dst_index = dst_row_start + x * 4;
            output[dst_index] = gray;
            output[dst_index + 1] = gray;
            output[dst_index + 2] = gray;
            output[dst_index + 3] = 255;
        }
    }

    output
}
