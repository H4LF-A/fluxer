#ifndef WEBRTC_MFT_H264_ENCODER_IMPL_H_
#define WEBRTC_MFT_H264_ENCODER_IMPL_H_

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <memory>
#include <vector>

#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "d3d11_device_manager.h"

namespace webrtc {

// A webrtc::VideoEncoder backed by a Windows Media Foundation Transform
// (MFT) hardware H.264 encoder. Unlike the NVENC-specific encoder, MFT is a
// vendor-neutral OS API - NVIDIA/AMD/Intel each register a hardware-
// accelerated MFT via their driver, so this single implementation covers
// all three. It consumes a FluxerGpuFrameBuffer's D3D11 texture directly
// when possible (see Encode()); any failure (unsupported format, adapter
// mismatch, transform error) causes Encode() to fail so the caller falls
// through to the CPU (ToI420) fallback path, exactly like the existing
// NVENC encoder's TryEncodeNativeGpuFrame -> PrepareNv12HostFrame fallback
// shape. This encoder never crashes the process on a hardware/driver
// problem - every failure mode is a logged, non-fatal return value.
class MftH264EncoderImpl : public VideoEncoder {
 public:
  explicit MftH264EncoderImpl(const SdpVideoFormat& format);
  ~MftH264EncoderImpl() override;

  int32_t InitEncode(const VideoCodec* codec_settings,
                     const Settings& settings) override;
  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override;
  int32_t Release() override;
  int32_t Encode(const VideoFrame& frame,
                 const std::vector<VideoFrameType>* frame_types) override;
  void SetRates(const RateControlParameters& rc_parameters) override;
  EncoderInfo GetEncoderInfo() const override;

 private:
  bool CreateTransform();
  bool ConfigureOutputType();
  // Picks and sets an input media type the transform actually advertises
  // support for; returns the chosen DXGI-compatible subtype, or GUID_NULL
  // if nothing usable was found (in which case this encoder can't consume
  // D3D11 textures directly and every Encode() call will fail over to the
  // CPU fallback).
  GUID ConfigureInputType();
  bool SubmitFrame(ID3D11Texture2D* texture,
                   UINT subresource,
                   int64_t timestamp_100ns,
                   bool force_keyframe);
  // Drains any output samples currently available; delivers each as an
  // encoded image via the registered callback. Returns false only on a
  // hard transform error (not on "no output yet").
  bool DrainOutput(const VideoFrame& source_frame);
  void DeliverEncodedImage(const uint8_t* data,
                           size_t size,
                           const VideoFrame& source_frame);
  void ResetState();

  const SdpVideoFormat format_;
  EncodedImageCallback* callback_ = nullptr;

  mft::D3D11EncodeDeviceManager device_manager_;
  Microsoft::WRL::ComPtr<IMFTransform> transform_;
  GUID input_subtype_ = GUID_NULL;
  DWORD input_stream_id_ = 0;
  DWORD output_stream_id_ = 0;
  DWORD output_sample_flags_ = 0;
  bool output_provides_samples_ = false;

  int width_ = 0;
  int height_ = 0;
  uint32_t max_framerate_ = 30;
  uint32_t target_bps_ = 0;
  bool streaming_ = false;
  bool force_next_keyframe_ = true;

  EncodedImage encoded_image_;
  H264BitstreamParser h264_bitstream_parser_;
  int64_t frame_count_ = 0;
};

}  // namespace webrtc

#endif  // WEBRTC_MFT_H264_ENCODER_IMPL_H_
