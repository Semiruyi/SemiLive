#include <semilive/common/measurement/latency_trace.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace semilive::common::measurement {
namespace {
constexpr std::size_t capacity = 100000;
struct Trace {
    std::string path, run, machine;
    std::int64_t frequency = 0;
    std::uint64_t dropped = 0;
    std::mutex mutex;
    std::vector<LatencyRecord> records;
    Trace() {
#ifdef _WIN32
        const auto* output = std::getenv("SEMILIVE_LATENCY_TRACE");
        const auto* id = std::getenv("SEMILIVE_LATENCY_RUN_ID");
        if (!output || !*output || !id || !*id) return;
        run = id;
        if (run.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
            throw std::runtime_error{"Invalid latency run ID"};
        char host[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD length = sizeof(host);
        LARGE_INTEGER value{};
        if (!GetComputerNameA(host, &length) || !QueryPerformanceFrequency(&value))
            throw std::runtime_error{"Cannot initialize latency clock"};
        machine = host;
        frequency = value.QuadPart;
        records.reserve(capacity);
        std::ofstream probe{output};
        if (!probe) throw std::runtime_error{"Cannot open latency trace"};
        path = output;
#endif
    }
    ~Trace() {
        if (path.empty()) return;
        std::ofstream out{path};
        out << "run_id,machine,clock,frequency,ssrc,rtp_timestamp,frame_id,capture,encode_begin,encode_complete,first_send,last_send_complete,first_receive,last_receive,au_ready,au_bytes,packet_count,key_frame\n";
        for (const auto& r : records)
            out << run << ',' << machine << ",qpc," << frequency << ',' << r.ssrc << ',' << r.rtp_timestamp << ',' << r.frame_id << ','
                << r.timing.capture << ',' << r.timing.encode_begin << ',' << r.timing.encode_complete << ','
                << r.first_send << ',' << r.last_send_complete << ',' << r.first_receive << ',' << r.last_receive << ','
                << r.au_ready << ',' << r.au_bytes << ',' << r.packet_count << ',' << r.key_frame << '\n';
        out.flush();
        std::ofstream meta{path + ".status"};
        meta << "records=" << records.size() << "\ndropped=" << dropped << "\nwrite_ok=" << static_cast<bool>(out) << '\n';
        if (!out || !meta) std::fprintf(stderr, "Failed to write latency trace: %s\n", path.c_str());
    }
};
Trace& trace() { static Trace value; return value; }
}
bool latency_enabled() noexcept {
    try { return !trace().path.empty(); }
    catch (const std::exception& e) { std::fprintf(stderr, "Latency initialization failed: %s\n", e.what()); std::abort(); }
}
std::int64_t latency_ticks() noexcept {
    if (!latency_enabled()) return 0;
#ifdef _WIN32
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
#else
    return 0;
#endif
}
void record_latency(const LatencyRecord& record) noexcept {
    if (!latency_enabled()) return;
    auto& sink = trace();
    std::lock_guard lock{sink.mutex};
    if (sink.records.size() == capacity) { ++sink.dropped; return; }
    sink.records.push_back(record);
}
}
