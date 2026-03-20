# python plot_throughput.py

import matplotlib.pyplot as plt
import numpy as np

def plot_paper_bar_chart():
    # ==========================================
    # 1. 自定义数据区域 (您可以根据实际测试结果修改这里)
    # ==========================================
    # X轴的两组场景
    groups = ['Mixed-1K', 'Mixed-8K']
    
    # 两个系统的吞吐量数据 (单位: e.g., KOPS / MB/s)
    # 这里的列表长度必须和 groups 的长度一致
    rocksdb_throughput = [120.5, 85.2]   # RocksDB 在 Mixed-1K 和 Mixed-8K 的吞吐
    terarkdb_throughput = [165.8, 110.4] # TerarkDB 在 Mixed-1K 和 Mixed-8K 的吞吐
    
    # Y轴标签
    y_label = 'Throughput (KOPS)'
    
    # ==========================================
    # 2. 全局样式设置 (符合学术论文规范)
    # ==========================================
    # 设置字体为 Times New Roman 或默认的 serif，更具学术感
    plt.rcParams['font.family'] = 'serif'
    plt.rcParams['font.size'] = 14
    plt.rcParams['axes.labelsize'] = 16
    plt.rcParams['xtick.labelsize'] = 14
    plt.rcParams['ytick.labelsize'] = 14
    plt.rcParams['legend.fontsize'] = 14

    # 创建画布 (宽 7 英寸, 高 5 英寸)
    fig, ax = plt.subplots(figsize=(7, 5))
    
    # ==========================================
    # 3. 绘制柱状图
    # ==========================================
    x = np.arange(len(groups))  # 组的X轴位置: [0, 1]
    width = 0.3  # 柱子的宽度
    
    # 绘制 RocksDB (带斜线纹理)
    rects1 = ax.bar(x - width/2, rocksdb_throughput, width, 
                    label='RocksDB', 
                    color='#4C72B0',      # 沉稳的蓝色
                    edgecolor='black',    # 黑色边框
                    hatch='//',           # 添加斜线纹理，黑白打印友好
                    zorder=3)             # 确保柱子在网格线之上
    
    # 绘制 TerarkDB (带点状纹理)
    rects2 = ax.bar(x + width/2, terarkdb_throughput, width, 
                    label='TerarkDB', 
                    color='#DD8452',      # 沉稳的橙色
                    edgecolor='black', 
                    hatch='..', 
                    zorder=3)

    # ==========================================
    # 4. 图表修饰与布局
    # ==========================================
    # 设置Y轴标签
    ax.set_ylabel(y_label, fontweight='bold')
    
    # 设置X轴刻度和标签
    ax.set_xticks(x)
    ax.set_xticklabels(groups, fontweight='bold')
    
    # 设置Y轴下限从0开始，上限留出15%的空间放图例
    ax.set_ylim(0, max(max(rocksdb_throughput), max(terarkdb_throughput)) * 1.2)
    
    # 添加图例 (放在右上角，去掉图例的边框使其更干净)
    ax.legend(loc='upper right', frameon=False)
    
    # 添加水平网格线 (虚线，透明度低一点)，便于对齐数值
    ax.grid(axis='y', linestyle='--', alpha=0.7, zorder=0)
    
    # 去掉上方和右侧的边框线条 (学术图表通常不需要)
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    
    # （可选）在柱子上显示具体数值
    def autolabel(rects):
        for rect in rects:
            height = rect.get_height()
            ax.annotate(f'{height}',
                        xy=(rect.get_x() + rect.get_width() / 2, height),
                        xytext=(0, 3),  # 垂直偏移3个点
                        textcoords="offset points",
                        ha='center', va='bottom',
                        fontsize=12)
            
    autolabel(rects1)
    autolabel(rects2)

    # 自动调整子图参数，使之填充整个图像区域
    fig.tight_layout()

    # ==========================================
    # 5. 保存与展示
    # ==========================================
    # 保存为高分辨率的 PDF (推荐用于LaTeX) 和 PNG
    plt.savefig('throughput_comparison.pdf', dpi=300, bbox_inches='tight')
    plt.savefig('throughput_comparison.png', dpi=300, bbox_inches='tight')
    print("图表已成功保存为 throughput_comparison.pdf 和 throughput_comparison.png")

if __name__ == '__main__':
    plot_paper_bar_chart()