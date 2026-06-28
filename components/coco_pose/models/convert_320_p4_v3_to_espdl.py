from ppq import *
from ppq.api import *
import json
import onnx

# 加载配置文件
with open('pose_320_p4_v3_config.json', 'r') as f:
    config = json.load(f)

def main():
    # 加载量化后的ONNX模型
    print('Loading quantized model...')
    model = onnx.load('coco_pose_yolo11n_pose_320_p4_v3_int8.onnx')

    # 创建PPQ图
    print('Creating PPQ graph...')
    graph = BaseGraph(name='coco_pose_yolo11n_pose_320_p4_v3', built_from=model)

    # 设置目标平台
    print('Setting target platform...')
    graph = dispatch_graph(graph, platform=TargetPlatform.ESPDL_INT8)

    # 导出ESPDL模型
    print('Exporting ESPDL model...')
    export_ppq_graph(
        graph=graph,
        platform=TargetPlatform.ESPDL_INT8,
        graph_save_to='coco_pose_yolo11n_pose_320_p4_v3.espdl',
        config_save_to='coco_pose_yolo11n_pose_320_p4_v3.json',
        save_as_espdl=True)

    print('ESPDL conversion completed successfully!')

if __name__ == '__main__':
    main() 