// quest_bench_ncnn.cpp -- time a depth model with ncnn on the Quest, on the
// GPU through Vulkan or on the CPU.
//
//   quest_bench_ncnn MODEL.param MODEL.bin INPUT.bin OUTPUT.bin H W gpu|cpu THREADS RUNS
//
// INPUT.bin is the same float32 3xHxW frame quest_bench takes; the first
// run's depth map goes to OUTPUT.bin for comparison with the PC's. The model
// is a pnnx conversion whose input blob is in0 (w, h, d=3, c=1) and output
// out0. Built with the NDK against ncnn's android-vulkan release.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "net.h"
#include "gpu.h"

static double now_ms()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char **argv)
{
    if (argc != 10) {
        fprintf(stderr, "usage: %s MODEL.param MODEL.bin INPUT.bin OUTPUT.bin H W gpu|cpu THREADS RUNS\n", argv[0]);
        return 2;
    }
    int h = atoi(argv[5]), w = atoi(argv[6]), threads = atoi(argv[8]), runs = atoi(argv[9]);
    bool gpu = !strcmp(argv[7], "gpu");

    ncnn::Mat in(w, h, 3, 1);
    FILE *f = fopen(argv[3], "rb");
    if (!f || fread(in.data, sizeof(float), (size_t)w * h * 3, f) != (size_t)w * h * 3) {
        fprintf(stderr, "cannot read %s\n", argv[3]);
        return 1;
    }
    fclose(f);

    if (gpu) {
        ncnn::create_gpu_instance();
        if (ncnn::get_gpu_count() == 0) { fprintf(stderr, "no Vulkan GPU\n"); return 1; }
        const ncnn::GpuInfo &info = ncnn::get_gpu_info(0);
        printf("gpu %s, fp16 storage %d arithmetic %d\n", info.device_name(),
               info.support_fp16_storage(), info.support_fp16_arithmetic());
    }

    ncnn::Net net;
    net.opt.use_vulkan_compute = gpu;
    net.opt.num_threads = threads;
    // BENCH_FP32=1 keeps everything in float32, to tell precision from speed
    bool fp16 = !getenv("BENCH_FP32");
    net.opt.use_fp16_packed = fp16;
    net.opt.use_fp16_storage = fp16;
    net.opt.use_fp16_arithmetic = fp16;
    double t0 = now_ms();
    if (net.load_param(argv[1]) || net.load_model(argv[2])) { fprintf(stderr, "cannot load model\n"); return 1; }
    printf("load %.0f ms\n", now_ms() - t0);

    std::vector<double> times;
    double first = 0;
    for (int i = 0; i <= runs; i++) {
        ncnn::Extractor ex = net.create_extractor();
        ncnn::Mat out;
        t0 = now_ms();
        ex.input("in0", in.dims == 4 && getenv("BENCH_3D") ? in.reshape(in.w, in.h, 3) : in);
        if (ex.extract("out0", out)) { fprintf(stderr, "extract failed\n"); return 1; }
        double t = now_ms() - t0;
        if (i == 0) {
            first = t;
            f = fopen(argv[4], "wb");
            if (f) { fwrite(out.data, sizeof(float), out.total(), f); fclose(f); }
            printf("output %d x %d x %d\n", out.w, out.h, out.c);
        } else {
            times.push_back(t);
        }
    }
    std::sort(times.begin(), times.end());
    printf("%s %d threads %dx%d: first %.1f ms, median %.1f ms, best %.1f ms over %d runs\n",
           gpu ? "vulkan" : "cpu", threads, w, h, first, times[times.size() / 2], times[0], runs);
    net.clear();
    if (gpu) ncnn::destroy_gpu_instance();
    return 0;
}
