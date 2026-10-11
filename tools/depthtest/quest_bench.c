/*
 * quest_bench.c -- time a depth model with ONNX Runtime on the Quest itself.
 *
 *   quest_bench MODEL.onnx INPUT.bin OUTPUT.bin H W THREADS EP RUNS
 *
 * INPUT.bin holds one float32 1x1x3xHxW frame (tools/depthtest writes it from
 * a game frame); the first run's predicted_depth goes to OUTPUT.bin so it can
 * be compared with the PC's. EP is cpu, xnnpack, nnapi, qnn-htp or qnn-gpu. Prints the first
 * run (which includes the one-off setup) and the median and best of the rest.
 * Built with the NDK against ONNX Runtime's Android package, run over adb.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "onnxruntime_c_api.h"
#ifndef NO_NNAPI           /* the QNN build of ONNX Runtime has no NNAPI */
#include "nnapi_provider_factory.h"
#endif

static const OrtApi *ort;

static void check(OrtStatus *s, const char *what)
{
    if (!s) return;
    fprintf(stderr, "%s: %s\n", what, ort->GetErrorMessage(s));
    exit(1);
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    OrtEnv *env;
    OrtSessionOptions *so;
    OrtSession *session;
    OrtMemoryInfo *mem;
    OrtValue *input = NULL, *output = NULL;
    const char *in_names[] = { "pixel_values" }, *out_names[] = { "predicted_depth" };
    int64_t shape[5];
    size_t n;
    float *data;
    FILE *f;
    int h, w, threads, runs, i;
    const char *ep;
    double t0, first, *times;

    if (argc != 9) {
        fprintf(stderr, "usage: %s MODEL INPUT OUTPUT H W THREADS cpu|xnnpack|nnapi|qnn-htp|qnn-gpu RUNS\n", argv[0]);
        return 2;
    }
    h = atoi(argv[4]); w = atoi(argv[5]); threads = atoi(argv[6]); ep = argv[7]; runs = atoi(argv[8]);
    ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);

    n = (size_t)3 * h * w;
    data = malloc(n * sizeof *data);
    f = fopen(argv[2], "rb");
    if (!f || fread(data, sizeof *data, n, f) != n) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    fclose(f);

    /* BENCH_LOG=1 shows which execution provider each part of the graph got */
    check(ort->CreateEnv(getenv("BENCH_LOG") ? ORT_LOGGING_LEVEL_INFO : ORT_LOGGING_LEVEL_WARNING, "bench", &env), "env");
    check(ort->CreateSessionOptions(&so), "options");
    check(ort->SetIntraOpNumThreads(so, threads), "threads");
    check(ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), "optimisation");
    if (!strcmp(ep, "xnnpack")) {
        char nt[8];
        const char *keys[] = { "intra_op_num_threads" }, *vals[] = { nt };
        snprintf(nt, sizeof nt, "%d", threads);
        check(ort->SessionOptionsAppendExecutionProvider(so, "XNNPACK", keys, vals, 1), "xnnpack");
    } else if (!strcmp(ep, "qnn-htp")) {
        /* Hexagon NPU, float32 run at fp16; needs the onnxruntime-android-qnn
         * build, Qualcomm's QNN libraries beside it and ADSP_LIBRARY_PATH */
        const char *keys[] = { "backend_path", "htp_performance_mode", "enable_htp_fp16_precision" };
        /* QNN_DIR: the directory of the QNN libraries that match this ONNX
         * Runtime; the Quest ships its own, older libQnnHtp.so */
        char path[512];
        const char *vals[] = { path, "burst", "1" };
        snprintf(path, sizeof path, "%s/libQnnHtp.so", getenv("QNN_DIR") ? getenv("QNN_DIR") : ".");
        check(ort->SessionOptionsAppendExecutionProvider(so, "QNN", keys, vals, 3), "qnn-htp");
    } else if (!strcmp(ep, "qnn-gpu")) {
        char path[512];
        const char *keys[] = { "backend_path" }, *vals[] = { path };
        snprintf(path, sizeof path, "%s/libQnnGpu.so", getenv("QNN_DIR") ? getenv("QNN_DIR") : ".");
        check(ort->SessionOptionsAppendExecutionProvider(so, "QNN", keys, vals, 1), "qnn-gpu");
#ifndef NO_NNAPI
    } else if (!strcmp(ep, "nnapi")) {
        check(OrtSessionOptionsAppendExecutionProvider_Nnapi(so, NNAPI_FLAG_USE_FP16), "nnapi");
#endif
    }
    t0 = now_ms();
    check(ort->CreateSession(env, argv[1], so, &session), "session");
    printf("load %.0f ms\n", now_ms() - t0);

    shape[0] = 1; shape[1] = 1; shape[2] = 3; shape[3] = h; shape[4] = w;
    check(ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem), "memory");
    check(ort->CreateTensorWithDataAsOrtValue(mem, data, n * sizeof *data, shape, 5,
                                              ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input), "tensor");

    times = malloc(sizeof *times * (size_t)runs);
    for (i = 0; i <= runs; i++) {
        t0 = now_ms();
        check(ort->Run(session, NULL, in_names, (const OrtValue *const *)&input, 1, out_names, 1, &output), "run");
        if (i == 0) {
            float *y;
            first = now_ms() - t0;
            check(ort->GetTensorMutableData(output, (void **)&y), "output");
            f = fopen(argv[3], "wb");
            if (f) { fwrite(y, sizeof *y, (size_t)h * w, f); fclose(f); }
        } else {
            times[i - 1] = now_ms() - t0;
        }
        ort->ReleaseValue(output);
        output = NULL;
    }
    qsort(times, (size_t)runs, sizeof *times, cmp);
    printf("%s %d threads %dx%d: first %.1f ms, median %.1f ms, best %.1f ms over %d runs\n",
           ep, threads, w, h, first, times[runs / 2], times[0], runs);
    return 0;
}
