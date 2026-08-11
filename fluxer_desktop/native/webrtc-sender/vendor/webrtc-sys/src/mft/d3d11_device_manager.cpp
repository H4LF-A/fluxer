#include "d3d11_device_manager.h"

#include <dxgi.h>

#include "rtc_base/logging.h"

namespace webrtc {
namespace mft {

D3D11EncodeDeviceManager::D3D11EncodeDeviceManager() = default;
D3D11EncodeDeviceManager::~D3D11EncodeDeviceManager() = default;

bool D3D11EncodeDeviceManager::Initialize() {
  if (IsInitialized()) {
    return true;
  }

  D3D_FEATURE_LEVEL feature_level = {};
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
      device_.GetAddressOf(), &feature_level, nullptr);
  if (FAILED(hr) || !device_) {
    RTC_LOG(LS_WARNING)
        << "MFT encoder: failed to create D3D11 device with video support, "
           "hr="
        << hr;
    device_.Reset();
    return false;
  }

  Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
  if (SUCCEEDED(device_.As(&multithread))) {
    multithread->SetMultithreadProtected(TRUE);
  }

  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  if (SUCCEEDED(device_.As(&dxgi_device))) {
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(dxgi_device->GetAdapter(adapter.GetAddressOf()))) {
      DXGI_ADAPTER_DESC desc = {};
      if (SUCCEEDED(adapter->GetDesc(&desc))) {
        adapter_luid_ = desc.AdapterLuid;
      }
    }
  }

  UINT reset_token = 0;
  hr = MFCreateDXGIDeviceManager(&reset_token, device_manager_.GetAddressOf());
  if (FAILED(hr) || !device_manager_) {
    RTC_LOG(LS_WARNING)
        << "MFT encoder: MFCreateDXGIDeviceManager failed, hr=" << hr;
    device_.Reset();
    device_manager_.Reset();
    return false;
  }

  hr = device_manager_->ResetDevice(device_.Get(), reset_token);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MFT encoder: IMFDXGIDeviceManager::ResetDevice "
                           "failed, hr="
                        << hr;
    device_.Reset();
    device_manager_.Reset();
    return false;
  }

  return true;
}

bool D3D11EncodeDeviceManager::AdapterMatches(LUID other) const {
  if (!IsInitialized()) {
    return false;
  }
  return adapter_luid_.LowPart == other.LowPart &&
         adapter_luid_.HighPart == other.HighPart;
}

}  // namespace mft
}  // namespace webrtc
