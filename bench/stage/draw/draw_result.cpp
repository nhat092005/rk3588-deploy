#include "draw_result.h"

#include "json_writer.h"

namespace {
constexpr int schemaVersion{1};

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

void writeMethod(JsonWriter& json, const DrawMethodResult& result) {
    json.beginObject();
    json.key("label");
    json.text(drawLabel(result.kind));
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
    if (result.difference) {
        json.key("diff_bytes");
        json.integer(result.difference->differing);
        json.key("diff_max");
        json.integer(result.difference->largest);
        json.key("diff_over_16_bytes");
        json.integer(result.difference->overSixteen);
    }
    json.endObject();
}

void writePoint(JsonWriter& json, const DrawPointResult& point) {
    json.beginObject();
    json.key("width");
    json.integer(point.size.width);
    json.key("height");
    json.integer(point.size.height);
    json.key("boxes");
    json.integer(point.boxes);
    if (!point.error.empty()) {
        json.key("error");
        json.text(point.error);
    }
    json.key("methods");
    json.beginArray();
    for (const DrawMethodResult& result : point.methods) {
        writeMethod(json, result);
    }
    json.endArray();
    json.endObject();
}
} // namespace

std::string drawResultJson(const DrawRunInfo& info, const std::vector<DrawPointResult>& points) {
    JsonWriter json;
    json.beginObject();
    json.key("schema");
    json.integer(schemaVersion);
    json.key("stage");
    json.text("draw");
    json.key("sweep");
    json.text(info.sweep);
    json.key("pattern");
    json.text(info.pattern);
    json.key("heap");
    json.text(info.heap);
    json.key("frame");
    json.text(info.frame);
    json.key("runs");
    json.integer(info.runs);
    json.key("warmup");
    json.integer(info.warmup);
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
    for (const DrawPointResult& point : points) {
        writePoint(json, point);
    }
    json.endArray();
    json.endObject();
    return json.str();
}
