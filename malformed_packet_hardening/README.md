# Opus 畸形包解码加固

目标：截断包、非法帧长、损坏 TOC 和损坏 padding 只能返回错误码，不崩溃、
不越界；合法包的 PCM 输出逐样本不变。

## 复现与验证

```sh
cmake -S . -B build-asan \
  -DOPUS_BUILD_TESTING=ON -DOPUS_ASSERTIONS=ON -DOPUS_HARDENING=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -O1 -g' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-asan -j4
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
  ./build-asan/test_opus_malformed 1000000
```

固定点构建将 `-DOPUS_FIXED_POINT=ON -DOPUS_ENABLE_FLOAT_API=OFF` 加到
`cmake` 参数后再执行相同测试。

## 样本

`samples.txt` 中每行是 `名称 十六进制字节`。用
`printf '%s' '<hex>' | xxd -r -p > name.opus` 可还原为二进制包。

| 样本 | 预期 |
| --- | --- |
| `truncated_toc` | `0x03` 后缺少帧数字节，`OPUS_INVALID_PACKET` |
| `zero_frame_count` | code 3 的帧数为 0，`OPUS_INVALID_PACKET` |
| `excessive_duration` | 24 个 60 ms 帧超过 120 ms，`OPUS_INVALID_PACKET` |
| `truncated_vbr_size` | VBR 长度码缺少第二个字节，`OPUS_INVALID_PACKET` |
| `vbr_size_past_end` | 第一帧长度超过剩余数据，`OPUS_INVALID_PACKET` |
| `padding_past_end` | padding 计数超过剩余数据，`OPUS_INVALID_PACKET` |
| `frame_past_limit` | 单帧 1276 字节，超过 1275 字节上限，`OPUS_INVALID_PACKET` |
| `valid_silk_8k_mono_20ms` | 合法 SILK 包，解码 160 样本 |
| `valid_celt_48k_stereo_10ms` | 合法 CELT 包，解码 480 样本/声道 |

合法样本的黄金 PCM 在
`tests/test_opus_malformed_samples.h`（浮点）和
`tests/test_opus_malformed_samples_fixed.h`（定点）/
`tests/test_opus_malformed_samples_fixed_res24.h`（定点 RES24）中，测试
逐样本 `memcmp`，不是仅比较长度或哈希。

## 根因与修复

1. `opus_packet_has_lbrr()` 在检查 `len` 前读取 `packet[0]`。对 `len==0`、
   负长度或 `packet==NULL` 会越界/空指针。现在先检查 `packet` 和 `len`，
   不足一个 TOC 字节返回 `OPUS_BAD_ARG`。
2. `opus_packet_get_samples_per_frame()` 对非法 `Fs` 和超长采样率使用
   `int` 左移/乘法，可能发生有符号溢出。现在检查指针与 `Fs>0`，采样数
   使用 `opus_int64` 计算。
3. `opus_packet_get_nb_samples()` 的 `count*samples` 和 120 ms 检查改用
   `opus_int64`，并拒绝 0 帧、非正采样率和超过 `INT_MAX` 的结果。
4. `opus_packet_parse_impl()` 入口拒绝空数据，错误时将偏移输出清零；
   在返回成功前用原始包长重新验证每个 `size[i]`（`0..1275`）、帧指针推进量、
   padding 长度和 `packet_offset`，避免任何内部累计误差或将来修改造成越界。
5. 定点 PLC 的 `old_overlap` 展开路径先做 32 位 `SUB32/ADD32` 再
   `SATURATE`，畸形 CELT 包可触发有符号溢出 UB。改为 64 位中间值并做
   `[-SIG_SAT, SIG_SAT]` 饱和，合法 PLC 输出不变。

## 统计

见 `results.txt`。摘要：

- 浮点 ASan/UBSan：464 个截断/TOC 用例 + 1,000,000 个随机变异包，
  `crashes=0`、`oob=0`。
- 定点 ASan/UBSan：463 个截断/TOC 用例 + 1,000,000 个随机变异包，
  `crashes=0`、`oob=0`。
- 完整 ASan/UBSan CTest：6/6 通过。
- 合法样本输出与原始提交 `0145a624` 的浮点、定点解码结果逐字节一致。
