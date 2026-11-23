// probe_attrs.cc —— 只读探针: 打印 RKNN 模型的输入/输出属性
//
// 用途(Phase 1 审查): 量化"want_float=1 到底让驱动多写多少字节 / 多做多少次
// 反量化", 为"切 int8 直读"的收益评估提供硬数据。
//
// 用法: probe_attrs <model.rknn>
#include <cstdio>
#include <cstring>
#include <vector>
#include <rknn_api.h>

static const char* qnt_name(rknn_tensor_qnt_type t) {
    switch (t) {
        case RKNN_TENSOR_QNT_NONE:      return "NONE";
        case RKNN_TENSOR_QNT_DFP:       return "DFP";
        case RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC: return "AFFINE_ASYM";
        default: return "?";
    }
}

static const char* type_name(rknn_tensor_type t) {
    switch (t) {
        case RKNN_TENSOR_FLOAT32: return "FLOAT32";
        case RKNN_TENSOR_FLOAT16: return "FLOAT16";
        case RKNN_TENSOR_INT8:    return "INT8";
        case RKNN_TENSOR_UINT8:   return "UINT8";
        case RKNN_TENSOR_INT16:   return "INT16";
        default: return "?";
    }
}

static const char* fmt_name(rknn_tensor_format f) {
    switch (f) {
        case RKNN_TENSOR_NCHW: return "NCHW";
        case RKNN_TENSOR_NHWC: return "NHWC";
        default: return "?";
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "用法: probe_attrs <model.rknn>\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data((size_t)sz);
    if (fread(data.data(), 1, data.size(), f) != data.size()) { fclose(f); return 2; }
    fclose(f);

    rknn_context ctx = 0;
    if (rknn_init(&ctx, data.data(), (uint32_t)data.size(), RKNN_FLAG_PRIOR_HIGH, nullptr) != 0) {
        fprintf(stderr, "rknn_init 失败\n");
        return 1;
    }
    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    printf("模型: %s\n", argv[1]);
    printf("输入数=%u 输出数=%u\n", io_num.n_input, io_num.n_output);

    for (uint32_t i = 0; i < io_num.n_input && i < 4; ++i) {
        rknn_tensor_attr a; memset(&a, 0, sizeof(a));
        a.index = i;
        rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &a, sizeof(a));
        printf("  IN[%u] type=%s fmt=%s n_dims=%u dims=[", i, type_name(a.type), fmt_name(a.fmt), a.n_dims);
        for (uint32_t k = 0; k < a.n_dims && k < 6; ++k) printf("%u%s", a.dims[k], k + 1 < a.n_dims ? "," : "");
        printf("] n_elems=%u size=%u qnt=%s zp=%d scale=%.10g\n",
               a.n_elems, a.size, qnt_name(a.qnt_type), a.zp, a.scale);
    }

    unsigned long long tot_float = 0, tot_int8 = 0;
    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        rknn_tensor_attr a; memset(&a, 0, sizeof(a));
        a.index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a));
        printf("  OUT[%u] name=%s type=%s fmt=%s n_dims=%u dims=[", i,
               a.name ? a.name : "(null)", type_name(a.type), fmt_name(a.fmt), a.n_dims);
        for (uint32_t k = 0; k < a.n_dims && k < 6; ++k) printf("%u%s", a.dims[k], k + 1 < a.n_dims ? "," : "");
        printf("] n_elems=%u size=%u qnt=%s zp=%d scale=%.10g\n",
               a.n_elems, a.size, qnt_name(a.qnt_type), a.zp, a.scale);
        if (a.type == RKNN_TENSOR_INT8) {
            tot_int8  += a.n_elems;
            tot_float += (unsigned long long)a.n_elems * 4;
        } else {
            tot_float += a.size;
            tot_int8  += a.size;
        }
    }
    printf(">> 全部输出 want_float=1 时驱动要写的字节 = %llu ; int8 直读时 = %llu ; 多余 = %llu (%.1f KB)\n",
           tot_float, tot_int8, tot_float - tot_int8, (tot_float - tot_int8) / 1024.0);
    rknn_destroy(ctx);
    return 0;
}
