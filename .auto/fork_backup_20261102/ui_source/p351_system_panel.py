"""整机测试页：单独草稿，不改变现有AD9361独立调试字段。"""
import queue
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox
import p351_system_control as control
import p351_final_rf as final_rf

class SystemPanel:
    def __init__(self,app,parent):
        self.app=app;self.busy=False;self.events=queue.Queue();self.serial=0;self.updated=0
        self.variables={}
        # 屏幕为主同步：PC草稿跟随屏幕/板端提交状态；PC编辑期间暂停回填。
        self.follow_screen=tk.BooleanVar(app,value=True)
        self.pc_editing=0
        self.edit_pause_until=0.0
        self.synced_signature=None
        self.mirror_sig=None
        self.field_idx={'frequency':0,'rate':1,'power':2,'mode':3,'code':4,
                        'framed':5,'bytes':6,'count':7,'repeat':8,
                        'sync':9,'bits':10}
        form=ttk.LabelFrame(parent,text='最终射频设置 · 与屏幕同步（屏幕为主）',padding=12)
        form.pack(fill='x')
        fields=[('frequency','发射频率 / MHz','5137.0'),('power','发射功率 / dBm','-30.0'),
                ('rate','码率 / bit/s（步进1）','4000000'),('mode','调制','PCM/FM'),
                ('code','PCM码型','NRZ-L'),('framed','帧同步','开'),('bytes','小帧 / byte','512'),
                ('count','复帧小帧数','1'),('sync','同步字 / HEX','FE6B2840'),('bits','同步字长度 / bit','32'),
                ('repeat','重复方式','无限')]
        choices={'mode':('PCM/FM','SOQPSK-TG'),'code':tuple(final_rf.LINES),'framed':('关','开'),'repeat':('单次','无限')}
        for row,(name,title,default) in enumerate(fields):
            v=self.variables[name]=tk.StringVar(app,value=default)
            ttk.Label(form,text=title).grid(row=row//2,column=(row%2)*2,sticky='w',padx=6,pady=4)
            widget=(ttk.Combobox(form,textvariable=v,values=choices[name],state='readonly',width=22)
                    if name in choices else ttk.Entry(form,textvariable=v,width=24))
            widget.grid(row=row//2,column=(row%2)*2+1,sticky='ew',padx=6,pady=4)
            if name in choices:
                widget.bind('<<ComboboxSelected>>',lambda *_:self.note_edit())
            else:
                widget.bind('<FocusIn>',lambda *_:self.note_edit(1))
                widget.bind('<FocusOut>',lambda *_:(self.note_edit(-1),self.push_edit(field)))
        bar0=ttk.Frame(parent);bar0.pack(fill='x',pady=(6,0))
        ttk.Checkbutton(bar0,text='跟随屏幕（屏幕为主：屏幕/板端状态变化时自动回填本页草稿）',
            variable=self.follow_screen).pack(side='left')
        self.sync_label=ttk.Label(bar0,text='（等待状态同步）',style='Hint.TLabel');self.sync_label.pack(side='left',padx=(12,0))
        form.columnconfigure(1,weight=1);form.columnconfigure(3,weight=1)
        ttk.Label(parent,text='此页控制整机：AD9361固定70 MHz/目标-30 dBm，外10 MHz，LO2自动设置；需要有效标定及硬件许可。\n独立调试页仍允许可变AD9361频率与衰减。目标/估算不是在线功率实测。',foreground='#9a6700',wraplength=1200).pack(anchor='w',pady=8)
        bar=ttk.Frame(parent);bar.pack(fill='x')
        self.apply=ttk.Button(bar,text='整机应用并发射',command=self.start);self.apply.pack(side='left')
        self.query=ttk.Button(bar,text='读取最终RF状态',command=self.read);self.query.pack(side='left',padx=12)
        ttk.Button(bar,text='紧急停止所有发射',command=app.emergency_off).pack(side='left')
        self.text=tk.StringVar(app,value='尚未查询；不以草稿值宣称已发射。')
        ttk.Label(parent,textvariable=self.text,wraplength=1250,justify='left').pack(fill='x',pady=12)
        app.after(100,self.poll)

    def note_edit(self,delta=0):
        # Entry获得焦点计为编辑中；Combobox选择视为一次完整编辑（立即暂停回填，2秒后恢复）。
        if delta:
            self.pc_editing=max(0,self.pc_editing+delta)
            if delta>0: self.edit_pause_until=time.monotonic()+0
        else:
            self.edit_pause_until=time.monotonic()+2.0

    def backfill_from_state(self,state):
        """屏幕为主：把PS提交状态回填到本页草稿（与屏幕草稿变量同源）。"""
        w=state.get
        import p351_final_rf as final_rf
        values={
            'frequency':f'{w("final_khz")/1000:.1f}',
            'power':f'{final_rf.signed(w("target_mdbm"))/1000:.1f}',
            'rate':str(w('rate')),
            'framed':'开' if w('framed') else '关',
            'bytes':str(w('fbytes')),
            'count':str(w('fcount')),
            'sync':f'{((w("sync_high")&1)<<32 | w("sync_low")):X}',
            'bits':str(w('sync_bits')),
            'repeat':'无限' if w('repeat') else '单次',
        }
        if w('mode')==4: values['mode']='PCM/FM'; values['code']=final_rf.LINES[w('code')] if w('code')<len(final_rf.LINES) else 'NRZ-L'
        elif w('mode')==3: values['mode']='SOQPSK-TG'
        changed=0
        for k,v in values.items():
            if self.variables[k].get()!=v:
                self.variables[k].set(v); changed+=1
        return changed

    def plan(self):
        v={k:x.get().strip() for k,x in self.variables.items()}
        p=control.SystemPlan(mode=int(v['mode']=='SOQPSK-TG'),rate=int(v['rate']),code=final_rf.LINES.index(v['code']),
            framed=int(v['framed']=='开'),frame_bytes=int(v['bytes']),frame_count=int(v['count']),
            infinite=int(v['repeat']=='无限'),sync=int(v['sync'],16),sync_bits=int(v['bits']),
            final_khz=control.scaled(v['frequency'],1000),target_mdbm=control.scaled(v['power'],1000))
        p.arguments();return p

    def start(self):
        try:p=self.plan()
        except ValueError as exc:messagebox.showerror('整机参数错误',str(exc));return
        if not messagebox.askyesno('确认整机发射','将经上变频链路输出最终射频。请确认实体负载、衰减和仪器输入保护。\n缺标定/参考/锁定时PS会拒绝；是否提交？'):return
        self.launch(p)

    def read(self):self.launch(None)

    def launch(self,plan):
        if self.busy or self.app._p351_closing:return
        if any(getattr(self.app,n,False) for n in ('worker_active','config_busy','network_busy','tfs_active')):
            self.text.set('请等待其他控制事务完成');return
        if plan is not None and any(getattr(self.app,n,False) for n in ('monitor_running','receiver_busy','receiver_capture_busy')):
            self.text.set('请先停止I/Q监视并等待接收操作完成，再提交整机事务');return
        try:endpoint=self.app._endpoint()
        except ValueError as exc:self.text.set(str(exc));return
        self.busy=True;self.serial+=1
        token=(self.serial,self.app.p351_discovery_epoch,endpoint)
        self.text.set('等待PS整机状态；RF尚未确认')
        self.apply.configure(state='disabled');self.query.configure(state='disabled')
        def worker():
            try:
                state=(final_rf.read_status(*endpoint) if plan is None else control.apply_system(*endpoint,plan,
                    progress=lambda s:self.events.put((token,None,s,False))))
                self.events.put((token,state,None,True))
            except Exception as exc:self.events.put((token,None,str(exc),True))
        threading.Thread(target=worker,daemon=True).start()

    def poll(self):
        if self.app._p351_closing:return
        try:
            while True:
                token,state,error,done=self.events.get_nowait()
                if token[0]!=self.serial:continue
                if done:
                    self.busy=False;self.apply.configure(state='normal');self.query.configure(state='normal')
                if token[1]!=self.app.p351_discovery_epoch or token[2]!=self.app._p351_verified_endpoint:
                    self.updated=0;self.text.set('地址/身份已变更，旧整机状态已清除');continue
                self.updated=time.monotonic() if state else 0
                self.text.set(state.text() if state else error+'；不沿用旧RF ON')
        except queue.Empty:pass
        if self.updated and time.monotonic()-self.updated>4:
            self.updated=0;self.text.set('最终RF状态已过期，需重新读取；未改变板端任务。')
        self.sync_tick()
        self.app.after(100,self.poll)

    def sync_tick(self):
        """屏幕为主：PC草稿跟随屏幕草稿镜像（2.5s 轮询 SDRQ）。"""
        mirror=getattr(self.app,'screen_mirror',None)
        if not mirror:
            if self.app.p351_network_confirmed:
                self.app.sync_hint.set('等待屏幕草稿镜像…')
            return
        vals=mirror['vars']
        if mirror['edit']:
            self.app.sync_hint.set('屏幕编辑中（暂停回填与推送）')
            return
        if self.busy or self.pc_editing or time.monotonic()<self.edit_pause_until:
            self.app.sync_hint.set('PC编辑中，暂停回填')
            return
        if not self.follow_screen.get():
            return
        sig=tuple(vals[:13])
        if sig==self.synced_signature:
            return
        self.backfill_from_mirror(vals)
        self.synced_signature=sig
        self.app.sync_hint.set('已同步屏幕草稿 '+time.strftime('%H:%M:%S'))

    def backfill_from_mirror(self,vals):
        """镜像→PC字段：df/1000→MHz，-dpo/2→dB（屏幕 0.5dB 幅值约定）。"""
        df,dr,dpo,dm,dc,dfr,db,dn,di,dsl,dsh,dsb = vals[:12]
        import p351_final_rf as final_rf
        values={
            'frequency':f'{df/1000:.1f}',
            'power':f'{-dpo/2:.1f}',
            'rate':str(dr),
            'mode':'PCM/FM' if dm==0 else 'SOQPSK-TG',
            'code':final_rf.LINES[dc] if dc<len(final_rf.LINES) else 'NRZ-L',
            'framed':'开' if dfr else '关',
            'bytes':str(db),
            'count':str(dn),
            'repeat':'无限' if di else '单次',
            'sync':f'{((dsh&0xFFFF)<<16 | (dsl&0xFFFF)):X}',
            'bits':str(dsb),
        }
        for k,v in values.items():
            if self.variables[k].get()!=v:
                self.variables[k].set(v)

    def push_edit(self,field):
        # 编辑提交：与最近镜像对比，真实变更才推送（由 FocusOut 链调用）
        mirror=getattr(self.app,'screen_mirror',None)
        if mirror:
            self.push_edit_to_screen(field)

    def push_edit_to_screen(self,field):
        """PC编辑提交→SDRW 写屏幕草稿变量（屏幕 250ms 内重绘可见）。"""
        import p351_screen_mirror as mirror
        idx=self.field_idx.get(field)
        if idx is None: return
        v=self.variables[field].get().strip()
        conv={'frequency':lambda x:int(round(float(x)*1000)) & 0xFFFFFFFF,
              'rate':lambda x:int(x)&0xFFFFFFFF,
              'power':lambda x:(int(round(abs(float(x))*2)) & 0xFFFFFFFF),
              'bytes':lambda x:int(x)&0xFFFFFFFF,
              'count':lambda x:int(x)&0xFFFFFFFF,
              'bits':lambda x:int(x)&0xFFFFFFFF,
              'mode':lambda x:1 if x=='SOQPSK-TG' else 0,
              'framed':lambda x:1 if x=='开' else 0,
              'repeat':lambda x:1 if x=='无限' else 0,
              'code':lambda x:0}
        fn=conv.get(field)
        if fn is None: return
        try: value=fn(v)
        except Exception: return
        r=mirror.write_draft(self.app.board_ip.get().strip(),
                             self.app.local_ip.get().strip(), idx, value)
        self.app.sync_hint.set({0:'已推送到屏幕',1:'推送被拒（序号无效）',
                               2:'屏幕编辑中，推送稍后重试',3:'推送未入队',
                                None:'推送无应答（板卡忙/离线）'}[r])
