#!/usr/bin/env python3
"""验证 LED 音乐律动：查询/设置模式、播放音乐时 live_peak 数据。"""
import serial, time, json

PORT = 'COM6'


def main():
    ser = serial.Serial(PORT, 115200, timeout=0.2)
    ser.reset_input_buffer()

    def send(cmd, params=None, cid=1, wait=12):
        o = {'cmd': cmd, 'id': cid}
        if params is not None:
            o['params'] = params
        ser.write((('XTC:' + json.dumps(o, ensure_ascii=False, separators=(',', ':')) + '\n').encode()))
        ser.flush()
        buf = ''
        t0 = time.time()
        while time.time() - t0 < wait:
            if ser.in_waiting:
                buf += ser.read(ser.in_waiting).decode('utf-8', 'replace')
            for line in buf.splitlines():
                if line.startswith('XTR:') and (f'"id":{cid}' in line):
                    try:
                        return json.loads(line[4:])
                    except Exception:
                        pass
            time.sleep(0.04)
        return None

    # PING 握手（WiFi 重试窗口可能阻塞命令）
    ok = False
    for k in range(8):
        if send(0, cid=k + 1, wait=4):
            print(f'PING OK (attempt {k})')
            ok = True
            break
        time.sleep(1)
    if not ok:
        print('PING FAIL')
        ser.close()
        return

    # 1. 查询初始状态
    r = send(17, cid=10)
    print('1) initial:', json.dumps(r, ensure_ascii=False) if r else 'TIMEOUT')

    # 2. 设置模式=1(单色律动), 颜色=4(青)
    r = send(17, {'op': 'set_mode', 'mode': 1}, cid=11)
    print('2) set_mode(1):', r.get('status') if r else 'TIMEOUT')
    r = send(17, {'op': 'set_color', 'color': 4}, cid=12)
    print('   set_color(4):', r.get('status') if r else 'TIMEOUT')

    # 3. 再查询确认
    r = send(17, cid=13)
    print('3) after set:', json.dumps(r, ensure_ascii=False) if r else 'TIMEOUT')

    # 4. 播放一首歌（先重扫，MUSIC_CONTROL op=2 index=0）
    send(16, {'op': 1}, cid=14, wait=12)
    time.sleep(1)
    st = send(15, cid=15, wait=12)
    count = st['data'].get('count') if st and 'data' in st else 0
    print(f'4) music count={count}')
    if count and count > 0:
        send(16, {'op': 2, 'index': 0}, cid=16, wait=4)
        # 采样 live_peak 若干次（等待解码开始）
        peaks = []
        for i in range(8):
            time.sleep(0.6)
            rr = send(17, cid=100 + i, wait=6)
            if rr and 'data' in rr:
                d = rr['data']
                peaks.append(d.get('live_peak', 0))
                print(f'   t={0.6*(i+1):.1f}s mode={d.get("mode")} active={d.get("active")} live_peak={d.get("live_peak")}')
        print('   peak samples:', peaks, 'max=', max(peaks) if peaks else 0)
    else:
        print('   no music, skip playback test')

    # 5. 停止播放，确认 live_peak 归零
    send(16, {'op': 3}, cid=200, wait=6)
    time.sleep(1)
    r = send(17, cid=201, wait=8)
    print('5) after stop:', json.dumps(r, ensure_ascii=False) if r else 'TIMEOUT')

    # 6. 恢复默认（关闭律动）
    send(17, {'op': 'set_mode', 'mode': 0}, cid=202, wait=8)
    r = send(17, cid=203, wait=8)
    print('6) restored:', json.dumps(r, ensure_ascii=False) if r else 'TIMEOUT')

    ser.close()


if __name__ == '__main__':
    main()
