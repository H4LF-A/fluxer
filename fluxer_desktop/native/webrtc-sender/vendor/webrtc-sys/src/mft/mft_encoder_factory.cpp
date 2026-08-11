#include "mft_encoder_factory.h"

#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include "mft_h264_encoder_impl.h"
#include "rtc_base/logging.h"

namespace webrtc {

namespace {

// Media Foundation is reference-counted across MFStartup/MFShutdown calls
// within a process; we only ever start it up (never shut it down here)
// since other MFT consumers in this process (if any, now or later) would
// be affected by an unbalanced shutdown, and the OS reclaims MF resources
// on process exit regardless.
bool EnsureMediaFoundationStarted() {
  static bool started = false;
  static bool start_succeeded = false;
  if (!started) {
    started = true;
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    start_succeeded = SUCCEEDED(hr);
    if (!start_succeeded) {
      RTC_LOG(LS_WARNING) << "MFT encoder: MFStartup failed, hr=" << hr;
    }
  }
  return start_succeeded;
}

}  // namespace

MftVideoEncoderFactory::MftVideoEncoderFactory() {
  std::map<std::string, std::string> baseline_parameters = {
      {"profile-level-id", "42e01f"},
      {"level-asymmetry-allowed", "1"},
      {"packetization-mode", "1"},
  };
  supported_formats_.push_back(SdpVideoFormat("H264", baseline_parameters));
}

MftVideoEncoderFactory::~MftVideoEncoderFactory() {}

bool MftVideoEncoderFactory::IsSupported() {
  if (!EnsureMediaFoundationStarted()) {
    return false;
  }

  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  MFT_REGISTER_TYPE_INFO output_info = {MFMediaType_Video, MFVideoFormat_H264};
  HRESULT hr = MFTEnumEx(
      MFT_CATEGORY_VIDEO_ENCODER,
      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
      &output_info, &activates, &count);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MFT encoder: MFTEnumEx failed, hr=" << hr;
    return false;
  }
  for (UINT32 i = 0; i < count; ++i) {
    if (activates[i]) activates[i]->Release();
  }
  if (activates) CoTaskMemFree(activates);

  if (count == 0) {
    RTC_LOG(LS_INFO)
        << "MFT encoder: no hardware H264 encoder MFT registered on this "
           "system (older/low-end GPU, or driver doesn't expose one).";
    return false;
  }

  RTC_LOG(LS_INFO) << "MFT encoder: found " << count
                   << " hardware H264 encoder MFT(s).";
  return true;
}

std::unique_ptr<VideoEncoder> MftVideoEncoderFactory::Create(
    const Environment& env,
    const SdpVideoFormat& format) {
  for (const auto& supported_format : supported_formats_) {
    if (format.IsSameCodec(supported_format)) {
      RTC_LOG(LS_INFO) << "Using MFT hardware encoder for H264";
      return std::make_unique<MftH264EncoderImpl>(format);
    }
  }
  return nullptr;
}

std::vector<SdpVideoFormat> MftVideoEncoderFactory::GetSupportedFormats()
    const {
  return supported_formats_;
}

std::vector<SdpVideoFormat> MftVideoEncoderFactory::GetImplementations()
    const {
  return supported_formats_;
}

}  // namespace webrtc
