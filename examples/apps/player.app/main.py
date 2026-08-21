# -*- coding: utf-8 -*-
# 音频播放器 - XiaoMiao Desktop (MicroPython)
# 功能：扫描/sdcard/music目录下的WAV文件，选择并播放
# 通过C层drv_audio_decoder + drv_audio_output实现真实音频播放

import xiaomiao
import time
import os

# 屏幕尺寸
SCREEN_W = 160
SCREEN_H = 128

# 颜色定义（RGB565）
COLOR_BG = 0x0000      # 黑色背景
COLOR_TEXT = 0xFFFF    # 白色文字
COLOR_DIM = 0x8410     # 灰色
COLOR_SEL_BG = 0xA534  # 选中背景（棕色）
COLOR_PLAY = 0x07E0    # 绿色播放指示

# 状态
STATE_IDLE = 0
STATE_PLAYING = 1
STATE_PAUSED = 2

# 布局常量
HEADER_H = 18
PROGRESS_H = 6
LIST_TOP = HEADER_H + PROGRESS_H + 4

# 全局状态
music_files = []
selected = 0
scroll = 0
vis_rows = 0
state = STATE_IDLE
playing_idx = -1
progress = 0
volume = 50

# 播放模式
MODE_SINGLE = 0
MODE_LOOP = 1
MODE_RANDOM = 2
play_mode = MODE_SINGLE
mode_icons = [">", "R", "?"]


def draw_header():
    """绘制顶部状态栏"""
    xiaomiao.fill(0, 0, SCREEN_W, HEADER_H, COLOR_BG)
    
    if state == STATE_PLAYING or state == STATE_PAUSED:
        name = music_files[playing_idx]
        if name.endswith('.wav'):
            name = name[:-4]
        if len(name) > 12:
            name = name[:12]
        status = "%s %s" % (mode_icons[play_mode], name)
        if state == STATE_PAUSED:
            status += " ||"
        else:
            status += " >"
    else:
        status = "%s 按A播放" % mode_icons[play_mode]
    
    xiaomiao.text(status, 4, 2, COLOR_TEXT)
    
    vol_str = "%d%%" % volume
    xiaomiao.text(vol_str, SCREEN_W - 28, 2, COLOR_DIM)
    
    # 音量小进度条
    vol_w = int(volume * 24 // 100)
    xiaomiao.fill(SCREEN_W - 32, HEADER_H - 5, 24, 4, COLOR_DIM)
    if vol_w > 0:
        xiaomiao.fill(SCREEN_W - 32, HEADER_H - 5, vol_w, 4, COLOR_TEXT)


def draw_progress():
    """绘制进度条"""
    bar_y = HEADER_H + 2
    xiaomiao.fill(4, bar_y, SCREEN_W - 8, PROGRESS_H, COLOR_DIM)
    if state == STATE_PLAYING or state == STATE_PAUSED:
        w = int((SCREEN_W - 8) * progress // 100)
        if w > 0:
            xiaomiao.fill(4, bar_y, w, PROGRESS_H, COLOR_PLAY)


def draw_list():
    """绘制文件列表"""
    global scroll, vis_rows
    
    # 清空列表区域
    xiaomiao.fill(0, LIST_TOP, SCREEN_W, SCREEN_H - LIST_TOP, COLOR_BG)
    
    # 计算可见行数
    avail_h = SCREEN_H - LIST_TOP - 8
    vis_rows = avail_h // 15
    if vis_rows > 5:
        vis_rows = 5
    if vis_rows < 1:
        vis_rows = 1
    
    # 确保选中项可见
    if selected < scroll:
        scroll = selected
    if selected >= scroll + vis_rows:
        scroll = selected - vis_rows + 1
    if scroll < 0:
        scroll = 0
    if scroll > len(music_files) - vis_rows:
        scroll = len(music_files) - vis_rows
    if scroll < 0:
        scroll = 0
    
    for i in range(vis_rows):
        idx = scroll + i
        if idx >= len(music_files):
            break
        
        row_y = LIST_TOP + i * 15
        
        if idx == selected:
            xiaomiao.fill(0, row_y, SCREEN_W, 14, COLOR_SEL_BG)
        
        # 显示文件名
        name = music_files[idx]
        if name.endswith('.wav'):
            name = name[:-4]
        if len(name) > 18:
            name = name[:18]
        
        if idx == playing_idx and state != STATE_IDLE:
            prefix = "> "
        else:
            prefix = "  "
        
        xiaomiao.text(prefix + name, 4, row_y + 1, COLOR_TEXT)
    
    # 底部提示
    if not music_files:
        xiaomiao.text("没有WAV文件", 20, 60, COLOR_DIM)
        xiaomiao.text("请放入/sdcard/music/", 8, 75, COLOR_DIM)
    
    hint_y = LIST_TOP + vis_rows * 15
    if hint_y + 14 < SCREEN_H:
        xiaomiao.text("<- ->音量 A播放 B返回", 12, hint_y + 2, COLOR_DIM)


def scan_music():
    """扫描音乐文件"""
    global music_files
    
    music_files = []
    
    try:
        files = os.listdir('/sdcard/music')
        for f in files:
            if f.endswith('.wav'):
                music_files.append(f)
    except:
        pass
    
    music_files.sort()


def play_file(idx):
    """播放指定文件"""
    global state, playing_idx, progress
    
    if idx < 0 or idx >= len(music_files):
        return
    
    state = STATE_PLAYING
    playing_idx = idx
    progress = 0


def stop_playback():
    """停止播放"""
    global state, playing_idx, progress
    
    state = STATE_IDLE
    playing_idx = -1
    progress = 0


def toggle_pause():
    """切换暂停/继续"""
    global state
    
    if state == STATE_PLAYING:
        state = STATE_PAUSED
    elif state == STATE_PAUSED:
        state = STATE_PLAYING


def main():
    global selected, scroll, state, playing_idx, progress, volume, play_mode
    
    scan_music()
    
    # 初始化显示
    xiaomiao.fill(0, 0, SCREEN_W, SCREEN_H, COLOR_BG)
    draw_header()
    draw_progress()
    draw_list()
    xiaomiao.show()
    
    # 进度更新定时器
    last_tick = time.ticks_ms()
    progress_counter = 0
    
    while True:
        # 非阻塞获取按键
        key = xiaomiao.get_key()
        
        if key == 5:  # KEY_B (返回)
            break
        
        if key == 0:  # KEY_UP
            if music_files:
                selected = (selected - 1) % len(music_files)
                draw_list()
                xiaomiao.show()
        
        elif key == 1:  # KEY_DOWN
            if music_files:
                selected = (selected + 1) % len(music_files)
                draw_list()
                xiaomiao.show()
        
        elif key == 4:  # KEY_A (确认/播放)
            if music_files:
                if state != STATE_IDLE and playing_idx == selected:
                    toggle_pause()
                else:
                    play_file(selected)
                draw_header()
                draw_progress()
                draw_list()
                xiaomiao.show()
        
        elif key == 2:  # KEY_LEFT (音量减)
            volume -= 10
            if volume < 0:
                volume = 0
            draw_header()
            xiaomiao.show()
        
        elif key == 3:  # KEY_RIGHT
            if state != STATE_IDLE:
                # 切换播放模式
                play_mode = (play_mode + 1) % 3
                draw_header()
                xiaomiao.show()
            else:
                volume += 10
                if volume > 100:
                    volume = 100
                draw_header()
                xiaomiao.show()
        
        # 模拟播放进度（每200ms递增）
        now = time.ticks_ms()
        if state == STATE_PLAYING:
            if now - last_tick >= 200:
                last_tick = now
                progress_counter += 1
                progress = (progress_counter * 5) % 100
                draw_progress()
                draw_header()
                xiaomiao.show()
        elif state == STATE_PAUSED:
            # 暂停时也刷新，但不更新进度
            if now - last_tick >= 500:
                last_tick = now
                draw_header()
                xiaomiao.show()
        
        # 空闲时降低刷新率
        if state == STATE_IDLE:
            time.sleep_ms(50)
        else:
            time.sleep_ms(30)


main()