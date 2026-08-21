# -*- coding: utf-8 -*-
# 贪吃蛇游戏 - XiaoMiao Desktop (MicroPython 版)
#
# 通过 xiaomiao 模块驱动真机屏幕与按键：
#   - xiaomiao.fill(color)        清屏
#   - xiaomiao.rect_fill(x,y,w,h,color)  填充矩形
#   - xiaomiao.show()             上屏刷新
#   - xiaomiao.get_key()          非阻塞读按键（-1=无）
#   - xiaomiao.millis()           毫秒时间戳
#   - xiaomiao.sleep_ms(ms)       睡眠
#   - xiaomiao.width()/height()   屏幕尺寸
#
# 按键：KEY_UP=0 KEY_DOWN=1 KEY_LEFT=2 KEY_RIGHT=3 KEY_A=4 KEY_B=5
# 颜色：0xRRGGBB 整数

import xiaomiao

# ========== 常量 ==========
W = xiaomiao.width()
H = xiaomiao.height()

# 游戏区（顶部状态栏下方，底部 dock 上方）
TOP = 12
BOTTOM = H - 8
AREA_H = BOTTOM - TOP
CELL = 4                       # 每格 4px
COLS = W // CELL               # 160/4 = 40
ROWS = AREA_H // CELL          # (128-12-8)/4 = 27

# 颜色
COL_BG = 0x1a1a2e
COL_WALL = 0x16213e
COL_SNAKE = 0x22c55e
COL_HEAD = 0x86efac
COL_FOOD = 0xef4444
COL_TEXT = 0xe2e8f0
COL_OVER = 0xf43f5e

# 方向
DIR_UP = 0
DIR_DOWN = 1
DIR_LEFT = 2
DIR_RIGHT = 3

# ========== 初始化 ==========
xiaomiao.init()
xiaomiao.fill(COL_BG)
xiaomiao.show()


def draw_border():
    """绘制游戏区域边框"""
    xiaomiao.rect(0, TOP, W, AREA_H, COL_WALL, False)


def draw_score(score):
    """顶栏显示分数（简化为格子计数，无字体渲染时用色块顶栏示意）"""
    # 游戏区域顶部已留状态栏，分数通过颜色渐变顶条表示
    pass


def draw_snake(snake, head):
    """绘制蛇身"""
    for i, (cx, cy) in enumerate(snake):
        color = COL_HEAD if i == head else COL_SNAKE
        xiaomiao.rect_fill(cx * CELL, TOP + cy * CELL, CELL - 1, CELL - 1, color)


def draw_food(food):
    """绘制食物"""
    xiaomiao.rect_fill(food[0] * CELL, TOP + food[1] * CELL, CELL, CELL, COL_FOOD)


def new_food(snake):
    """生成不落在蛇身上的新食物"""
    import random
    while True:
        fx = random.randint(0, COLS - 1)
        fy = random.randint(0, ROWS - 1)
        if (fx, fy) not in snake:
            return (fx, fy)


def game_over(score):
    """游戏结束：红色覆盖层 + 返回 True 表示重开，False 表示退出"""
    xiaomiao.rect_fill(0, TOP + AREA_H // 2 - 10, W, 20, COL_OVER)
    xiaomiao.show()
    # 等待按键：A=重开 B=退出（B键由系统处理返回）
    while True:
        k = xiaomiao.get_key()
        if k == xiaomiao.KEY_A:
            return True   # 重开
        if k == xiaomiao.KEY_B:
            return False  # 退出
        xiaomiao.sleep_ms(50)


def game():
    """游戏主循环（用循环替代递归，避免栈溢出）"""
    while True:
        # 蛇初始在中间，向右
        snake = [(COLS // 2, ROWS // 2), (COLS // 2 - 1, ROWS // 2),
                 (COLS // 2 - 2, ROWS // 2)]
        head_idx = 0
        dir_ = DIR_RIGHT
        food = new_food(snake)
        score = 0
        speed_ms = 150
        dead = False

        # 单局游戏循环
        while not dead:
            # 按键处理（非阻塞，每帧读一次）
            k = xiaomiao.get_key()
            if k == xiaomiao.KEY_UP:
                dir_ = DIR_UP
            elif k == xiaomiao.KEY_DOWN:
                dir_ = DIR_DOWN
            elif k == xiaomiao.KEY_LEFT:
                dir_ = DIR_LEFT
            elif k == xiaomiao.KEY_RIGHT:
                dir_ = DIR_RIGHT

            # 计算新头位置
            hx, hy = snake[head_idx]
            if dir_ == DIR_UP:
                hy -= 1
            elif dir_ == DIR_DOWN:
                hy += 1
            elif dir_ == DIR_LEFT:
                hx -= 1
            else:
                hx += 1

            # 移动蛇（新头插入，尾部修剪）
            snake.insert(0, (hx, hy))
            ate = (hx, hy) == food
            if not ate:
                snake.pop()
            else:
                score += 1
                if speed_ms > 60:
                    speed_ms -= 5
                food = new_food(snake)

            # 绘制
            xiaomiao.fill(COL_BG)
            draw_border()
            draw_snake(snake, 0)
            draw_food(food)

            # 碰撞检测（边框/自身）
            if hx < 0 or hx >= COLS or hy < 0 or hy >= ROWS:
                dead = True
            elif (hx, hy) in snake[1:]:
                dead = True

            if not dead:
                # 上屏
                xiaomiao.show()
                # 帧率控制
                xiaomiao.sleep_ms(speed_ms)

        # 游戏结束：显示覆盖层，等待重开或退出
        xiaomiao.show()  # 先显示最后一帧
        restart = game_over(score)
        if not restart:
            return  # B键退出


# 启动
game()