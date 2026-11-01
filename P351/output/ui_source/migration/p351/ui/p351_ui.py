"""独立P351控制台：共用算法界面，明确绑定新板协议与真实接口。"""
from pathlib import Path
import math
import sys
import time
import traceback
import queue
import threading
import ipaddress

HERE=(Path(sys._MEIPASS)/'migration/p351/ui' if getattr(sys,'frozen',False)
      else Path(__file__).resolve().parent)
ROOT=HERE.parents[2]
sys.path.insert(0,str(ROOT/'ui'))
import ad9361tx_ui as shared
import p351_board_config
import p351_discovery
import p351_reference
import p351_tfs_control
import p351_receiver_control as receiver
import p351_final_rf as final_rf
from p351_system_panel import SystemPanel

# Explicit runtime bindings: the shared UI prepends its own module directory,
# so relying on search-path shadowing can silently select Z7 implementations.
shared.read_config=p351_board_config.read_config
shared.confirm_rf_config=p351_board_config.confirm_rf_config
shared.confirm_reference=p351_reference.confirm_reference
shared.tfs_control_action=p351_tfs_control.control
original_board_read=shared.read_board_u32

def read_board_value(board,local,query,reply):
    # RFRX是异步命令：不能只等80ms再读取一个仍合法的旧RX LO，
    # 随后强制MONS=RX，掩盖PS因校准/PLL失败而拒绝外部接收的结果。
    if query==b'RXFQ':
        for _ in range(12):
            if original_board_read(board,local,b'RXSQ',b'RXSA') & 15 == 15:
                break
            time.sleep(0.020)
        else:
            raise ValueError('RX外部接收配置未确认：检查LVDS训练、RX PLL/调谐及MON源；不启动采集')
    return original_board_read(board,local,query,reply)

shared.read_board_u32=read_board_value
original_askyesno=shared.messagebox.askyesno

def confirm_dialog(title,message,**kwargs):
    if title=='确认外接射频自收安全':
        message=('P351外部自收会将RX2本振设为TX本振，并接收真实射频。\n\n'
            'TRX2经板载PA，须按PA最大输出及RX输入限值选择固定50 Ω衰减器：\n'
            'TRX2 → 衰减器 → RX2。禁止只有SMA线直接连接。\n\n'
            '开始监视可能启动当前发射配置，仍受板型/射频资格保护。确认接线和输入电平安全？')
    return original_askyesno(title,message,**kwargs)

shared.messagebox.askyesno=confirm_dialog
shared.UI_BUILD='P351 · 2026.10.20 r10 · 独立直发 / 整机最终RF测试'
tk,ttk=shared.tk,shared.ttk

def attenuation_mdb(text):
    value=float(text)
    if not math.isfinite(value) or not 0<=value<=89.75:
        raise ValueError('AD9361衰减范围0–89.75 dB')
    steps=round(value*4)
    if not math.isclose(value*4,steps,abs_tol=1e-7):
        raise ValueError('AD9361衰减须为0.25 dB步进')
    return steps*250

def descendants(widget):
    for child in widget.winfo_children():
        yield child
        yield from descendants(child)

class P351ControlUI(shared.ControlUI):
    def __init__(self):
        self.p351_identified=False
        self.p351_hardware_qualified=False
        # 连接身份与发射资格分开：默认IP只作查找候选，不能当作已连接。
        self.p351_network_confirmed=False
        self.p351_network_events=queue.Queue()
        self.p351_discovery_epoch=0
        self._p351_verified_endpoint=None
        self._p351_allow_unconfirmed_stop=False
        self._p351_auto_local=''
        self._p351_local_programmatic=False
        self._p351_closing=False
        self.final_rf_busy=False
        self.final_rf_events=queue.Queue()
        self.final_rf_token=None
        self.final_rf_serial=0
        self.final_rf_updated=0
        self._p351_discovery_request=None
        super().__init__()
        self.title('P351 校验仿真器 · TX2 / RX2 · TRX2')
        self.tx1_zero_attenuation_dbm.set('')
        self.tx2_zero_attenuation_dbm.set('')
        self.rf_channel_title.set('TRX2 · AD9361 TX2（待板型确认）')
        self.monitor_channel_title.set('TX2数字I/Q · TRX2输出')
        self.monitor_route_hint.set('TX DAC在LVDS前，不证明TRX2已有射频；本阶段以TX2为主，RX2保留且不自动启动。')
        self.tx_lo_mhz.set('2400')
        self.tx_bw_mhz.set('56')
        self.pcm_frequency_offset_hz.set('0')
        self.reference_source.set('外部10 MHz参考')
        self.reference_status.set('待板端回读；默认选择外部10MHz驯服40MHz，AD9361 REFCLK仍为40MHz。')
        self.source_type.set('PN11');self.pn_seed.set('7FF')
        self.valid_bits.set('2047');self.bit_rate_mbps.set('4')
        self._source_changed()
        self._update_power_summaries()
        self.after(100,self._poll_receiver_events)
        self.after(80,self._poll_p351_network)
        self.after(100,self._poll_final_rf)
        self.after(1000,self._final_rf_tick)

    def _build_network(self,parent):
        self.board_ip.set('')
        try:interfaces=p351_discovery.candidate_interfaces()
        except p351_discovery.DiscoveryError:interfaces=[]
        self._p351_auto_local=interfaces[0].local_ip if interfaces else ''
        self.local_ip.set(self._p351_auto_local)
        self.local_ip.trace_add('write',self._p351_local_changed)
        self.board_address_caption=tk.StringVar(self,value='板卡（测试读取）')
        frame=ttk.Frame(parent);frame.pack(anchor='e')
        ttk.Label(frame,textvariable=self.board_address_caption).grid(row=0,column=0,sticky='w')
        ttk.Entry(frame,textvariable=self.board_ip,width=15,state='readonly').grid(row=0,column=1,padx=(6,14))
        ttk.Label(frame,text='本机（有线网口）').grid(row=0,column=2,sticky='w')
        self.p351_local_entry=ttk.Entry(frame,textvariable=self.local_ip,width=15)
        self.p351_local_entry.grid(row=0,column=3,padx=(6,10))
        self.network_test_button=ttk.Button(frame,text='测试 / 读取地址',command=self.test_connection)
        self.network_test_button.grid(row=0,column=4)
        # P351不沿用旧板NRES/PING；重新查找也只读，不重置正在运行的板卡。
        self.network_reset_button=ttk.Button(frame,text='重新查找',command=self.reset_connection)
        self.network_reset_button.grid(row=0,column=5,padx=(6,0))
        self.p351_local_refresh_button=ttk.Button(frame,text='刷新本机IP',command=self.refresh_local_address)
        self.p351_local_refresh_button.grid(row=0,column=6,padx=(6,0))
        ttk.Button(frame,text='读取板卡配置',command=self.refresh_board_configuration).grid(row=1,column=6,padx=(6,0))
        status=ttk.Frame(frame);status.grid(row=1,column=0,columnspan=6,sticky='e',pady=(5,0))
        self.connection_square=tk.Label(status,text='■',width=2,background='#ffffff',foreground='#94a3b8',font=('Segoe UI Symbol',10))
        self.connection_square.pack(side='left')
        ttk.Label(status,textvariable=self.connection_state,style='Hint.TLabel').pack(side='left')
        ttk.Label(status,text='  UDP 40000 · P351身份校验',style='Hint.TLabel').pack(side='left')
        self.temperature_text=tk.StringVar(self,value='AD9361温度：先点击测试确认板卡')
        ttk.Label(frame,textvariable=self.temperature_text,style='Hint.TLabel').grid(row=2,column=0,columnspan=7,sticky='e',pady=(4,0))
        self.config_status.set('地址未确认；点击测试只读查找，不启动发射或采集')
        ttk.Label(frame,textvariable=self.config_status,style='Hint.TLabel').grid(row=3,column=0,columnspan=7,sticky='e',pady=(4,0))

    def _p351_network_idle(self):
        if getattr(getattr(self,'system_panel',None),'busy',False):return False
        if any(self.__dict__.get(name,False) for name in ('worker_active','network_busy','config_busy',
                'temperature_busy','final_rf_busy','tfs_active','monitor_running','receiver_busy','receiver_capture_busy')):
            return False
        return not any(worker is not None and worker.is_alive() for worker in
                       (getattr(self,'transfer_thread',None),getattr(self,'monitor_thread',None)))

    def _p351_invalidate_network(self):
        # 保留上次确认的地址仅供显式紧急STOP；不能继续用于普通控制。
        self.p351_network_confirmed=False
        self.p351_identified=False
        self.p351_hardware_qualified=False
        self.rf_ports_verified=False
        self.applied_waveform_signature=None
        self.receiver_last=None
        self.receiver_fields_loaded=False
        receiver_status=self.__dict__.get('receiver_status')
        if receiver_status is not None:
            receiver_status.set('板卡地址/身份待确认；旧RX状态已清除，未改变板端解调。')
        self.temperature_updated=0
        self.final_rf_busy=False
        self.final_rf_token=None
        self.final_rf_updated=0
        if 'final_rf_text' in self.__dict__:
            self.final_rf_text.set('最终RF：地址/身份未确认；旧状态已清除，未改变板端任务。')
        self.temperature_text.set('AD9361温度：地址未确认，旧读数已清除')
        self.p351_status.set('板卡身份未确认；普通控制受保护。请点击测试重新读取真实地址。')
        self.board_address_caption.set('板卡（上次/未确认）' if self.board_ip.get() else '板卡（测试读取）')

    def _p351_local_changed(self,*_):
        if self._p351_local_programmatic:return
        self._control_generation+=1
        self._p351_invalidate_network()
        self.config_status.set('本机地址已编辑；请重新点击测试，不沿用旧板卡身份')
        self._set_connection_status('idle','本机地址已变更，待测试')

    def _p351_network_controls(self,busy):
        state='disabled' if busy else 'normal'
        for name in ('network_test_button','network_reset_button','p351_local_refresh_button','p351_local_entry'):
            getattr(self,name).configure(state=state)

    def test_connection(self):
        if not self._p351_network_idle():
            self._log('地址测试未启动：请先停止I/Q监视并等待当前操作/只读查询完成。')
            return
        local=self.local_ip.get().strip()
        preferred=local if local!=self._p351_auto_local else ''
        previous=(self._p351_verified_endpoint or (shared.FIXED_BOARD_IP,''))[0]
        self.p351_discovery_epoch+=1
        self._control_generation+=1
        epoch=self.p351_discovery_epoch
        self._p351_discovery_request=(epoch,self._current_waveform_signature(),self._control_generation,local)
        self._p351_invalidate_network()
        self.network_busy=True
        self._p351_network_controls(True)
        self._set_connection_status('busy','只读查找P351地址与身份…')
        self.config_status.set('DSCI发现 + CFGQ身份校验；不发送PING、重置、发射或采集命令')
        threading.Thread(target=self._p351_discovery_worker,
            args=(self.p351_network_events,epoch,preferred,previous),daemon=True).start()

    @staticmethod
    def _p351_discovery_worker(out,epoch,preferred,previous):
        # 后台线程只处理网络/队列，不读取或修改任何Tk变量。
        try:result=p351_discovery.choose_single(preferred,previous,timeout=3.0)
        except Exception as exc:out.put((epoch,None,exc))
        else:out.put((epoch,result,None))

    def _p351_finish_discovery(self,epoch,result,error):
        if self._p351_closing or epoch!=self.p351_discovery_epoch:return
        request=self._p351_discovery_request
        if request is None or request[0]!=epoch:return
        self.network_busy=False
        self._p351_network_controls(False)
        self._p351_discovery_request=None
        if error is not None or result is None:
            self._p351_invalidate_network()
            self._set_connection_status('error','地址/身份未确认')
            self.config_status.set(str(error or '没有有效P351回包；未采用默认地址'))
            self._log('只读地址测试失败：'+self.config_status.get())
            for detail in getattr(error,'diagnostics',()):self._log('查址详情：'+str(detail))
            return
        # Entry虽在测试期间禁用，仍防止其他回调修改网口后套用旧结果。
        if self.local_ip.get().strip()!=request[3]:
            self._p351_invalidate_network()
            self._set_connection_status('idle','本机地址已改变，请重新测试')
            return
        self._p351_local_programmatic=True
        try:
            self.board_ip.set(result.board_ip)
            self.local_ip.set(result.local_ip)
        finally:self._p351_local_programmatic=False
        self._p351_auto_local=result.local_ip
        self._p351_verified_endpoint=(result.board_ip,result.local_ip)
        self.p351_network_confirmed=True
        self.board_address_caption.set('板卡（已读取）')
        self._p351_accept_identity(result.config)
        self._set_connection_status('ok',f'P351身份已确认 · {result.rtt_ms:.1f} ms')
        self._log(f'只读确认P351：板卡{result.board_ip}，本机{result.local_ip}（{result.interface_name}）；'
                  '已收到匹配CFGA。地址来自实包，不改板卡IP，不启动发射/采集。')
        token=(request[1],request[2],result.board_ip,result.local_ip)
        if not self._apply_board_configuration(result.config,token):
            self.config_status.set('地址/身份已确认；查询期间草稿有变化，未覆盖。需要时点击读取板卡配置。')

    def _poll_p351_network(self):
        if self._p351_closing:return
        try:
            while True:
                item=self.p351_network_events.get_nowait()
                if len(item)==3:self._p351_finish_discovery(*item)
                else:
                    kind,epoch,endpoint,value,error=item
                    if kind!='temperature':continue
                    self.temperature_busy=False
                    if (epoch!=self.p351_discovery_epoch or not self.p351_network_confirmed
                            or endpoint!=(self.board_ip.get().strip(),self.local_ip.get().strip())):continue
                    if error:self.temperature_text.set('AD9361温度：不可用（'+error+'）；非0℃')
                    else:
                        self.temperature_updated=time.monotonic()
                        self.temperature_text.set(f'AD9361：{value:.1f} ℃ · 芯片传感器估算/未做单点校准 · '
                                                  f'{time.strftime("%H:%M:%S")}更新 · Zynq未接入')
        except queue.Empty:pass
        self.after(80,self._poll_p351_network)

    def _temperature_tick(self):
        if self._p351_closing:return
        if self.p351_network_confirmed and time.monotonic()-self.temperature_updated>6:
            self.temperature_text.set('AD9361温度：等待读取/无新数据')
        if self.p351_network_confirmed and not any(getattr(self,name,False) for name in
                ('temperature_busy','worker_active','network_busy','config_busy','tfs_active')):
            try:endpoint=self._endpoint()
            except ValueError:pass
            else:
                self.temperature_busy=True
                epoch=self.p351_discovery_epoch
                out=self.p351_network_events
                def worker():
                    try:value,error=shared.read_chip_temperature(*endpoint),''
                    except (OSError,ValueError) as exc:value,error=None,str(exc)
                    out.put(('temperature',epoch,endpoint,value,error))
                threading.Thread(target=worker,daemon=True).start()
        self.after(2000,self._temperature_tick)

    def _endpoint(self):
        verified=self._p351_verified_endpoint
        if self._p351_allow_unconfirmed_stop and verified is not None:return verified
        current=(self.board_ip.get().strip(),self.local_ip.get().strip())
        if not self.p351_network_confirmed or current!=verified:
            raise ValueError('板卡实际地址尚未确认；请接好网线后点击“测试 / 读取地址”。')
        for text in current:
            address=ipaddress.IPv4Address(text)
            if address.is_unspecified or address.is_multicast or address.is_loopback or not 0<int(address)>>24<224:
                raise ValueError('板卡/本机必须使用有效且不同的单播IPv4地址')
        if current[0]==current[1]:raise ValueError('板卡和本机不能使用相同IPv4地址')
        return current

    def refresh_final_rf(self):
        if self._p351_closing or self.final_rf_busy or not self.p351_network_confirmed:
            return
        if any(getattr(self,n,False) for n in ('worker_active','network_busy','config_busy','tfs_active')):
            return
        try: endpoint=self._endpoint()
        except ValueError: return
        self.final_rf_serial+=1
        token=(self.p351_discovery_epoch,endpoint,self.final_rf_serial)
        self.final_rf_token=token
        self.final_rf_busy=True
        threading.Thread(target=self._final_rf_worker,
            args=(self.final_rf_events,token,endpoint),daemon=True).start()

    @staticmethod
    def _final_rf_worker(out,token,endpoint):
        # 不读取Tk，不下发APPLY/STOP/TFS写；旧PS无响应也不改走其他命令。
        try: value,error=final_rf.read_pc_status(*endpoint),None
        except Exception as exc: value,error=None,str(exc)
        out.put((token,value,error))

    def _poll_final_rf(self):
        if self._p351_closing:return
        try:
            while True:
                token,value,error=self.final_rf_events.get_nowait()
                if token!=self.final_rf_token:continue
                self.final_rf_token=None
                self.final_rf_busy=False
                if (not self.p351_network_confirmed or token[0]!=self.p351_discovery_epoch or
                        token[1]!=self._p351_verified_endpoint or
                        token[1]!=(self.board_ip.get().strip(),self.local_ip.get().strip())):continue
                if error:
                    self.final_rf_updated=0
                    self.final_rf_text.set('AD9361状态：不可用 / RF ?（'+error+'）；不沿用旧ON。')
                else:
                    self.final_rf_updated=time.monotonic()
                    self.final_rf_text.set(value.pc_text())
        except queue.Empty:pass
        self.after(100,self._poll_final_rf)

    def _final_rf_tick(self):
        if self._p351_closing:return
        if self.final_rf_updated and time.monotonic()-self.final_rf_updated>4:
            self.final_rf_text.set('AD9361状态：已过期 / RF ?；不沿用旧ON，未改变板端任务。')
            self.final_rf_updated=0
        self.refresh_final_rf()
        self.after(1000,self._final_rf_tick)

    def refresh_local_address(self):
        if not self._p351_network_idle():
            self._log('本机地址未刷新：请等待操作结束并停止I/Q监视。');return
        try:interfaces=p351_discovery.candidate_interfaces()
        except p351_discovery.DiscoveryError as exc:
            interfaces=[];self._log(str(exc))
        self._p351_auto_local=interfaces[0].local_ip if interfaces else ''
        self.local_ip.set(self._p351_auto_local)
        self._set_connection_status('idle','有线本机地址已刷新，待测试' if interfaces else '物理以太网未连接/没有IPv4')
        self._log('仅刷新本机网卡信息；板卡地址须点击测试读取，不自动扫描Wi-Fi/VPN。')

    def reset_connection(self):
        self.test_connection()

    def emergency_off(self):
        self._p351_allow_unconfirmed_stop=True
        try:super().emergency_off()
        finally:self._p351_allow_unconfirmed_stop=False

    def _close(self):
        self._p351_closing=True
        self.p351_discovery_epoch+=1
        self.monitor_stop.set()
        self.transfer_cancel.set()
        # 关闭窗口只结束本地任务；仅测试后关闭也不能隐式发送STR0/MONS等。
        # 板端发射/接收须由各自的停止按钮显式停止。
        self.destroy()

    def _build(self):
        self.tx1_attenuation_db=tk.StringVar(self,value='89.75')
        self.p351_status=tk.StringVar(self,value='待只读确认P351；PC独立设置AD9361 LO和衰减，屏幕整机方案单独保留。')
        outer=ttk.Frame(self,padding=(18,12));outer.pack(fill='both',expand=True)
        header=ttk.Frame(outer);header.pack(fill='x')
        title=ttk.Frame(header);title.pack(side='left',fill='x',expand=True)
        ttk.Label(title,text='P351 校验仿真器控制台',style='Title.TLabel').pack(anchor='w')
        ttk.Label(title,text='TX2A → 板载PA/开关 → TRX2 · RX2 · 1R1T · 61.44 MSPS · P351 V9',style='Sub.TLabel').pack(anchor='w')
        network=ttk.Frame(header);network.pack(side='right',anchor='ne',padx=(18,0))
        self._build_network(network)
        ttk.Label(outer,textvariable=self.p351_status,foreground='#9a6700',wraplength=1300).pack(anchor='w',pady=(8,0))
        final_panel=ttk.LabelFrame(outer,text='AD9361直发状态 · PS只读 · 不换算功放输出功率',padding=8)
        final_panel.pack(fill='x',pady=(6,0))
        self.final_rf_text=tk.StringVar(self,value='AD9361：尚未确认板卡；不会把编辑框当作已应用配置。')
        ttk.Label(final_panel,textvariable=self.final_rf_text,wraplength=1350,justify='left').pack(side='left',fill='x',expand=True)
        ttk.Button(final_panel,text='刷新AD9361状态',command=self.refresh_final_rf).pack(side='right',anchor='n')
        self.book=ttk.Notebook(outer);self.book.pack(fill='both',expand=True,pady=(8,8))
        rf_page=ttk.Frame(self.book);self.bit_page=ttk.Frame(self.book)
        monitor=ttk.Frame(self.book,padding=12)
        self.book.add(rf_page,text='P351 发射控制');self.book.add(self.bit_page,text='数据与帧')
        self.book.add(monitor,text='TX / RX2 I/Q监控')
        rx_page=ttk.Frame(self.book,padding=12);self.book.add(rx_page,text='RX2 实时解调')
        self._build_receiver_tab(rx_page)
        self.receiver_page=rx_page
        self.receiver_view_active=False
        self.book.bind('<<NotebookTabChanged>>',self._receiver_tab_changed,add='+')
        self.bit_tab=self._scrollable_content(self.bit_page,padding=12)
        self._build_rf_tab(self._scrollable_content(rf_page,padding=10))
        system_page=ttk.Frame(self.book)
        self.book.add(system_page,text='整机最终RF测试')
        self.system_panel=SystemPanel(self,self._scrollable_content(system_page,padding=12))
        self._build_monitor_tab(monitor);self._build_bit_tab(self.bit_tab);self._build_log(outer)
        ttk.Button(monitor,text='保存最近I/Q（接收分析用NPZ）',command=self.save_receiver_capture).pack(anchor='e',pady=(4,0))

    def _channel_controls(self,parent,mode,power,zero_attenuation_power,summary):
        shared.ChoiceBar(parent,variable=mode,values=shared.VISIBLE_MODE_VALUES,
            labels={'PCM/FM（PL）':'PCM/FM','SOQPSK-TG（PL）':'SOQPSK-TG','正弦波/CW（PL）':'正弦 / CW'},
            command=self._mode_changed,width=9).grid(row=0,column=0,columnspan=2,sticky='ew')
        ttk.Label(parent,text='AD9361 TX衰减 (dB)').grid(row=1,column=0,sticky='w',pady=(12,0))
        ttk.Spinbox(parent,textvariable=self.tx1_attenuation_db,from_=0,to=89.75,increment=.25,width=18).grid(
            row=1,column=1,padx=(10,0),pady=(12,0),sticky='ew')
        ttk.Label(parent,text='TRX2直发：LO 70–6000 MHz；衰减0–89.75 dB（0.25 dB步进），默认89.75 dB。CW主峰=LO+2 MHz；低衰减前请核对实体负载及仪器输入限值。',foreground='#9a6700',wraplength=1000).grid(
            row=2,column=0,columnspan=2,sticky='w',pady=(8,0))
        ttk.Label(parent,textvariable=summary,wraplength=850,style='Hint.TLabel').grid(
            row=3,column=0,columnspan=2,sticky='w',pady=(6,0))
        self.tx1_attenuation_db.trace_add('write',lambda *_:self._update_power_summaries())
        mode.trace_add('write',lambda *_:self._update_power_summaries())
        parent.columnconfigure(1,weight=1)

    def _build_tfs_tab(self,tab):
        super()._build_tfs_tab(tab)
        for widget in descendants(tab):
            if isinstance(widget,ttk.Label) and str(widget.cget('text')).startswith('独立PL SPI'):
                widget.configure(text='P351 JM1 · PL SPI默认10 MHz · Bank12 3.3 V\n'
                    '5=SCK，6=MOSI，7=LE，8=MISO，9=LD；10–16=V1–V7衰减线。\n'
                    '17=板TX→屏RX，18=板RX←屏TX；3/4=GND。TFS独立12 V供电并共地；100 MHz参考来自外部时钟板。')

    def _set_rf_ports(self,rx,tx):
        if (rx,tx)!=(2,2):raise ValueError('P351仅支持物理通道2的1R1T配置')
        super()._set_rf_ports(rx,tx)
        self.rf_channel_title.set('TRX2 · AD9361 TX2（1R1T）')
        self.tx_channel_frame.configure(text=self.rf_channel_title.get())
        self.monitor_channel_title.set('TX2数字I/Q · TRX2 / RX2')
        self.monitor_route_hint.set('TX DAC位于LVDS前，不证明TRX2已有射频。RX2保留；只有显式选择接收操作才配置RX。禁止PA输出仅用SMA直连RX。')

    def _p351_accept_identity(self,config):
        self.p351_identified=True
        self.p351_hardware_qualified=config.hardware_qualified
        self.p351_status.set('P351 V9 / TX2身份已确认；'+('人工直发软件许可已启用（非硬件验收）。' if self.p351_hardware_qualified
                            else '候选固件保持射频保护，禁止发射。')+' UI直接设置AD9361 LO和衰减，不换算功放输出功率。')

    def _apply_board_configuration(self,config,token):
        if not self.p351_network_confirmed or tuple(token[2:])!=self._p351_verified_endpoint:
            return False
        accepted=super()._apply_board_configuration(config,token)
        if accepted:
            self._p351_accept_identity(config)
            self.config_status.set('已回读P351 / TX2：LO与AD9361衰减；这是寄存器配置，不是TRX2功率测量。')
            self.applied_waveform_signature=None
        return accepted

    def _read_rf_values(self):
        lo,bw=float(self.tx_lo_mhz.get()),float(self.tx_bw_mhz.get())
        if not math.isfinite(lo) or not 70<=lo<=6000:raise ValueError('TX LO范围70–6000 MHz')
        if not math.isfinite(bw) or not shared.AD9361_TX_BW_MIN_MHZ<=bw<=shared.AD9361_TX_BW_MAX_MHZ:
            raise ValueError('AD9361模拟滤波带宽范围0.2–56 MHz')
        if self.tx2_mode.get()!='不发射':raise ValueError('P351没有第二逻辑发送槽；不能以槽1选择物理TX2')
        active=self.tx1_mode.get()!='不发射'
        if active and not self.p351_identified:raise ValueError('请先只读回读P351 V9配置；未确认板型，禁止发射')
        if active and not self.p351_hardware_qualified:raise ValueError('P351候选固件尚未通过硬件资格；发射保护未放行')
        att=attenuation_mdb(self.tx1_attenuation_db.get())/1000 if active else 0.0
        return lo,bw,att,0.0

    def _confirm_direct_tx(self):
        # 纯参数构包/板端回读不弹窗；只有显式启动操作才确认低衰减。
        if self.tx1_mode.get()=='不发射':return True
        try:lo,bw,att,_=self._read_rf_values()
        except ValueError:return True # 原启动入口负责显示具体参数错误。
        if att>=40:return True
        return shared.messagebox.askyesno('确认低衰减直发',
            f'即将设置AD9361 LO={lo:g} MHz、衰减={att:g} dB。\n\n'
            '本设置不使用功放增益或-30dBm功率模型，TRX2实际功率尚未标定。\n'
            '请确认已接适当的50Ω负载/实体衰减器，仪器输入和其他RF设备可承受。\n'
            '不要连接仅支持70MHz的上变频输入，也不要仅用SMA线直连RX。\n\n继续本次发射？',parent=self)

    def _start_reference_worker(self,*,clock_only):
        if not clock_only and not self._confirm_direct_tx():
            self._set_operation_status('idle','已取消低衰减发射，未下发配置')
            return
        return super()._start_reference_worker(clock_only=clock_only)

    def start_bit_transfer(self):
        if not self._confirm_direct_tx():
            self._set_operation_status('idle','已取消低衰减发射，未下发配置')
            return
        return super().start_bit_transfer()

    def _receiver_tab_changed(self,*_):
        self.receiver_view_active=self.book.select()==str(self.receiver_page)

    def _refclk_packet(self):
        if not self.p351_identified:raise ValueError('请先回读P351 V9板型，再配置参考时钟')
        return super()._refclk_packet()

    def _loopback_monitor_worker(self,board,local,refresh_hz,monitor_source):
        try:
            # 只读确认新板及RX能力，之后才能下发RFRX/MONS等接收配置。
            config=p351_board_config.read_config(board,local)
            if monitor_source.startswith('RX ADC'):
                p351_board_config.require_rx_capabilities(config,demod=False)
        except (OSError,ValueError) as exc:
            self.events.put(('monitor_error',str(exc)))
            self.events.put(('monitor_stopped',None))
            return
        self.monitor_capture_request=b'SNAP'
        try:
            groups=original_board_read(board,local,b'IQSZ',b'IQSA')
        except (OSError,ValueError):
            self.events.put(('log','固件未确认短帧能力：兼容SNAP长块（820分片）；建议加载r5 ELF。'))
        else:
            if groups!=4096:
                self.events.put(('monitor_error',f'短帧能力不匹配：{groups}，请核对ELF版本'))
                self.events.put(('monitor_stopped',None))
                return
            self.monitor_capture_request=b'SNM1'
            self.events.put(('log','已确认r5短帧：4096组/26分片；原SNAP长块和RF参数不变。'))
        super()._loopback_monitor_worker(board,local,refresh_hz,monitor_source)

    def start_loopback_monitor(self):
        state=getattr(self,'receiver_last',None)
        if state is not None and state.configured:
            self._error('接收服务忙','先在RX2实时解调页停止解调，再开始大块I/Q监视。');return
        super().start_loopback_monitor()

    def _current_waveform_signature(self):
        return (*super()._current_waveform_signature(),self.tx1_attenuation_db.get())

    def _render_monitor_frame(self,value):
        super()._render_monitor_frame(value)
        if isinstance(value,dict) and isinstance(value.get('payload'),bytes) and shared.np is not None:
            data=shared.np.frombuffer(value['payload'],dtype='<i2')
            if data.size and data.size%4==0:
                self.receiver_capture=(data.reshape(-1,4)[:,:2].copy(),{
                    'sample_rate_hz':61440000,'capture_id':value.get('capture_id'),
                    'source':self.monitor_active_source,'rx_lo_readback_at_start_hz':self.monitor_rx_lo_hz,
                    'rx_bw_readback_at_start_hz':self.monitor_rx_bw_hz,
                    'timestamp':value.get('timestamp'),'rf_tested':False,
                    'note':'单次完整采样块；两次SNAP之间有空隙，禁止直接拼接；LO/BW为启动时回读。'})

    def save_receiver_capture(self):
        import json
        from tkinter import filedialog
        capture=getattr(self,'receiver_capture',None)
        if capture is None:
            self._error('没有采集数据','请先取得完整I/Q采集块');return
        path=filedialog.asksaveasfilename(parent=self,title='保存完整I/Q采集块',defaultextension='.npz',
            filetypes=[('接收I/Q数据','*.npz')])
        if path:
            data,metadata=capture
            try:shared.np.savez_compressed(path,i=data[:,0],q=data[:,1],metadata=json.dumps(metadata,ensure_ascii=False))
            except OSError as exc:self._error('保存失败',exc);return
            self._log(f'已保存{len(data)}组真实I/Q：{path}；非射频验收结果。')

    def _build_receiver_tab(self,parent):
        self.receiver_events=queue.Queue();self.receiver_busy=False;self.receiver_last=None
        self.receiver_capture_events=queue.Queue();self.receiver_capture_busy=False
        self.receiver_next_poll=0.;self.receiver_fields_loaded=False
        self.receiver_status=tk.StringVar(self,value='未回读。PL实时解调候选版，尚未通过实板射频验收。')
        self.receiver_vars={name:tk.StringVar(self,value=value) for name,value in {
            'center':'5137','rate':'4','bw':'20','gain':'10','mode':'PCM/FM',
            'line':'NRZ-L','frame':'512','sync':'FE6B2840','sync_bits':'32','count':'1','sfid':'0'}.items()}
        ttk.Label(parent,text='独立RX2 · 100 kbit/s–20 Mbit/s · PN11小帧/ID型复帧接收',style='Title.TLabel').pack(anchor='w')
        ttk.Label(parent,text='中心频率不跟随TX。原I/Q监视与实时位流服务互斥；请先停止I/Q监视。\n'
            '改变RX LO/BW时要求TX关闭，避免RX校准扰动正在发射的链路。PCM/FM七码型与SOQPSK-TG；支持16–33位同步及8位SFID。',
            wraplength=1200,style='Hint.TLabel').pack(anchor='w',pady=(8,12))
        grid=ttk.Frame(parent);grid.pack(fill='x')
        fields=[('center','RX中心频率/MHz'),('rate','码率/Mbit/s'),('bw','RX模拟滤波带宽/MHz'),('gain','RX手动增益/dB'),
                ('mode','调制方式'),('line','PCM编码'),('frame','小帧长度/字节'),('sync','同步字/HEX'),
                ('sync_bits','同步长度/bit'),('count','复帧内小帧数量'),('sfid','首SFID（0/1）')]
        for index,(key,label) in enumerate(fields):
            row,col=divmod(index,4);ttk.Label(grid,text=label).grid(row=row*2,column=col,sticky='w',padx=8)
            if key in ('mode','line'):
                widget=ttk.Combobox(grid,textvariable=self.receiver_vars[key],values=('PCM/FM','SOQPSK-TG') if key=='mode' else receiver.LINE_CODES,state='readonly',width=22)
            else:widget=ttk.Entry(grid,textvariable=self.receiver_vars[key],width=24)
            widget.grid(row=row*2+1,column=col,sticky='ew',padx=8,pady=(4,14));grid.columnconfigure(col,weight=1)
        buttons=ttk.Frame(parent);buttons.pack(fill='x',pady=10)
        ttk.Button(buttons,text='应用并开始RX2解调',command=lambda:self._receiver_action('start')).pack(side='left',padx=6)
        ttk.Button(buttons,text='停止解调（不关闭TX）',command=lambda:self._receiver_action('stop')).pack(side='left',padx=6)
        ttk.Button(buttons,text='回读当前RX配置',command=lambda:self._receiver_action('query',load_fields=True)).pack(side='left',padx=6)
        ttk.Button(buttons,text='保存2秒解调位流',command=self._capture_receiver_bits).pack(side='left',padx=6)
        ttk.Label(parent,textvariable=self.receiver_status,wraplength=1200,foreground='#17456b').pack(anchor='w',pady=12)
        ttk.Label(parent,text='锁帧依据同步字与间隔，PN11仅做锁帧后的比特比较，不参与选择解调结果。\n'
            'UDP丢包与PN误码分开统计。增益/数字衰减不能保护RX端：板载PA输出禁止仅用SMA线直连RX2。\n'
            '20 Mbit/s PCM/FM建议RX滤波带宽至少30 MHz，按实际信号调整；该带宽不是测得的B99。\n'
            'PL已接入连续位同步环；跟踪范围、长时间稳定性与噪声性能仍需离线及实板验收。',
            wraplength=1200,style='Hint.TLabel').pack(anchor='w',pady=12)

    def _receiver_config_from_fields(self):
        v={k:var.get().strip() for k,var in self.receiver_vars.items()}
        def hz(key):
            value=float(v[key])*1000000
            if not math.isfinite(value) or not value.is_integer():raise ValueError('频率/码率须为整数Hz/bit/s')
            return int(value)
        mode=0 if v['mode']=='PCM/FM' else 1
        line=receiver.LINE_CODES.index(v['line']) if not mode else 0
        return receiver.ReceiverConfig(hz('center'),hz('rate'),hz('bw'),int(v['gain']),mode,line,
            int(v['frame']),int(v['sync'],16),sync_bits=int(v['sync_bits']),frame_count=int(v['count']),sfid_start=int(v['sfid'])).validate()

    def _receiver_action(self,action,load_fields=False,quiet=False):
        if self.receiver_busy:return
        try:
            board,local=self._endpoint()
            cfg=self._receiver_config_from_fields() if action=='start' else receiver.ReceiverConfig(enabled=False)
            if action=='start' and (self.worker_active or self.monitor_running):raise ValueError('请先停止I/Q监视并等待发射参数事务完成')
            if action=='start' and not self.p351_identified:raise ValueError('先回读P351 V9配置，再开始RX2解调')
        except (ValueError,AttributeError) as exc:
            if not quiet:self._error('RX参数错误',exc)
            return
        self.receiver_busy=True
        def worker():
            try:
                if action=='start':
                    p351_board_config.require_rx_capabilities(p351_board_config.read_config(board,local))
                state=receiver.query(board,local) if action=='query' else receiver.apply(board,local,cfg)
                self.receiver_events.put((state,None,load_fields,(board,local)))
            except (OSError,ValueError) as exc:self.receiver_events.put((None,str(exc),False,(board,local)))
        threading.Thread(target=worker,daemon=True).start()

    def _poll_receiver_events(self):
        if self._p351_closing:return
        try:
            while True:
                state,error,load_fields,endpoint=self.receiver_events.get_nowait();self.receiver_busy=False
                if (not self.p351_network_confirmed or self.network_busy
                        or endpoint!=(self.board_ip.get().strip(),self.local_ip.get().strip())):continue
                if state is None:
                    self.receiver_status.set('RX回读失败：'+error+'；不能确认锁定/误码率。')
                    self.receiver_last=None
                else:
                    self.receiver_last=state;w=state.words
                    if load_fields or not self.receiver_fields_loaded:
                        cfg=state.config()
                        values={'center':f'{cfg.center_hz/1e6:g}','rate':f'{cfg.bit_rate/1e6:g}',
                            'bw':f'{cfg.bandwidth_hz/1e6:g}','gain':str(cfg.gain_db),'mode':'SOQPSK-TG' if cfg.mode else 'PCM/FM',
                            'line':receiver.LINE_CODES[cfg.line_code],'frame':str(cfg.frame_bytes),'sync':f'{cfg.sync_word:08X}',
                            'sync_bits':str(cfg.sync_bits),'count':str(cfg.frame_count),'sfid':str(cfg.sfid_start)}
                        for key,value in values.items():self.receiver_vars[key].set(value)
                        self.receiver_fields_loaded=True
                    status='数字锁帧' if state.locked else ('搜索同步' if state.configured else '已停止')
                    if w[3]:status=receiver.ERRORS.get(w[3],str(w[3]))
                    ber='--（未锁帧或等待复帧起点）' if state.ber is None else f'{state.ber:.3g}'
                    trim='不可用' if state.clock_trim_ppm is None else f'{state.clock_trim_ppm:+.2f} ppm（数字估计）'
                    self.receiver_status.set(f'{status} · epoch={w[2]} · PN11 BER={ber}\n'
                        f'已比较{state.payload_bits} bit，误码{state.errors}，完整载荷帧{state.frames}；'
                        f'网络未发送包{w[18]}，发送序号{w[19]}。码钟微调：{trim}。\n'
                        f'SFID最近={w[30]&255}，下个={(w[30]>>9)&511}；PN参考'+('已对齐' if state.pn_aligned else '未对齐')+'。数字锁帧不等于射频验收。')
        except queue.Empty:pass
        try:
            while True:
                result=self.receiver_capture_events.get_nowait();self.receiver_capture_busy=False;self._log(result)
        except queue.Empty:pass
        if (self.__dict__.get('receiver_view_active',False) and
                self.p351_network_confirmed and self.p351_identified and not self.network_busy
                and not self.receiver_busy and time.monotonic()>=self.receiver_next_poll):
            self.receiver_next_poll=time.monotonic()+1;self._receiver_action('query',quiet=True)
        self.after(100,self._poll_receiver_events)

    def _capture_receiver_bits(self):
        from tkinter import filedialog
        import p351_receiver_capture
        if getattr(self,'receiver_capture_busy',False):return
        if self.receiver_busy or not self.receiver_last or not self.receiver_last.locked:
            self._error('无法采集位流','先应用RX2参数并确认数字锁帧；本功能不自动启动/重配射频。');return
        try:board,local=self._endpoint()
        except ValueError as exc:self._error('无法采集位流',exc);return
        path=filedialog.asksaveasfilename(parent=self,title='保存恢复位流（选择新文件名）',defaultextension='.bin',
            filetypes=[('打包解调位流','*.bin')])
        if not path:return
        self.receiver_capture_busy=True
        def worker():
            try:
                report=p351_receiver_capture.capture(board,local,path)
                result=f'位流保存完成：{path}；{report["bits"]} bit/{report["packets"]}包。首bit不保证是帧头，参数与连续性见同名.json。'
            except Exception as exc:result=f'位流保存失败：{exc}；若已采集，.part仅供排查，不能当作连续有效数据。'
            self.receiver_capture_events.put(result) # Tk控件只在主线程更新。
        threading.Thread(target=worker,daemon=True).start()

    def _update_power_summaries(self):
        if not hasattr(self,'tx1_attenuation_db'):return
        try:att=attenuation_mdb(self.tx1_attenuation_db.get())/1000
        except ValueError as exc:self.tx1_power_summary.set(str(exc));return
        off=self.tx1_mode.get()=='不发射'
        self.tx1_power_summary.set(('RF关闭；' if off else '待应用：')+f'AD9361衰减{att:g} dB；不计算功放增益，不将衰减值当作dBm。')

    def _power_log_value(self,channel,attenuation_db):
        return 'RF关闭' if channel!=1 or self.tx1_mode.get()=='不发射' else f'AD9361衰减{attenuation_db:g} dB（TRX2功率未标定）'

    def _log(self,text):
        super()._log(str(text).replace('TX2A/RX2A','TRX2/RX2').replace('TX2A→RX2A','TRX2→RX2'))

def main():
    try:
        if '--self-test' in sys.argv:
            if shared.confirm_reference is not p351_reference.confirm_reference:
                raise RuntimeError('P351 reference binding failed')
            if shared.np is None or shared.Figure is None:
                raise RuntimeError('NumPy/Matplotlib unavailable')
            probe=tk.Tk();probe.withdraw();probe.update_idletasks();probe.destroy()
            if getattr(sys,'frozen',False):
                (Path(sys.executable).parent/'portable_self_test.ok').write_text(
                    'PASS P351/Tk/NumPy/Matplotlib/system-control imports; no network\n',encoding='utf-8')
            print('P351_UI_SELF_TEST_PASS: explicit P351 protocol, Python/Tk/NumPy/Matplotlib; no network')
            return 0
        P351ControlUI().mainloop()
        return 0
    except Exception:
        details=traceback.format_exc()
        (HERE/'p351_ui_startup_error.log').write_text(details,encoding='utf-8')
        print(details,file=sys.stderr)
        return 1

if __name__=='__main__':raise SystemExit(main())
