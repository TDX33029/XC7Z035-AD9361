"""480×272 可点击离线参考；不打开串口、网口或启动真实发射。"""
import tkinter as tk
from PIL import ImageTk
import screen_design as sd


class Demo:
    def __init__(self, root):
        self.root, self.d, self.s = root, sd.DEFAULTS.copy(), {}
        self.page, self.entry, self.picker = 0, None, False
        self.power_half = 0
        self.inputbad, self.seq = False, 0
        self.pending, self.last_packet = False, None
        self.fail_next, self.link, self.stopped = False, True, False
        # F7只选择合成模型，不代表板卡实际已标定；默认保持未就绪。
        self.synthetic_model = False
        root.title('P351 最终射频设置 · 480×272 · 离线演示')
        root.resizable(False, False)
        self.canvas = tk.Canvas(root, width=480, height=272, highlightthickness=0)
        self.canvas.pack()
        self.canvas.bind('<Button-1>', lambda e: self.tap(e.x, e.y))
        root.bind('<Escape>', lambda e: root.destroy())
        root.bind('<F8>', lambda e: self.toggle_link())
        root.bind('<F9>', lambda e: self.inject_failure())
        root.bind('<F7>', lambda e: self.toggle_model())
        self.notice = '离线演示 · 等待板端状态'
        self.render()

    def render(self):
        keypad = ((*self.entry, self.power_half) if self.entry and self.entry[0] == 'dpo' else self.entry)
        self.image = ImageTk.PhotoImage(sd.draw(self.page, self.d, self.s, self.notice, keypad, self.picker))
        self.canvas.delete('all')
        self.canvas.create_image(0, 0, anchor='nw', image=self.image)

    def toggle_link(self):
        self.link = not self.link
        if not self.link:
            self.s['valid'] = False
            self.stop()
        self.notice = '模拟链路恢复，需重新应用' if self.link else '模拟断线：安全停发，需重新应用'
        self.render()

    def inject_failure(self):
        self.fail_next = True
        self.notice = '下一次查询将模拟失败'
        self.render()

    def toggle_model(self):
        self.synthetic_model = not self.synthetic_model
        self.s['valid'] = False
        self.notice = ('离线：合成标定模型，可测试成功路径' if self.synthetic_model
                       else '离线：未就绪模型，不发射')
        self.render()

    def stop(self):
        self.seq = self.seq % 65535+1
        self.pending, self.stopped = False, True
        self.last_packet = bytes.fromhex('A5 5A 01 02 FF FF FF')
        self.entry, self.picker = None, False
        self.s['valid'] = False
        self.notice = '模拟STOP已发送；等待读回'

    def tap(self, x, y):
        if not 0 <= x < 480 or not 0 <= y < 272:
            return
        if 384 <= x <= 471 and 3 <= y <= 30:
            self.stop()
        elif y >= 244:
            self.page, self.entry, self.picker = x//sd.NAV_WIDTH, None, False
        elif self.picker:
            for kx, ky, w, h, value, title in sd.PICK_BOXES:
                if kx <= x < kx+w and ky <= y < ky+h:
                    self.d['dc'], self.picker = value, False
                    self.notice = '码型草稿已修改'
                    break
            kx, ky, w, h = sd.PICK_CANCEL
            if kx <= x < kx+w and ky <= y < ky+h:
                self.picker = False
        elif self.entry:
            name, value = self.entry
            for kx, ky, w, h, title in sd.keys_for(name):
                if not (kx <= x < kx+w and ky <= y < ky+h):
                    continue
                if len(title) == 1 and title in '0123456789ABCDEF':
                    radix = 16 if name in sd.HEX_FIELDS else 10
                    if value <= (4095 if radix == 16 else 9999999):
                        value = value*radix+int(title, radix)
                    else:
                        self.inputbad = True
                        self.notice = '输入越界，请清空或退格'
                elif title == '清空':
                    value, self.inputbad = 0, False
                    self.power_half = 0
                elif title == '退格':
                    if name == 'dpo' and self.power_half:
                        self.power_half = 0
                    else:
                        value //= 16 if name in sd.HEX_FIELDS else 10
                    self.inputbad = False
                elif title == '切换 .0/.5':
                    self.power_half = 1-self.power_half
                    self.inputbad = False
                elif title == '取消':
                    self.entry = None
                elif title == '确定':
                    lo, hi = sd.LIMITS[name]
                    target = value*2+self.power_half if name == 'dpo' else value
                    if not self.inputbad and lo <= target <= hi and (name != 'df' or sd.valid_frequency(target)):
                        self.d[name], self.entry = target, None
                        self.notice = '参数草稿已修改'
                    else:
                        self.notice = '参数错误，请核对范围'
                if self.entry:
                    self.entry = (name, value)
                break
        else:
            for kind, name, label, kx, ky, w, h in sd.PAGES[self.page]:
                if not (kx <= x < kx+w and ky <= y < ky+h):
                    continue
                if kind == 'field':
                    self.entry, self.inputbad = (name, self.d[name]), False
                    if name == 'dpo':
                        self.entry = (name, self.d[name]//2)
                        self.power_half = self.d[name] % 2
                elif kind == 'choice' and self.d['dm'] == 0:
                    self.picker = True
                elif kind == 'group':
                    for bx, by, bw, bh, value, title in sd.selection_boxes(name, kx, ky, w, h):
                        if bx <= x < bx+bw and by <= y < by+bh:
                            self.d[name] = value
                            self.notice = '参数草稿已修改'
                            break
                elif kind == 'button':
                    self.action(name)
                break
        self.render()

    def load_snapshot(self):
        s = self.s
        if not s.get('valid'):
            return
        if 100000 <= s.get('rate', 0) <= 20000000:
            self.d['dr'] = s['rate']
        if s.get('source') == 1:
            self.d.update(dc=s['code'], dfr=s['framed'], db=s['fbytes'], dn=s['fcount'], di=s['runflag'])
        if 16 <= s.get('syncbits', 0) <= 33:
            self.d.update(dsb=s['syncbits'], dsl=s['synclo'], dsh=s['synchi'], dsx=s['syncbit'])
        if s.get('rfmode') in (3, 4):
            self.d['dm'] = 1 if s['rfmode'] == 3 else 0
        if sd.final_plan_valid(s):
            self.d.update(df=s['outkhz'], dpo=s['rfpmag']//500)

    def action(self, name):
        if name == 'navsync':
            self.page = 2
            return
        if name == 'checkframe':
            try:
                sd.validate(self.d)
                self.notice = '参数检查通过'
            except ValueError as error:
                self.notice = '参数错误：'+str(error)
            return
        if self.pending:
            self.notice = '等待板端确认'
            return
        if not self.link:
            self.notice = '模拟断线：未发送请求'
            return
        if name == 'readtx':
            self.load_snapshot()
        try:
            op, args = sd.request(name, self.d)
        except ValueError as error:
            self.notice = '参数错误：'+str(error)
            return
        self.seq = self.seq % 65535+1
        self.last_packet = sd.packet(self.seq, op, args)
        self.pending = True
        self.stopped = False
        self.notice = '离线模型等待回执'
        token = self.seq
        def done():
            if token != self.seq or self.stopped:
                return
            self.pending = False
            if not self.link:
                self.notice = '模拟确认超时'
                self.render()
                return
            model = getattr(self, 'synthetic_model', False)
            failed = self.fail_next
            rc = 3 if failed else 8 if name == 'applytx' and not model else 0
            self.fail_next = False
            if name == 'applytx' and rc == 0:
                self.s.update(valid=True, sysvalid=1, route=2, rfmode=4 if self.d['dm'] == 0 else 3,
                              outkhz=self.d['df'], rfpmag=self.d['dpo']*500,
                              rfexpect=self.d['dpo']*500, rfmodel=1, rffault=0, rfphase=4,
                              pcal=1, pcmpcal=1, soqpcal=1, ifvalid=1, ifkhz=70000,
                              iftarget=30000, ifatten=120, refsrc=1, refok=1,
                              qualified=1, rfvalid=1, tempok=0, source=1,
                              code=self.d['dc'] if self.d['dm'] == 0 else 0,
                              rate=self.d['dr'], framed=self.d['dfr'], fbytes=self.d['db'],
                              fcount=self.d['dn'], runflag=self.d['di'], syncbits=self.d['dsb'],
                              synclo=self.d['dsl'], synchi=self.d['dsh'], syncbit=self.d['dsx'])
            elif failed:
                self.s.update(valid=True, sysvalid=0, rfvalid=1, rfmode=0, rfphase=5,
                              rffault=3, ifvalid=0, rfmodel=0)
            elif not self.s.get('valid'):
                self.s.update(valid=True, pcal=0, pcmpcal=0, soqpcal=0, ifvalid=0, ifkhz=0,
                              iftarget=30000, ifatten=359, refsrc=1, refok=0,
                              qualified=0, rfvalid=1, rfmode=0, rfphase=0,
                              sysvalid=0, rfmodel=0, rffault=0, tempok=0)
            self.s.update(ackseq=token, ackop=op, ackrc=rc)
            self.notice = ('离线合成：已应用，非实板证明' if name == 'applytx' and rc == 0
                           else '离线：未就绪，未执行发射' if rc == 8
                           else '离线：数字事务故障，安全停发' if rc
                           else '离线：模拟状态回执')
            self.render()
        self.root.after(350, done)


def main():
    root = tk.Tk()
    Demo(root)
    root.mainloop()


if __name__ == '__main__':
    main()
