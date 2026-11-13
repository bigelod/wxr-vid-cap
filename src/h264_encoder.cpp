#include "h264_encoder.h"

#include <cstring>

#include "codec_api.h"

namespace {

std::wstring EncoderDllPath()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0) {
        return L"openh264.dll";
    }
    std::wstring result(path, length);
    const std::size_t slash = result.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L"openh264.dll";
    }
    return result.substr(0, slash) + L"\\openh264.dll";
}

}  // namespace

H264Encoder::~H264Encoder()
{
    Shutdown();
}

bool H264Encoder::Initialize(int width, int height, int fps, int bitrateKbps, std::wstring& error)
{
    Shutdown();

    if (width <= 0 || height <= 0 || fps <= 0 || bitrateKbps <= 0) {
        error = L"Invalid encoder parameters.";
        return false;
    }

    module_ = LoadLibraryW(EncoderDllPath().c_str());
    if (module_ == nullptr) {
        module_ = LoadLibraryW(L"openh264.dll");
    }
    if (module_ == nullptr) {
        error = L"openh264.dll was not found next to the EXE (error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }

    create_ = reinterpret_cast<CreateEncoderFn>(GetProcAddress(module_, "WelsCreateSVCEncoder"));
    destroy_ = reinterpret_cast<DestroyEncoderFn>(GetProcAddress(module_, "WelsDestroySVCEncoder"));
    if (create_ == nullptr || destroy_ == nullptr) {
        error = L"openh264.dll does not export the expected encoder entry points.";
        Shutdown();
        return false;
    }

    if (create_(&encoder_) != 0 || encoder_ == nullptr) {
        error = L"WelsCreateSVCEncoder failed.";
        Shutdown();
        return false;
    }

    SEncParamExt parameters = {};
    if (encoder_->GetDefaultParams(&parameters) != 0) {
        error = L"Could not read OpenH264 default parameters.";
        Shutdown();
        return false;
    }

    parameters.iUsageType = CAMERA_VIDEO_REAL_TIME;
    parameters.iPicWidth = width;
    parameters.iPicHeight = height;
    parameters.iTargetBitrate = bitrateKbps * 1000;
    parameters.iRCMode = RC_BITRATE_MODE;
    parameters.fMaxFrameRate = static_cast<float>(fps);
    parameters.iTemporalLayerNum = 1;
    parameters.iSpatialLayerNum = 1;
    parameters.uiIntraPeriod = static_cast<unsigned int>(fps) * 2;
    parameters.iComplexityMode = LOW_COMPLEXITY;
    parameters.iMultipleThreadIdc = 1;
    parameters.bEnableFrameSkip = false;
    parameters.bEnableAdaptiveQuant = false;
    parameters.bEnableSceneChangeDetect = true;
    parameters.iEntropyCodingModeFlag = 0;  // CAVLC, baseline-compatible
    parameters.bPrefixNalAddingCtrl = false;
    parameters.bEnableLongTermReference = false;

    parameters.sSpatialLayers[0].iVideoWidth = width;
    parameters.sSpatialLayers[0].iVideoHeight = height;
    parameters.sSpatialLayers[0].fFrameRate = static_cast<float>(fps);
    parameters.sSpatialLayers[0].iSpatialBitrate = parameters.iTargetBitrate;
    parameters.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;

    const int initialized = encoder_->InitializeExt(&parameters);
    if (initialized != 0) {
        error = L"OpenH264 InitializeExt failed with code " + std::to_wstring(initialized) + L".";
        Shutdown();
        return false;
    }

    int traceLevel = WELS_LOG_QUIET;
    encoder_->SetOption(ENCODER_OPTION_TRACE_LEVEL, &traceLevel);

    width_ = width;
    height_ = height;
    return true;
}

bool H264Encoder::Encode(const std::vector<std::uint8_t>& i420, std::int64_t pts90k, bool& keyframe,
                         std::vector<std::uint8_t>& annexB, std::wstring& error)
{
    keyframe = false;
    annexB.clear();

    if (encoder_ == nullptr) {
        error = L"The encoder is not initialized.";
        return false;
    }

    const std::size_t lumaSize = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    const std::size_t chromaSize = static_cast<std::size_t>(width_ / 2) * static_cast<std::size_t>(height_ / 2);
    if (i420.size() != lumaSize + chromaSize * 2) {
        error = L"Frame size does not match the encoder configuration.";
        return false;
    }

    SSourcePicture picture = {};
    picture.iColorFormat = videoFormatI420;
    picture.iPicWidth = width_;
    picture.iPicHeight = height_;
    picture.iStride[0] = width_;
    picture.iStride[1] = width_ / 2;
    picture.iStride[2] = width_ / 2;
    picture.uiTimeStamp = static_cast<long long>(pts90k / 90);  // milliseconds
    picture.pData[0] = const_cast<std::uint8_t*>(i420.data());
    picture.pData[1] = const_cast<std::uint8_t*>(i420.data() + lumaSize);
    picture.pData[2] = const_cast<std::uint8_t*>(i420.data() + lumaSize + chromaSize);

    SFrameBSInfo info = {};
    const int result = encoder_->EncodeFrame(&picture, &info);
    if (result != 0) {
        error = L"OpenH264 EncodeFrame failed with code " + std::to_wstring(result) + L".";
        return false;
    }

    if (info.eFrameType == videoFrameTypeSkip) {
        return true;
    }

    if (info.iLayerNum <= 0 || info.iFrameSizeInBytes <= 0) {
        return true;
    }

    keyframe = (info.eFrameType == videoFrameTypeIDR);
    const SLayerBSInfo& layer = info.sLayerInfo[0];
    annexB.assign(layer.pBsBuf, layer.pBsBuf + info.iFrameSizeInBytes);
    return true;
}

void H264Encoder::Shutdown()
{
    if (encoder_ != nullptr) {
        encoder_->Uninitialize();
        if (destroy_ != nullptr) {
            destroy_(encoder_);
        }
        encoder_ = nullptr;
    }
    create_ = nullptr;
    destroy_ = nullptr;
    if (module_ != nullptr) {
        FreeLibrary(module_);
        module_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
}
