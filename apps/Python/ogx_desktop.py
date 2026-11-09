"""Native PySide6 companion. Qt owns the UI; asyncio owns device I/O."""
from __future__ import annotations

import asyncio
import contextlib
import dataclasses
import json
import math
import os
from pathlib import Path
import sys
import threading
import time
from typing import Any

from PySide6.QtCore import Qt, QEvent, QPointF, QRectF, QStandardPaths, QThread, QTimer, Signal
from PySide6.QtGui import QColor, QDesktopServices, QFont, QIcon, QPainter, QPen, QPixmap
from PySide6.QtCore import QUrl
from PySide6.QtWidgets import (QApplication, QCheckBox, QColorDialog, QComboBox, QDialog,
    QDialogButtonBox, QDoubleSpinBox, QFormLayout, QFrame, QGridLayout, QHBoxLayout,
    QLabel, QLineEdit, QMainWindow, QMessageBox, QProgressBar, QPushButton, QScrollArea,
    QSizePolicy, QSlider, QSpinBox, QStackedWidget, QVBoxLayout, QWidget)

from ogx_remote import BUTTONS, DPAD, PadState, RemoteClient, RemoteError, device_url, sdl_state
from ogx_discovery import discover

VERSION = '0.1.5'
ASSETS = Path(__file__).resolve().parent / 'desktop_assets'
THEMES = json.loads((ASSETS / 'themes.json').read_text(encoding='utf-8'))
THEME_LABELS = {'darkone':'Darkone Green', 'theia-dark':'Theia Dark', 'midnight':'Midnight Blue',
    'synthwave':'Synthwave', 'ember':'Ember Copper', 'terminal':'Retro Terminal',
    'theia-light':'Theia Light', 'arctic-light':'Arctic Light'}
K = Qt.Key
DEFAULT_KEYS = {'a':int(K.Key_Space), 'b':int(K.Key_E), 'x':int(K.Key_Q), 'y':int(K.Key_R),
    'lb':int(K.Key_Z), 'rb':int(K.Key_C), 'lt':int(K.Key_1), 'rt':int(K.Key_2),
    'l3':int(K.Key_F), 'r3':int(K.Key_G), 'back':int(K.Key_Backspace), 'start':int(K.Key_Return),
    'guide':None, 'up':int(K.Key_Up), 'down':int(K.Key_Down), 'left':int(K.Key_Left), 'right':int(K.Key_Right),
    'lx_pos':int(K.Key_D), 'lx_neg':int(K.Key_A), 'ly_pos':int(K.Key_W), 'ly_neg':int(K.Key_S),
    'rx_pos':int(K.Key_L), 'rx_neg':int(K.Key_J), 'ry_pos':int(K.Key_I), 'ry_neg':int(K.Key_K)}
KEY_LABELS = {'a':'A','b':'B','x':'X','y':'Y','lb':'White / LB','rb':'Black / RB','lt':'LT','rt':'RT',
    'back':'Back','start':'Start','guide':'Guide','l3':'L3','r3':'R3','up':'D-pad up','down':'D-pad down',
    'left':'D-pad left','right':'D-pad right','lx_pos':'Left stick →','lx_neg':'Left stick ←',
    'ly_pos':'Left stick ↑','ly_neg':'Left stick ↓','rx_pos':'Right stick →','rx_neg':'Right stick ←',
    'ry_pos':'Right stick ↑','ry_neg':'Right stick ↓'}


def centred(p: PadState) -> bool:
    return not p.buttons and not p.dpad and p.lt <= 8 and p.rt <= 8 and all(
        abs(getattr(p, axis)) <= 4096 for axis in ('lx','ly','rx','ry'))


def keyboard_snapshot(keys: set[int], bindings: dict, held: set[str], axes: dict, triggers: dict) -> PadState:
    def down(name):
        return name in held or (bindings.get(name) is not None and bindings[name] in keys)
    buttons = sum(bit for name,bit in BUTTONS.items() if down(name))
    dpad = sum(bit for name,bit in DPAD.items() if down(name))
    if dpad & 3 == 3: dpad &= ~3
    if dpad & 12 == 12: dpad &= ~12
    def axis(name):
        return axes.get(name, 0) or (0 if down(name+'_pos') == down(name+'_neg') else 32767 if down(name+'_pos') else -32768)
    return PadState(buttons=buttons,dpad=dpad,**{name:axis(name) for name in ('lx','ly','rx','ry')},
        lt=max(triggers.get('lt',0),255 if down('lt') else 0), rt=max(triggers.get('rt',0),255 if down('rt') else 0))


class NetworkWorker(QThread):
    result = Signal(int, str, object, str)
    control = Signal(object)
    ready = Signal()

    def __init__(self, rate=60):
        super().__init__()
        self.rate = rate
        self.loop = None
        self.client = None
        self._sample_lock = threading.Lock()
        self._sample = PadState()
        self._sample_at = 0.0
        self._sample_ready = False
        self._focused = True
        self._gate = True
        self._running = False
        self._starting = False
        self._acquire_task = None
        self._tasks = set()
        self._stopping = None
        self._command_lock = None
        self._release_lock = None
        self._intent = 0
        self._shutdown_requested = threading.Event()

    def set_sample(self, pad: PadState, *, ready=True):
        with self._sample_lock:
            self._sample, self._sample_at, self._sample_ready = pad, time.monotonic(), ready

    def pause(self, focused: bool):
        with self._sample_lock:
            self._focused = focused
            self._sample = PadState()
            self._sample_ready = False
            self._gate = True

    def submit(self, ticket: int, operation: str, payload=None):
        if self.loop is None or self._stopping is None or self._stopping.is_set():
            self.result.emit(ticket, operation, None, 'The connection worker is not ready.')
            return
        asyncio.run_coroutine_threadsafe(self._execute(ticket, operation, payload), self.loop)

    def stop(self):
        self._shutdown_requested.set()
        if self.loop is not None:
            self.loop.call_soon_threadsafe(self._stopping.set)

    async def _stop_control(self):
        self._intent += 1
        self._running = False
        self._gate = True
        async with self._release_lock:
            if self.client:
                await self.client.release()  # Invalidates an acquisition awaiting its reply.
            acquisition = self._acquire_task
            if acquisition and acquisition is not asyncio.current_task() and not acquisition.done():
                await asyncio.gather(acquisition, return_exceptions=True)
        self._emit_control(PadState())

    async def _dispatch(self, operation, payload):
        if operation == 'discover':
            return await asyncio.to_thread(discover)
        if operation == 'release':
            await self._stop_control()
            return {}
        if operation == 'disconnect':
            async with self._command_lock:
                await self._stop_control()
                if self.client:
                    await self.client.close()
                    self.client = None
                return {}
        if operation == 'connect':
            async with self._command_lock:
                await self._stop_control()
                if self.client:
                    await self.client.close()
                self.client = RemoteClient(payload['host'], reconnect=False)
                try:
                    await self.client.__aenter__()
                    status = await self.client.status()
                    if status.get('remote_api',0) < 2:
                        raise RemoteError('This firmware does not advertise Remote API v2.')
                    return status
                except BaseException:
                    await self.client.close()
                    self.client = None
                    raise
        if not self.client:
            raise RemoteError('Connect to an adapter first.')
        if operation == 'acquire':
            if self._starting or self._running:
                raise RemoteError('Control is already starting or active.')
            self._starting = True
            intent = self._intent
            self._acquire_task = asyncio.current_task()
            try:
                await self.client.acquire('python-qt')
                if intent != self._intent:
                    raise RemoteError('Controller acquisition cancelled.')
                self._running = True
                self._gate = True
                return {}
            finally:
                self._starting = False
                self._acquire_task = None
                self._emit_control(PadState())
        if operation == 'status':
            return await self.client.status()
        if operation == 'api':
            if payload.get('body') is not None:
                await self._stop_control()
            return await self.client.request(payload['path'],payload.get('body'))
        raise RemoteError('Unsupported desktop operation.')

    async def _execute(self, ticket, operation, payload):
        task = asyncio.current_task()
        self._tasks.add(task)
        try:
            data = await self._dispatch(operation, payload)
            self.result.emit(ticket, operation, data, '')
        except asyncio.CancelledError:
            pass
        except Exception as error:
            self.result.emit(ticket, operation, None, str(error))
        finally:
            self._tasks.discard(task)

    def _emit_control(self, pad, error=''):
        self.control.emit({'active':self._running, 'starting':self._starting,
            'needs_neutral':self._gate, 'pad':dataclasses.asdict(pad), 'error':error,
            'rate_hz':min(self.rate,self.client.input_hz) if self.client else self.rate,
            'at':time.monotonic()})

    def _input_state(self) -> PadState:
        with self._sample_lock:
            if not self._focused or not self._sample_ready or time.monotonic()-self._sample_at >= .2:
                self._gate = True
                return PadState()
            if self._gate:
                if centred(self._sample):
                    self._gate = False
                return PadState()
            return self._sample

    async def _stream(self):
        last_emit = 0.0
        last_frame = time.monotonic()
        while True:
            began = time.monotonic()
            if began-last_frame >= .25:self._gate = True
            last_frame = began
            pad = PadState()
            if self._running and self.client:
                pad = self._input_state()
                try:
                    await self.client.send(pad)
                except (RemoteError, OSError) as error:
                    self._running = False
                    self._gate = True
                    self._emit_control(PadState(),str(error))
                    try:
                        await self._stop_control()
                    except (RemoteError, OSError) as cleanup:
                        self._emit_control(PadState(),str(cleanup))
            if began-last_emit >= 1/min(60,self.rate):
                self._emit_control(pad if self._running else PadState())
                last_emit = began
            hz = min(self.rate,self.client.input_hz) if self.client else self.rate
            await asyncio.sleep(max(.001,1/max(10,hz)-(time.monotonic()-began)))

    async def _serve(self):
        self._stopping = asyncio.Event()
        self._command_lock = asyncio.Lock()
        self._release_lock = asyncio.Lock()
        if self._shutdown_requested.is_set():self._stopping.set()
        self.ready.emit()
        stream = asyncio.create_task(self._stream())
        try:
            await self._stopping.wait()
        finally:
            self._running = False
            stream.cancel()
            for task in tuple(self._tasks): task.cancel()
            await asyncio.gather(stream,*tuple(self._tasks),return_exceptions=True)
            if self.client:
                try:
                    await asyncio.wait_for(self.client.close(),6)
                except Exception:
                    if self.client.session:
                        await self.client.session.close()
                self.client = None

    def run(self):
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        try:
            self.loop.run_until_complete(self._serve())
            self.loop.run_until_complete(self.loop.shutdown_asyncgens())
        finally:
            self.loop.close()
            self.loop = None


class SdlControllers:
    """SDL standard mappings on the GUI/main thread, including macOS."""
    def __init__(self):
        self.pg = self.pads = self.current = None
        self.index = -1
        self.error = ''
        try:
            os.environ.setdefault('PYGAME_HIDE_SUPPORT_PROMPT','1')
            import pygame
            from pygame._sdl2 import controller
            pygame.display.init()
            controller.init()
            self.pg,self.pads = pygame,controller
        except Exception as error:
            self.error = str(error)

    def scan(self):
        if self.pg is None:return []
        try:
            self.pg.event.get()  # Drain events so long sessions cannot fill SDL's queue.
            if self.current and not self.current.attached():self.close_controller()
            return [(i,self.pads.name_forindex(i) or f'Controller {i+1}')
                for i in range(self.pads.get_count()) if self.pads.is_controller(i)]
        except Exception as error:
            self.error = str(error)
            self.close_controller()
            return []

    def select(self,index):
        if self.current and self.index == index and self.current.attached():return
        self.close_controller()
        if self.pads is not None and index >= 0:
            try:
                self.current = self.pads.Controller(index)
                self.index = index
            except Exception as error:self.error=str(error)

    def sample(self):
        if self.current is None:return None
        try:
            self.pg.event.get()
            if not self.current.attached():return None
            return sdl_state(self.current,self.pg)
        except Exception as error:
            self.error=str(error)
            return None

    def close_controller(self):
        if self.current is not None:
            with contextlib.suppress(Exception):self.current.quit()
        self.current=None
        self.index=-1

    def close(self):
        self.close_controller()
        if self.pads is not None:self.pads.quit()
        if self.pg is not None:self.pg.quit()


def stylesheet(c):
    return '''
    QWidget { color:%(ink)s; font-size:13px; }
    QMainWindow, QWidget#root { background:%(bg)s; }
    QWidget#page, QStackedWidget, QScrollArea > QWidget { background:%(bg)s; }
    QLabel { background:transparent; }
    QLabel[role="muted"] { color:%(muted)s; }
    QLabel[role="eyebrow"] { color:%(teal)s; font-size:10px; font-weight:700; letter-spacing:1px; }
    QLabel[role="heading"] { font-size:30px; font-weight:750; }
    QLabel[role="section"] { font-size:17px; font-weight:700; }
    QLabel#brand { font-size:22px; font-weight:750; }
    QLabel#footer { color:%(muted)s; border-top:1px solid %(line)s; padding-top:10px; }
    QLabel#chip { color:%(teal)s; border:1px solid %(line)s; border-radius:13px; padding:5px 12px; }
    QLabel#controlMessage { color:%(muted)s; background:%(raised)s; border-radius:9px; padding:11px; }
    QFrame#card, QFrame#sidebar { background:%(surface)s; border:1px solid %(line)s; border-radius:15px; }
    QPushButton { background:%(raised)s; color:%(ink)s; border:1px solid %(line)s; border-radius:8px; padding:9px 13px; }
    QPushButton:hover { border-color:%(teal)s; }
    QPushButton:pressed, QPushButton[pressed="true"] { background:%(green)s; color:%(primary-ink)s; }
    QPushButton:focus { border:2px solid %(teal)s; padding:8px 12px; }
    QPushButton:disabled { color:%(muted)s; background:%(surface)s; }
    QPushButton[role="primary"] { background:%(green)s; border-color:%(green)s; color:%(primary-ink)s; font-weight:700; }
    QPushButton[role="primary"]:hover { background:%(primary-hover)s; }
    QPushButton[role="primary"]:disabled { background:%(raised)s; color:%(muted)s; border-color:%(line)s; }
    QPushButton[role="nav"] { text-align:left; background:transparent; border:1px solid transparent; padding:11px 12px; }
    QPushButton[role="nav"]:checked { background:%(raised)s; border-left:3px solid %(green)s; font-weight:700; }
    QPushButton[role="white"] { background:#eeeeea; color:#172016; font-weight:700; }
    QPushButton[role="black"] { background:#191d1a; color:#f4f6ef; font-weight:700; }
    QPushButton#padButton { padding:6px 10px; }
    QPushButton#padButton:focus { padding:5px 9px; }
    QPushButton#padButton[pressed="true"] { background:%(green)s; color:%(primary-ink)s; border-color:%(green)s; }
    QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background:%(raised)s; color:%(ink)s; border:1px solid %(line)s; border-radius:7px; padding:8px; min-height:18px; }
    QComboBox::drop-down { border:0; width:20px; }
    QComboBox::down-arrow { image:url("%(arrow)s"); width:10px; height:7px; }
    QComboBox QAbstractItemView { background:%(raised)s; color:%(ink)s; selection-background-color:%(green)s; selection-color:%(primary-ink)s; }
    QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color:%(teal)s; }
    QCheckBox { spacing:9px; padding:4px 0; }
    QCheckBox::indicator { width:17px; height:17px; background:%(raised)s; border:1px solid %(line)s; border-radius:4px; }
    QCheckBox::indicator:checked { background:%(green)s; border-color:%(teal)s; }
    QScrollArea { background:transparent; border:0; }
    QScrollBar:vertical { background:%(bg)s; width:9px; }
    QScrollBar::handle:vertical { background:%(line)s; min-height:24px; border-radius:4px; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
    QSlider::groove:horizontal { height:6px; background:%(line)s; border-radius:3px; }
    QSlider::sub-page:horizontal { background:%(teal)s; border-radius:3px; }
    QSlider::handle:horizontal { background:%(green)s; width:15px; margin:-5px 0; border-radius:7px; }
    QProgressBar { border:1px solid %(line)s; border-radius:5px; background:%(raised)s; height:10px; text-align:center; }
    QProgressBar::chunk { background:%(teal)s; border-radius:4px; }
    QToolTip { background:%(raised)s; color:%(ink)s; border:1px solid %(teal)s; }
    QDialog, QMessageBox { background:%(bg)s; }
    ''' % {**c,'arrow':(ASSETS/'chevron.svg').as_posix()}


def label(text,role=None):
    widget=QLabel(text)
    widget.setWordWrap(True)
    if role:widget.setProperty('role',role)
    return widget


def button(text,callback=None,role=None):
    widget=QPushButton(text)
    widget.setCursor(Qt.CursorShape.PointingHandCursor)
    if role:widget.setProperty('role',role)
    if callback:widget.clicked.connect(callback)
    return widget


def card(title,subtitle=''):
    frame=QFrame()
    frame.setObjectName('card')
    layout=QVBoxLayout(frame)
    layout.setContentsMargins(20,18,20,18)
    layout.setSpacing(13)
    if title:layout.addWidget(label(title,'section'))
    if subtitle:layout.addWidget(label(subtitle,'muted'))
    return frame,layout


class Facts(QFrame):
    def __init__(self,title):
        super().__init__()
        self.setObjectName('card')
        self.box=QVBoxLayout(self)
        self.box.setContentsMargins(20,18,20,18)
        self.box.addWidget(label(title,'section'))
        self.rows=QWidget()
        self.form=QFormLayout(self.rows)
        self.form.setContentsMargins(0,7,0,0)
        self.form.setSpacing(12)
        self.form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
        self.box.addWidget(self.rows)
        self.update_facts([('Connection','Not connected')])

    def update_facts(self,items):
        while self.form.rowCount():self.form.removeRow(0)
        for name,value in items:
            self.form.addRow(label(name,'muted'),label('—' if value is None or value=='' else str(value)))


class Stick(QWidget):
    changed=Signal(int,int)
    def __init__(self):
        super().__init__()
        self.setMinimumSize(110,110)
        self.setMaximumSize(150,130)
        self.setSizePolicy(QSizePolicy.Policy.Expanding,QSizePolicy.Policy.Expanding)
        self.setAccessibleName('Analog stick; drag to move and release to centre')
        self.x=self.y=0
        self.dragging=False
        self.interactive=False
        self.colors=THEMES['darkone']

    def paintEvent(self,event):
        p=QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        radius=min(self.width(),self.height())*.38
        centre=QPointF(self.width()/2,self.height()/2)
        p.setPen(QPen(QColor(self.colors['line']),1.5))
        p.setBrush(QColor(self.colors['raised']))
        p.drawEllipse(centre,radius,radius)
        p.drawLine(QPointF(centre.x()-radius,centre.y()),QPointF(centre.x()+radius,centre.y()))
        p.drawLine(QPointF(centre.x(),centre.y()-radius),QPointF(centre.x(),centre.y()+radius))
        knob=centre+QPointF(self.x/32768*radius*.72,-self.y/32768*radius*.72)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(self.colors['green']))
        p.drawEllipse(knob,12,12)

    def set_pad(self,x,y):
        if not self.dragging:self.x,self.y=x,y;self.update()

    def _move(self,event):
        radius=min(self.width(),self.height())*.38
        dx=(event.position().x()-self.width()/2)/(radius*.72)
        dy=-(event.position().y()-self.height()/2)/(radius*.72)
        size=math.hypot(dx,dy)
        if size>1:dx/=size;dy/=size
        self.x,self.y=round(dx*32767),round(dy*32767)
        self.changed.emit(self.x,self.y)
        self.update()

    def mousePressEvent(self,event):
        if self.interactive and event.button()==Qt.MouseButton.LeftButton:
            self.dragging=True;self.grabMouse();self._move(event)

    def mouseMoveEvent(self,event):
        if self.dragging:self._move(event)

    def mouseReleaseEvent(self,event):
        if self.dragging:
            self.releaseMouse();self.reset()

    def reset(self):
        if QWidget.mouseGrabber() is self:self.releaseMouse()
        self.dragging=False;self.x=self.y=0;self.changed.emit(0,0);self.update()


class KeyCapture(QDialog):
    def __init__(self,title,parent):
        super().__init__(parent)
        self.setWindowTitle('Assign '+title)
        self.value=None
        layout=QVBoxLayout(self)
        layout.addWidget(label('Press a key for '+title,'section'))
        layout.addWidget(label('Esc cancels. F8 and Esc remain reserved for control/release.','muted'))
        buttons=QDialogButtonBox(QDialogButtonBox.StandardButton.Cancel)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self.resize(410,140)

    def keyPressEvent(self,event):
        key=event.key()
        if key==int(K.Key_Escape):self.reject();return
        if valid_key(key):self.value=key;self.accept()


def valid_key(key):
    return type(key) is int and (int(K.Key_A)<=key<=int(K.Key_Z) or int(K.Key_0)<=key<=int(K.Key_9) or
        key in [int(k) for k in (K.Key_Space,K.Key_Return,K.Key_Enter,K.Key_Backspace,K.Key_Up,K.Key_Down,K.Key_Left,K.Key_Right,
            K.Key_Minus,K.Key_Equal,K.Key_Comma,K.Key_Period,K.Key_Slash,K.Key_Semicolon,K.Key_Apostrophe,K.Key_BracketLeft,K.Key_BracketRight,K.Key_Backslash)])


def key_name(key):
    if key is None:return 'Unassigned'
    names={int(K.Key_Space):'Space',int(K.Key_Return):'Enter',int(K.Key_Enter):'Num Enter',int(K.Key_Backspace):'Backspace',
        int(K.Key_Up):'↑',int(K.Key_Down):'↓',int(K.Key_Left):'←',int(K.Key_Right):'→'}
    return names.get(key,chr(key) if 32<=key<=126 else str(key))


class CompanionWindow(QMainWindow):
    def __init__(self,host='192.168.4.1',port=8765,rate=60,*,worker=None,controllers=None):
        super().__init__()
        self.setWindowTitle('DC-OGX-W | Darkone Customs')
        self.setWindowIcon(QIcon(str(ASSETS/'webui-logo.png')))
        self.resize(1120,820)
        self.setMinimumSize(860,640)
        self.online=False
        self.epoch=0
        self.status={}
        self.active=False
        self.starting=False
        self.focused=True
        self.closing=False
        self.busy=False
        self.status_busy=False
        self.status_failures=0
        self.current_page='control'
        self.ticket=0
        self.callbacks={}
        self.keys=set()
        self.held=set()
        self.axes={'lx':0,'ly':0,'rx':0,'ry':0}
        self.triggers={'lt':0,'rt':0}
        self.port=port
        self.initial_host=host
        self.gate=True
        self.pad_buttons={}
        self.form_widgets={}
        self.data={}
        self.dirty=set()
        self.loading_form=False
        self.settings_buttons=[]
        self.prefs=self.load_preferences()
        self.bindings=self.prefs.get('keys',DEFAULT_KEYS).copy()
        self.colors=THEMES[self.prefs.get('theme','darkone')]
        self.worker=worker or NetworkWorker(rate)
        self.controllers=controllers or SdlControllers()
        self.build_ui(host)
        self.worker.result.connect(self.on_result)
        self.worker.control.connect(self.on_control)
        self.worker.finished.connect(self.worker_finished)
        self.worker.start()
        self.apply_theme(self.prefs.get('theme','darkone'))
        QApplication.instance().installEventFilter(self)
        self.sample_timer=QTimer(self);self.sample_timer.timeout.connect(self.sample_input);self.sample_timer.start(16)
        self.scan_timer=QTimer(self);self.scan_timer.timeout.connect(self.scan_controllers);self.scan_timer.start(1000)
        self.status_timer=QTimer(self);self.status_timer.timeout.connect(self.refresh_status);self.status_timer.start(6500)
        self.scan_controllers()
        self.set_online(False)
        self.notify('Ready. Connect to your adapter.')

    def preferences_path(self):
        root=Path(QStandardPaths.writableLocation(QStandardPaths.StandardLocation.AppConfigLocation))
        return root/'python-companion.json'

    def load_preferences(self):
        defaults={'theme':'darkone','host':'','source':'keyboard','keys':DEFAULT_KEYS.copy()}
        try:
            path=self.preferences_path()
            if not path.exists():return defaults
            if path.stat().st_size>65536:return defaults
            saved=json.loads(path.read_text(encoding='utf-8'))
            if not isinstance(saved,dict):return defaults
            if saved.get('theme') in THEMES:defaults['theme']=saved['theme']
            if isinstance(saved.get('host'),str):defaults['host']=saved['host'][:255]
            if saved.get('source') in ('keyboard','gamepad'):defaults['source']=saved['source']
            keys=saved.get('keys',{})
            if isinstance(keys,dict) and set(keys)==set(DEFAULT_KEYS):
                values=[v for v in keys.values() if v is not None]
                if all(valid_key(v) for v in values) and len(values)==len(set(values)):defaults['keys']=keys
        except (OSError,ValueError,TypeError):pass
        return defaults

    def save_preferences(self):
        self.prefs.update(theme=self.theme.currentData(),host=self.address.text().strip(),
            source=self.source.currentData(),keys=self.bindings)
        try:
            path=self.preferences_path();path.parent.mkdir(parents=True,exist_ok=True)
            temporary=path.with_suffix('.tmp')
            temporary.write_text(json.dumps(self.prefs,indent=2),encoding='utf-8')
            os.replace(temporary,path)
        except OSError as error:self.notify('Preferences could not be saved: '+str(error),True)

    def build_ui(self,host):
        root=QWidget();root.setObjectName('root');self.setCentralWidget(root)
        shell=QVBoxLayout(root);shell.setContentsMargins(26,22,26,18);shell.setSpacing(20)
        header=QHBoxLayout()
        logo=QLabel();logo.setPixmap(QPixmap(str(ASSETS/'webui-logo.png')).scaled(46,46,Qt.AspectRatioMode.KeepAspectRatio,Qt.TransformationMode.SmoothTransformation))
        header.addWidget(logo)
        brand=QVBoxLayout();name=label('DC-OGX-W');name.setObjectName('brand');name.setWordWrap(False);brand.addWidget(name)
        subtitle=label('CONTROLLER ADAPTER','eyebrow');subtitle.setWordWrap(False);brand.addWidget(subtitle);header.addLayout(brand)
        header.addStretch()
        self.chip=label('● Offline');self.chip.setObjectName('chip');self.chip.setFixedHeight(32);header.addWidget(self.chip)
        header.addWidget(button('Connect',lambda:self.navigate('connection')))
        shell.addLayout(header)
        body=QHBoxLayout();body.setSpacing(20);shell.addLayout(body,1)
        sidebar=QFrame();sidebar.setObjectName('sidebar');sidebar.setFixedWidth(185)
        nav=QVBoxLayout(sidebar);nav.setContentsMargins(12,16,12,16);nav.setSpacing(5)
        self.nav_buttons={}
        self.pages=QStackedWidget();self.page_indices={}
        for key,title in [('control','Control'),('profiles','Profiles'),('network','Wi-Fi'),('led','LED'),('device','Device'),('connection','Connection'),('about','About')]:
            b=button(title,lambda checked=False,key=key:self.navigate(key),'nav');b.setCheckable(True);b.setChecked(key=='control');nav.addWidget(b);self.nav_buttons[key]=b
            page=QWidget();page.setObjectName('page');layout=QVBoxLayout(page);layout.setContentsMargins(0,0,9,5);layout.setSpacing(18)
            scroll=QScrollArea();scroll.setWidgetResizable(True);scroll.setWidget(page)
            self.page_indices[key]=self.pages.addWidget(scroll)
            getattr(self,'build_'+key)(layout)
            layout.addStretch()
        nav.addStretch()
        nav.addWidget(label('APPEARANCE','eyebrow'))
        self.theme=QComboBox()
        for key,title in THEME_LABELS.items():self.theme.addItem(title,key)
        self.theme.setCurrentIndex(self.theme.findData(self.prefs['theme']))
        self.theme.currentIndexChanged.connect(lambda:self.theme_changed())
        nav.addWidget(self.theme)
        nav.addWidget(label('Saved on this computer.','muted'))
        nav.addSpacing(10);nav.addWidget(label('PYTHON COMPANION\nPySide6 · '+VERSION,'muted'))
        body.addWidget(sidebar);body.addWidget(self.pages,1)
        self.footer=label('Ready.');self.footer.setObjectName('footer');shell.addWidget(self.footer)

    def intro(self,layout,eyebrow,title,subtitle):
        box=QVBoxLayout();box.setSpacing(6);box.addWidget(label(eyebrow.upper(),'eyebrow'))
        box.addWidget(label(title,'heading'));box.addWidget(label(subtitle,'muted'));layout.addLayout(box)

    def build_control(self,layout):
        self.intro(layout,'Live input','Take the controls.','Keyboard, on-screen controls or a controller connected to your computer.')
        panel,box=card('');box.setSpacing(8);box.setContentsMargins(18,16,18,16)
        row=QHBoxLayout()
        self.source=QComboBox();self.source.addItem('Keyboard + on-screen','keyboard');self.source.addItem('PC controller · SDL','gamepad')
        self.source.setCurrentIndex(self.source.findData(self.prefs['source']));self.source.currentIndexChanged.connect(self.source_changed)
        self.controller=QComboBox();self.controller.setMinimumWidth(160);self.controller.currentIndexChanged.connect(self.controller_changed)
        row.addWidget(self.source);row.addWidget(self.controller,1);box.addLayout(row)
        actions=QHBoxLayout();self.take_button=button('Take control · F8',self.take_control,'primary');self.release_button=button('Release',self.release_control)
        self.control_chip=label('Released');self.control_chip.setObjectName('chip')
        actions.addWidget(self.take_button);actions.addWidget(self.release_button);actions.addStretch();actions.addWidget(self.control_chip);box.addLayout(actions)
        self.control_message=label('Connect, then Take control. Wi-Fi input is selected automatically.');self.control_message.setObjectName('controlMessage');box.addWidget(self.control_message)
        shoulders=QHBoxLayout()
        self.trigger_sliders={}
        self.trigger_values={}
        for name,title,role in [('lt','White / LB','white'),('rt','Black / RB','black')]:
            group=QVBoxLayout();top=QHBoxLayout();top.addWidget(label(name.upper(),'muted'))
            slider=QSlider(Qt.Orientation.Horizontal);slider.setRange(0,255);slider.setAccessibleName(name.upper()+' trigger')
            slider.valueChanged.connect(lambda value,key=name:self.trigger_changed(key,value))
            slider.sliderReleased.connect(lambda key=name:self.reset_trigger(key))
            top.addWidget(slider,1);value_label=label('0','muted');top.addWidget(value_label)
            self.trigger_sliders[name]=slider;self.trigger_values[name]=value_label;group.addLayout(top)
            group.addWidget(self.pad_button(title,'lb' if name=='lt' else 'rb',role));shoulders.addLayout(group,1)
        box.addLayout(shoulders)
        middle=QHBoxLayout();dpad=QGridLayout()
        for name,title,r,c in [('up','↑',0,1),('left','←',1,0),('right','→',1,2),('down','↓',2,1)]:dpad.addWidget(self.pad_button(title,name),r,c)
        middle.addLayout(dpad,1);middle.addStretch()
        menus=QVBoxLayout()
        for name,title in [('back','Back'),('start','Start'),('guide','Guide')]:menus.addWidget(self.pad_button(title,name))
        middle.addLayout(menus);middle.addStretch();face=QGridLayout()
        for name,title,r,c in [('y','Y',0,1),('x','X',1,0),('b','B',1,2),('a','A',2,1)]:face.addWidget(self.pad_button(title,name),r,c)
        middle.addLayout(face,1);box.addLayout(middle)
        sticks=QHBoxLayout();self.sticks={};self.axis_labels={}
        for side,title in [('l','LEFT STICK'),('r','RIGHT STICK')]:
            group=QVBoxLayout();stick=Stick();stick.changed.connect(lambda x,y,side=side:self.stick_changed(side,x,y));self.sticks[side]=stick
            group.addWidget(stick,0,Qt.AlignmentFlag.AlignHCenter)
            caption=QHBoxLayout();caption.addWidget(label(title,'eyebrow'));caption.addWidget(self.pad_button(side.upper()+'3',side+'3'));group.addLayout(caption)
            readout=label('0 / 0','muted');self.axis_labels[side]=readout;group.addWidget(readout,0,Qt.AlignmentFlag.AlignHCenter);sticks.addLayout(group,1)
        box.addLayout(sticks);layout.addWidget(panel)
        mapping,mapbox=card('Keyboard mapping','Click an assignment, then press a key. Conflicts swap automatically.')
        grid=QGridLayout();self.key_buttons={}
        for i,(name,title) in enumerate(KEY_LABELS.items()):
            b=button(key_name(self.bindings[name]),lambda checked=False,name=name:self.capture_key(name));b.setAccessibleName(title+' assignment')
            self.key_buttons[name]=b;row,col=divmod(i,2);grid.addWidget(label(title,'muted'),row,col*2);grid.addWidget(b,row,col*2+1)
        mapbox.addLayout(grid);mapbox.addWidget(button('Reset keys',self.reset_keys));layout.addWidget(mapping)

    def pad_button(self,title,name,role=None):
        b=button(title,role=role);b.setObjectName('padButton');b.setMinimumWidth(44);b.setAccessibleName(KEY_LABELS.get(name,name))
        b.pressed.connect(lambda:self.hold_button(name,True));b.released.connect(lambda:self.hold_button(name,False))
        self.pad_buttons[name]=b;return b

    def build_connection(self,layout):
        host=self.initial_host
        self.intro(layout,'Connection','Find your adapter.','Discover nearby adapters or connect using an IP address or local hostname.')
        panel,box=card('Connect to adapter','Pico W, Pico 2 W and ESP8285 clone firmware with Remote API v2.')
        box.addWidget(label('DEVICE ADDRESS','eyebrow'));self.address=QLineEdit(host if host!='192.168.4.1' else self.prefs.get('host') or host)
        self.address.setPlaceholderText('192.168.4.1 or dc-ogx-w.local');self.address.returnPressed.connect(self.connect_adapter);box.addWidget(self.address)
        row=QHBoxLayout();self.connect_button=button('Connect',self.connect_adapter,'primary');self.disconnect_button=button('Disconnect',self.disconnect_adapter)
        row.addWidget(self.connect_button);row.addWidget(self.disconnect_button);row.addStretch();box.addLayout(row)
        self.discover_button=button('Discover nearby adapters',self.discover_adapters);box.addWidget(self.discover_button)
        self.discovered=QVBoxLayout();box.addLayout(self.discovered)
        box.addWidget(label('Discovery uses UDP 38473 on the same LAN. Manual IP works when discovery or mDNS is blocked.','muted'));layout.addWidget(panel)
        self.health=Facts('Connection health');layout.addWidget(self.health)

    def build_profiles(self,layout):
        self.intro(layout,'Profiles','Choose your profile.','Select a saved controller profile and the adapter operating mode.')
        panel,box=card('Adapter operating mode','Physical accepts wired USB and available Bluetooth. Configuration is a settings state.')
        self.mode_buttons={}
        for key,title,subtitle in [('physical','Physical','Wired USB / Bluetooth controller'),('wifi','Wi-Fi remote','Desktop and browser input'),('configure','Configuration','Settings and calibration; console output is neutral')]:
            b=button(title+'\n'+subtitle,lambda checked=False,key=key:self.change_mode(key));b.setCheckable(True);box.addWidget(b);self.mode_buttons[key]=b
        self.mode_note=label('Connect to read the current mode.','muted');box.addWidget(self.mode_note);layout.addWidget(panel)
        panel,box=card('Saved profiles','Use Configuration mode before selecting a profile when required by the firmware.')
        box.addWidget(button('Refresh profiles',lambda:self.load_page('profiles',force=True)))
        self.profile_list=QVBoxLayout();box.addLayout(self.profile_list);layout.addWidget(panel)
        layout.addWidget(label('Full profile editing, calibration and firmware updates remain available in the device WebUI.','muted'))
        layout.addWidget(button('Open device WebUI',self.open_webui))

    def form_card(self,layout,kind,title,subtitle):
        panel,box=card(title,subtitle);box.addWidget(button('Refresh',lambda:self.load_page('device' if kind=='oled' else 'network' if kind=='network' else 'led',force=True)))
        form=QFormLayout();form.setSpacing(13);form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
        box.addLayout(form);self.form_widgets[kind]={}
        layout.addWidget(panel);return box,form

    def field(self,kind,form,key,title,widget):
        self.form_widgets[kind][key]=widget;widget.setAccessibleName(title)
        form.addRow(label(title,'muted'),widget)
        signal=widget.toggled if isinstance(widget,QCheckBox) else widget.valueChanged if isinstance(widget,(QSlider,QSpinBox,QDoubleSpinBox)) else widget.currentIndexChanged if isinstance(widget,QComboBox) else widget.textEdited
        signal.connect(lambda *_:self.mark_dirty(kind));return widget

    def check(self,kind,form,key,title):return self.field(kind,form,key,title,QCheckBox())
    def number(self,kind,form,key,title,low,high):
        w=QSpinBox();w.setRange(low,high);return self.field(kind,form,key,title,w)

    def build_device(self,layout):
        self.intro(layout,'Device','Configure your display.','Brightness, idle behavior and startup options for the 128 × 32 OLED.')
        box,form=self.form_card(layout,'oled','OLED display','Apply previews the current session. Save writes settings and restarts the adapter.')
        self.check('oled',form,'enabled','Display enabled')
        self.number('oled',form,'brightness','Brightness · 0–255',0,255)
        self.number('oled',form,'idle_brightness','Idle brightness · 0–255',0,255)
        self.number('oled',form,'dim_after_s','Dim after · seconds',0,3600)
        self.number('oled',form,'off_after_s','Screen off after · seconds',0,3600)
        for key,title in [('invert','Invert pixels'),('flip','Rotate 180°'),('fade','Startup fade')]:self.check('oled',form,key,title)
        self.config_buttons(box,'oled')

    def build_led(self,layout):
        self.intro(layout,'RGB indicator','Set your colour.','Profile colours and diagnostic states follow the device WebUI.')
        box,form=self.form_card(layout,'led','RGB indicator','Preview colours with Apply, or save and restart to retain them.')
        self.check('led',form,'enabled','LED enabled');self.number('led',form,'brightness','Brightness · 0–255',0,255)
        mode=QComboBox()
        for key,title in [('combined','Profiles + diagnostics'),('profiles','Profiles only'),('diagnostics','Diagnostics only')]:mode.addItem(title,key)
        self.field('led',form,'mode','Display mode',mode)
        box.addWidget(label('PROFILE COLOURS','eyebrow'));self.profile_colors=QGridLayout();box.addLayout(self.profile_colors)
        box.addWidget(label('DIAGNOSTIC COLOURS','eyebrow'));self.diagnostic_colors=QGridLayout();box.addLayout(self.diagnostic_colors)
        self.color_buttons={};self.config_buttons(box,'led')

    def build_network(self,layout):
        self.intro(layout,'Wi-Fi','Stay connected.','Join a 2.4 GHz home network and manage the recovery connection.')
        box,form=self.form_card(layout,'network','Home network','Saving restarts the adapter. Reconnect using its current IP after boot.')
        self.check('network',form,'station_enabled','Join home Wi-Fi on startup')
        ssid=QLineEdit();ssid.setMaxLength(32);self.field('network',form,'station_ssid','Network name',ssid)
        password=QLineEdit();password.setEchoMode(QLineEdit.EchoMode.Password);password.setMaxLength(64);password.setPlaceholderText('Blank keeps the saved password')
        self.field('network',form,'station_password','Password',password)
        self.check('network',form,'open','Open network · no password')
        hostname=QLineEdit();hostname.setMaxLength(32);self.field('network',form,'hostname','Local hostname',hostname)
        row=QHBoxLayout()
        for title,callback in [('Save & join',self.save_wifi),('Forget network',lambda:self.save_wifi(True))]:
            b=button(title,callback,'primary' if title.startswith('Save') else None);self.settings_buttons.append(('network',b));row.addWidget(b)
        box.addLayout(row)
        self.network_facts=Facts('Network status');layout.addWidget(self.network_facts)
        b=button('Retry home Wi-Fi',lambda:self.write_api('/api/network/retry',{},'Station retry requested.'))
        self.settings_buttons.append(('online',b));layout.addWidget(b)

    def build_about(self,layout):
        self.intro(layout,'About','Made for DC-OGX-W.','A native Python companion by Darkone Customs.')
        info=Facts('Python companion '+VERSION);info.update_facts([('Interface','PySide6 / Qt 6'),('Controller input','Keyboard · On-screen · SDL standard mappings'),
            ('Transport','UDP · HTTP · WebSocket'),('Target systems','Windows · Linux · macOS'),('Preferences','Local user configuration folder')]);layout.addWidget(info)
        panel,box=card('One adapter, the same controls','The original Python command line, timed sequences, RemoteClient / PadState API and localhost browser bridge remain available.')
        box.addWidget(label('The desktop retains the WebUI branding and eight themes. Input uses the adapter’s current profile, mappings and calibration.','muted'))
        box.addWidget(label('Use the device WebUI for full profile editing, calibration and OTA firmware upload. Physical includes USB and supported Bluetooth; Wi-Fi remote accepts PC input.','muted'))
        box.addWidget(label('SDL controller availability depends on the operating system, driver and mapping database. Hardware and OS testing is still required.','muted'))
        box.addWidget(button('Open device WebUI',self.open_webui));layout.addWidget(panel)

    def config_buttons(self,box,kind):
        row=QHBoxLayout()
        for action,title in [('apply','Apply now'),('save','Save & restart')]:
            b=button(title,lambda checked=False,kind=kind,action=action:self.save_config(kind,action),'primary' if action=='apply' else None)
            self.settings_buttons.append((kind,b));row.addWidget(b)
        box.addLayout(row)

    def notify(self,text,error=False):
        self.footer.setText(text)
        self.footer.setStyleSheet('color:'+self.colors['bad' if error else 'muted']+';')

    def apply_theme(self,name):
        self.colors=THEMES.get(name,THEMES['darkone'])
        self.setStyleSheet(stylesheet(self.colors))
        for stick in self.sticks.values():stick.colors=self.colors;stick.update()

    def theme_changed(self):
        self.apply_theme(self.theme.currentData());self.save_preferences()

    def set_online(self,online):
        if self.online!=online:self.epoch+=1
        self.online=online
        if not online:self.active=False;self.status_failures=0;self.reset_input()
        self.chip.setText('● Status delayed' if online and self.status_failures>=2 else '● Connected' if online else '● Offline')
        self.connect_button.setEnabled(not online and not self.busy)
        self.disconnect_button.setEnabled(online and not self.busy)
        self.address.setEnabled(not online and not self.busy)
        self.take_button.setEnabled(online and not self.active and not self.starting and not self.busy)
        self.release_button.setEnabled(self.active or self.starting)
        physical=self.status.get('control',{}).get('physical_supported') is not False
        pending=self.status.get('control',{}).get('mode_pending') is True
        for key,b in self.mode_buttons.items():b.setEnabled(online and not self.busy and not pending and (physical or key=='wifi'))
        for kind,b in self.settings_buttons:
            available=kind=='online' or (kind in self.data and self.data[kind].get('available') is not False)
            b.setEnabled(online and available and not self.busy)
        self.set_pad_available()

    def submit(self,operation,payload=None,callback=None,*,busy=False,scoped=True):
        self.ticket+=1
        ticket=self.ticket
        self.callbacks[ticket]=(callback,self.epoch if scoped else None,busy)
        if busy:self.busy=True;self.set_online(self.online)
        self.worker.submit(ticket,operation,payload)
        return ticket

    def on_result(self,ticket,operation,data,error):
        entry=self.callbacks.pop(ticket,None)
        if entry is None:return
        callback,epoch,busy=entry
        if operation=='status':self.status_busy=False
        if operation=='discover':self.discover_button.setEnabled(True)
        if busy:self.busy=False
        if epoch is not None and epoch!=self.epoch:
            self.set_online(self.online);return
        if error:
            if operation=='status':
                self.status_failures+=1
                if self.status_failures==2:self.notify('Status telemetry is delayed. Check the adapter connection.',True)
            elif operation=='disconnect':
                self.set_online(False);self.notify('Disconnected locally. '+error,True)
            else:self.notify(error,True)
        elif callback:
            try:callback(data)
            except (ValueError,TypeError,KeyError) as failure:self.notify('Unexpected adapter response: '+str(failure),True)
        self.set_online(self.online)

    def connect_adapter(self):
        if self.busy or self.online:return
        try:host=device_url(self.address.text())
        except ValueError as error:self.notify(str(error),True);return
        def connected(status):
            self.data.clear();self.dirty.clear();self.status_failures=0
            self.set_online(True);self.update_status(status);self.save_preferences()
            self.notify('Connected to '+str(status.get('board','DC-OGX-W'))+'. Take control automatically selects Wi-Fi input.')
            self.load_page(self.current_page)
        self.submit('connect',{'host':host},connected,busy=True,scoped=False)
        self.notify('Connecting to the adapter…')

    def disconnect_adapter(self):
        if not self.online:return
        self.reset_input()
        def disconnected(_):
            self.set_online(False);self.data.clear();self.dirty.clear();self.notify('Disconnected. Local controller session stopped.')
            self.health.update_facts([('Connection','Not connected')]);self.network_facts.update_facts([('Connection','Not connected')])
        self.submit('disconnect',callback=disconnected,busy=True)

    def discover_adapters(self):
        if not self.discover_button.isEnabled():return
        self.discover_button.setEnabled(False);self.notify('Looking for adapters on the local network…')
        def discovered(items):
            self.clear_layout(self.discovered)
            for item in items:
                address=f"{item['ip']}:{item['http_port']}"
                b=button(f"{item['board']}  ·  {address}",lambda checked=False,address=address:self.pick_adapter(address))
                self.discovered.addWidget(b)
            self.notify(f'Found {len(items)} adapter(s).' if items else 'No discovery reply. Try the current IP and check the private-network firewall.')
        self.submit('discover',callback=discovered,scoped=False)

    def pick_adapter(self,address):
        if self.online:self.notify('Disconnect from the current adapter before selecting another.');return
        self.address.setText(address);self.connect_adapter()

    def refresh_status(self):
        if not self.online or self.status_busy or self.busy or self.active or self.starting:return
        self.status_busy=True
        self.submit('status',callback=self.update_status)

    def update_status(self,status):
        self.status=status;self.status_failures=0
        n=status.get('network',{});c=status.get('control',{})
        self.health.update_facts([('Board',n.get('board',status.get('board'))),('Firmware',status.get('firmware')),
            ('Remote API',status.get('remote_api')),('IP address',n.get('ip')),('Active profile',status.get('applied_profile_id',status.get('profile_id'))),
            ('USB output','Connected' if n.get('usb_configured') else 'Not configured'),('Wired controller','Connected' if n.get('wired_connected') else 'Not connected'),
            ('Bluetooth','Connected' if c.get('bluetooth_connected') else 'Available' if c.get('bluetooth_supported') else 'Unavailable'),('Input mode',c.get('mode'))])
        self.network_facts.update_facts([('Network',n.get('ssid')),('Address',n.get('ip')),('Station IP',n.get('station_ip')),
            ('Hostname',n.get('hostname')),('Radio',n.get('transport')),('Home Wi-Fi','Connected' if n.get('station_connected') else 'Not connected'),
            ('Recovery AP','Active' if n.get('ap_active') else 'Standby'),('Reconnects',n.get('station_reconnects')),('Error',n.get('station_error') or n.get('error') or 'None')])
        names={'physical':'Physical · USB / Bluetooth','wifi':'Wi-Fi remote','configure':'Configuration'}
        self.mode_note.setText('Current: '+names.get(c.get('mode'),str(c.get('mode','Unknown')))+(' · transition pending' if c.get('mode_pending') else '')+
            ('\nBluetooth is unavailable on this board. Physical USB remains available when supported.' if c.get('bluetooth_supported') is False else ''))
        for key,b in self.mode_buttons.items():b.setChecked(c.get('mode')==key)
        self.set_online(self.online)

    def navigate(self,page):
        if page==self.current_page:return
        if self.current_page=='control' and (self.active or self.starting):self.release_control()
        self.reset_input();self.current_page=page
        self.pages.setCurrentIndex(self.page_indices[page])
        for key,b in self.nav_buttons.items():b.setChecked(key==page)
        self.load_page(page)

    def load_page(self,page,force=False):
        if not self.online:return
        if page in ('connection','profiles','network'):self.refresh_status()
        kind={'device':'oled','led':'led','network':'network'}.get(page)
        if kind:
            if kind in self.dirty and not force:return
            if kind in self.dirty and not self.confirm('Reload settings?','Discard your unsaved '+kind+' edits and reload from the adapter?'):return
            path='/api/network/config' if kind=='network' else '/api/'+kind
            self.submit('api',{'path':path},lambda data,kind=kind:self.populate_form(kind,data))
        elif page=='profiles':self.submit('api',{'path':'/api/profiles'},self.populate_profiles)

    def populate_profiles(self,data):
        self.clear_layout(self.profile_list)
        for profile in data.get('profiles',[]):
            active=data.get('applied_profile_id',data.get('profile_id'))==profile['id']
            title=profile.get('name') or f"Profile {profile['id']}"
            b=button(title+(' · Active' if active else ''),lambda checked=False,ident=profile['id']:self.select_profile(ident),'primary' if active else None)
            self.profile_list.addWidget(b)
        if not data.get('profiles'):self.profile_list.addWidget(label('No saved profiles were returned.','muted'))

    def select_profile(self,ident):
        if not self.online or self.busy:return
        if self.status.get('dirty') and not self.confirm('Replace unsaved profile?','Selecting a profile replaces the unsaved profile edits on the adapter.'):return
        self.write_api('/api/profile/select',{'id':ident},f'Selected profile {ident}.',after=lambda:self.load_page('profiles'))

    def change_mode(self,mode):
        if not self.online or self.busy:return
        self.write_api('/api/control/mode',{'mode':mode},'Mode change requested. Wait for the adapter to finish.',after=self.refresh_status)
        QTimer.singleShot(1500,self.refresh_status)

    def mark_dirty(self,kind):
        if not self.loading_form:self.dirty.add(kind)

    def populate_form(self,kind,data):
        self.loading_form=True
        try:
            self.data[kind]=data
            for key,w in self.form_widgets[kind].items():
                value=data.get(key)
                if kind=='network' and key=='station_password':value=''
                if kind=='network' and key=='open':value=bool(data.get('station_enabled') and data.get('station_ssid') and not data.get('password_saved'))
                if isinstance(w,QCheckBox):w.setChecked(bool(value))
                elif isinstance(w,QSpinBox):w.setValue(value if type(value) is int else 0)
                elif isinstance(w,QComboBox):w.setCurrentIndex(max(0,w.findData(value)))
                else:w.setText(value if isinstance(value,str) else '')
            if kind=='led':
                self.clear_layout(self.profile_colors);self.clear_layout(self.diagnostic_colors);self.color_buttons={}
                for i,color in enumerate(data.get('profile_colors',[])):self.add_color('profile_'+str(i),'Profile '+str(i+1),color,self.profile_colors,i)
                for i,(key,color) in enumerate(data.get('diagnostic_colors',{}).items()):self.add_color('diagnostic_'+key,key.replace('_',' ').title(),color,self.diagnostic_colors,i)
            self.dirty.discard(kind)
        finally:self.loading_form=False
        self.set_online(self.online)
        if data.get('available') is False:self.notify(kind.upper()+' is not enabled in this firmware build.')

    def add_color(self,key,title,value,grid,index):
        b=button(value,lambda checked=False,key=key:self.choose_color(key));b.setProperty('colorValue',value)
        self.color_buttons[key]=b;self.paint_color_button(b,value)
        grid.addWidget(label(title,'muted'),index//2,index%2*2);grid.addWidget(b,index//2,index%2*2+1)

    def paint_color_button(self,b,value):
        color=QColor(value)
        if not color.isValid():color=QColor('#000000')
        ink='#101510' if color.lightness()>150 else '#ffffff'
        b.setStyleSheet(f'background:{color.name()};color:{ink};border:1px solid {self.colors["line"]};')
        b.setText(color.name().upper());b.setProperty('colorValue',color.name().upper())

    def choose_color(self,key):
        b=self.color_buttons[key]
        value=QColorDialog.getColor(QColor(b.property('colorValue')),self,'Select colour')
        if value.isValid():self.paint_color_button(b,value.name());self.mark_dirty('led')

    def collect_form(self,kind):
        values={}
        for key,w in self.form_widgets[kind].items():
            values[key]=w.isChecked() if isinstance(w,QCheckBox) else w.value() if isinstance(w,QSpinBox) else w.currentData() if isinstance(w,QComboBox) else w.text()
        if kind=='led':
            values['profile_colors']=[self.color_buttons['profile_'+str(i)].property('colorValue') for i in range(len(self.data['led'].get('profile_colors',[])))]
            values['diagnostic_colors']={key:self.color_buttons['diagnostic_'+key].property('colorValue') for key in self.data['led'].get('diagnostic_colors',{})}
        return values

    def save_config(self,kind,action):
        if not self.online or self.busy or kind not in self.data:return
        if action=='save' and not self.confirm('Save and restart?','Save '+kind.upper()+' settings to the adapter and restart it?'):return
        body=self.collect_form(kind)
        def saved(_):
            self.dirty.discard(kind)
            if action=='save':self.set_online(False)
            self.notify('Settings saved. Reconnect after the adapter restarts.' if action=='save' else 'Settings applied for this session.')
        self.reset_input();self.submit('api',{'path':f'/api/{kind}/{action}','body':body},saved,busy=True)

    def save_wifi(self,forget=False):
        if not self.online or self.busy or 'network' not in self.data:return
        values=self.collect_form('network');old=self.data['network']
        if forget:body={'forget':True}
        else:
            import re
            hostname=values['hostname'].strip().lower()
            if not re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?',hostname):self.notify('Enter a valid local hostname.',True);return
            ssid=values['station_ssid'];password=values['station_password']
            if len(ssid.encode('utf-8'))>32 or (values['station_enabled'] and not ssid):self.notify('Network name must contain 1–32 UTF-8 bytes.',True);return
            if password and not (8<=len(password.encode('utf-8'))<=63 or re.fullmatch('[0-9a-fA-F]{64}',password)):
                self.notify('Use an 8–63 byte Wi-Fi password or a 64-character hex key.',True);return
            if values['station_enabled'] and not values['open'] and not password and not (ssid==old.get('station_ssid') and old.get('password_saved')):
                self.notify('Enter a password or select an open network.',True);return
            body={'station_enabled':values['station_enabled'],'station_ssid':ssid,'hostname':hostname}
            if values['open']:body['station_password']=''
            elif password:body['station_password']=password
        if not self.confirm('Save Wi-Fi and restart?','Forget the saved home network and restart?' if forget else 'Save network settings and restart the adapter?'):return
        def saved(_):
            self.form_widgets['network']['station_password'].clear();self.dirty.discard('network');self.set_online(False)
            self.notify('Network settings saved. Reconnect using the current IP after restart.')
        self.reset_input();self.submit('api',{'path':'/api/network/config','body':body},saved,busy=True)

    def write_api(self,path,body,message,after=None):
        if not self.online or self.busy:return
        self.reset_input()
        def complete(_):
            self.notify(message)
            if after:after()
        self.submit('api',{'path':path,'body':body},complete,busy=True)

    def confirm(self,title,text):
        return QMessageBox.question(self,title,text,QMessageBox.StandardButton.Yes|QMessageBox.StandardButton.No,QMessageBox.StandardButton.No)==QMessageBox.StandardButton.Yes

    def open_webui(self):
        if not self.online:self.notify('Connect to an adapter first.',True);return
        self.release_control(show=False)
        QDesktopServices.openUrl(QUrl(device_url(self.address.text())+'/'))

    def take_control(self):
        if not self.online or self.active or self.starting or self.busy:return
        if self.source.currentData()=='gamepad' and self.controllers.current is None:
            self.notify('Connect an SDL-mapped controller or choose keyboard input.',True);return
        self.reset_input();self.starting=True;self.set_online(self.online)
        def acquired(_):self.notify('Controller session acquired. Centre controls before starting. Esc releases.')
        self.submit('acquire',callback=acquired,busy=True)

    def release_control(self,show=True):
        self.reset_input()
        def released(_):
            self.active=False;self.starting=False
            self.refresh_status()
            if show:self.notify('Controller session stopped.')
        self.submit('release',callback=released,scoped=False)

    def source_changed(self):
        if not hasattr(self,'worker') or not hasattr(self,'controller'):return
        self.reset_input();self.save_preferences();self.set_pad_available()

    def controller_changed(self):
        self.reset_input();self.controllers.select(self.controller.currentData() if self.controller.currentData() is not None else -1)

    def scan_controllers(self):
        choices=self.controllers.scan()
        previous=self.controller.currentData()
        old=[(self.controller.itemData(i),self.controller.itemText(i)) for i in range(self.controller.count())]
        if not self.controller.count() or (old!=choices and not (not choices and old==[(-1,'No mapped controller')])):
            self.controller.blockSignals(True);self.controller.clear()
            for ident,name in choices:self.controller.addItem(name,ident)
            if not choices:self.controller.addItem('No mapped controller',-1)
            found=self.controller.findData(previous)
            self.controller.setCurrentIndex(max(0,found));self.controller.blockSignals(False)
        selected=self.controller.currentData()
        if selected!=self.controllers.index or self.controllers.current is None:
            self.reset_input();self.controllers.select(selected if selected is not None else -1)
        self.controller.setEnabled(self.source.currentData()=='gamepad')

    def set_pad_available(self):
        available=self.online and self.active and self.source.currentData()=='keyboard' and self.current_page=='control'
        for b in self.pad_buttons.values():b.setEnabled(available)
        for slider in self.trigger_sliders.values():slider.setEnabled(available)
        for stick in self.sticks.values():stick.interactive=available

    def hold_button(self,name,down):
        if down and self.active and self.source.currentData()=='keyboard':self.held.add(name)
        else:self.held.discard(name)
        self.sample_input()

    def trigger_changed(self,key,value):
        self.trigger_values[key].setText(str(value))
        if self.active and self.source.currentData()=='keyboard':self.triggers[key]=value;self.sample_input()

    def reset_trigger(self,key):
        self.triggers[key]=0;self.trigger_sliders[key].setValue(0);self.sample_input()

    def stick_changed(self,side,x,y):
        self.axes[side+'x'],self.axes[side+'y']=x,y
        self.sample_input()

    def reset_input(self):
        self.keys.clear();self.held.clear();self.gate=True
        for key in self.axes:self.axes[key]=0
        for key in self.triggers:self.triggers[key]=0
        if hasattr(self,'trigger_sliders'):
            for slider in self.trigger_sliders.values():slider.blockSignals(True);slider.setValue(0);slider.blockSignals(False)
        if hasattr(self,'sticks'):
            for stick in self.sticks.values():stick.blockSignals(True);stick.reset();stick.blockSignals(False)
        self.worker.pause(self.focused)

    def sample_input(self):
        ready=self.focused and self.current_page=='control' and not self.closing
        pad=PadState()
        if ready and self.active:
            if self.source.currentData()=='gamepad':
                sample=self.controllers.sample()
                ready=sample is not None
                pad=sample or PadState()
            else:pad=keyboard_snapshot(self.keys,self.bindings,self.held,self.axes,self.triggers)
        self.worker.set_sample(pad,ready=ready)

    def on_control(self,status):
        if time.monotonic()-status.get('at',0)>.3:return
        self.active=status['active'];self.starting=status['starting']
        self.control_chip.setText('● Owned' if self.active else 'Starting…' if self.starting else 'Released')
        message='Connect to your adapter and take control.'
        if self.active:
            if not self.focused:message='Input paused while the app is unfocused. Centre controls before resuming.'
            elif self.source.currentData()=='gamepad' and self.controllers.current is None:message='No mapped PC controller. Input stays neutral until it reconnects.'
            elif status['needs_neutral']:message='Centre sticks and release all controls to begin.'
            else:message=f"Controller active · {status['rate_hz']} Hz · Esc or F8 releases."
        elif self.starting:message='Selecting Wi-Fi input and starting a controller session…'
        elif self.online:message='Connected. Take control automatically selects Wi-Fi input.'
        self.control_message.setText(message)
        p=status.get('pad',dataclasses.asdict(PadState()))
        for name,b in self.pad_buttons.items():
            pressed=bool(p['buttons']&BUTTONS.get(name,0) or p['dpad']&DPAD.get(name,0))
            if bool(b.property('pressed'))!=pressed:
                b.setProperty('pressed',pressed);b.style().unpolish(b);b.style().polish(b)
        for side,stick in self.sticks.items():
            stick.set_pad(p[side+'x'],p[side+'y']);self.axis_labels[side].setText(f"{p[side+'x']} / {p[side+'y']}")
        for key in ('lt','rt'):
            self.trigger_values[key].setText(str(p[key]))
            if not self.trigger_sliders[key].isSliderDown():
                self.trigger_sliders[key].blockSignals(True);self.trigger_sliders[key].setValue(p[key]);self.trigger_sliders[key].blockSignals(False)
        if status.get('error'):self.notify(status['error'],True)
        self.set_online(self.online)

    def capture_key(self,name):
        self.release_control(show=False)
        dialog=KeyCapture(KEY_LABELS[name],self)
        if dialog.exec()==QDialog.DialogCode.Accepted:
            conflict=next((key for key,value in self.bindings.items() if key!=name and value==dialog.value),None)
            if conflict:self.bindings[conflict]=self.bindings[name]
            self.bindings[name]=dialog.value
            for key,b in self.key_buttons.items():b.setText(key_name(self.bindings[key]))
            self.save_preferences()

    def reset_keys(self):
        self.release_control(show=False);self.bindings=DEFAULT_KEYS.copy()
        for key,b in self.key_buttons.items():b.setText(key_name(self.bindings[key]))
        self.save_preferences();self.notify('Keyboard mapping reset.')

    def eventFilter(self,obj,event):
        kind=event.type()
        if kind in (QEvent.Type.MouseButtonRelease,QEvent.Type.FocusOut,QEvent.Type.TouchEnd,QEvent.Type.TouchCancel):
            for key,slider in self.trigger_sliders.items():
                if obj is slider:QTimer.singleShot(0,lambda key=key:self.reset_trigger(key))
        if kind in (QEvent.Type.KeyPress,QEvent.Type.KeyRelease):
            if QApplication.activeModalWidget() is not None:return False
            key=event.key()
            if event.isAutoRepeat():return key in self.keys
            if kind==QEvent.Type.KeyPress and key==int(K.Key_Escape) and (self.active or self.starting):self.release_control();return True
            if kind==QEvent.Type.KeyPress and key==int(K.Key_F8):
                self.release_control() if self.active or self.starting else self.take_control();return True
            if kind==QEvent.Type.KeyRelease and key in self.keys:self.keys.discard(key);self.sample_input();return True
            editing=isinstance(QApplication.focusWidget(),(QLineEdit,QComboBox,QSpinBox,QDoubleSpinBox))
            if not editing and self.active and self.focused and self.current_page=='control' and self.source.currentData()=='keyboard' and key in self.bindings.values():
                if kind==QEvent.Type.KeyPress:self.keys.add(key)
                else:self.keys.discard(key)
                self.sample_input();return True
        if kind in (QEvent.Type.WindowDeactivate,QEvent.Type.WindowActivate) and obj is self:
            self.focused=kind==QEvent.Type.WindowActivate
            self.reset_input();self.worker.pause(self.focused)
        return super().eventFilter(obj,event)

    @staticmethod
    def clear_layout(layout):
        while layout.count():
            item=layout.takeAt(0)
            if item.widget():item.widget().deleteLater()
            elif item.layout():CompanionWindow.clear_layout(item.layout())

    def closeEvent(self,event):
        if not self.worker.isRunning():
            self.controllers.close();QApplication.instance().removeEventFilter(self);event.accept();return
        event.ignore()
        if self.closing:return
        self.closing=True;self.reset_input();self.worker.pause(False)
        self.sample_timer.stop();self.scan_timer.stop();self.status_timer.stop()
        self.centralWidget().setEnabled(False);self.notify('Releasing control and closing…')
        self.worker.stop()

    def worker_finished(self):
        if self.closing:self.close()


def run_desktop(host='192.168.4.1',*,port=8765,rate=60):
    app=QApplication.instance() or QApplication([sys.argv[0]])
    app.setOrganizationName('Darkone Customs');app.setApplicationName('DC-OGX-W')
    app.setStyle('Fusion')
    font=app.font();font.setPointSize(10);app.setFont(font)
    window=CompanionWindow(host,port,rate)
    window.show()
    return app.exec()
