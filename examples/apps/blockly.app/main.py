# -*- coding: utf-8 -*-
# 积木编辑器 - XiaoMiao Desktop (MicroPython 版)
# 将积木序列转换为MicroPython代码并执行。
# 使用 xiaomiao 模块驱动真机屏幕与按键。
#
# 模式：
#   0 = 分类选择   (左右键在分类栏切换)
#   1 = 积木选择   (上下键选积木，A键添加)
#   2 = 程序编辑   (上下键选程序项，A键菜单)
#   3 = 参数编辑   (上下键调参，A键确认)
#   4 = 菜单       (上下键选择，A键执行)
#
# 按键：
#   UP/DOWN     选择/调参
#   LEFT/RIGHT  切换面板
#   A           确认/添加/菜单
#   B           返回/退出

import xiaomiao

# ========== 常量 ==========
W = xiaomiao.width()     # 160
H = xiaomiao.height()    # 128
TOP = 10

# 分类颜色
CAT_COL = [
    0xE64B3C,  # 0 事件
    0x22C55E,  # 1 运动
    0x3B82F6,  # 2 外观
    0xDC2626,  # 3 控制
    0xF59E0B,  # 4 运算
    0x8B5CF6,  # 5 变量
    0x06B6D4,  # 6 声音
]
# 界面颜色
COL_BG = 0x0f172a
COL_SEL_BG = 0x1e293b
COL_HL = 0x22d466
COL_TEXT = 0xe2e8f0
COL_DIM = 0x64748b
COL_WHITE = 0xffffff

# 积木数据：7个分类 × 4个积木
# 每项: (代码模板, 有参数, 默认参数, 参数最小值, 参数最大值)
BLOCKS = [
    # 0 事件
    [
        ("print('启动')", False, 0, 0, 0),      # 当启动时
        ("print('按键')", False, 0, 0, 0),      # 当按键按下
        ("print('消息')", False, 0, 0, 0),      # 当收到消息
        ("while True:", False, 0, 0, 0),         # 重复执行
    ],
    # 1 运动
    [
        ("print('移动{}')", True, 10, 1, 100),      # 移动
        ("print('转向{}')", True, 15, 1, 100),      # 转向
        ("print('随机位置')", False, 0, 0, 0),      # 移到随机
        ("print('滑行{}秒')", True, 1, 1, 10),      # 滑行
    ],
    # 2 外观
    [
        ("print('你好')", False, 0, 0, 0),         # 说你好
        ("print('显示')", False, 0, 0, 0),         # 显示
        ("print('隐藏')", False, 0, 0, 0),         # 隐藏
        ("print('切换造型')", False, 0, 0, 0),     # 切换造型
    ],
    # 3 控制
    [
        ("import time\ntime.sleep({})", True, 1, 1, 60),    # 等待
        ("for _ in range({}):", True, 10, 1, 100),          # 重复
        ("if True:", False, 0, 0, 0),                       # 如果那么
        ("break", False, 0, 0, 0),                          # 停止
    ],
    # 4 运算
    [
        ("print(1+1)", False, 0, 0, 0),         # 加
        ("print(2-1)", False, 0, 0, 0),         # 减
        ("print(2*3)", False, 0, 0, 0),         # 乘
        ("print(7%2)", False, 0, 0, 0),         # 取余
    ],
    # 5 变量
    [
        ("x = 0", False, 0, 0, 0),              # 设变量
        ("x = x + 1\nprint(x)", False, 0, 0, 0),# 变量+1
        ("print(x)", False, 0, 0, 0),           # 显示变量
        ("x = 0", False, 0, 0, 0),              # 清空变量
    ],
    # 6 声音
    [
        ("print('播放音效')", False, 0, 0, 0),  # 播放音效
        ("print('播放旋律')", False, 0, 0, 0),  # 播放旋律
        ("print('静音')", False, 0, 0, 0),      # 静音
        ("print('音量{}')", True, 50, 0, 100),  # 音量
    ],
]

CAT_CNT = 7
BLK_CNT = 4
MAX_PROG = 16

# 菜单项
MENU_ITEMS = [0x22C55E, 0xEF4444, 0x3B82F6, 0xF59E0B]  # 运行/删除/上移/下移颜色

# 面板模式
PANEL_CAT = 0   # 分类/积木选择
PANEL_PROG = 1  # 程序编辑

# ========== 状态变量 ==========
cat_sel = 0        # 当前分类索引
blk_sel = 0        # 当前积木索引
prog_sel = 0       # 程序序列选中项
prog = []          # 程序序列 [(cat, blk, param), ...]
mode = 0           # 见顶部注释
panel = PANEL_CAT  # 当前面板
param_val = 0      # 参数编辑值
param_min = 0
param_max = 0
menu_sel = 0
scroll = 0         # 程序区滚动


def draw_cat_bar():
    """绘制顶部分类栏 - 7个彩色块"""
    bw = W // CAT_CNT
    for i in range(CAT_CNT):
        x = i * bw
        if i == cat_sel:
            xiaomiao.rect_fill(x, 0, bw, TOP, CAT_COL[i])
        else:
            xiaomiao.rect_fill(x, 0, bw, TOP, 0x1e293b)
            # 底部小色条
            xiaomiao.rect_fill(x, TOP - 3, bw, 3, CAT_COL[i])
    # 选中高亮边框
    xiaomiao.rect(cat_sel * bw, 0, bw, TOP, COL_WHITE, False)


def draw_block_list():
    """绘制当前分类的积木列表"""
    y = TOP + 2
    bh = 24
    gap = 2
    for i in range(BLK_CNT):
        color = CAT_COL[cat_sel]
        is_sel = (i == blk_sel)
        # 背景
        if is_sel:
            xiaomiao.rect_fill(2, y, W - 4, bh, COL_SEL_BG)
            xiaomiao.rect(2, y, W - 4, bh, COL_HL, False)
        else:
            xiaomiao.rect_fill(2, y, W - 4, bh, 0x1e293b)
        # 左侧颜色条
        xiaomiao.rect_fill(2, y, 4, bh, color)
        # 序号点
        xiaomiao.rect_fill(10, y + bh // 2 - 2, 4, 4, color)
        # 参数指示（右侧）
        _, has_p, default_p, _, _ = BLOCKS[cat_sel][i]
        if has_p:
            xiaomiao.rect_fill(W - 16, y + 4, 12, bh - 8, 0x475569)
            # 参数值指示条
            pw = int((default_p / 100) * 10)
            if pw < 2: pw = 2
            xiaomiao.rect_fill(W - 14, y + 6, pw, bh - 12, 0x94a3b8)
        # 选中标记
        if is_sel:
            xiaomiao.rect_fill(W - 4, y + 4, 2, bh - 8, COL_HL)
        y += bh + gap


def draw_program():
    """绘制程序序列"""
    xiaomiao.rect_fill(0, TOP, W, H - TOP - 8, COL_BG)
    if not prog:
        # 空程序提示 - 虚线框
        cx = W // 2 - 20
        cy = TOP + (H - TOP - 8) // 2 - 8
        xiaomiao.rect(cx, cy, 40, 16, COL_DIM, False)
        return

    y = TOP + 2
    bh = 16
    gap = 2
    max_v = (H - TOP - 8 - 4) // (bh + gap)

    for i in range(max_v):
        idx = scroll + i
        if idx >= len(prog):
            break
        c, b, p = prog[idx]
        color = CAT_COL[c]
        is_sel = (idx == prog_sel)
        # 背景
        if is_sel:
            xiaomiao.rect_fill(2, y, W - 4, bh, COL_SEL_BG)
            xiaomiao.rect(2, y, W - 4, bh, COL_HL, False)
        else:
            xiaomiao.rect_fill(2, y, W - 4, bh, 0x1e293b)
        # 序号
        xiaomiao.rect_fill(4, y + 2, 6, bh - 4, color)
        # 分类色块
        xiaomiao.rect_fill(14, y + 2, 8, bh - 4, color)
        # 参数指示
        _, has_p, _, _, _ = BLOCKS[c][b]
        if has_p:
            xiaomiao.rect_fill(W - 20, y + 3, 16, bh - 6, 0x475569)
            pw = max(2, int((p / 100) * 12))
            xiaomiao.rect_fill(W - 18, y + 5, pw, bh - 10, 0x94a3b8)
        y += bh + gap


def draw_param():
    """绘制参数编辑界面"""
    xiaomiao.rect_fill(0, TOP, W, H - TOP - 8, COL_BG)
    # 进度条
    bx = 10
    by = TOP + 30
    bw = W - 20
    bh = 24
    xiaomiao.rect_fill(bx, by, bw, bh, 0x334155)
    if param_max > param_min:
        ratio = (param_val - param_min) / (param_max - param_min)
        fw = max(4, int(bw * ratio))
    else:
        fw = bw // 2
    xiaomiao.rect_fill(bx, by, fw, bh, COL_HL)
    xiaomiao.rect(bx, by, bw, bh, COL_WHITE, False)
    # 上下箭头指示
    ax = W // 2 - 4
    xiaomiao.rect_fill(ax, TOP + 10, 8, 2, COL_DIM)
    xiaomiao.rect_fill(ax + 3, TOP + 12, 2, 6, COL_DIM)
    xiaomiao.rect_fill(ax + 3, TOP + 4, 2, 6, COL_DIM)


def draw_menu():
    """绘制菜单"""
    y = TOP + 4
    ih = 22
    gap = 2
    colors = MENU_ITEMS
    for i in range(4):
        is_sel = (i == menu_sel)
        if is_sel:
            xiaomiao.rect_fill(4, y, W - 8, ih, COL_SEL_BG)
            xiaomiao.rect(4, y, W - 8, ih, COL_HL, False)
        else:
            xiaomiao.rect_fill(4, y, W - 8, ih, 0x1e293b)
        # 左侧色条
        xiaomiao.rect_fill(4, y, 4, ih, colors[i])
        # 选中三角
        if is_sel:
            xiaomiao.rect_fill(W - 8, y + 4, 4, ih - 8, COL_HL)
        y += ih + gap


def gen_code():
    """生成MicroPython代码"""
    lines = []
    for c, b, p in prog:
        tpl, has_p, _, _, _ = BLOCKS[c][b]
        if has_p:
            lines.append(tpl.format(p))
        else:
            lines.append(tpl)
    if not lines:
        lines.append("print('空程序')")
    return "\n".join(lines)


def run_program():
    """执行生成的代码"""
    code = gen_code()
    xiaomiao.fill(0x0f172a)
    xiaomiao.rect_fill(0, 0, W, TOP, 0x22C55E)
    xiaomiao.show()
    try:
        exec(code)
    except Exception as e:
        xiaomiao.fill(0x0f172a)
        xiaomiao.rect_fill(0, 0, W, TOP, 0xEF4444)
        xiaomiao.show()
        xiaomiao.sleep_ms(1000)


# ========== 主循环 ==========

def main():
    global cat_sel, blk_sel, prog_sel, prog, mode, panel
    global param_val, param_min, param_max, menu_sel, scroll

    xiaomiao.init()
    prog = []

    while True:
        k = xiaomiao.get_key()

        # ===== 参数编辑模式 =====
        if mode == 3:
            if k == xiaomiao.KEY_UP:
                param_val += 1
                if param_val > param_max: param_val = param_max
            elif k == xiaomiao.KEY_DOWN:
                param_val -= 1
                if param_val < param_min: param_val = param_min
            elif k == xiaomiao.KEY_A:
                if prog_sel < len(prog):
                    c, b, _ = prog[prog_sel]
                    prog[prog_sel] = (c, b, param_val)
                mode = 2
                panel = PANEL_PROG
            elif k == xiaomiao.KEY_B:
                mode = 2
                panel = PANEL_PROG

        # ===== 菜单模式 =====
        elif mode == 4:
            if k == xiaomiao.KEY_UP:
                menu_sel = (menu_sel - 1) % 4
            elif k == xiaomiao.KEY_DOWN:
                menu_sel = (menu_sel + 1) % 4
            elif k == xiaomiao.KEY_A:
                s = menu_sel
                mode = 2
                panel = PANEL_PROG
                if s == 0:  # 运行
                    run_program()
                    xiaomiao.init()
                elif s == 1 and prog:  # 删除
                    prog.pop(prog_sel)
                    if prog_sel >= len(prog) and prog:
                        prog_sel = len(prog) - 1
                    if scroll > 0 and prog_sel < scroll:
                        scroll = max(0, scroll - 1)
                elif s == 2 and len(prog) > 1 and prog_sel > 0:  # 上移
                    prog[prog_sel], prog[prog_sel - 1] = prog[prog_sel - 1], prog[prog_sel]
                    prog_sel -= 1
                    if prog_sel < scroll:
                        scroll = max(0, scroll - 1)
                elif s == 3 and len(prog) > 1 and prog_sel < len(prog) - 1:  # 下移
                    prog[prog_sel], prog[prog_sel + 1] = prog[prog_sel + 1], prog[prog_sel]
                    prog_sel += 1
                    max_v = (H - TOP - 8 - 4) // 18
                    if prog_sel >= scroll + max_v:
                        scroll = min(len(prog) - max_v, scroll + 1)
            elif k == xiaomiao.KEY_B:
                mode = 2
                panel = PANEL_PROG

        # ===== 程序编辑模式 =====
        elif mode == 2:
            if not prog:
                mode = 0
                panel = PANEL_CAT
                continue
            if k == xiaomiao.KEY_UP:
                prog_sel = (prog_sel - 1) % len(prog)
                max_v = (H - TOP - 8 - 4) // 18
                if prog_sel < scroll:
                    scroll = max(0, scroll - 1)
            elif k == xiaomiao.KEY_DOWN:
                prog_sel = (prog_sel + 1) % len(prog)
                max_v = (H - TOP - 8 - 4) // 18
                if prog_sel >= scroll + max_v:
                    scroll = min(len(prog) - max_v, scroll + 1)
            elif k == xiaomiao.KEY_LEFT:
                panel = PANEL_CAT
                mode = 0
            elif k == xiaomiao.KEY_RIGHT:
                c, b, _ = prog[prog_sel]
                _, has_p, _, _, _ = BLOCKS[c][b]
                if has_p:
                    param_val = prog[prog_sel][2]
                    param_min = BLOCKS[c][b][3]
                    param_max = BLOCKS[c][b][4]
                    mode = 3
            elif k == xiaomiao.KEY_A:
                mode = 4
                menu_sel = 0
            elif k == xiaomiao.KEY_B:
                return  # 退出应用，系统处理返回

        # ===== 分类/积木选择模式 =====
        else:
            if k == xiaomiao.KEY_LEFT:
                if panel == PANEL_CAT:
                    if len(prog) > 0:
                        panel = PANEL_PROG
                        mode = 2
                        prog_sel = min(prog_sel, len(prog) - 1)
                else:
                    panel = PANEL_CAT
            elif k == xiaomiao.KEY_RIGHT:
                if panel == PANEL_CAT:
                    if len(prog) > 0:
                        panel = PANEL_PROG
                        mode = 2
                else:
                    panel = PANEL_CAT
            elif k == xiaomiao.KEY_UP:
                if blk_sel > 0:
                    blk_sel -= 1
                else:
                    if cat_sel > 0:
                        cat_sel -= 1
                        blk_sel = BLK_CNT - 1
                    else:
                        cat_sel = CAT_CNT - 1
                        blk_sel = BLK_CNT - 1
            elif k == xiaomiao.KEY_DOWN:
                if blk_sel < BLK_CNT - 1:
                    blk_sel += 1
                else:
                    if cat_sel < CAT_CNT - 1:
                        cat_sel += 1
                        blk_sel = 0
                    else:
                        cat_sel = 0
                        blk_sel = 0
            elif k == xiaomiao.KEY_A:
                if len(prog) < MAX_PROG:
                    _, has_p, default_p, pmin, pmax = BLOCKS[cat_sel][blk_sel]
                    if has_p:
                        prog.append((cat_sel, blk_sel, default_p))
                        # 进入参数编辑
                        prog_sel = len(prog) - 1
                        param_val = default_p
                        param_min = pmin
                        param_max = pmax
                        mode = 3
                    else:
                        prog.append((cat_sel, blk_sel, 0))
                        prog_sel = len(prog) - 1
                        # 自动切换到程序面板
                        panel = PANEL_PROG
                        mode = 2
            elif k == xiaomiao.KEY_B:
                return  # 退出应用，系统处理返回

        # ===== 绘制画面 =====
        if mode == 3:
            draw_param()
        elif mode == 4:
            draw_menu()
        elif mode == 2:
            draw_program()
        else:
            if panel == PANEL_CAT:
                draw_cat_bar()
                draw_block_list()
            else:
                draw_program()

        xiaomiao.show()
        xiaomiao.sleep_ms(30)


main()