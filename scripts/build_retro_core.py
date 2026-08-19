#!/usr/bin/env python3
"""
Build retro-core FAT partition image with subsetted NotoSansSC font.

This script is designed to run in GitHub Actions CI (Ubuntu 24.04).
It uses mtools instead of loop mount, so no root/sudo is needed.
Steps:
  1. Download NotoSansSC from Aliyun mirror
  2. Extract Chinese characters used in the project source code
  3. Subset the font using fonttools (keep only used chars)
  4. Create FAT12 partition image with mtools
  5. Output the retro-core image path

Usage:
  python3 scripts/build_retro_core.py [--project-dir /path/to/project]
"""

import os, sys, subprocess, re, glob, argparse, tempfile, shutil

# ============================================================
# Configuration
# ============================================================
FONT_URL = "https://mirrors.aliyun.com/github/releases/googlefonts/noto-cjk/Sans2.004/18_NotoSansSC.zip"
FONT_ZIP = "NotoSansSC.zip"
FONT_OTF = "NotoSansSC-Regular.otf"
FONT_SUBSET = "NotoSansSC-Regular.subset.otf"

RETRO_CORE_SIZE = 0x120000  # 1,179,648 bytes (1.125MB)
RETRO_CORE_IMG = "retro-core.img"
FONT_DST_DIR = "Fonts"
FONT_DST_NAME = "NotoSansSC-Regular.otf"  # 最终在 FAT 映像中的文件名

# LVGL common Chinese characters + symbols
LVGL_COMMON = (
    '的一是不了人我在有他这之来中大上们个到说时要去子就也那下看过可吗没得还'
    '小多后都好对她会家学年日时分钟秒月年周星期上午下午今天昨天明天现在当前'
    '设置返回确认取消保存退出打开关闭播放暂停停止继续上一曲下一首单曲循环列表'
    '随机音量进度加载文件错误成功失败完成等待处理中请输入选择搜索连接断开开启'
    '关闭开关启用禁用自动手动模式明亮黑暗主题字体大小默认应用系统信息关于版本'
    '号语言英文中文简体繁体网络WiFi密码账号刷新扫描'
)

# 项目中需要扫描的扩展名
SRC_EXTENSIONS = ('.c', '.h', '.py', '.json', '.md', '.txt', '.yml', '.yaml', '.cfg', '.conf')

# 需要排除的目录
EXCLUDE_DIRS = {'.git', 'micropython', 'build', 'managed_components'}


def extract_chinese_chars(project_dir: str) -> str:
    """从项目源码中提取所有中文字符"""
    chars = set()
    
    for root, dirs, files in os.walk(project_dir):
        # 跳过排除目录
        rel_root = os.path.relpath(root, project_dir)
        if any(part in EXCLUDE_DIRS for part in rel_root.split(os.sep)):
            continue
        
        for f in files:
            if not f.endswith(SRC_EXTENSIONS):
                continue
            fp = os.path.join(root, f)
            try:
                with open(fp, 'r', errors='replace') as fh:
                    content = fh.read()
                for ch in content:
                    cp = ord(ch)
                    # CJK Unified Ideographs + CJK Symbols + Fullwidth Forms
                    if (0x4E00 <= cp <= 0x9FFF) or \
                       (0x3000 <= cp <= 0x303F) or \
                       (0xFF00 <= cp <= 0xFFEF):
                        chars.add(ch)
            except Exception:
                pass
    
    # 添加 LVGL 常用中文
    chars.update(LVGL_COMMON)
    
    # 添加数字、英文、符号
    ascii_chars = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'
    symbols = '!@#$%^&*()_+-=[]{}|;:,.<>?/~`\'\" \n\t'
    chars.update(ascii_chars + symbols)
    
    # LVGL 图标符号
    icon_chars = '▶●○←→↑↓✓✗⚡☀☁★☆♪♫♬'
    chars.update(icon_chars)
    
    return ''.join(sorted(chars))


def ensure_fonttools():
    """确保 fonttools 已安装"""
    try:
        import fontTools
    except ImportError:
        print("Installing fonttools...")
        subprocess.run([sys.executable, '-m', 'pip', 'install', 'fonttools'],
                       check=True, capture_output=True)


def download_font(work_dir: str):
    """从阿里云镜像下载 NotoSansSC 字体包"""
    zip_path = os.path.join(work_dir, FONT_ZIP)
    if os.path.exists(zip_path):
        print(f"Font zip already exists: {zip_path}, skipping download")
    else:
        print(f"Downloading NotoSansSC from Aliyun mirror...")
        subprocess.run([
            'curl', '-s', '-L', '-o', zip_path,
            '-H', 'User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36',
            '-H', 'Referer: https://mirrors.aliyun.com/',
            FONT_URL
        ], check=True, timeout=120)
        print(f"Downloaded: {os.path.getsize(zip_path)} bytes")
    
    # 解压 OTF 字体
    font_otf = os.path.join(work_dir, FONT_OTF)
    if not os.path.exists(font_otf):
        print("Extracting OTF font...")
        subprocess.run(['unzip', '-o', zip_path, FONT_OTF, '-d', work_dir],
                       check=True, capture_output=True)
        print(f"Extracted: {os.path.getsize(font_otf)} bytes")
    else:
        print(f"OTF font already exists: {font_otf}")
    
    return font_otf


def subset_font(font_otf: str, chars: str, output_path: str):
    """使用 fonttools 子集化字体"""
    print(f"Subsetting font to {len(chars)} characters...")
    
    from fontTools.subset import Subsetter, Options
    from fontTools.ttLib import TTFont
    
    opts = Options()
    opts.flavor = None  # 保持 OTF
    opts.layout_features = ['*']
    opts.name_IDs = ['*']
    opts.name_languages = ['*']
    opts.notdef_outline = True
    opts.recalc_bounds = True
    opts.recalc_timestamp = False
    opts.canonical_order = True
    
    font = TTFont(font_otf)
    subsetter = Subsetter(options=opts)
    subsetter.populate(unicodes=[ord(c) for c in chars])
    subsetter.subset(font)
    font.save(output_path)
    
    size = os.path.getsize(output_path)
    print(f"Subset font saved: {size/1024:.1f} KB ({size} bytes)")
    return output_path


def create_retro_core_image(work_dir: str, font_subset: str) -> str:
    """
    使用 mtools 创建 FAT12 分区映像并将字体放入其中。
    mtools 不需要 loop 设备，可在 CI 中正常工作。
    """
    img_path = os.path.join(work_dir, RETRO_CORE_IMG)
    font_dst = f"{FONT_DST_DIR}/{FONT_DST_NAME}"
    
    # 1. 创建空映像
    print(f"Creating FAT12 image: {RETRO_CORE_SIZE} bytes...")
    with open(img_path, 'wb') as f:
        f.write(b'\x00' * RETRO_CORE_SIZE)
    
    # 2. 格式化为 FAT12
    print("Formatting as FAT12...")
    result = subprocess.run(
        ['mkfs.fat', '-F', '12', '-n', 'RETRO_CORE', img_path],
        capture_output=True, text=True, timeout=30
    )
    if result.returncode != 0:
        print(f"mkfs.fat error: {result.stderr}")
        # 尝试 FAT16 作为备选
        print("Retrying with FAT16...")
        subprocess.run(
            ['mkfs.fat', '-F', '16', '-n', 'RETRO_CORE', img_path],
            check=True, capture_output=True, timeout=30
        )
    
    # 3. 使用 mtools 创建目录并复制字体
    print("Copying font to image with mtools...")
    # 设置 mtools 使用的驱动器
    env = os.environ.copy()
    env['MTOOLS_SKIP_CHECK'] = '1'
    
    # 创建 Fonts 目录
    subprocess.run(
        ['mmd', '-i', img_path, f'::{FONT_DST_DIR}'],
        check=True, capture_output=True, timeout=15, env=env
    )
    print(f"  Created directory: {FONT_DST_DIR}")
    
    # 复制字体文件（DEST 名称不带 subset 后缀，与固件代码约定一致）
    subprocess.run(
        ['mcopy', '-i', img_path, font_subset, f'::{font_dst}'],
        check=True, capture_output=True, timeout=15, env=env
    )
    print(f"  Copied font: {font_dst}")
    
    # 4. 验证
    result = subprocess.run(
        ['mdir', '-i', img_path, '::'],
        capture_output=True, text=True, timeout=15, env=env
    )
    print(f"  Image contents:\n{result.stdout}")
    
    # 5. 检查文件大小
    result = subprocess.run(
        ['minfo', '-i', img_path],
        capture_output=True, text=True, timeout=15, env=env
    )
    for line in result.stdout.split('\n'):
        if 'sectors' in line.lower() or 'free' in line.lower() or 'size' in line.lower():
            print(f"  {line.strip()}")
    
    img_size = os.path.getsize(img_path)
    print(f"Retro-core image: {img_size} bytes ({img_size/1024:.1f} KB)")
    return img_path


def merge_bin(work_dir: str, retro_img: str):
    """
    合并 bootloader + partition-table + app + retro-core 为完整 bin。
    使用 esptool.py merge_bin。
    输出为 xiaomiao-desktop-full.bin（区别于 xiaomiao-loader 用的合并 bin）。
    """
    # 构建产物路径
    build_dir = os.path.join(work_dir, 'build')
    bootloader = os.path.join(build_dir, 'bootloader', 'bootloader.bin')
    partition_table = os.path.join(build_dir, 'partition_table', 'partition-table.bin')
    app_bin = os.path.join(build_dir, 'xiaomiao-desktop.bin')
    output = os.path.join(work_dir, 'xiaomiao-desktop-full.bin')
    
    # 检查所有文件是否存在
    files_to_check = [
        (bootloader, "bootloader.bin"),
        (partition_table, "partition-table.bin"),
        (app_bin, "xiaomiao-desktop.bin"),
        (retro_img, "retro-core.img"),
    ]
    for path, name in files_to_check:
        if not os.path.exists(path):
            print(f"WARNING: {name} not found at {path}")
            return None
    
    # 合并
    print("Merging binaries...")
    cmd = [
        sys.executable, '-m', 'esptool', '--chip', 'esp32', 'merge_bin',
        '--flash_mode', 'qio',
        '--flash_freq', '80m',
        '--flash_size', '4MB',
        '--output', output,
        '0x1000', bootloader,
        '0x8000', partition_table,
        '0xA0000', app_bin,
        '0x2E0000', retro_img,
    ]
    subprocess.run(cmd, check=True, timeout=120)
    
    size = os.path.getsize(output)
    print(f"Merged binary: {size/1024:.1f} KB ({size} bytes)")
    return output


def main():
    parser = argparse.ArgumentParser(description='Build retro-core partition with font')
    parser.add_argument('--project-dir', default=os.getcwd(),
                       help='Project root directory')
    parser.add_argument('--skip-download', action='store_true',
                       help='Skip font download (use existing)')
    parser.add_argument('--skip-merge', action='store_true',
                       help='Skip final merge_bin step')
    args = parser.parse_args()
    
    project_dir = os.path.abspath(args.project_dir)
    work_dir = tempfile.mkdtemp(prefix='retro_core_')
    
    try:
        # Step 1: Extract Chinese characters from source
        print("=" * 60)
        print("Step 1: Extracting Chinese characters from source code...")
        print("=" * 60)
        chars = extract_chinese_chars(project_dir)
        print(f"  Found {len(chars)} unique characters")
        
        # Step 2: Download font
        print("\n" + "=" * 60)
        print("Step 2: Downloading NotoSansSC font...")
        print("=" * 60)
        if args.skip_download and os.path.exists(os.path.join(project_dir, FONT_OTF)):
            font_otf = os.path.join(project_dir, FONT_OTF)
        else:
            font_otf = download_font(work_dir)
        
        # Step 3: Subset font
        print("\n" + "=" * 60)
        print("Step 3: Subsetting font...")
        print("=" * 60)
        ensure_fonttools()
        font_subset = os.path.join(work_dir, FONT_SUBSET)
        subset_font(font_otf, chars, font_subset)
        
        # Step 4: Create retro-core FAT image
        print("\n" + "=" * 60)
        print("Step 4: Creating retro-core FAT image...")
        print("=" * 60)
        retro_img = create_retro_core_image(work_dir, font_subset)
        
        # Copy the image to project dir
        final_img = os.path.join(project_dir, RETRO_CORE_IMG)
        shutil.copy2(retro_img, final_img)
        print(f"\nRetro-core image saved to: {final_img}")
        
        # Step 5: Merge binaries
        if not args.skip_merge:
            print("\n" + "=" * 60)
            print("Step 5: Merging binaries...")
            print("=" * 60)
            merged = merge_bin(project_dir, final_img)
            if merged:
                print(f"\n✅ Final merged binary: {merged}")
        else:
            print("\n⏭️  Skipping merge (--skip-merge)")
        
        print(f"\n✅ All done! Working directory: {work_dir}")
        return 0
    
    except Exception as e:
        print(f"\n❌ Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 1
    finally:
        # Cleanup temp dir
        try:
            shutil.rmtree(work_dir, ignore_errors=True)
        except Exception:
            pass


if __name__ == '__main__':
    sys.exit(main())