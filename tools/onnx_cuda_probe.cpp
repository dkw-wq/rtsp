#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <cstdio>
#include <io.h>
#endif

namespace {

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

void printProviders(const std::vector<std::string>& providers) {
    std::cout << "available_providers=";
    for (size_t i = 0; i < providers.size(); ++i) {
        if (i != 0) {
            std::cout << ",";
        }
        std::cout << providers[i];
    }
    std::cout << '\n';
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path modelPath =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::path("models/det_500m.onnx");

    try {
        const std::vector<std::string> providers = Ort::GetAvailableProviders();
        printProviders(providers);

        if (std::find(providers.begin(), providers.end(), "CUDAExecutionProvider") ==
            providers.end()) {
            std::cerr << "CUDAExecutionProvider is not available\n";
            return 2;
        }

        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "onnx_cuda_probe");
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
        sessionOptions.SetIntraOpNumThreads(1);

        Ort::CUDAProviderOptions cudaOptions;
        cudaOptions.Update({
            {"device_id", "0"},
            {"arena_extend_strategy", "kNextPowerOfTwo"},
            {"cudnn_conv_algo_search", "EXHAUSTIVE"},
            {"do_copy_in_default_stream", "1"},
        });
        sessionOptions.AppendExecutionProvider_CUDA_V2(*cudaOptions);

#ifdef _WIN32
        ScopedStderrSilencer onnxSchemaNoiseSilencer;
#endif

#ifdef _WIN32
        const std::wstring wideModelPath = modelPath.wstring();
        Ort::Session session(env, wideModelPath.c_str(), sessionOptions);
#else
        Ort::Session session(env, modelPath.string().c_str(), sessionOptions);
#endif

#ifdef _WIN32
        onnxSchemaNoiseSilencer.restore();
#endif

        std::cout << "cuda_session=ok\n";
        std::cout << "model=" << modelPath.string() << '\n';
        return 0;
    } catch (const Ort::Exception& ex) {
        std::cerr << "onnxruntime_error=" << ex.what() << '\n';
        return 3;
    } catch (const std::exception& ex) {
        std::cerr << "error=" << ex.what() << '\n';
        return 4;
    }
}
