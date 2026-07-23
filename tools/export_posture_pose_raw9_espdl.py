#!/usr/bin/env python3
import argparse
import json
import os
import shutil
from pathlib import Path

import cv2
import numpy as np
import onnx
import torch
from torch.utils.data import DataLoader, Dataset
from ultralytics import YOLO
from ultralytics.engine.exporter import Exporter, arange_patch, try_export
from ultralytics.nn.modules import Attention, Pose
from ultralytics.utils import LOGGER, colorstr
from ultralytics.utils.checks import check_requirements


class ESPPose(Pose):
    def forward(self, x):
        box0 = self.cv2[0](x[0])
        score0 = self.cv3[0](x[0])
        box1 = self.cv2[1](x[1])
        score1 = self.cv3[1](x[1])
        box2 = self.cv2[2](x[2])
        score2 = self.cv3[2](x[2])
        kpt0 = self.cv4[0](x[0])
        kpt1 = self.cv4[1](x[1])
        kpt2 = self.cv4[2](x[2])
        return box0, score0, box1, score1, box2, score2, kpt0, kpt1, kpt2


class ESPAttention(Attention):
    def forward(self, x):
        b, c, h, w = x.shape
        n = h * w
        qkv = self.qkv(x)
        q, k, v = qkv.view(-1, self.num_heads, self.key_dim * 2 + self.head_dim, n).split(
            [self.key_dim, self.key_dim, self.head_dim], dim=2
        )
        attn = (q.transpose(-2, -1) @ k) * self.scale
        attn = attn.softmax(dim=-1)
        x = (v @ attn.transpose(-2, -1)).view(-1, c, h, w) + self.pe(v.reshape(-1, c, h, w))
        return self.proj(x)


class Raw9PoseExporter(Exporter):
    @try_export
    def export_onnx(self, prefix=colorstr("ONNX:")):
        requirements = ["onnx>=1.14.0"]
        if self.args.simplify:
            requirements += ["onnxsim", "onnxruntime" + ("-gpu" if torch.cuda.is_available() else "")]
        check_requirements(requirements)

        opset_version = self.args.opset or 13
        LOGGER.info(f"\n{prefix} starting raw9 export with onnx {onnx.__version__} opset {opset_version}...")
        output_names = ["box0", "score0", "box1", "score1", "box2", "score2", "kpt0", "kpt1", "kpt2"]
        f = str(self.file.with_suffix(".onnx"))
        dynamic = self.args.dynamic
        if dynamic:
            dynamic = {"images": {0: "batch"}}
            for name in output_names:
                dynamic[name] = {0: "batch"}

        with arange_patch(self.args):
            torch.onnx.export(
                self.model,
                self.im,
                f,
                verbose=False,
                opset_version=opset_version,
                do_constant_folding=True,
                input_names=["images"],
                output_names=output_names,
                dynamic_axes=dynamic or None,
                dynamo=False,
            )

        model_onnx = onnx.load(f)
        if self.args.simplify:
            try:
                import onnxsim

                LOGGER.info(f"{prefix} simplifying with onnxsim {onnxsim.__version__}...")
                model_onnx, _ = onnxsim.simplify(model_onnx)
            except Exception as exc:
                LOGGER.warning(f"{prefix} simplifier failure: {exc}")

        for k, v in self.metadata.items():
            meta = model_onnx.metadata_props.add()
            meta.key, meta.value = k, str(v)

        onnx.save(model_onnx, f)
        return f, model_onnx


class Raw9YOLO(YOLO):
    def export(self, **kwargs):
        self._check_is_pytorch_model()
        custom = {"imgsz": self.model.args["imgsz"], "batch": 1, "data": None, "device": None, "verbose": False}
        args = {**self.overrides, **custom, **kwargs, "mode": "export"}
        return Raw9PoseExporter(overrides=args, _callbacks=self.callbacks)(model=self.model)


class CalibrationDataset(Dataset):
    def __init__(self, directory):
        self.files = sorted(
            p for p in Path(directory).iterdir() if p.suffix.lower() in {".jpg", ".jpeg", ".png"}
        )

    def __len__(self):
        return len(self.files)

    def __getitem__(self, index):
        img = cv2.imread(str(self.files[index]))
        if img is None:
            raise ValueError(f"failed to read calibration image: {self.files[index]}")
        img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
        img = img.astype(np.float32) / 255.0
        return torch.from_numpy(img).permute(2, 0, 1)


def patch_model(model):
    for module in model.modules():
        if isinstance(module, Attention):
            module.forward = ESPAttention.forward.__get__(module)
        if isinstance(module, Pose):
            module.forward = ESPPose.forward.__get__(module)


def quantize_espdl(onnx_path, output_path, calib_dir, imgsz):
    from esp_ppq.api import espdl_quantize_onnx
    from esp_ppq.api.setting import QuantizationSettingFactory

    dataset = CalibrationDataset(calib_dir)
    if len(dataset) == 0:
        raise RuntimeError(f"empty calibration directory: {calib_dir}")

    dataloader = DataLoader(dataset, batch_size=1, shuffle=True, num_workers=0, collate_fn=lambda b: torch.stack(b))
    setting = QuantizationSettingFactory.espdl_setting()
    setting.quantize_parameter_setting.calib_algorithm = "kl"
    setting.quantize_parameter_setting.calibration_method = "percentage"
    setting.quantize_parameter_setting.percentile = 99.99

    espdl_quantize_onnx(
        onnx_import_file=str(onnx_path),
        espdl_export_file=str(output_path),
        calib_dataloader=dataloader,
        calib_steps=min(100, len(dataloader)),
        input_shape=[1, 3, imgsz, imgsz],
        target="esp32p4",
        num_of_bits=8,
        collate_fn=None,
        setting=setting,
        device="cpu",
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--calib-dir", required=True)
    parser.add_argument("--imgsz", type=int, default=256)
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    local_model_path = output_dir / Path(args.model).name
    if Path(args.model).resolve() != local_model_path.resolve():
        shutil.copy2(args.model, local_model_path)

    model = Raw9YOLO(str(local_model_path))
    patch_model(model)
    exported = Path(model.export(format="onnx", simplify=False, opset=13, dynamic=False, imgsz=args.imgsz))
    onnx_path = output_dir / "coco_pose_yolo11n_pose_256_p4_raw9.onnx"
    shutil.copy2(exported, onnx_path)

    espdl_path = output_dir / "coco_pose_yolo11n_pose_256_p4.espdl"
    quantize_espdl(onnx_path, espdl_path, args.calib_dir, args.imgsz)

    info = {
        "source_model": str(Path(args.model).resolve()),
        "onnx": str(onnx_path.resolve()),
        "espdl": str(espdl_path.resolve()),
        "expected_outputs": ["box0", "score0", "box1", "score1", "box2", "score2", "kpt0", "kpt1", "kpt2"],
    }
    (output_dir / "export_raw9_summary.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
    print(json.dumps(info, indent=2))


if __name__ == "__main__":
    os.environ.setdefault("YOLO_CONFIG_DIR", "/private/tmp/Ultralytics")
    os.environ.setdefault("MPLCONFIGDIR", "/private/tmp/matplotlib")
    main()
