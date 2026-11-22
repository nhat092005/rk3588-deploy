#include "preprocess_result.h"

#include "json_writer.h"

namespace {
constexpr int schemaVersion{2};

void writeFreq(JsonWriter& json, const FreqState& state) {
    json.beginObject();
    json.key("locked");
    json.boolean(state.locked);
    json.key("entries");
    json.beginArray();
    for (const FreqEntry& entry : state.entries) {
        json.beginObject();
        json.key("name");
        json.text(entry.name);
        json.key("governor");
        json.text(entry.governor);
        json.key("frequency");
        json.text(entry.frequency);
        json.endObject();
    }
    json.endArray();
    json.endObject();
}

void writeMethod(JsonWriter& json, const MethodResult& result) {
    json.beginObject();
    json.key("label");
    json.text(methodLabel(result.kind));
    json.key("status");
    json.text(statusName(result.status));
    if (result.status != MethodStatus::Ok) {
        json.key("message");
        json.text(result.message);
        json.endObject();
        return;
    }
    json.key("threads");
    json.integer(result.timing.threads);
    json.key("p50_ms");
    json.number(result.timing.latency.p50);
    json.key("p95_ms");
    json.number(result.timing.latency.p95);
    json.key("p99_ms");
    json.number(result.timing.latency.p99);
    json.key("cpu_ms_per_frame");
    json.number(result.timing.cpuMsPerFrame);
    json.key("system_cpu_percent");
    json.number(result.timing.systemCpuPercent);
    if (result.timing.pacing) {
        json.key("achieved_fps");
        json.number(result.timing.pacing->achievedFps);
        json.key("frames");
        json.integer(result.timing.pacing->frames);
        json.key("late_frames");
        json.integer(result.timing.pacing->lateFrames);
    }
    if (result.timing.npu) {
        json.key("npu_runs");
        json.integer(result.timing.npu->runs);
        json.key("npu_p50_ms");
        json.number(result.timing.npu->p50Ms);
    }
    if (result.difference) {
        json.key("diff_mean");
        json.number(result.difference->mean);
        json.key("diff_max");
        json.number(result.difference->largest);
        json.key("diff_over_16_percent");
        json.number(result.difference->bigShare);
    }
    json.endObject();
}

void writePoint(JsonWriter& json, const PointResult& point) {
    json.beginObject();
    json.key("source_width");
    json.integer(point.source.width);
    json.key("source_height");
    json.integer(point.source.height);
    json.key("target");
    json.integer(point.target);
    if (point.fps) {
        json.key("fps");
        json.integer(*point.fps);
    }
    if (!point.error.empty()) {
        json.key("error");
        json.text(point.error);
    }
    json.key("methods");
    json.beginArray();
    for (const MethodResult& result : point.methods) {
        writeMethod(json, result);
    }
    json.endArray();
    json.endObject();
}
} // namespace

std::string resultJson(const RunInfo& info, const std::vector<PointResult>& points) {
    JsonWriter json;
    json.beginObject();
    json.key("schema");
    json.integer(schemaVersion);
    json.key("stage");
    json.text("preprocess");
    json.key("source_range");
    json.text("full");
    json.key("sweep");
    json.text(info.sweep);
    json.key("pattern");
    json.text(info.pattern);
    json.key("heap");
    json.text(info.heap);
    json.key("background");
    json.text(info.background);
    if (!info.npuModel.empty()) {
        json.key("npu_model");
        json.text(info.npuModel);
        json.key("npu_cores");
        json.text(info.npuCores);
        if (info.npuAlone) {
            json.key("npu_alone_runs");
            json.integer(info.npuAlone->runs);
            json.key("npu_alone_p50_ms");
            json.number(info.npuAlone->p50Ms);
        }
    }
    json.key("runs");
    json.integer(info.runs);
    json.key("warmup");
    json.integer(info.warmup);
    if (info.sweep == "fps") {
        json.key("seconds");
        json.integer(info.seconds);
    }
    json.key("opencv");
    json.text(info.opencv);
    json.key("opencv_build");
    json.beginArray();
    for (const std::string& line : info.build) {
        json.text(line);
    }
    json.endArray();
    json.key("frequency_before");
    writeFreq(json, info.before);
    json.key("frequency_after");
    writeFreq(json, info.after);
    json.key("points");
    json.beginArray();
    for (const PointResult& point : points) {
        writePoint(json, point);
    }
    json.endArray();
    json.endObject();
    return json.str();
}
