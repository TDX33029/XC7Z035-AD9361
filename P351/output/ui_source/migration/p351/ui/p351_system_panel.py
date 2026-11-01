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
        form=ttk.LabelFrame(parent,text='最终射频设置 · PS本地PN11 · 与屏幕共用执行链路',padding=12)
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
        form.columnconfigure(1,weight=1);form.columnconfigure(3,weight=1)
        ttk.Label(parent,text='此页控制整机：AD9361固定70 MHz/目标-30 dBm，外10 MHz，LO2自动设置；需要有效标定及硬件许可。\n独立调试页仍允许可变AD9361频率与衰减。目标/估算不是在线功率实测。',foreground='#9a6700',wraplength=1200).pack(anchor='w',pady=8)
        bar=ttk.Frame(parent);bar.pack(fill='x')
        self.apply=ttk.Button(bar,text='整机应用并发射',command=self.start);self.apply.pack(side='left')
        self.query=ttk.Button(bar,text='读取最终RF状态',command=self.read);self.query.pack(side='left',padx=12)
        ttk.Button(bar,text='紧急停止所有发射',command=app.emergency_off).pack(side='left')
        self.text=tk.StringVar(app,value='尚未查询；不以草稿值宣称已发射。')
        ttk.Label(parent,textvariable=self.text,wraplength=1250,justify='left').pack(fill='x',pady=12)
        app.after(100,self.poll)

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
        self.app.after(100,self.poll)
