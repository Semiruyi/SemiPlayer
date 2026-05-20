fn find_fxc() -> Option<std::path::PathBuf> {
    // Try PATH first
    if let Ok(output) = std::process::Command::new("where").arg("fxc.exe").output() {
        if output.status.success() {
            let path = String::from_utf8_lossy(&output.stdout);
            if let Some(line) = path.lines().next() {
                return Some(std::path::PathBuf::from(line.trim()));
            }
        }
    }

    // Search Windows SDK
    let sdk_root = std::path::Path::new(r"C:\Program Files (x86)\Windows Kits\10\bin");
    if sdk_root.exists() {
        if let Ok(entries) = std::fs::read_dir(sdk_root) {
            let mut versions: Vec<_> = entries
                .filter_map(|e| e.ok())
                .filter(|e| e.path().join("x64/fxc.exe").exists())
                .collect();
            versions.sort_by(|a, b| b.file_name().cmp(&a.file_name()));
            if let Some(latest) = versions.first() {
                return Some(latest.path().join("x64/fxc.exe"));
            }
        }
    }

    None
}

fn main() {
    let shader_src = "src/render/gpu/d3d11/shaders/nv12_to_bgra_cs.hlsl";
    let out_dir = std::env::var("OUT_DIR").unwrap();
    let cso_path = format!("{}/nv12_to_bgra_cs.cso", out_dir);

    let fxc = find_fxc().expect("fxc.exe not found — ensure Windows SDK is installed");

    let result = std::process::Command::new(&fxc)
        .args([
            "/T", "cs_5_0",
            "/E", "main",
            "/O3",
        ])
        .arg(format!("/Fo{}", cso_path))
        .arg(shader_src)
        .output();

    match result {
        Ok(output) => {
            if !output.status.success() {
                let stderr = String::from_utf8_lossy(&output.stderr);
                panic!("fxc shader compilation failed:\n{}", stderr);
            }
        }
        Err(e) => {
            panic!("fxc execution failed: {}", e);
        }
    }

    println!("cargo:rerun-if-changed={}", shader_src);
}
