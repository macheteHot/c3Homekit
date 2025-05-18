import os
import subprocess

buildDir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build")
output_bin = os.path.join(buildDir, "c3Homekit_merged.bin")

nvs_bin = os.path.join(buildDir, "nvs.bin")
phy_init_bin = os.path.join(buildDir, "phy_init.bin")
bootloader_bin = os.path.join(buildDir, "bootloader/bootloader.bin")
partition_table_bin = os.path.join(
    buildDir, "partition_table/partition-table.bin")
app_bin = os.path.join(buildDir, "c3Homekit.bin")

# 检查 nvs.bin 和 phy_init.bin 是否存在
parts = [
    ("0x0", bootloader_bin),
    ("0x8000", partition_table_bin),
]
if os.path.exists(nvs_bin):
    parts.append(("0x9000", nvs_bin))
if os.path.exists(phy_init_bin):
    parts.append(("0xf000", phy_init_bin))
parts.append(("0x10000", app_bin))

# 检查所有必需的 bin 文件是否存在
for addr, binfile in parts:
    if not os.path.exists(binfile):
        print(f"[警告] 文件不存在: {binfile} (地址 {addr})，将跳过该分区。")

# 只合并存在的 bin 文件
merge_args = []
for addr, binfile in parts:
    if os.path.exists(binfile):
        merge_args.extend([addr, binfile])

if not merge_args:
    print("未找到任何可合并的 bin 文件，退出。")
    exit(1)

cmd = [
    "esptool.py", "--chip", "esp32c3", "merge_bin", "-o", output_bin
] + merge_args

print("运行命令:")
print(" ", " ".join(cmd))

try:
    subprocess.check_call(cmd)
    print(f"合并完成: {output_bin}")
except subprocess.CalledProcessError as e:
    print(f"合并失败: {e}")
    exit(1)
