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
