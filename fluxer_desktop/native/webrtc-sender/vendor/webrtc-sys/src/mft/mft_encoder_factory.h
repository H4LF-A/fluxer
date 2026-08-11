#ifndef WEBRTC_MFT_ENCODER_FACTORY_H_
#define WEBRTC_MFT_ENCODER_FACTORY_H_

#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace webrtc {

// Vendor-neutral Windows hardware encoder factory backed by Media
// Foundation Transforms (MFT). NVIDIA/AMD/Intel each register a
// hardware-accelerated H.264 MFT via their GPU driver, so a single
// MFTEnumEx probe (IsSupported()) covers all three, unlike the
// NVIDIA-specific NvidiaVideoEncoderFactory.
class MftVideoEncoderFactory : public VideoEncoderFactory {
 public:
  MftVideoEncoderFactory();
  ~MftVideoEncoderFactory() override;

  // Probes for a hardware (MFT_ENUM_FLAG_HARDWARE) H.264 encoder MFT.
  // Returns false gracefully (never throws) if Media Foundation isn't
  // available or no hardware encoder is registered (e.g. older/low-end
  // GPU) - callers should treat that as "MFT unavailable," not fatal.
  static bool IsSupported();

  std::unique_ptr<VideoEncoder> Create(const Environment& env,
                                       const SdpVideoFormat& format) override;

  std::vector<SdpVideoFormat> GetSupportedFormats() const override;
  std::vector<SdpVideoFormat> GetImplementations() const override;

  std::unique_ptr<EncoderSelectorInterface> GetEncoderSelector()
      const override {
    return nullptr;
  }

 private:
  std::vector<SdpVideoFormat> supported_formats_;
};

}  // namespace webrtc

#endif  // WEBRTC_MFT_ENCODER_FACTORY_H_
