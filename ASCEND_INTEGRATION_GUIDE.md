# Huawei Atlas 300V Pro Integration Guide

This document describes how to enable and use the Huawei Atlas 300V Pro TPU hardware acceleration for video encoding/decoding and YOLOv8 inference in the BXC Video Analyzer.

## Architecture Overview

The integration follows a **Manager-Worker** pattern with the following components:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              Scheduler (Manager)                              │
│  - Registry of active RTSP streams                                           │
│  - Spawns/removes Workers for streams                                        │
│  - Manages DetectorPool (now supports Ascend backend)                        │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
              ┌───────────────────────┼───────────────────────┐
              ▼                       ▼                       ▼
┌─────────────────────┐ ┌─────────────────────┐ ┌─────────────────────┐
│    Worker (CPU)     │ │  Worker (Hardware)  │ │    Worker (N)       │
│  FFmpeg + ONNX RT   │ │  VDEC + ACL NPU     │ │                     │
└─────────────────────┘ └─────────────────────┘ └─────────────────────┘
                                      │
                      ┌───────────────┼───────────────┐
                      ▼               ▼               ▼
              ┌─────────────┐ ┌─────────────┐ ┌─────────────┐
              │   Model A   │ │   Model B   │ │   Model C   │
              │ (YOLOv8n)   │ │ (custom)    │ │ (custom)    │
              └─────────────┘ └─────────────┘ └─────────────┘
```

## File Structure

New files added in `Analyzer/Core/hw_ascend/`:

| File | Description |
|------|-------------|
| `ascend_common.h` | Common types and error codes |
| `decoder_interface.h/cpp` | Abstract decoder interface + FFmpeg implementation |
| `inference_interface.h/cpp` | Abstract inference interface + ONNX adapter |
| `ascend_pipeline.h/cpp` | MindX SDK integration (VDEC, VPC, ACL) |
| `ascend_decoder.h/cpp` | Ascend VDEC decoder implementation |
| `ascend_inference.h/cpp` | Ascend ACL inference implementation |
| `stream_processor.h/cpp` | Unified processing interface with backend switching |

## Building with Ascend Support

### Prerequisites

1. **Huawei CANN Toolkit** (version 6.0 or later)
   ```bash
   # Default installation path
   export ASCEND_TOOLKIT_HOME=/usr/local/Ascend/ascend-toolkit/latest
   ```

2. **MindX SDK** (for mxVision graph pipeline - optional)

3. **Model Conversion**: Convert YOLOv8 ONNX models to `.om` format using the `atc` tool:
   ```bash
   atc --model=yolov8n.onnx \
       --framework=5 \
       --output=yolov8n \
       --soc_version=Ascend310P3 \
       --input_shape="images:1,3,640,640" \
       --insert_op_conf=aipp.cfg
   ```

   Example `aipp.cfg` for Dynamic AIPP:
   ```
   aipp_op {
     aipp_mode: dynamic
     related_input_rank: 0
   }
   ```

### CMake Configuration

```bash
cmake -B build \
    -DUSE_ASCEND=ON \
    -DASCEND_TOOLKIT_HOME=/usr/local/Ascend/ascend-toolkit/latest \
    ..
```

If Ascend is not found, the build will fall back to CPU/CUDA mode automatically.

## Configuration

In your configuration file, set:

```json
{
  "use_accelerator": true,
  "model_dir": "/path/to/om/models"
}
```

The system will automatically:
1. Detect if Ascend hardware is available
2. Use VDEC for hardware video decoding
3. Use ACL for NPU inference
4. Fall back to CPU/CUDA if hardware is unavailable

## Runtime Behavior

### Stream Processing Pipeline

**CPU Path** (fallback):
```
RTSP → FFmpeg Demux → FFmpeg Decode (CPU) → sws_scale → ONNX Runtime → Detection
```

**Hardware Path** (Ascend):
```
RTSP → FFmpeg Demux → VDEC Decode (TPU) → VPC Resize (TPU) → ACL Infer (NPU) → Detection
```

### Key Features

1. **Zero-Copy Processing**: Decoded frames remain in TPU device memory for inference, eliminating expensive host-device transfers.

2. **Dynamic Model Management**: Models can be linked/unlinked to streams at runtime using:
   ```cpp
   stream_processor_->AddModel(config);
   stream_processor_->RemoveModel(model_id);
   ```

3. **Multi-Model Branching**: Same decoded frame can be shared across multiple models for different detection tasks.

4. **Automatic Fallback**: If hardware initialization fails, the system automatically falls back to CPU processing.

## API Reference

### StreamProcessor

```cpp
// Create processor with hardware acceleration
StreamProcessorConfig config;
config.use_hardware = true;
config.model_path = "/path/to/model.om";

auto processor = StreamProcessor::Create(config);
processor->Initialize(codec_params, time_base);

// Process frames
processor->SendPacket(packet);
auto frame = processor->ReceiveFrame();
// frame->detections contains inference results
```

### AscendPipeline (Low-level)

```cpp
auto& manager = AscendPipelineManager::Instance();
manager.InitializeRuntime();

auto pipeline = manager.GetPipeline(0);  // Device 0

// Create decoder channel
int channel = pipeline->CreateVdecChannel(config);

// Load model
int model = pipeline->LoadModel(model_config);

// Link model to decoder for zero-copy
pipeline->LinkModelToChannel(model, channel);
```

## Performance Considerations

1. **Resolution**: Atlas 300V Pro supports up to 128+ concurrent 1080p streams for decoding.

2. **Batch Size**: For maximum NPU utilization, consider batching inference requests.

3. **Memory**: Device memory is automatically managed. Use `DestroyVdecChannel()` and `UnloadModel()` to free resources.

4. **AIPP vs VPC**: 
   - Dynamic AIPP: Preprocessing in inference node (simpler)
   - VPC: Explicit preprocessing (more control)

## Troubleshooting

### Common Issues

1. **"Device not found"**: Check Ascend driver installation:
   ```bash
   npu-smi info
   ```

2. **"Model load failed"**: Ensure model is converted with matching SOC version:
   ```bash
   atc --soc_version=<your_soc_version> ...
   ```

3. **"Channel allocation failed"**: Too many concurrent streams. Check device limits.

### Logging

Enable debug logging:
```cpp
// In config.json
{"log_level": "debug"}
```

Check Ascend-specific logs:
```bash
export ASCEND_SLOG_PRINT_TO_STDOUT=1
```
