#include "mft_h264_encoder_impl.h"

#include <codecapi.h>
#include <icodecapi.h>
#include <mferror.h>

#include <algorithm>

#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "common_video/h264/h264_common.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "livekit/video_frame_buffer.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

namespace webrtc {

namespace {

// DXGI_FORMAT values, mirrored here so this file doesn't need to depend on
// win-game-capture's Rust crate. Kept in sync with the values
// win-game-capture actually produces (dxgi_capture.rs / wgc_capture.rs):
// DXGI_FORMAT_B8G8R8A8_UNORM = 87, DXGI_FORMAT_NV12 = 103.
constexpr uint32_t kDxgiFormatBgra8Unorm = 87;
constexpr uint32_t kDxgiFormatNv12 = 103;

int64_t MillisecondsToHns(int64_t ms) {
  return ms * 10000;
}

}  // namespace

MftH264EncoderImpl::MftH264EncoderImpl(const SdpVideoFormat& format)
    : format_(format) {}

MftH264EncoderImpl::~MftH264EncoderImpl() {
  Release();
}

void MftH264EncoderImpl::ResetState() {
  if (transform_ && streaming_) {
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
  }
  transform_.Reset();
  streaming_ = false;
  input_subtype_ = GUID_NULL;
  output_provides_samples_ = false;
}

bool MftH264EncoderImpl::CreateTransform() {
  if (!device_manager_.Initialize()) {
    return false;
  }

  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  MFT_REGISTER_TYPE_INFO output_info = {MFMediaType_Video, MFVideoFormat_H264};
  HRESULT hr = MFTEnumEx(
      MFT_CATEGORY_VIDEO_ENCODER,
      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
      &output_info, &activates, &count);
  if (FAILED(hr) || count == 0) {
    RTC_LOG(LS_WARNING) << "MFT encoder: no hardware H264 MFT found at "
                           "InitEncode time, hr="
                        << hr << ", count=" << count;
    for (UINT32 i = 0; i < count; ++i) {
      if (activates[i]) activates[i]->Release();
    }
    if (activates) CoTaskMemFree(activates);
    return false;
  }

  Microsoft::WRL::ComPtr<IMFTransform> transform;
  hr = activates[0]->ActivateObject(IID_PPV_ARGS(transform.GetAddressOf()));
  for (UINT32 i = 0; i < count; ++i) {
    if (activates[i]) activates[i]->Release();
  }
  CoTaskMemFree(activates);
  if (FAILED(hr) || !transform) {
    RTC_LOG(LS_WARNING) << "MFT encoder: ActivateObject failed, hr=" << hr;
    return false;
  }

  Microsoft::WRL::ComPtr<IMFAttributes> attributes;
  if (SUCCEEDED(transform->GetAttributes(attributes.GetAddressOf()))) {
    UINT32 is_async = 0;
    if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) &&
        is_async) {
      attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    }
  }

  hr = transform->ProcessMessage(
      MFT_MESSAGE_SET_D3D_MANAGER,
      reinterpret_cast<ULONG_PTR>(device_manager_.device_manager().Get()));
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING)
        << "MFT encoder: SET_D3D_MANAGER failed, hr=" << hr
        << " (continuing - encoder may still work via CPU-copied samples, "
           "but zero-copy will not be used)";
  }

  DWORD input_count = 0;
  DWORD output_count = 0;
  transform->GetStreamCount(&input_count, &output_count);
  DWORD input_ids[1] = {0};
  DWORD output_ids[1] = {0};
  DWORD stream_id_count = 1;
  hr = transform->GetStreamIDs(1, input_ids, 1, output_ids);
  // MF_E_NOT_IMPLEMENTED here just means stream IDs are 0..N-1, which is
  // what we already assumed.
  if (SUCCEEDED(hr)) {
    input_stream_id_ = input_ids[0];
    output_stream_id_ = output_ids[0];
  } else {
    input_stream_id_ = 0;
    output_stream_id_ = 0;
  }

  transform_ = transform;
  return true;
}

bool MftH264EncoderImpl::ConfigureOutputType() {
  Microsoft::WRL::ComPtr<IMFMediaType> output_type;
  HRESULT hr = MFCreateMediaType(output_type.GetAddressOf());
  if (FAILED(hr)) return false;

  output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  output_type->SetUINT32(MF_MT_AVG_BITRATE,
                         target_bps_ > 0 ? target_bps_ : 2000000);
  output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE,
                     static_cast<UINT32>(width_),
                     static_cast<UINT32>(height_));
  MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, max_framerate_, 1);
  MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

  hr = transform_->SetOutputType(output_stream_id_, output_type.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MFT encoder: SetOutputType failed, hr=" << hr;
    return false;
  }

  MFT_OUTPUT_STREAM_INFO stream_info = {};
  if (SUCCEEDED(transform_->GetOutputStreamInfo(output_stream_id_,
                                                &stream_info))) {
    output_provides_samples_ =
        (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
  }

  Microsoft::WRL::ComPtr<ICodecAPI> codec_api;
  if (SUCCEEDED(transform_.As(&codec_api))) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = target_bps_ > 0 ? target_bps_ : 2000000;
    codec_api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
    VariantClear(&var);

    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = VARIANT_TRUE;
    codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &var);
    VariantClear(&var);
  }

  return true;
}

GUID MftH264EncoderImpl::ConfigureInputType() {
  const GUID kCandidates[] = {MFVideoFormat_NV12, MFVideoFormat_ARGB32};
  for (const GUID& candidate : kCandidates) {
    for (DWORD i = 0;; ++i) {
      Microsoft::WRL::ComPtr<IMFMediaType> available_type;
      HRESULT hr = transform_->GetInputAvailableType(
          input_stream_id_, i, available_type.GetAddressOf());
      if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) {
        break;
      }
      GUID subtype = GUID_NULL;
      if (FAILED(available_type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        continue;
      }
      if (!IsEqualGUID(subtype, candidate)) {
        continue;
      }
      MFSetAttributeSize(available_type.Get(), MF_MT_FRAME_SIZE,
                         static_cast<UINT32>(width_),
                         static_cast<UINT32>(height_));
      MFSetAttributeRatio(available_type.Get(), MF_MT_FRAME_RATE,
                         max_framerate_, 1);
      MFSetAttributeRatio(available_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1,
                         1);
      available_type->SetUINT32(MF_MT_INTERLACE_MODE,
                               MFVideoInterlace_Progressive);
      if (SUCCEEDED(
              transform_->SetInputType(input_stream_id_, available_type.Get(),
                                       0))) {
        return candidate;
      }
    }
  }
  RTC_LOG(LS_WARNING)
      << "MFT encoder: transform advertises neither NV12 nor ARGB32 input - "
         "this hardware encoder cannot be used for zero-copy screen share "
         "capture, falling back to CPU encode for every frame.";
  return GUID_NULL;
}

int32_t MftH264EncoderImpl::InitEncode(const VideoCodec* codec_settings,
                                       const Settings& settings) {
  if (!codec_settings || codec_settings->codecType != kVideoCodecH264) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (codec_settings->width < 1 || codec_settings->height < 1 ||
      codec_settings->maxFramerate == 0) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  Release();

  width_ = codec_settings->width;
  height_ = codec_settings->height;
  max_framerate_ = codec_settings->maxFramerate;
  target_bps_ = codec_settings->startBitrate * 1000;
  force_next_keyframe_ = true;
  frame_count_ = 0;

  const size_t new_capacity =
      CalcBufferSize(VideoType::kI420, width_, height_);
  encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
  encoded_image_._encodedWidth = width_;
  encoded_image_._encodedHeight = height_;
  encoded_image_.set_size(0);

  if (!CreateTransform()) {
    ResetState();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (!ConfigureOutputType()) {
    ResetState();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  input_subtype_ = ConfigureInputType();
  if (IsEqualGUID(input_subtype_, GUID_NULL)) {
    ResetState();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  streaming_ = true;

  RTC_LOG(LS_INFO) << "MFT hardware H264 encoder initialized: " << width_
                   << "x" << height_ << " @ " << max_framerate_
                   << "fps, target_bps=" << target_bps_
                   << ", input_subtype="
                   << (IsEqualGUID(input_subtype_, MFVideoFormat_NV12)
                           ? "NV12"
                           : "ARGB32");
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MftH264EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MftH264EncoderImpl::Release() {
  ResetState();
  return WEBRTC_VIDEO_CODEC_OK;
}

bool MftH264EncoderImpl::SubmitFrame(ID3D11Texture2D* texture,
                                     UINT subresource,
                                     int64_t timestamp_100ns,
                                     bool force_keyframe) {
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture,
                                         subresource, FALSE,
                                         buffer.GetAddressOf());
  if (FAILED(hr) || !buffer) {
    RTC_LOG(LS_WARNING)
        << "MFT encoder: MFCreateDXGISurfaceBuffer failed, hr=" << hr;
    return false;
  }

  Microsoft::WRL::ComPtr<IMFSample> sample;
  hr = MFCreateSample(sample.GetAddressOf());
  if (FAILED(hr) || !sample) {
    return false;
  }
  sample->AddBuffer(buffer.Get());
  sample->SetSampleTime(timestamp_100ns);
  sample->SetSampleDuration(MillisecondsToHns(1000) /
                            std::max<uint32_t>(max_framerate_, 1));

  if (force_keyframe) {
    Microsoft::WRL::ComPtr<ICodecAPI> codec_api;
    if (SUCCEEDED(transform_.As(&codec_api))) {
      VARIANT var;
      VariantInit(&var);
      var.vt = VT_UI4;
      var.ulVal = 1;
      codec_api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
      VariantClear(&var);
    }
  }

  hr = transform_->ProcessInput(input_stream_id_, sample.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MFT encoder: ProcessInput failed, hr=" << hr;
    return false;
  }
  return true;
}

bool MftH264EncoderImpl::DrainOutput(const VideoFrame& source_frame) {
  for (;;) {
    MFT_OUTPUT_DATA_BUFFER output_buffer = {};
    output_buffer.dwStreamID = output_stream_id_;

    Microsoft::WRL::ComPtr<IMFSample> owned_sample;
    if (!output_provides_samples_) {
      MFT_OUTPUT_STREAM_INFO stream_info = {};
      transform_->GetOutputStreamInfo(output_stream_id_, &stream_info);
      Microsoft::WRL::ComPtr<IMFMediaBuffer> out_buffer;
      HRESULT alloc_hr = MFCreateMemoryBuffer(
          std::max<DWORD>(stream_info.cbSize, 1 << 20),
          out_buffer.GetAddressOf());
      if (FAILED(alloc_hr)) {
        return false;
      }
      MFCreateSample(owned_sample.GetAddressOf());
      owned_sample->AddBuffer(out_buffer.Get());
      output_buffer.pSample = owned_sample.Get();
    }

    DWORD status = 0;
    HRESULT hr = transform_->ProcessOutput(0, 1, &output_buffer, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      return true;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      RTC_LOG(LS_WARNING)
          << "MFT encoder: transform requested a stream change mid-session "
             "(unsupported here); resolution/format change requires a "
             "full re-InitEncode.";
      return false;
    }
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "MFT encoder: ProcessOutput failed, hr=" << hr;
      if (output_buffer.pEvents) output_buffer.pEvents->Release();
      return false;
    }

    IMFSample* sample = output_provides_samples_ ? output_buffer.pSample
                                                  : owned_sample.Get();
    if (sample) {
      Microsoft::WRL::ComPtr<IMFMediaBuffer> media_buffer;
      if (SUCCEEDED(sample->ConvertToContiguousBuffer(
              media_buffer.GetAddressOf()))) {
        BYTE* data = nullptr;
        DWORD max_len = 0;
        DWORD cur_len = 0;
        if (SUCCEEDED(
                media_buffer->Lock(&data, &max_len, &cur_len)) &&
            data && cur_len > 0) {
          DeliverEncodedImage(data, cur_len, source_frame);
          media_buffer->Unlock();
        }
      }
    }
    if (output_provides_samples_ && output_buffer.pSample) {
      output_buffer.pSample->Release();
    }
    if (output_buffer.pEvents) {
      output_buffer.pEvents->Release();
    }
  }
}

void MftH264EncoderImpl::DeliverEncodedImage(const uint8_t* data,
                                             size_t size,
                                             const VideoFrame& source_frame) {
  if (!callback_) {
    return;
  }
  encoded_image_._encodedWidth = width_;
  encoded_image_._encodedHeight = height_;
  encoded_image_.SetRtpTimestamp(source_frame.rtp_timestamp());
  encoded_image_.SetSimulcastIndex(0);
  encoded_image_.ntp_time_ms_ = source_frame.ntp_time_ms();
  encoded_image_.capture_time_ms_ = source_frame.render_time_ms();
  encoded_image_.rotation_ = source_frame.rotation();
  encoded_image_.content_type_ = VideoContentType::UNSPECIFIED;
  encoded_image_.timing_.flags = VideoSendTiming::kInvalid;
  encoded_image_._frameType = VideoFrameType::kVideoFrameDelta;
  encoded_image_.SetColorSpace(source_frame.color_space());

  std::vector<H264::NaluIndex> nalu_indices =
      H264::FindNaluIndices(MakeArrayView(data, size));
  for (const auto& index : nalu_indices) {
    const H264::NaluType nalu_type =
        H264::ParseNaluType(data[index.payload_start_offset]);
    if (nalu_type == H264::kIdr) {
      encoded_image_._frameType = VideoFrameType::kVideoFrameKey;
      break;
    }
  }

  encoded_image_.SetEncodedData(EncodedImageBuffer::Create(data, size));
  encoded_image_.set_size(size);

  h264_bitstream_parser_.ParseBitstream(encoded_image_);
  encoded_image_.qp_ = h264_bitstream_parser_.GetLastSliceQp().value_or(-1);

  CodecSpecificInfo codec_info;
  codec_info.codecType = kVideoCodecH264;
  codec_info.codecSpecific.H264.packetization_mode =
      H264PacketizationMode::NonInterleaved;

  const auto result = callback_->OnEncodedImage(encoded_image_, &codec_info);
  if (result.error != EncodedImageCallback::Result::OK) {
    RTC_LOG(LS_ERROR) << "MFT encoder: OnEncodedImage callback failed "
                      << result.error;
  }
}

int32_t MftH264EncoderImpl::Encode(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  if (!transform_ || IsEqualGUID(input_subtype_, GUID_NULL)) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (!callback_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (frame_types != nullptr && !frame_types->empty() &&
      (*frame_types)[0] == VideoFrameType::kEmptyFrame) {
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }

  const auto buffer = input_frame.video_frame_buffer();
  const auto* gpu_frame =
      buffer ? livekit_ffi::AsFluxerGpuFrameBuffer(buffer.get()) : nullptr;
  if (!gpu_frame ||
      gpu_frame->kind() !=
          livekit_ffi::FluxerGpuFrameBuffer::Kind::kD3D11Texture) {
    // Not a D3D11 GPU texture (e.g. this is a non-screen-share video track,
    // or a frame that already arrived as I420/NV12) - this encoder only
    // handles the GPU-texture screen-share path. Let the caller fall back.
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  const GUID expected_subtype =
      IsEqualGUID(input_subtype_, MFVideoFormat_NV12)
          ? MFVideoFormat_NV12
          : MFVideoFormat_ARGB32;
  const uint32_t expected_dxgi_format =
      IsEqualGUID(expected_subtype, MFVideoFormat_NV12) ? kDxgiFormatNv12
                                                        : kDxgiFormatBgra8Unorm;
  if (gpu_frame->dxgi_format() != expected_dxgi_format) {
    // This particular frame's format doesn't match what we negotiated at
    // InitEncode time (e.g. capture switched from BGRA passthrough to an
    // NV12-converted frame or vice versa). Fail this frame over to the CPU
    // fallback rather than risk feeding a mismatched format into the MFT.
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  HRESULT hr = device_manager_.device()->OpenSharedResource(
      reinterpret_cast<HANDLE>(
          static_cast<uintptr_t>(gpu_frame->d3d11_handle())),
      IID_PPV_ARGS(texture.GetAddressOf()));
  if (FAILED(hr) || !texture) {
    RTC_LOG(LS_WARNING)
        << "MFT encoder: OpenSharedResource failed, hr=" << hr
        << " (likely a cross-adapter shared handle on a hybrid-GPU system)";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  const bool force_keyframe =
      force_next_keyframe_ ||
      (frame_types && !frame_types->empty() &&
       (*frame_types)[0] == VideoFrameType::kVideoFrameKey);
  force_next_keyframe_ = false;

  const int64_t timestamp_100ns =
      MillisecondsToHns(input_frame.render_time_ms()) + frame_count_;
  ++frame_count_;

  if (!SubmitFrame(texture.Get(), 0, timestamp_100ns, force_keyframe)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (!DrainOutput(input_frame)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

void MftH264EncoderImpl::SetRates(const RateControlParameters& parameters) {
  if (!transform_) {
    return;
  }
  if (parameters.framerate_fps < 1.0 ||
      parameters.bitrate.get_sum_bps() == 0) {
    return;
  }
  target_bps_ = parameters.bitrate.GetSpatialLayerSum(0);
  max_framerate_ = static_cast<uint32_t>(parameters.framerate_fps);

  Microsoft::WRL::ComPtr<ICodecAPI> codec_api;
  if (SUCCEEDED(transform_.As(&codec_api))) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = target_bps_;
    codec_api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
    VariantClear(&var);
  }
}

VideoEncoder::EncoderInfo MftH264EncoderImpl::GetEncoderInfo() const {
  EncoderInfo info;
  info.supports_native_handle = false;
  info.implementation_name = "MFT H264 Encoder";
  info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  info.is_hardware_accelerated = true;
  info.supports_simulcast = false;
  info.preferred_pixel_formats = {VideoFrameBuffer::Type::kNV12,
                                  VideoFrameBuffer::Type::kI420};
  return info;
}

}  // namespace webrtc
