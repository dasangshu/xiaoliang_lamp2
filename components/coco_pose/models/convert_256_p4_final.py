import os
import json
import torch
from torch.utils.data import Dataset, DataLoader
from ppq import *
from ppq.api import *
import numpy as np
from PIL import Image

def create_config():
    """创建256尺寸P4模型的配置"""
    config = {
        "batch_size": 32,
        "input_shape": [1, 3, 256, 256],
        "target": "esp32p4",
        "num_of_bits": 8,
        "quant_setting": {
            "default": {
                "algorithm": "kl",
                "calibration_method": "percentage", 
                "percentile": 99.99,
                "per_channel": False
            }
        }
    }
    
    with open('pose_256_p4_final_config.json', 'w') as f:
        json.dump(config, f, indent=2)
    
    return config

class CaliDataset(Dataset):
    def __init__(self, path, img_shape=256):
        super().__init__()
        self.img_shape = img_shape
        self.img_list = []
        for root, _, files in os.walk(path):
            for file in files:
                if file.endswith('.jpg') or file.endswith('.png'):
                    self.img_list.append(os.path.join(root, file))

    def __len__(self):
        return len(self.img_list)

    def __getitem__(self, idx):
        img = Image.open(self.img_list[idx]).convert('RGB')
        img = img.resize((self.img_shape, self.img_shape))
        img = np.array(img).transpose(2, 0, 1)  # HWC to CHW
        img = img.astype(np.float32) / 255.0
        return torch.from_numpy(img)

def main():
    # 创建配置文件
    print('Creating config for 256x256 P4 model...')
    config = create_config()
    
    # 设置参数
    BATCHSIZE = config['batch_size']
    INPUT_SHAPE = config['input_shape']  # [N, C, H, W]
    DEVICE = 'cpu'
    PLATFORM = TargetPlatform.ESPDL_INT8
    ONNX_PATH = 'yolo11n-pose-256.onnx'
    ESPDL_PATH = 'coco_pose_yolo11n_pose_256_p4_final.espdl'
    
    # 检查ONNX文件是否存在
    if not os.path.exists(ONNX_PATH):
        print(f'ERROR: {ONNX_PATH} not found!')
        return
    
    # 检查校准数据集是否存在
    if not os.path.exists('calib_yolo11n-pose'):
        print('ERROR: Calibration dataset calib_yolo11n-pose not found!')
        print('Please make sure the calibration dataset is available.')
        return
    
    # 准备校准数据加载器
    print('Preparing calibration dataset...')
    dataset = CaliDataset('calib_yolo11n-pose', img_shape=256)
    dataloader = DataLoader(
        dataset=dataset,
        batch_size=BATCHSIZE,
        shuffle=True,
        num_workers=0)

    # 创建量化设置
    print('Creating quantization settings...')
    quant_setting = QuantizationSettingFactory.espdl_setting()
    quant_setting.quantize_parameter_setting.calib_algorithm = config['quant_setting']['default']['algorithm']
    quant_setting.quantize_parameter_setting.calibration_method = config['quant_setting']['default']['calibration_method']
    quant_setting.quantize_parameter_setting.percentile = config['quant_setting']['default']['percentile']
    quant_setting.quantize_parameter_setting.per_channel = config['quant_setting']['default']['per_channel']

    # 执行量化
    print(f'Quantizing 256x256 model to {PLATFORM}...')
    try:
        quantized = espdl_quantize_onnx(
            onnx_import_file=ONNX_PATH,
            espdl_export_file=ESPDL_PATH,
            calib_dataloader=dataloader,
            calib_steps=32,
            setting=quant_setting,
            platform=PLATFORM,
            device=DEVICE,
            input_shape=INPUT_SHAPE,
            collate_fn=None)

        # 导出量化后的模型
        print('Exporting quantized model...')
        export_ppq_graph(
            graph=quantized,
            platform=PLATFORM,
            graph_save_to='coco_pose_yolo11n_pose_256_p4_final_int8.onnx',
            config_save_to='coco_pose_yolo11n_pose_256_p4_final.json',
            save_as_espdl=True)

        print('256x256 P4 quantization completed successfully!')
        
    except Exception as e:
        print(f'ERROR during quantization: {e}')
        print('Try checking if all dependencies are properly installed.')

if __name__ == '__main__':
    main() 