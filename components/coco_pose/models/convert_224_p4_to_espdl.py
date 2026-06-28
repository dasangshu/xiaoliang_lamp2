import sys
import os
sys.path.append('esp-ppq')  # 添加 esp-ppq 到路径

try:
    from ppq import *
    from ppq.api import *
    import json
    import onnx
    print("Using esp-ppq")
except ImportError:
    print("esp-ppq not found, trying standard ppq...")
    try:
        from ppq import *
        from ppq.api import *
        import json
        import onnx
        print("Using standard ppq")
    except ImportError:
        print("ERROR: Neither esp-ppq nor standard ppq found!")
        sys.exit(1)

def create_config():
    """创建224尺寸P4模型的配置"""
    config = {
        "target": "esp32p4",
        "num_of_bits": 8,
        "batch_size": 32,
        "input_shape": [1, 3, 224, 224],
        "calibration_samples": 100,
        "model_name": "coco_pose_yolo11n_pose_224_p4"
    }
    
    with open('pose_224_p4_config.json', 'w') as f:
        json.dump(config, f, indent=2)
    
    return config

def main():
    # 创建配置文件
    print('Creating config for 224x224 P4 model...')
    config = create_config()
    
    # 检查输入ONNX文件是否存在
    input_model = 'yolo11n-pose-224.onnx'
    if not os.path.exists(input_model):
        print(f'ERROR: Input model {input_model} not found!')
        return
    
    print(f'Loading model: {input_model}')
    model = onnx.load(input_model)
    
    # 量化模型
    print('Quantizing model for ESP32-P4...')
    try:
        # 使用QuantizationSettingFactory.espdl_setting()的默认设置
        setting = QuantizationSettingFactory.espdl_setting(
            platform=TargetPlatform.ESPDL_INT8, 
            calib_steps=config["calibration_samples"]
        )
        
        # 创建随机校准数据
        calib_data = [torch.randn(config["input_shape"]) for _ in range(config["calibration_samples"])]
        
        # 量化模型
        quantized = quantize_onnx_model(
            onnx_import_file=input_model,
            calib_dataloader=calib_data,
            calib_steps=config["calibration_samples"],
            input_shape=config["input_shape"],
            setting=setting,
            collate_fn=lambda x: x.to('cpu')
        )
        
        # 导出量化后的ONNX模型
        quantized_onnx = f'coco_pose_yolo11n_pose_224_p4_int8.onnx'
        print(f'Exporting quantized ONNX: {quantized_onnx}')
        export_ppq_graph(
            graph=quantized,
            platform=TargetPlatform.ONNX,
            graph_save_to=quantized_onnx
        )
        
        # 导出ESPDL模型
        espdl_model = f'coco_pose_yolo11n_pose_224_p4.espdl'
        espdl_config = f'coco_pose_yolo11n_pose_224_p4.json'
        print(f'Exporting ESPDL model: {espdl_model}')
        export_ppq_graph(
            graph=quantized,
            platform=TargetPlatform.ESPDL_INT8,
            graph_save_to=espdl_model,
            config_save_to=espdl_config,
            save_as_espdl=True
        )
        
        print('224x224 P4 ESPDL conversion completed successfully!')
        
    except Exception as e:
        print(f'ERROR during quantization: {e}')
        print('Trying alternative approach...')
        
        # 备选方案：直接加载ONNX并转换
        try:
            graph = BaseGraph(name='coco_pose_yolo11n_pose_224_p4', built_from=model)
            graph = dispatch_graph(graph, platform=TargetPlatform.ESPDL_INT8)
            
            espdl_model = f'coco_pose_yolo11n_pose_224_p4.espdl'
            espdl_config = f'coco_pose_yolo11n_pose_224_p4.json'
            
            export_ppq_graph(
                graph=graph,
                platform=TargetPlatform.ESPDL_INT8,
                graph_save_to=espdl_model,
                config_save_to=espdl_config,
                save_as_espdl=True
            )
            
            print('224x224 P4 ESPDL conversion completed successfully (alternative method)!')
            
        except Exception as e2:
            print(f'ERROR in alternative approach: {e2}')

if __name__ == '__main__':
    main() 