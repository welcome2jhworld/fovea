# Model candidates survey

Surveyed 2026-09-13 against the Hugging Face API, PyPI and GitHub (read-only, nothing downloaded).
Sizes are the sum of `.safetensors` files in the `main` tree listing, decimal GB.
"transformers min" is the first tagged release whose `src/transformers/models/<type>/` directory exists, cross-checked with the model card.

## Summary table

| Candidate | HF id | Exists | Gated | License | model_type / architecture | Weights (bytes) | GB | transformers min | Video input | Embedding dim |
|---|---|---|---|---|---|---|---|---|---|---|
| Qwen3.5 4B | `Qwen/Qwen3.5-4B` | yes | no | Apache-2.0 | `qwen3_5` / `Qwen3_5ForConditionalGeneration` | 9,319,828,096 | 9.32 (BF16, 4.66B) | 5.2.0 (card: "latest from main") | yes, Qwen3VLProcessor protocol | n/a |
| Marlin 2B | `NemoStation/Marlin-2B` | yes | yes (`auto`, form: name / affiliation / use) | Apache-2.0 | `qwen3_5` / `MarlinForConditionalGeneration` (custom code) | 5,443,677,224 | 5.44 (BF16, 2.72B) | 5.7.0 (card) | yes, `.caption()` / `.find()` + raw chat | n/a |
| Marlin 2B MLX 8-bit | `NemoStation/Marlin-2B-MLX-8bit` | yes | no (API `gated: false`; card text claims a form) | Apache-2.0 | `qwen3_5` / `MarlinForConditionalGeneration`, MLX affine 8-bit g64 | 2,662,909,845 | 2.66 | n/a (mlx-vlm; config stamped 5.7.0) | yes, `mlx_vlm.generate --video --fps` | n/a |
| Qwen3-VL Embedding 2B | `Qwen/Qwen3-VL-Embedding-2B` | yes | no | Apache-2.0 | `qwen3_vl` / `Qwen3VLForConditionalGeneration` (sentence-transformers wrapper) | 4,255,140,312 | 4.26 (BF16, 2.13B) | 4.57.0 (card) | yes, `{"video": path, "fps", "max_frames"}` | 2048 (MRL 64..2048) |
| SigLIP 2 base | `google/siglip2-base-patch16-224` | yes | no | Apache-2.0 | `siglip` (SiglipModel) | 1,500,800,904 | 1.50 (F32, 375M) | 4.49.0 | no (still images, 224x224) | 768 |
| Qwen3-VL Reranker 2B | `Qwen/Qwen3-VL-Reranker-2B` | yes | no | Apache-2.0 | `qwen3_vl` / `Qwen3VLForConditionalGeneration` (yes/no logit score) | 4,255,140,312 | 4.26 (BF16, 2.13B) | 4.57.0 (card) | yes, `"fps"` in inputs dict | n/a (scalar score) |
| Qwen3-VL 4B Instruct (fallback VLM) | `Qwen/Qwen3-VL-4B-Instruct` | yes | no | Apache-2.0 | `qwen3_vl` / `Qwen3VLForConditionalGeneration` | 8,875,719,344 | 8.88 (BF16, 4.44B) | 4.57.0 | yes, Qwen3VLProcessor protocol | n/a |

Non-HF candidates:

| Component | Package | Version (date) | License | Notes |
|---|---|---|---|---|
| RF-DETR | `rfdetr` (PyPI) | 1.10.1 (2026-09-07) | Apache-2.0 (package + N/S/M/L and Seg weights); XL/2XL detection + `rfdetr_plus` are PML 1.0 | `RFDETRNano` 384px 30.5M, `RFDETRSmall` 512px 32.1M, `RFDETRMedium` 576px, `RFDETRLarge` 704px |
| ByteTrack | `supervision` (PyPI) | 0.30.2 (2026-09-04) | MIT | `supervision.ByteTrack`, numpy/scipy only, already a dependency of `rfdetr` |
| ByteTrack (alt) | `boxmot` (PyPI) | 25.0.0 (2026-09-09) | AGPL-3.0 | heavy deps (torch, timm, ultralytics extra); AGPL is a product licensing risk |
| transformers | `transformers` (PyPI) | 5.17.0 (2026-09-09) | Apache-2.0 | `qwen3_vl` present since 4.57.0, `qwen3_5` since 5.2.0; both present in 5.17.0 |
| mlx-vlm | `mlx-vlm` (PyPI) | 0.7.0 (2026-09-07) | MIT | requires `mlx>=0.32.2`, `transformers>=5.14.0`; model dirs include `qwen3_5`, `qwen3_vl`, `qwen3_vl_embedding`, `siglip`, `rfdetr` |

Series note: `Qwen/Qwen3.5-4B` exists (created 2026-02-27, 7.1M downloads). Siblings under the `Qwen` org: `Qwen3.5-0.8B`, `-2B`, `-4B`, `-9B`, `-27B`, `-35B-A3B`, `-122B-A10B`, `-397B-A17B`, each with `-Base`, and FP8 / GPTQ-Int4 variants for 27B and up. All `image-text-to-text`, Apache-2.0, `qwen3_5`.

## Shared video protocol (Qwen3-VL and Qwen3.5)

Both families ship `processor_class: Qwen3VLProcessor` with `Qwen2VLImageProcessorFast` and `Qwen3VLVideoProcessor`, so frames and timestamps are passed identically.

Processor kwargs (defaults from `video_preprocessor_config.json` and `video_processing_qwen3_vl.py` at transformers v5.17.0):

- `fps` = 2 (frames per second to sample); `num_frames` is the alternative and is mutually exclusive with `fps`
- `min_frames` = 4, `max_frames` = 768, `do_sample_frames` = True
- `size` = `{"shortest_edge": 4096, "longest_edge": 25165824}`: a total pixel budget across all sampled frames; per-frame pixels are limited to `min(max_video_tokens * 32**2, longest_edge / num_frames)`, floored at `1.05 * shortest_edge`
- `patch_size` 16, `temporal_patch_size` 2, `merge_size` 2, so one video token covers a 32x32 area of two consecutive frames
- Image kwargs: `size = {"shortest_edge": 65536, "longest_edge": 16777216}` pixels (`min_pixels` / `max_pixels` are the same numbers under older names; the embedding repo pins `min_pixels: 4096`, `max_pixels: 1310720`)

Timestamps are not passed by the caller; the processor writes them into the prompt. For every temporal patch (2 frames) `replace_video_token` emits

```
<{t:.1f} seconds><|vision_start|><|video_pad|>...<|video_pad|><|vision_end|>
```

where `t` is the mean of the two frame times computed as `frame_index / video_metadata.fps`. If `video_metadata` is missing (pre-decoded frames passed as a list) the processor warns and assumes 24 fps, so always pass metadata (`fps`, `total_num_frames`, `frames_indices`) when feeding your own frames.

Chat-template usage (from the Qwen3-VL-4B-Instruct card, image example; the video form only swaps the content item):

```python
from transformers import Qwen3VLForConditionalGeneration, AutoProcessor

model = Qwen3VLForConditionalGeneration.from_pretrained(
    "Qwen/Qwen3-VL-4B-Instruct", dtype="auto", device_map="auto"
)
processor = AutoProcessor.from_pretrained("Qwen/Qwen3-VL-4B-Instruct")

messages = [{"role": "user", "content": [
    {"type": "video", "video": "file:///path/clip.mp4"},
    {"type": "text", "text": "Describe this video."},
]}]
inputs = processor.apply_chat_template(
    messages, tokenize=True, add_generation_prompt=True,
    return_dict=True, return_tensors="pt",
    fps=2, max_frames=64,
).to(model.device)
generated_ids = model.generate(**inputs, max_new_tokens=128)
```

Qwen3.5 card, served through vLLM (OpenAI-compatible), shows the same knobs via `mm_processor_kwargs`:

```python
chat_response = client.chat.completions.create(
    model="Qwen/Qwen3.5-4B",
    messages=messages,   # content item: {"type": "video_url", "video_url": {"url": "..."}}
    extra_body={
        "top_k": 20,
        "mm_processor_kwargs": {"fps": 2, "do_sample_frames": True},
    },
)
```

Long video note from the Qwen3.5 card: the released `video_preprocessor_config.json` is conservative; for hour-scale clips set `{"longest_edge": 469762048, "shortest_edge": 4096}` (224k video tokens).

## Per-model notes

### Qwen/Qwen3.5-4B

- Created 2026-02-27, last modified 2026-03-02, `pipeline_tag: image-text-to-text`, base `Qwen/Qwen3.5-4B-Base`.
- Architecture: 32 text layers in a 3:1 `linear_attention` / `full_attention` pattern (Gated DeltaNet hybrid), hidden 2560, vocab 248,320, 262,144 context, MTP head (`mtp_num_hidden_layers: 1`); vision tower depth 24, hidden 1024, patch 16, `deepstack_visual_indexes: []`.
- `config.transformers_version` is stamped `4.57.0.dev0` (internal build); the architecture first appears in tagged transformers v5.2.0 (v5.1.0 has no `models/qwen3_5`). Card says "The latest transformers is required" and installs from git main.
- Thinking is on by default; the card states `/think` and `/nothink` soft switches are not supported, disable via API parameters.
- Runtime: vLLM and SGLang need main/nightly builds per the card; `transformers serve` works. GGUF: `unsloth/Qwen3.5-4B-GGUF` (Q4_K_M 2.74 GB, Q8_0 4.48 GB, BF16 8.42 GB, plus `mmproj-BF16.gguf` 0.68 GB for vision), `bartowski/Qwen_Qwen3.5-4B-GGUF`, `lmstudio-community/Qwen3.5-4B-GGUF`. MLX: `mlx-community/Qwen3.5-4B-MLX-4bit` 3.03 GB, `lmstudio-community/Qwen3.5-4B-MLX-4bit`. mlx-vlm README uses `Qwen/Qwen3.5-4B` directly as a server and speculative-decoding example (`z-lab/Qwen3.5-4B-DFlash` drafter).

### NemoStation/Marlin-2B

- Fine-tune of `Qwen/Qwen3.5-2B` for dense video captioning and temporal grounding; created 2026-05-13, 592 likes, 4.3k downloads.
- Gated `auto` (auto-approval after a form: "Full name", "Affiliation or company", "What do you want to use Marlin for?"). Raw file access returns 401 until accepted; the API tree listing and config metadata are public. An unofficial GGUF conversion (`jadeonrails/marlin-2b-gguf`, text 4.79 GB + vision projector 0.67 GB) targets `llama-mtmd-cli --video`; it is not from the authors.
- Custom modeling code (`modeling_marlin.py`, `auto_map: AutoModelForCausalLM -> MarlinForConditionalGeneration`), so `trust_remote_code=True` is required. Requirements from the card: `transformers>=5.7.0`, `torch>=2.11.0`, `torchcodec`, `qwen-vl-utils>=0.0.14`, `av`, `pillow`.
- Video preprocessing is fixed by env vars set inside the modeling code (override in the shell before importing transformers): `FORCE_QWENVL_VIDEO_READER=torchcodec`, `VIDEO_MAX_PIXELS=200704` (~448x448 per frame), `FPS=2.0`, `FPS_MAX_FRAMES=240` (~2 min), `FPS_MIN_FRAMES=4`.
- Output formats: caption mode gives `Scene: <paragraph>` then `Events: <X.X - Y.Y> <description>`; find mode returns `From X.X to Y.Y.`. Every response starts with a `<think>` token that the helper methods strip.
- Card snippet:

```python
import torch
from transformers import AutoModelForCausalLM

marlin = AutoModelForCausalLM.from_pretrained(
    "NemoStation/Marlin-2B",
    trust_remote_code=True,
    dtype=torch.bfloat16,
    device_map={"": "cuda"},
)
result = marlin.caption("video.mp4")
print(result["scene"])
for ev in result["events"]:
    print(f"<{ev['start']:.1f} - {ev['end']:.1f}> {ev['description']}")

result = marlin.find("video.mp4", event="a person enters the room")
print(result["span"])  # (14.3, 18.2) or None
```

Raw path (same processor protocol as Qwen3.5):

```python
processor = AutoProcessor.from_pretrained("NemoStation/Marlin-2B", trust_remote_code=True)
messages = [{"role": "user", "content": [
    {"type": "video", "video": "video.mp4"},
    {"type": "text", "text": "Your custom prompt here"},
]}]
inputs = processor.apply_chat_template(
    messages, tokenize=True, add_generation_prompt=True,
    return_tensors="pt", return_dict=True,
).to(model.device)
```

- The card targets CUDA (`device_map={"": "cuda"}`); MPS is not documented. 2.72B params in BF16 is 5.44 GB; the safetensors index reports 2.21B, which is inconsistent metadata.

### NemoStation/Marlin-2B-MLX-8bit

- 8-bit MLX conversion by the authors (`mlx_vlm.convert --hf-path NemoStation/Marlin-2B -q --q-bits 8`), affine, group size 64, 2.66 GB, config stamped `transformers_version: 5.7.0`.
- The API reports `gated: false` and the README fetched without auth, although the card's frontmatter says access uses the base model's form. Treat as ungated until the API changes.
- Card usage: `python -m mlx_vlm.generate --model NemoStation/Marlin-2B-MLX-8bit --video clip.mp4 --fps 2 --prompt "Describe the video."`. The card warns that dense captioning works on mlx-vlm's one-shot path but temporal grounding needs a timestamp-aware serving path (it names SGLang-MLX) so per-frame time reaches the model.
- No `video_preprocessor_config.json` in the repo (404); `preprocessor_config.json` is the stock Qwen3VLProcessor image config.

### Qwen/Qwen3-VL-Embedding-2B

- Built on `Qwen/Qwen3-VL-2B-Instruct`; 28 layers, 32K sequence length, embedding dim 2048 with MRL (user-defined 64..2048), instruction aware, supports text, image, screenshot, video and mixed inputs, 30+ languages.
- Card requirements for the transformers path: `transformers>=4.57.0`, `qwen-vl-utils>=0.0.14`, `torch==2.8.0`. `config.transformers_version` 4.57.1.
- sentence-transformers path (dim 2048 shown in the output comment):

```python
from sentence_transformers import SentenceTransformer

model = SentenceTransformer("Qwen/Qwen3-VL-Embedding-2B")
query_embeddings = model.encode(queries)
doc_embeddings = model.encode(documents)   # str, image URL, or {"text": ..., "image": ...}
print(query_embeddings.shape, doc_embeddings.shape)
# (4, 2048) (3, 2048)
model.encode(queries, prompt="Retrieve relevant documents for the query.")
```

- Video protocol from `scripts/qwen3_vl_embedding.py` (the `Qwen3VLEmbedder` class): pass `{"video": "/path/clip.mp4"}` or `{"video": [frame paths or PIL images]}`, optional `fps` / `max_frames` per input, `instruction`. Script defaults: `FPS = 1`, `MAX_FRAMES = 64`, `MIN_PIXELS = 4096`, `MAX_PIXELS = 1,843,200`, `FRAME_MAX_PIXELS = 786,432`, `MAX_TOTAL_PIXELS = 7,864,320`, `MAX_LENGTH = 8192`. A path is passed to `qwen_vl_utils.process_vision_info` with `{'fps', 'max_frames'}`; a frame list is uniformly subsampled by `sample_frames(num_frames, max_frames)` and passed with `total_pixels`. Repo `video_preprocessor_config.json` uses `fps: 2`, `min_frames: 4`, `max_frames: 768`.

```python
from scripts.qwen3_vl_embedding import Qwen3VLEmbedder
model = Qwen3VLEmbedder(model_name_or_path="Qwen/Qwen3-VL-Embedding-2B")
embeddings = model.process(queries + documents)   # each item: {"text"|"image"|"video", "instruction"?, "fps"?, "max_frames"?}
```

- Runtime: vLLM (`llm.embed`) and SGLang examples on the card. mlx-vlm 0.7.0 lists Qwen3-VL-Embedding as a supported embedding-server architecture and has `models/qwen3_vl_embedding`. No official MLX/GGUF quant; community GGUFs exist (`DevQuasar/Qwen.Qwen3-VL-Embedding-2B-GGUF`, `mradermacher/...`), unverified.

### google/siglip2-base-patch16-224

- SigLIP 2 base, patch 16, fixed 224x224 input (`SiglipImageProcessor`, resample bilinear, mean/std 0.5); 375M params stored in F32 (1.50 GB). `config.transformers_version` 4.49.0.dev0; SigLIP 2 requires transformers >= 4.49.
- `config.json` relies on `SiglipVisionConfig` / `SiglipTextConfig` defaults, so hidden and projection size are the base defaults: 768. Text vocab 256,000 (multilingual Gemma tokenizer).
- No video input. Image-only encoder; the card's embedding snippet:

```python
from transformers import AutoModel, AutoProcessor
model = AutoModel.from_pretrained("google/siglip2-base-patch16-224", device_map="auto").eval()
processor = AutoProcessor.from_pretrained("google/siglip2-base-patch16-224")
inputs = processor(images=[image], return_tensors="pt").to(model.device)
image_embeddings = model.get_image_features(**inputs)
```

- Variants at other resolutions: `-256`, `-384`, `-512`, and `siglip2-base-patch16-naflex` (variable aspect). MLX: `mlx-community/siglip2-base-patch16-224-8bit` 0.40 GB; mlx-vlm has `models/siglip`. ONNX: `onnx-community/siglip2-base-patch16-224-ONNX`.

### Qwen/Qwen3-VL-Reranker-2B

- Same backbone as the embedding model; scores a (query, document) pair through a yes/no logit (`1_LogitScore`, `additional_chat_templates/reranker.jinja`; vLLM uses `classifier_from_token: ["no", "yes"]`). Sigmoid maps scores to 0..1.
- Requirements: `transformers>=4.57.0`, `qwen-vl-utils>=0.0.14`, `torch==2.8.0`; `config.transformers_version` 4.57.0.
- sentence-transformers path:

```python
from sentence_transformers import CrossEncoder
model = CrossEncoder("Qwen/Qwen3-VL-Reranker-2B")
prompt = "Retrieve images or text relevant to the user's query."
scores = model.predict([(query, doc) for doc in documents], prompt=prompt)
rankings = model.rank(query, documents, prompt=prompt)
```

- Video protocol via `scripts/qwen3_vl_reranker.py`: a single dict with `"instruction"`, `"query": {"text"|"image"|"video"}`, `"documents": [...]`, and a top-level `"fps": 1.0`.

```python
from scripts.qwen3_vl_reranker import Qwen3VLReranker
model = Qwen3VLReranker(model_name_or_path="Qwen/Qwen3-VL-Reranker-2B")
scores = model.process({
    "instruction": "Retrieve images or text relevant to the user's query.",
    "query": {"text": "A woman playing with her dog on a beach at sunset."},
    "documents": [{"text": "..."}, {"image": "..."}, {"video": "clip.mp4"}],
    "fps": 1.0,
})
```

- mlx-vlm 0.7.0 reranker server: "Qwen3-VL rerankers also accept objects containing text, image, image_url, video, or video_url".

### Qwen/Qwen3-VL-4B-Instruct (fallback VLM)

- Released 2025-10-11, 4.1M downloads, 36 text layers, hidden 2560, 262,144 context, DeepStack vision (`deepstack_visual_indexes: [5, 11, 17]`), interleaved MRoPE and text-timestamp alignment for temporal grounding.
- `config.transformers_version` 4.57.0.dev0; the model directory exists from v4.57.0. Card recommends installing from git main or `transformers==4.57.0`.
- Video is handled by the shared protocol above; the card itself only shows an image example. Recommended VL sampling from the card: `top_p 0.8`, `top_k 20`, `temperature 0.7`, `presence_penalty 1.5`, `out_seq_length 16384`.
- Runtime: vLLM stable. GGUF (official): `Qwen/Qwen3-VL-4B-Instruct-GGUF` with `Qwen3VL-4B-Instruct-Q4_K_M.gguf` 2.50 GB, `Q8_0` 4.28 GB, `F16` 8.05 GB, `mmproj-...-Q8_0.gguf` 0.45 GB, `mmproj-...-F16.gguf` 0.84 GB; also `unsloth/`, `lmstudio-community/`, `bartowski/`. MLX: `lmstudio-community/Qwen3-VL-4B-Instruct-MLX-4bit` 3.09 GB, `-MLX-8bit` 5.10 GB, `-5bit`, `-6bit`; `mlx-community/Qwen3-VL-4B-Instruct-4bit`. mlx-vlm README uses `Qwen/Qwen3-VL-4B-Instruct` as its server example.

### RF-DETR (roboflow/rf-detr, PyPI `rfdetr`)

- 1.10.1 on PyPI (2026-09-07), GitHub default branch `develop`, ICLR 2026, repo license Apache-2.0. `requires_dist`: `torch>=2.2.0`, `torchvision>=0.17.0`, `transformers>=5.1.0,<6`, `supervision>=0.29.0`, `pydantic>=2`. Extras: `onnx`, `coreml` (`coremltools>=8,<10`, `torch<2.12`), `tensorrt`, `tflite`, `executorch`, `train`, `plus`.
- License split (README "License" section): "The open-source `rfdetr` package and Apache-designated model weights are licensed under Apache License 2.0" while "Plus components, including the `rfdetr_plus` extension and RF-DETR-XL / RF-DETR-2XL detection models, are licensed under PML 1.0". Every N/S/M/L detection and Seg N..2XL row in the README table is marked Apache 2.0.
- Detection classes and published numbers: `RFDETRNano` (384x384, 30.5M params, COCO AP50:95 48.4, 2.3 ms), `RFDETRSmall` (512x512, 32.1M, 53.0, 3.5 ms), `RFDETRMedium` (576x576, 33.7M, 54.7), `RFDETRLarge` (704x704, 33.9M, 56.5). Config defaults `pretrain_weights = "rf-detr-nano.pth"` etc., downloaded on first instantiation (host not verified in this survey). HF mirrors: `Roboflow/rf-detr-nano` 0.12 GB, `Roboflow/rf-detr-small` 0.13 GB, `Roboflow/rf-detr-base` 0.13 GB, `Roboflow/rf-detr-medium`, `-large`, all Apache-2.0 safetensors. GGUF for `rfdetr.cpp`: `mudler/rfdetr-cpp-nano`. mlx-vlm has a `models/rfdetr` directory.
- README snippet:

```python
import supervision as sv
from rfdetr import RFDETRMedium
from rfdetr.assets.coco_classes import COCO_CLASSES

model = RFDETRMedium()
detections = model.predict("https://media.roboflow.com/dog.jpg", threshold=0.5)
```

### ByteTrack

- `supervision` 0.30.2 (MIT, 2026-09-04): `src/supervision/tracker/byte_tracker/core.py`, `class ByteTrack(track_activation_threshold=0.25, lost_track_buffer=30, minimum_matching_threshold=..., frame_rate=..., minimum_consecutive_frames=...)` with `update_with_detections(detections)`. Pure numpy/scipy, no torch, and `rfdetr` already depends on `supervision>=0.29.0`, so it costs nothing extra. Recommended.
- `boxmot` 25.0.0 (AGPL-3.0, 2026-09-09) also implements ByteTrack plus ReID trackers, but pulls torch, timm, opencv, pandas, pyarrow, and the AGPL license is a distribution risk for Fovea. Not recommended for the core.

## Runtime notes

- transformers 5.17.0 (2026-09-09) supports every candidate: `qwen3_vl` (since 4.57.0), `qwen3_5` (since 5.2.0), `siglip` (SigLIP 2 since 4.49). The machine's anaconda transformers 4.46.3 supports none of them; a fresh env is needed. `rfdetr` needs `transformers>=5.1.0,<6`, Marlin needs `>=5.7.0`, mlx-vlm needs `>=5.14.0`, so a single env at 5.17.0 satisfies all.
- mlx-vlm 0.7.0 (2026-09-07, MIT): `mlx>=0.32.2` (mlx is not installed here), model dirs `qwen3_5`, `qwen3_5_moe`, `qwen3_vl`, `qwen3_vl_embedding`, `siglip`, `rfdetr`. Video CLI: `mlx_vlm.generate --model <id> --video clip.mp4 --fps 2 --prompt "..."`. `generate/video.py`: processors with a native `video_processor` (Qwen3VLProcessor qualifies) receive the video unchanged; otherwise frames are decoded at `fps=2.0` and evenly subsampled to `max_frames=16` stills. The README's "Video Understanding supported models" list is stale (names Qwen2-VL/2.5-VL but not Qwen3-VL/3.5); `models/qwen3_vl/qwen3_vl.py` and `models/qwen3_5/qwen3_5.py` both consume `pixel_values_videos` / `video_grid_thw`. Embedding and reranker servers cover Qwen3-VL-Embedding and Qwen3-VL rerankers.
- llama.cpp / GGUF: official `Qwen/Qwen3-VL-4B-Instruct-GGUF` with mmproj; `unsloth/Qwen3.5-4B-GGUF` with `mmproj-BF16.gguf`; unofficial `jadeonrails/marlin-2b-gguf` used through `llama-mtmd-cli --video`. Converter support was not verified from source (the `convert_hf_to_gguf.py` on master is now a 13 KB stub).
- vLLM / SGLang: documented for Qwen3-VL, Qwen3.5 (main/nightly), Embedding and Reranker; CUDA-only, not applicable to this Mac.
- Marlin's card is CUDA-only in its examples; on Apple Silicon the authors point to the MLX 8-bit build via mlx-vlm.

## Download size vs free disk (M0 minimum set)

Free space at survey time: 328 MiB on `/System/Volumes/Data`. `~/.cache/huggingface/hub` holds none of the candidates (only `BAAI/bge-m3` 6.4 GB and two docling models 0.5 GB).

| M0 set | Weights | Total |
|---|---|---|
| A. `Qwen/Qwen3-VL-4B-Instruct` BF16 + `Qwen/Qwen3-VL-Embedding-2B` + `google/siglip2-base-patch16-224` | 8.88 + 4.26 + 1.50 | 14.63 GB |
| B. `Qwen/Qwen3.5-4B` BF16 + Embedding-2B + siglip2 | 9.32 + 4.26 + 1.50 | 15.08 GB |
| C. `NemoStation/Marlin-2B` BF16 + Embedding-2B + siglip2 | 5.44 + 4.26 + 1.50 | 11.20 GB |
| D. quantized: `lmstudio-community/Qwen3-VL-4B-Instruct-MLX-4bit` + Embedding-2B BF16 (no official quant) + `mlx-community/siglip2-base-patch16-224-8bit` | 3.09 + 4.26 + 0.40 | 7.75 GB |
| E. quantized: `NemoStation/Marlin-2B-MLX-8bit` + Embedding-2B BF16 + siglip2 8-bit | 2.66 + 4.26 + 0.40 | 7.32 GB |

Downloading is blocked: even siglip2 alone (1.50 GB) exceeds the 0.33 GB free. On top of weights, a fresh Python env with torch, transformers, mlx, mlx-vlm and sentence-transformers needs roughly 1.5 to 2 GB, and HF downloads stage temporary blobs in the cache.

Space that must be freed before M0:

- Quantized set (D or E): at least 10 GB.
- BF16 set (A or B): at least 18 GB; 20 GB gives headroom for a later MLX conversion of the embedding model.

Candidates the user could clear (not touched by this survey): `~/.cache/uv` 6.7 GB and `~/Library/Caches/pip` 1.7 GB are safe package caches (8.4 GB together, enough for the quantized set only); `~/.cache/huggingface/hub/models--BAAI--bge-m3` 6.4 GB if bge-m3 is no longer needed brings it to 14.8 GB, still short of the BF16 set. Alternatively point `HF_HOME` at an external volume.
