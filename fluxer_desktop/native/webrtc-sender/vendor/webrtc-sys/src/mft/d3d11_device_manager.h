#ifndef WEBRTC_MFT_D3D11_DEVICE_MANAGER_H_
#define WEBRTC_MFT_D3D11_DEVICE_MANAGER_H_

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

namespace webrtc {
namespace mft {

// Owns a single D3D11 device (created with video support) plus the
// IMFDXGIDeviceManager wrapper that Media Foundation hardware transforms
// need in order to consume D3D11 textures directly. This device is
// intentionally separate from win-game-capture's own capture device: it is
// only ever used to attach to MFT encoder instances, and is deliberately not
// pinned to a particular adapter (matching win-game-capture's own unpinned
// WGC device selection) - AdapterLuid()/callers are expected to detect a
// mismatch against the captured texture's adapter and fall back gracefully
// rather than assuming zero-copy always works (see MftH264EncoderImpl).
class D3D11EncodeDeviceManager {
 public:
  D3D11EncodeDeviceManager();
  ~D3D11EncodeDeviceManager();

  // Creates the D3D11 device and IMFDXGIDeviceManager. Safe to call once;
  // returns false (logging the reason) on any failure, never throws/crashes.
  bool Initialize();

  bool IsInitialized() const { return device_ != nullptr && device_manager_ != nullptr; }

  Microsoft::WRL::ComPtr<ID3D11Device> device() const { return device_; }
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> device_manager() const {
    return device_manager_;
  }
  LUID adapter_luid() const { return adapter_luid_; }

  // Returns true if a texture opened via OpenSharedResource on this
  // manager's device would succeed for a texture created on the given
  // adapter LUID - i.e. whether the two devices are on the same adapter.
  // Legacy NT shared handles (IDXGIResource::GetSharedHandle, which is what
  // win-game-capture produces) only work same-adapter; this is the
  // Phase-1-scope mismatch check called out in the plan (full cross-adapter
  // support is explicitly deferred).
  bool AdapterMatches(LUID other) const;

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> device_manager_;
  LUID adapter_luid_ = {};
};

}  // namespace mft
}  // namespace webrtc

#endif  // WEBRTC_MFT_D3D11_DEVICE_MANAGER_H_
