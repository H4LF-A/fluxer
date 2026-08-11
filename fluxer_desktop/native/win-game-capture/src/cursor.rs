// SPDX-License-Identifier: AGPL-3.0-or-later

// Backend-agnostic cursor compositing: reads the current OS cursor via the
// generic Win32 GetCursorInfo/GetIconInfo APIs (the same non-backend-specific
// mechanism Chromium's MouseCursorMonitorWin uses - no DLL injection, no
// hooking, safe alongside anti-cheat), then draws it onto a D3D11 texture
// via Direct2D interop. This exists specifically to fix Windows Graphics
// Capture's own cursor compositing bug (cursor invisible in some fullscreen
// games) by disabling WGC's automatic compositing and doing it ourselves.
//
// Any failure here (no cursor, unsupported shape, D2D error) is logged and
// composite() becomes a no-op for that frame - a frame that ships without a
// cursor drawn on it is the existing baseline behavior, never worse.

use windows::Win32::Foundation::{HWND, POINT};
use windows::Win32::Graphics::Direct2D::Common::{
    D2D1_ALPHA_MODE_PREMULTIPLIED, D2D1_PIXEL_FORMAT, D2D_RECT_F, D2D_SIZE_U,
};
use windows::Win32::Graphics::Direct2D::{
    D2D1_BITMAP_PROPERTIES1, D2D1_BITMAP_OPTIONS_NONE, D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
    D2D1CreateFactory, D2D1_FACTORY_TYPE_SINGLE_THREADED, D2D1_INTERPOLATION_MODE_LINEAR, ID2D1Bitmap1,
    ID2D1Device, ID2D1DeviceContext, ID2D1Factory1,
};
use windows::Win32::Graphics::Direct3D11::{D3D11_TEXTURE2D_DESC, ID3D11Device};
use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_B8G8R8A8_UNORM;
use windows::Win32::Graphics::Dxgi::IDXGIDevice;
use windows::Win32::Graphics::Dxgi::IDXGISurface;
use windows::Win32::Graphics::Gdi::{
    BITMAP, BITMAPINFO, BITMAPINFOHEADER, DIB_RGB_COLORS, DeleteObject, GetDIBits, GetObjectW, HBITMAP,
};
use windows::Win32::UI::WindowsAndMessaging::{CURSORINFO, GetCursorInfo, GetIconInfo, ICONINFO};
use windows::core::Interface;

struct CursorPixels {
    width: i32,
    height: i32,
    hotspot_x: i32,
    hotspot_y: i32,
    // Premultiplied BGRA, top-down, width*height*4 bytes.
    argb: Vec<u8>,
}

fn dib_pixels(bitmap: HBITMAP, width: i32, height: i32) -> Option<Vec<u8>> {
    // GetDIBits with a negative height requests a top-down DIB, matching the
    // row order we want without a manual flip.
    let mut info = BITMAPINFO {
        bmiHeader: BITMAPINFOHEADER {
            biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32,
            biWidth: width,
            biHeight: -height,
            biPlanes: 1,
            biBitCount: 32,
            biCompression: 0,
            ..Default::default()
        },
        ..Default::default()
    };
    let row_bytes = (width as usize) * 4;
    let mut buffer = vec![0u8; row_bytes * height as usize];
    let hdc = unsafe { windows::Win32::Graphics::Gdi::GetDC(None) };
    if hdc.is_invalid() {
        return None;
    }
    let copied = unsafe {
        GetDIBits(
            hdc,
            bitmap,
            0,
            height as u32,
            Some(buffer.as_mut_ptr().cast()),
            &mut info,
            DIB_RGB_COLORS,
        )
    };
    unsafe {
        windows::Win32::Graphics::Gdi::ReleaseDC(None, hdc);
    }
    if copied == 0 {
        return None;
    }
    Some(buffer)
}

fn has_meaningful_alpha(pixels: &[u8]) -> bool {
    // Real per-pixel alpha cursors (the common case on modern Windows) have
    // at least some non-0/non-255 alpha values, or a mix of fully-0 and
    // fully-255 that isn't just "everything opaque" (which is what you get
    // reading a color bitmap that has no alpha channel at all, since unused
    // bytes read back as 0xFF or 0x00 depending on the driver).
    let mut saw_transparent = false;
    let mut saw_opaque = false;
    for chunk in pixels.chunks_exact(4) {
        match chunk[3] {
            0 => saw_transparent = true,
            255 => saw_opaque = true,
            _ => return true,
        }
    }
    saw_transparent && saw_opaque
}

fn apply_and_mask(color: &mut [u8], mask_bits: &[u8], width: i32, height: i32) {
    let stride = ((width + 31) / 32 * 4) as usize;
    for y in 0..height as usize {
        for x in 0..width as usize {
            let byte = mask_bits.get(y * stride + x / 8).copied().unwrap_or(0);
            let bit_set = (byte >> (7 - (x % 8))) & 1 == 1;
            let idx = (y * width as usize + x) * 4;
            if let Some(alpha) = color.get_mut(idx + 3) {
                *alpha = if bit_set { 0 } else { 255 };
            }
        }
    }
}

fn premultiply(pixels: &mut [u8]) {
    for chunk in pixels.chunks_exact_mut(4) {
        let a = chunk[3] as u32;
        chunk[0] = ((chunk[0] as u32) * a / 255) as u8;
        chunk[1] = ((chunk[1] as u32) * a / 255) as u8;
        chunk[2] = ((chunk[2] as u32) * a / 255) as u8;
    }
}

fn read_cursor_pixels(icon_info: &ICONINFO) -> Option<CursorPixels> {
    let mut bmp = BITMAP::default();
    let mask_handle = icon_info.hbmMask;
    if mask_handle.is_invalid() {
        return None;
    }
    if unsafe {
        GetObjectW(
            mask_handle.into(),
            std::mem::size_of::<BITMAP>() as i32,
            Some((&mut bmp as *mut BITMAP).cast()),
        )
    } == 0
    {
        return None;
    }

    let has_color = !icon_info.hbmColor.is_invalid();
    let width = bmp.bmWidth;
    // A cursor with no separate color bitmap stores AND+XOR masks stacked
    // in one bitmap (top half AND, bottom half XOR); with a color bitmap,
    // hbmMask is just the AND mask at full height.
    let height = if has_color { bmp.bmHeight } else { bmp.bmHeight / 2 };
    if width <= 0 || height <= 0 || width > 512 || height > 512 {
        return None;
    }

    if has_color {
        let mut color_bmp = BITMAP::default();
        if unsafe {
            GetObjectW(
                icon_info.hbmColor.into(),
                std::mem::size_of::<BITMAP>() as i32,
                Some((&mut color_bmp as *mut BITMAP).cast()),
            )
        } == 0
        {
            return None;
        }
        let mut pixels = dib_pixels(icon_info.hbmColor, width, height)?;
        if !has_meaningful_alpha(&pixels) {
            let mask_stride = ((width + 15) / 16 * 2) as usize;
            let mask_pixels = dib_1bpp(mask_handle, width, bmp.bmHeight, mask_stride)?;
            apply_and_mask(&mut pixels, &mask_pixels, width, height);
        }
        premultiply(&mut pixels);
        return Some(CursorPixels {
            width,
            height,
            hotspot_x: icon_info.xHotspot as i32,
            hotspot_y: icon_info.yHotspot as i32,
            argb: pixels,
        });
    }

    // Monochrome: combined AND (top half) + XOR (bottom half) 1bpp mask.
    let mask_stride = ((width + 15) / 16 * 2) as usize;
    let full = dib_1bpp(mask_handle, width, bmp.bmHeight, mask_stride)?;
    let and_bits = &full[0..mask_stride * height as usize];
    let xor_bits = &full[mask_stride * height as usize..];
    let mut pixels = vec![0u8; (width * height * 4) as usize];
    for y in 0..height as usize {
        for x in 0..width as usize {
            let and_bit = ((and_bits.get(y * mask_stride + x / 8).copied().unwrap_or(0)) >> (7 - (x % 8))) & 1;
            let xor_bit = ((xor_bits.get(y * mask_stride + x / 8).copied().unwrap_or(0)) >> (7 - (x % 8))) & 1;
            let idx = (y * width as usize + x) * 4;
            let (color, alpha) = match (and_bit, xor_bit) {
                (0, 0) => (0u8, 255u8),   // black, opaque
                (0, 1) => (255u8, 255u8), // white, opaque
                _ => (0u8, 0u8),          // transparent (screen shows through)
            };
            pixels[idx] = color;
            pixels[idx + 1] = color;
            pixels[idx + 2] = color;
            pixels[idx + 3] = alpha;
        }
    }
    Some(CursorPixels {
        width,
        height,
        hotspot_x: icon_info.xHotspot as i32,
        hotspot_y: icon_info.yHotspot as i32,
        argb: pixels,
    })
}

fn dib_1bpp(bitmap: HBITMAP, width: i32, height: i32, stride: usize) -> Option<Vec<u8>> {
    let mut info = BITMAPINFO {
        bmiHeader: BITMAPINFOHEADER {
            biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32,
            biWidth: width,
            biHeight: -height,
            biPlanes: 1,
            biBitCount: 1,
            biCompression: 0,
            ..Default::default()
        },
        ..Default::default()
    };
    let mut buffer = vec![0u8; stride * height as usize];
    let hdc = unsafe { windows::Win32::Graphics::Gdi::GetDC(None) };
    if hdc.is_invalid() {
        return None;
    }
    let copied = unsafe {
        GetDIBits(
            hdc,
            bitmap,
            0,
            height as u32,
            Some(buffer.as_mut_ptr().cast()),
            &mut info,
            DIB_RGB_COLORS,
        )
    };
    unsafe {
        windows::Win32::Graphics::Gdi::ReleaseDC(None, hdc);
    }
    if copied == 0 {
        return None;
    }
    Some(buffer)
}

/// Current OS cursor position (screen coordinates) and pixel data, or None
/// if the cursor is hidden or couldn't be read.
struct CursorSnapshot {
    screen_x: i32,
    screen_y: i32,
    pixels: CursorPixels,
}

fn snapshot_cursor() -> Option<CursorSnapshot> {
    let mut info = CURSORINFO {
        cbSize: std::mem::size_of::<CURSORINFO>() as u32,
        ..Default::default()
    };
    unsafe { GetCursorInfo(&mut info) }.ok()?;
    if info.flags.0 != 1 {
        // CURSOR_SHOWING == 1; hidden or suppressed cursor.
        return None;
    }
    let mut icon_info = ICONINFO::default();
    unsafe { GetIconInfo(info.hCursor.into(), &mut icon_info) }.ok()?;
    let pixels = read_cursor_pixels(&icon_info);
    unsafe {
        if !icon_info.hbmColor.is_invalid() {
            let _ = DeleteObject(icon_info.hbmColor.into());
        }
        if !icon_info.hbmMask.is_invalid() {
            let _ = DeleteObject(icon_info.hbmMask.into());
        }
    }
    let pixels = pixels?;
    Some(CursorSnapshot {
        screen_x: info.ptScreenPos.x,
        screen_y: info.ptScreenPos.y,
        pixels,
    })
}

/// Lazily-created D2D device/context bound to a given D3D11 device, used to
/// alpha-blend the cursor bitmap onto capture output textures. One instance
/// is kept per capture session (see WgcState) rather than recreated per
/// frame.
pub struct CursorCompositor {
    context: ID2D1DeviceContext,
    cached_bitmap: Option<(i32, i32, ID2D1Bitmap1)>,
}

impl CursorCompositor {
    pub fn new(device: &ID3D11Device) -> Option<Self> {
        let dxgi_device: IDXGIDevice = device.cast().ok()?;
        let factory: ID2D1Factory1 =
            unsafe { D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, None) }.ok()?;
        let d2d_device: ID2D1Device = unsafe { factory.CreateDevice(&dxgi_device) }.ok()?;
        let context: ID2D1DeviceContext =
            unsafe { d2d_device.CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE) }.ok()?;
        Some(Self { context, cached_bitmap: None })
    }

    /// Draws the current OS cursor onto `target_texture` (must be a
    /// DXGI_FORMAT_B8G8R8A8_UNORM texture bound as a D2D render target
    /// candidate) if the cursor's screen position falls within
    /// [capture_origin, capture_origin + (target width, height)).
    /// `capture_origin_{x,y}` is the top-left of the captured content in
    /// screen coordinates (window rect origin for per-window capture,
    /// monitor rect origin for per-monitor capture).
    pub fn composite(
        &mut self,
        target_texture: &windows::Win32::Graphics::Direct3D11::ID3D11Texture2D,
        capture_origin_x: i32,
        capture_origin_y: i32,
    ) {
        // Guard against a format mismatch before touching D2D at all: the
        // D2D bitmap properties below assume BGRA8, but this compositor can
        // be called against the NV12 pipeline's HDR (FP16) input texture
        // too. Creating a D2D render-target bitmap with the wrong declared
        // format against a real surface is the kind of interop mistake that
        // can leave the D3D11/D2D device in a bad state rather than just
        // failing cleanly, corrupting whatever reads the texture next (the
        // NV12 video processor conversion) - so check the actual texture
        // format first and skip compositing entirely if it's not what we
        // expect, rather than trust CreateBitmapFromDxgiSurface to reject a
        // mismatch safely.
        let mut desc = D3D11_TEXTURE2D_DESC::default();
        unsafe {
            target_texture.GetDesc(&mut desc);
        }
        if desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM {
            return;
        }

        let Some(snapshot) = snapshot_cursor() else {
            return;
        };
        let local_x = (snapshot.screen_x - capture_origin_x) as f32 - snapshot.pixels.hotspot_x as f32;
        let local_y = (snapshot.screen_y - capture_origin_y) as f32 - snapshot.pixels.hotspot_y as f32;

        let Ok(surface) = target_texture.cast::<IDXGISurface>() else {
            return;
        };
        let props = D2D1_BITMAP_PROPERTIES1 {
            pixelFormat: D2D1_PIXEL_FORMAT {
                format: DXGI_FORMAT_B8G8R8A8_UNORM,
                alphaMode: D2D1_ALPHA_MODE_PREMULTIPLIED,
            },
            dpiX: 96.0,
            dpiY: 96.0,
            bitmapOptions: D2D1_BITMAP_OPTIONS_NONE,
            ..Default::default()
        };
        let Ok(target_bitmap) = (unsafe {
            self.context
                .CreateBitmapFromDxgiSurface(&surface, Some(&props as *const _))
        }) else {
            return;
        };
        unsafe {
            self.context.SetTarget(&target_bitmap);
        }

        let cursor_bitmap = match &self.cached_bitmap {
            Some((w, h, bmp)) if *w == snapshot.pixels.width && *h == snapshot.pixels.height => {
                // Cursor SHAPE may still have changed frame-to-frame with the
                // same dimensions (e.g. animated cursors) - re-upload pixels
                // into the existing bitmap rather than trusting the cache
                // blindly.
                let rect = windows::Win32::Graphics::Direct2D::Common::D2D_RECT_U {
                    left: 0,
                    top: 0,
                    right: snapshot.pixels.width as u32,
                    bottom: snapshot.pixels.height as u32,
                };
                let _ = unsafe {
                    bmp.CopyFromMemory(
                        Some(&rect),
                        snapshot.pixels.argb.as_ptr().cast(),
                        (snapshot.pixels.width * 4) as u32,
                    )
                };
                bmp.clone()
            }
            _ => {
                let bitmap_props = D2D1_BITMAP_PROPERTIES1 {
                    pixelFormat: D2D1_PIXEL_FORMAT {
                        format: DXGI_FORMAT_B8G8R8A8_UNORM,
                        alphaMode: D2D1_ALPHA_MODE_PREMULTIPLIED,
                    },
                    dpiX: 96.0,
                    dpiY: 96.0,
                    bitmapOptions: D2D1_BITMAP_OPTIONS_NONE,
                    ..Default::default()
                };
                let Ok(bmp) = (unsafe {
                    self.context.CreateBitmap(
                        D2D_SIZE_U {
                            width: snapshot.pixels.width as u32,
                            height: snapshot.pixels.height as u32,
                        },
                        Some(snapshot.pixels.argb.as_ptr().cast()),
                        (snapshot.pixels.width * 4) as u32,
                        &bitmap_props as *const _,
                    )
                }) else {
                    unsafe {
                        self.context.SetTarget(None);
                    }
                    return;
                };
                self.cached_bitmap = Some((snapshot.pixels.width, snapshot.pixels.height, bmp.clone()));
                bmp
            }
        };

        let dest_rect = D2D_RECT_F {
            left: local_x,
            top: local_y,
            right: local_x + snapshot.pixels.width as f32,
            bottom: local_y + snapshot.pixels.height as f32,
        };
        unsafe {
            self.context.BeginDraw();
            self.context.DrawBitmap(
                &cursor_bitmap,
                Some(&dest_rect as *const _),
                1.0,
                D2D1_INTERPOLATION_MODE_LINEAR,
                None,
                None,
            );
            let _ = self.context.EndDraw(None, None);
            self.context.SetTarget(None);
        }
    }
}

/// Screen-space top-left origin of a capture target, used to map
/// GetCursorInfo's screen coordinates into the captured content's local
/// coordinate space.
pub fn capture_origin_for_target(target: crate::wgc_capture::WgcCaptureTarget) -> Option<(i32, i32)> {
    match target {
        crate::wgc_capture::WgcCaptureTarget::Window(hwnd) => window_origin(hwnd),
        crate::wgc_capture::WgcCaptureTarget::Monitor(monitor) => monitor_origin(monitor),
    }
}

fn window_origin(hwnd: HWND) -> Option<(i32, i32)> {
    let mut rect = windows::Win32::Foundation::RECT::default();
    unsafe { windows::Win32::UI::WindowsAndMessaging::GetWindowRect(hwnd, &mut rect) }.ok()?;
    Some((rect.left, rect.top))
}

fn monitor_origin(monitor: windows::Win32::Graphics::Gdi::HMONITOR) -> Option<(i32, i32)> {
    let mut info = windows::Win32::Graphics::Gdi::MONITORINFO {
        cbSize: std::mem::size_of::<windows::Win32::Graphics::Gdi::MONITORINFO>() as u32,
        ..Default::default()
    };
    let ok = unsafe { windows::Win32::Graphics::Gdi::GetMonitorInfoW(monitor, &mut info) };
    if !ok.as_bool() {
        return None;
    }
    Some((info.rcMonitor.left, info.rcMonitor.top))
}

#[allow(dead_code)]
fn _unused_point(_: POINT) {}
