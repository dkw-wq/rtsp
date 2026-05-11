#include "onnx_scrfd_detector.hpp"

#include "config_loader.hpp"

#include <onnxruntime_cxx_api.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <cstdio>
#include <io.h>
#endif

namespace rtsp {

namespace {

struct TensorView {
    const float* data = nullptr;
    std::vector<int64_t> shape;

    int64_t size() const {
        if (shape.empty()) {
            return 0;
        }

        return std::accumulate(shape.begin(), shape.end(), int64_t{1},
                               [](int64_t lhs, int64_t rhs) {
                                   return lhs * std::max<int64_t>(rhs, 1);
                               });
    }

    int64_t rows(int columns) const {
        if (columns <= 0) {
            return 0;
        }

        const int64_t total = size();
        return total > 0 ? total / columns : 0;
    }
};

struct ScrfdOutputSet {
    TensorView scores;
    TensorView boxes;
    TensorView landmarks;
    int stride = 0;
};

struct CandidateFace {
    FaceBox box;
    float x1 = 0.0F;
    float y1 = 0.0F;
    float x2 = 0.0F;
    float y2 = 0.0F;
};

#ifdef _WIN32
class ScopedStderrSilencer {
public:
    ScopedStderrSilencer() {
        std::fflush(stderr);
        savedFd_ = _dup(_fileno(stderr));
        if (savedFd_ == -1) {
            return;
        }

        FILE* nullStream = nullptr;
        if (freopen_s(&nullStream, "NUL", "w", stderr) != 0) {
            _close(savedFd_);
            savedFd_ = -1;
        }
    }

    ~ScopedStderrSilencer() {
        restore();
    }

    ScopedStderrSilencer(const ScopedStderrSilencer&) = delete;
    ScopedStderrSilencer& operator=(const ScopedStderrSilencer&) = delete;

    void restore() {
        if (savedFd_ == -1) {
            return;
        }

        std::fflush(stderr);
        _dup2(savedFd_, _fileno(stderr));
        _close(savedFd_);
        savedFd_ = -1;
        clearerr(stderr);
    }

private:
    int savedFd_ = -1;
};
#endif

float area(const CandidateFace& face) {
    return std::max(0.0F, face.x2 - face.x1) * std::max(0.0F, face.y2 - face.y1);
}

float intersectionOverUnion(const CandidateFace& a, const CandidateFace& b) {
    const float left = std::max(a.x1, b.x1);
    const float top = std::max(a.y1, b.y1);
    const float right = std::min(a.x2, b.x2);
    const float bottom = std::min(a.y2, b.y2);
    const float intersection =
        std::max(0.0F, right - left) * std::max(0.0F, bottom - top);
    const float combined = area(a) + area(b) - intersection;
    return combined > 0.0F ? intersection / combined : 0.0F;
}

std::vector<CandidateFace> nms(std::vector<CandidateFace> candidates, float threshold) {
    std::sort(candidates.begin(), candidates.end(),
              [](const CandidateFace& lhs, const CandidateFace& rhs) {
                  return lhs.box.score > rhs.box.score;
              });

    std::vector<CandidateFace> kept;
    std::vector<bool> suppressed(candidates.size(), false);
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (suppressed[i]) {
            continue;
        }

        kept.push_back(candidates[i]);
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (!suppressed[j] &&
                intersectionOverUnion(candidates[i], candidates[j]) > threshold) {
                suppressed[j] = true;
            }
        }
    }

    return kept;
}

std::wstring widenPath(const std::string& path) {
    return std::filesystem::path(path).wstring();
}

std::string tensorName(Ort::AllocatorWithDefaultOptions& allocator,
                       const Ort::Session& session,
                       size_t index,
                       bool input) {
    Ort::AllocatedStringPtr name =
        input ? session.GetInputNameAllocated(index, allocator)
              : session.GetOutputNameAllocated(index, allocator);
    return name ? std::string(name.get()) : std::string{};
}

int strideFromName(const std::string& name) {
    for (int stride : {8, 16, 32}) {
        const std::string suffix = std::to_string(stride);
        if (name.find(suffix) != std::string::npos) {
            return stride;
        }
    }
    return 0;
}

bool containsAny(const std::string& value, std::initializer_list<const char*> needles) {
    const std::string lower = toLower(value);
    for (const char* needle : needles) {
        if (lower.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool providerAvailable(const std::string& providerName) {
    try {
        const std::vector<std::string> providers = Ort::GetAvailableProviders();
        return std::find(providers.begin(), providers.end(), providerName) != providers.end();
    } catch (const Ort::Exception& ex) {
        SPDLOG_WARN("Failed to query ONNX Runtime providers: {}", ex.what());
        return false;
    }
}

bool appendCudaProvider(Ort::SessionOptions& sessionOptions) {
    if (!providerAvailable("CUDAExecutionProvider")) {
        SPDLOG_WARN("ONNX Runtime CUDAExecutionProvider is not available; falling back to CPU");
        return false;
    }

    try {
        Ort::CUDAProviderOptions cudaOptions;
        cudaOptions.Update({
            {"device_id", "0"},
            {"arena_extend_strategy", "kNextPowerOfTwo"},
            {"cudnn_conv_algo_search", "EXHAUSTIVE"},
            {"do_copy_in_default_stream", "1"},
        });
        sessionOptions.AppendExecutionProvider_CUDA_V2(*cudaOptions);
        return true;
    } catch (const Ort::Exception& ex) {
        SPDLOG_WARN("Failed to append ONNX Runtime CUDA provider: {}; falling back to CPU",
                    ex.what());
        return false;
    }
}

int clampInt(int value, int minValue, int maxValue) {
    return std::max(minValue, std::min(value, maxValue));
}

float normalizeChannel(int value) {
    return (static_cast<float>(clampInt(value, 0, 255)) - 127.5F) / 128.0F;
}

void nv12ToRgbTensor(const MediaFrame& frame,
                     int inputWidth,
                     int inputHeight,
                     std::vector<float>& inputData) {
    const size_t channelSize = static_cast<size_t>(inputWidth) *
                               static_cast<size_t>(inputHeight);
    const size_t uvPlaneOffset = static_cast<size_t>(frame.width) *
                                 static_cast<size_t>(frame.height);

    for (int y = 0; y < inputHeight; ++y) {
        const int srcY = clampInt((y * frame.height) / inputHeight, 0, frame.height - 1);
        const uint8_t* yRow = frame.data.data() +
                              static_cast<size_t>(srcY) *
                                  static_cast<size_t>(frame.width);
        const uint8_t* uvRow = frame.data.data() + uvPlaneOffset +
                               static_cast<size_t>(srcY / 2) *
                                   static_cast<size_t>(frame.width);

        for (int x = 0; x < inputWidth; ++x) {
            const int srcX = clampInt((x * frame.width) / inputWidth, 0, frame.width - 1);
            const int uvX = srcX & ~1;
            const int yValue = static_cast<int>(yRow[srcX]);
            const int uValue = static_cast<int>(uvRow[uvX]);
            const int vValue = static_cast<int>(uvRow[uvX + 1]);

            const int c = std::max(0, yValue - 16);
            const int d = uValue - 128;
            const int e = vValue - 128;
            const int r = (298 * c + 409 * e + 128) >> 8;
            const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            const int b = (298 * c + 516 * d + 128) >> 8;

            const size_t offset = static_cast<size_t>(y) *
                                  static_cast<size_t>(inputWidth) +
                                  static_cast<size_t>(x);
            inputData[offset] = normalizeChannel(r);
            inputData[channelSize + offset] = normalizeChannel(g);
            inputData[channelSize * 2 + offset] = normalizeChannel(b);
        }
    }
}

} // namespace

class OnnxScrfdDetector::Impl {
public:
    bool initialize(const FaceDetectionOptions& options) {
        options_ = options;
        if (!std::filesystem::exists(options_.modelPath)) {
            SPDLOG_WARN("SCRFD model not found: {}", options_.modelPath);
            return false;
        }
        if (options_.inputWidth <= 0 || options_.inputHeight <= 0) {
            SPDLOG_WARN("Invalid SCRFD input size: {}x{}",
                        options_.inputWidth, options_.inputHeight);
            return false;
        }

        Ort::SessionOptions sessionOptions;
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
        sessionOptions.SetIntraOpNumThreads(1);

        const std::string requestedBackend = toLower(options_.backend);
        const bool wantsCuda = requestedBackend == "onnx_cuda" || requestedBackend == "cuda";
        activeBackendName_ = "onnx_cpu";
        if (wantsCuda && appendCudaProvider(sessionOptions)) {
            activeBackendName_ = "onnx_cuda";
        }

        try {
#ifdef _WIN32
            ScopedStderrSilencer onnxSchemaNoiseSilencer;
#endif
            env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "rtsp_scrfd");
            session_ = std::make_unique<Ort::Session>(
                *env_, widenPath(options_.modelPath).c_str(), sessionOptions);
#ifdef _WIN32
            onnxSchemaNoiseSilencer.restore();
#endif
        } catch (const Ort::Exception& ex) {
            SPDLOG_WARN("Failed to initialize SCRFD ONNX session: {}", ex.what());
            return false;
        }

        Ort::AllocatorWithDefaultOptions allocator;
        inputNames_.clear();
        outputNames_.clear();
        inputNamePtrs_.clear();
        outputNamePtrs_.clear();

        const size_t inputCount = session_->GetInputCount();
        if (inputCount == 0) {
            SPDLOG_WARN("SCRFD model has no inputs");
            return false;
        }
        for (size_t i = 0; i < inputCount; ++i) {
            inputNames_.push_back(tensorName(allocator, *session_, i, true));
        }
        for (const std::string& name : inputNames_) {
            inputNamePtrs_.push_back(name.c_str());
        }

        const size_t outputCount = session_->GetOutputCount();
        if (outputCount < 6) {
            SPDLOG_WARN("SCRFD model output count looks too small: {}", outputCount);
            return false;
        }
        for (size_t i = 0; i < outputCount; ++i) {
            outputNames_.push_back(tensorName(allocator, *session_, i, false));
        }
        for (const std::string& name : outputNames_) {
            outputNamePtrs_.push_back(name.c_str());
        }

        SPDLOG_INFO("SCRFD ONNX initialized: backend={}, model={}, inputs={}, outputs={}",
                    activeBackendName_,
                    options_.modelPath,
                    inputNames_.size(),
                    outputNames_.size());
        return true;
    }

    std::vector<FaceBox> detect(const MediaFrame& frame) {
        if (!session_ || frame.pixelFormat != MediaFrame::PixelFormat::NV12 ||
            frame.width <= 0 || frame.height <= 0) {
            return {};
        }

        const size_t ySize = static_cast<size_t>(frame.width) *
                             static_cast<size_t>(frame.height);
        if (frame.data.size() < ySize * 3 / 2) {
            return {};
        }

        std::vector<float> inputData(static_cast<size_t>(3) *
                                     static_cast<size_t>(options_.inputHeight) *
                                     static_cast<size_t>(options_.inputWidth));
        nv12ToRgbTensor(frame, options_.inputWidth, options_.inputHeight, inputData);

        std::array<int64_t, 4> inputShape = {
            1, 3, options_.inputHeight, options_.inputWidth
        };
        Ort::MemoryInfo memoryInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memoryInfo,
            inputData.data(),
            inputData.size(),
            inputShape.data(),
            inputShape.size());

        auto outputs = session_->Run(Ort::RunOptions{nullptr},
                                     inputNamePtrs_.data(),
                                     &inputTensor,
                                     1,
                                     outputNamePtrs_.data(),
                                     outputNamePtrs_.size());

        std::vector<TensorView> views;
        views.reserve(outputs.size());
        for (Ort::Value& output : outputs) {
            TensorView view;
            view.data = output.GetTensorData<float>();
            view.shape = output.GetTensorTypeAndShapeInfo().GetShape();
            views.push_back(std::move(view));
        }

        std::vector<ScrfdOutputSet> outputSets = groupOutputs(views);
        std::vector<CandidateFace> candidates;
        for (const ScrfdOutputSet& outputSet : outputSets) {
            decodeOutputSet(outputSet, frame.width, frame.height, candidates);
        }

        std::vector<CandidateFace> kept = nms(std::move(candidates), options_.nmsThreshold);
        std::vector<FaceBox> faces;
        faces.reserve(kept.size());
        for (const CandidateFace& face : kept) {
            faces.push_back(face.box);
        }
        return faces;
    }

    std::string backendName() const {
        return activeBackendName_;
    }

private:
    std::vector<ScrfdOutputSet> groupOutputs(const std::vector<TensorView>& views) const {
        std::array<ScrfdOutputSet, 3> sets{};
        const std::array<int, 3> fallbackStrides = {8, 16, 32};
        for (size_t i = 0; i < sets.size(); ++i) {
            sets[i].stride = fallbackStrides[i];
        }

        bool named = false;
        for (size_t i = 0; i < views.size() && i < outputNames_.size(); ++i) {
            const std::string& name = outputNames_[i];
            const int stride = strideFromName(name);
            if (stride == 0) {
                continue;
            }

            auto it = std::find(fallbackStrides.begin(), fallbackStrides.end(), stride);
            if (it == fallbackStrides.end()) {
                continue;
            }

            ScrfdOutputSet& set = sets[static_cast<size_t>(it - fallbackStrides.begin())];
            if (containsAny(name, {"score", "cls", "conf"})) {
                set.scores = views[i];
                named = true;
            } else if (containsAny(name, {"bbox", "box"})) {
                set.boxes = views[i];
                named = true;
            } else if (containsAny(name, {"kps", "landmark"})) {
                set.landmarks = views[i];
                named = true;
            }
        }

        if (!named && views.size() >= 6) {
            for (size_t i = 0; i < 3; ++i) {
                sets[i].scores = views[i];
                sets[i].boxes = views[i + 3];
                if (views.size() >= 9) {
                    sets[i].landmarks = views[i + 6];
                }
            }
        }

        std::vector<ScrfdOutputSet> result;
        for (const ScrfdOutputSet& set : sets) {
            if (set.scores.data != nullptr && set.boxes.data != nullptr) {
                result.push_back(set);
            }
        }
        return result;
    }

    void decodeOutputSet(const ScrfdOutputSet& set,
                         int frameWidth,
                         int frameHeight,
                         std::vector<CandidateFace>& candidates) const {
        const int featureWidth =
            static_cast<int>(std::ceil(static_cast<float>(options_.inputWidth) /
                                       static_cast<float>(set.stride)));
        const int featureHeight =
            static_cast<int>(std::ceil(static_cast<float>(options_.inputHeight) /
                                       static_cast<float>(set.stride)));
        const int pointCount = featureWidth * featureHeight;
        if (pointCount <= 0) {
            return;
        }

        const int scoreColumns = set.scores.shape.empty() ? 1 :
            static_cast<int>(std::max<int64_t>(set.scores.shape.back(), 1));
        const int boxColumns = set.boxes.shape.empty() ? 4 :
            static_cast<int>(std::max<int64_t>(set.boxes.shape.back(), 4));
        const int landmarkColumns =
            set.landmarks.data == nullptr || set.landmarks.shape.empty()
                ? 0
                : static_cast<int>(std::max<int64_t>(set.landmarks.shape.back(), 0));
        const int rows = static_cast<int>(std::min(set.scores.rows(scoreColumns),
                                                   set.boxes.rows(boxColumns)));
        if (rows <= 0) {
            return;
        }

        const int anchorsPerPoint = std::max(1, rows / pointCount);
        const float scaleX = static_cast<float>(frameWidth) /
                             static_cast<float>(options_.inputWidth);
        const float scaleY = static_cast<float>(frameHeight) /
                             static_cast<float>(options_.inputHeight);

        for (int row = 0; row < rows; ++row) {
            float score = 0.0F;
            if (scoreColumns == 1) {
                score = set.scores.data[row];
            } else {
                score = set.scores.data[row * scoreColumns + scoreColumns - 1];
            }
            if (score < options_.scoreThreshold) {
                continue;
            }

            const int pointIndex = row / anchorsPerPoint;
            const int yIndex = pointIndex / featureWidth;
            const int xIndex = pointIndex % featureWidth;
            const float anchorX = (static_cast<float>(xIndex) + 0.5F) *
                                  static_cast<float>(set.stride);
            const float anchorY = (static_cast<float>(yIndex) + 0.5F) *
                                  static_cast<float>(set.stride);

            const float* box = set.boxes.data + row * boxColumns;
            float x1 = (anchorX - box[0] * static_cast<float>(set.stride)) * scaleX;
            float y1 = (anchorY - box[1] * static_cast<float>(set.stride)) * scaleY;
            float x2 = (anchorX + box[2] * static_cast<float>(set.stride)) * scaleX;
            float y2 = (anchorY + box[3] * static_cast<float>(set.stride)) * scaleY;

            x1 = std::clamp(x1, 0.0F, static_cast<float>(frameWidth - 1));
            y1 = std::clamp(y1, 0.0F, static_cast<float>(frameHeight - 1));
            x2 = std::clamp(x2, 0.0F, static_cast<float>(frameWidth - 1));
            y2 = std::clamp(y2, 0.0F, static_cast<float>(frameHeight - 1));
            if (x2 <= x1 || y2 <= y1) {
                continue;
            }

            CandidateFace candidate;
            candidate.x1 = x1;
            candidate.y1 = y1;
            candidate.x2 = x2;
            candidate.y2 = y2;
            candidate.box.x = x1;
            candidate.box.y = y1;
            candidate.box.width = x2 - x1;
            candidate.box.height = y2 - y1;
            candidate.box.score = score;

            if (set.landmarks.data != nullptr && landmarkColumns >= 10) {
                const float* landmarks = set.landmarks.data + row * landmarkColumns;
                for (int i = 0; i < 5; ++i) {
                    candidate.box.landmarks[i * 2] =
                        (anchorX + landmarks[i * 2] * static_cast<float>(set.stride)) * scaleX;
                    candidate.box.landmarks[i * 2 + 1] =
                        (anchorY + landmarks[i * 2 + 1] * static_cast<float>(set.stride)) * scaleY;
                }
            }

            candidates.push_back(candidate);
        }
    }

    FaceDetectionOptions options_;
    std::string activeBackendName_ = "onnx_cpu";
    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    std::vector<std::string> inputNames_;
    std::vector<std::string> outputNames_;
    std::vector<const char*> inputNamePtrs_;
    std::vector<const char*> outputNamePtrs_;
};

OnnxScrfdDetector::OnnxScrfdDetector()
    : pImpl_(std::make_unique<Impl>()) {}

OnnxScrfdDetector::~OnnxScrfdDetector() = default;

bool OnnxScrfdDetector::initialize(const FaceDetectionOptions& options) {
    return pImpl_->initialize(options);
}

std::vector<FaceBox> OnnxScrfdDetector::detect(const MediaFrame& frame) {
    return pImpl_->detect(frame);
}

std::string OnnxScrfdDetector::backendName() const {
    return pImpl_->backendName();
}

} // namespace rtsp
