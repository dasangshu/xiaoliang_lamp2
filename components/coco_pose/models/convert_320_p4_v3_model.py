import os
import json
import torch
from torch.utils.data import Dataset, DataLoader
from ppq import *
from ppq.api import *
import numpy as np
from PIL import Image
import urllib.request
import zipfile

# 加载配置文件
with open('pose_320_p4_v3_config.json', 'r') as f:
    config = json.load(f)

# 设置参数
BATCHSIZE = config['batch_size']
INPUT_SHAPE = config['input_shape']  # [N, C, H, W]
DEVICE = 'cpu'
PLATFORM = TargetPlatform.ESPDL_INT8
ONNX_PATH = 'yolo11n-pose-320.onnx'
ESPDL_PATH = 'coco_pose_yolo11n_pose_320_p4_v3.espdl'

class CaliDataset(Dataset):
    def __init__(self, path, img_shape=320):
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

def download_calib_dataset():
    if not os.path.exists('calib_yolo11n-pose'):
        print('Downloading calibration dataset...')
        url = 'https://dl.espressif.com/public/calib_yolo11n-pose.zip'
        urllib.request.urlretrieve(url, 'calib_yolo11n-pose.zip')
        
        print('Extracting calibration dataset...')
        with zipfile.ZipFile('calib_yolo11n-pose.zip', 'r') as zip_ref:
            zip_ref.extractall('.')
        os.remove('calib_yolo11n-pose.zip')

def main():
    # 下载校准数据集
    download_calib_dataset()
    
    # 准备校准数据加载器
    dataset = CaliDataset('calib_yolo11n-pose', img_shape=320)
    dataloader = DataLoader(
        dataset=dataset,
        batch_size=BATCHSIZE,
        shuffle=True,
        num_workers=0)

    # 创建量化设置
    quant_setting = QuantizationSettingFactory.espdl_setting()
    quant_setting.quantize_parameter_setting.calib_algorithm = config['quant_setting']['default']['algorithm']
    quant_setting.quantize_parameter_setting.calibration_method = config['quant_setting']['default']['calibration_method']
    quant_setting.quantize_parameter_setting.percentile = config['quant_setting']['default']['percentile']
    quant_setting.quantize_parameter_setting.per_channel = config['quant_setting']['default']['per_channel']

    # 执行量化
    print(f'Quantizing model to {PLATFORM}...')
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
        graph_save_to='coco_pose_yolo11n_pose_320_p4_v3_int8.onnx',
        config_save_to='coco_pose_yolo11n_pose_320_p4_v3_int8.json',
        save_as_espdl=True)

    print('Quantization completed successfully!')

if __name__ == '__main__':
    main() 