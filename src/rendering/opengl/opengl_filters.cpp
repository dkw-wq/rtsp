#include "opengl_video_renderer.hpp"
#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>
namespace rtsp::rendering::opengl {
ShaderFilter parseFilterMode(std::string filterName) {
    std::transform(filterName.begin(), filterName.end(), filterName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (filterName == "none" || filterName == "off" || filterName == "normal") {
        return ShaderFilter::None;
    }
    if (filterName == "grayscale" || filterName == "gray" || filterName == "mono") {
        return ShaderFilter::Grayscale;
    }
    if (filterName == "warm") {
        return ShaderFilter::Warm;
    }
    if (filterName == "invert" || filterName == "negative") {
        return ShaderFilter::Invert;
    }
    if (filterName == "contrast") {
        return ShaderFilter::Contrast;
    }
    if (filterName == "saturation" || filterName == "saturate") {
        return ShaderFilter::Saturation;
    }
    SPDLOG_WARN("Unknown OpenGL filter '{}', using none", filterName);
    return ShaderFilter::None;
}
const char* filterName(ShaderFilter filterMode) {
    switch (filterMode) {
        case ShaderFilter::Grayscale:
            return "grayscale";
        case ShaderFilter::Warm:
            return "warm";
        case ShaderFilter::Invert:
            return "invert";
        case ShaderFilter::Contrast:
            return "contrast";
        case ShaderFilter::Saturation:
            return "saturation";
        default:
            return "none";
    }
}
ShaderFilterPipeline ShaderFilterPipeline::fromNames(const std::vector<std::string>& filterNames) {
    ShaderFilterPipeline pipeline;
    for (const std::string& name : filterNames) {
        if (pipeline.count >= pipeline.filters.size()) {
            SPDLOG_WARN("Ignoring OpenGL filter '{}' because the shader pipeline is full", name);
            continue;
        }
        const ShaderFilter filter = parseFilterMode(name);
        if (filter == ShaderFilter::None) {
            continue;
        }
        pipeline.filters[pipeline.count] = filter;
        ++pipeline.count;
    }
    return pipeline;
}
ShaderFilterPipeline ShaderFilterPipeline::single(ShaderFilter filter) {
    ShaderFilterPipeline pipeline;
    if (filter != ShaderFilter::None) {
        pipeline.filters[0] = filter;
        pipeline.count = 1;
    }
    return pipeline;
}
void ShaderFilterPipeline::setPreviewFilter(ShaderFilter filter) {
    *this = single(filter);
}
std::array<GLint, kMaxFilterStages> ShaderFilterPipeline::toUniformModes() const {
    std::array<GLint, kMaxFilterStages> values{};
    for (size_t i = 0; i < count; ++i) {
        values[i] = static_cast<GLint>(filters[i]);
    }
    return values;
}
std::string ShaderFilterPipeline::describe() const {
    if (count == 0) {
        return "none";
    }
    std::string result;
    for (size_t i = 0; i < count; ++i) {
        if (!result.empty()) {
            result += " -> ";
        }
        result += filterName(filters[i]);
    }
    return result;
}
} // namespace rtsp::rendering::opengl

