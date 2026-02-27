#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import math
import tkinter as tk

from pyftdi.i2c import I2cPort
from pyftdi.i2c import I2cController

PMU_ADDR    = 0x74
WIN_WIDTH   = 330
WIN_HEIGHT  = 240
UPDATE_RATE = 10

REF         = 0x00
IOUT_LIMIT  = 0x02
VOUT_SR     = 0x03
VOUT_FS     = 0x04
CDC         = 0x05
MODE        = 0x06
STATUS      = 0x07

MODE_OE     = 1 << 7
ILIMIT_EN   = 1 << 7
STATUS_SCP  = 1 << 7
STATUS_OCP  = 1 << 6
STATUS_OVP  = 1 << 5

INTFB       = 0.0564
VREF_MAX    = 0x7fe
IREF_MIN    = 45.0
IREF_STEP   = 0.5645

class TPS55289:
    com: I2cPort
    reg: bytearray

    def __init__(self, port: str, *, frequency: int = 400_000):
        i2c = I2cController()
        i2c.configure(port, frequency = frequency) # type: ignore
        self.com = i2c.get_port(PMU_ADDR)
        self.reg = bytearray(8)

    @property
    def vref(self) -> int:
        return int.from_bytes(self.reg[:2], 'little')

    @vref.setter
    def vref(self, vref: int):
        buf = (vref & 0x7ff).to_bytes(2, 'little')
        self.reg[REF:REF + 2] = buf
        self.com.write_to(REF, buf)

    @property
    def mode(self) -> int:
        return self.reg[MODE]

    @mode.setter
    def mode(self, mode: int):
        self.reg[MODE] = mode
        self.com.write_to(MODE, [mode])

    @property
    def status(self) -> int:
        return self.reg[STATUS]

    @property
    def output(self) -> bool:
        return self.mode & MODE_OE != 0

    @output.setter
    def output(self, enable: bool):
        if enable:
            self.mode |= MODE_OE
        else:
            self.mode &= ~MODE_OE

    @property
    def voltage(self) -> float:
        return (self.vref * IREF_STEP + IREF_MIN) / INTFB / 1000.0

    @voltage.setter
    def voltage(self, volt: float):
        vref = math.ceil(((volt * 1000.0 * INTFB) - IREF_MIN) / IREF_STEP)
        self.vref = max(min(vref, VREF_MAX), 0)

    def update(self):
        self.reg = bytearray(self.com.read_from(0, 8))
        assert len(self.reg) == 8

win = tk.Tk()
win.title('TPS55289')
win.update_idletasks()

pmu = TPS55289('ftdi://ftdi:2232h/1')
pmu.update()

vset = pmu.voltage
voltage = tk.DoubleVar()
output_en = tk.BooleanVar()

voltage.set(pmu.voltage)
output_en.set(pmu.output)

win.rowconfigure(0, weight = 1)
win.columnconfigure(0, weight = 1)

frame = tk.Frame(win)
frame.grid(row = 0, column = 0, sticky = 'nsew')

def update_volts():
    if vset != pmu.voltage:
        pmu.voltage = vset

def update_stats():
    pmu.update()
    voltage.set(pmu.voltage)
    output_en.set(pmu.output)

def render_stats():
    canvas_status.itemconfig(op_buck, fill = 'RoyalBlue1' if pmu.status & 3 else 'black')
    canvas_status.itemconfig(op_boost, fill = 'RoyalBlue1' if not pmu.status & 1 else 'black')
    canvas_status.itemconfig(status_oen, fill = 'green' if pmu.output else 'black')
    canvas_status.itemconfig(status_scp, fill = 'red' if pmu.status & STATUS_SCP else 'black')
    canvas_status.itemconfig(status_ocp, fill = 'red' if pmu.status & STATUS_OCP else 'black')
    canvas_status.itemconfig(status_ovp, fill = 'red' if pmu.status & STATUS_OVP else 'black')

def on_status_update():
    update_volts()
    update_stats()
    render_stats()
    win.after(UPDATE_RATE, on_status_update)

def on_output_en_toggle():
    pmu.output = output_en.get()

def on_fixed_voltage_select(volt: float):
    voltage.set(volt)
    on_voltage_slider_change(None)

def on_voltage_slider_change(*_):
    global vset
    vset = voltage.get()

slider_voltage = tk.Scale(
    master     = frame,
    command    = on_voltage_slider_change,
    from_      = IREF_MIN / INTFB / 1000.0,
    label      = 'Output Voltage',
    orient     = 'horizontal',
    resolution = IREF_STEP / 1000.0,
    to         = (VREF_MAX * IREF_STEP + IREF_MIN) / INTFB / 1000.0,
    variable   = voltage,
)

button_fixed_volts = [
    tk.Button(
        master  = frame,
        text    = f'{volt}V',
        height  = 2,
        command = (lambda v: lambda: on_fixed_voltage_select(v))(float(volt)),
    )
    for volt in [5, 9, 12, 15, 20]
]

checkbox_output_en = tk.Checkbutton(
    master   = frame,
    text     = 'Output enable',
    variable = output_en,
    command  = on_output_en_toggle,
)

canvas_status = tk.Canvas(master = frame, height = 50)
status_oen = canvas_status.create_oval(20, 10, 60, 50)
status_scp = canvas_status.create_oval(70, 10, 110, 50)
status_ocp = canvas_status.create_oval(120, 10, 160, 50)
status_ovp = canvas_status.create_oval(170, 10, 210, 50)
op_boost = canvas_status.create_rectangle(270, 10, 310, 50)
op_buck = canvas_status.create_rectangle(220, 10, 260, 50)

canvas_status.create_text(40, 30, text = 'OE')
canvas_status.create_text(90, 30, text = 'SCP')
canvas_status.create_text(140, 30, text = 'OCP')
canvas_status.create_text(190, 30, text = 'OVP')
canvas_status.create_text(240, 30, text = 'Buck')
canvas_status.create_text(290, 30, text = 'Boost')

canvas_status.itemconfig(op_buck, fill = 'black')
canvas_status.itemconfig(op_boost, fill = 'black')
canvas_status.itemconfig(status_oen, fill = 'black')
canvas_status.itemconfig(status_scp, fill = 'black')
canvas_status.itemconfig(status_ocp, fill = 'black')
canvas_status.itemconfig(status_ovp, fill = 'black')

canvas_status.grid(row = 3, columnspan = 5, sticky = 'nsew')
slider_voltage.grid(row = 0, columnspan = 5, padx = 10, sticky = 'nsew')
checkbox_output_en.grid(row = 2, columnspan = 5)

for i, btn in enumerate(button_fixed_volts):
    lpad = 0 if i != 0 else 10
    rpad = 0 if i != len(button_fixed_volts) - 1 else 10
    btn.grid(row = 1, column = i, padx = (lpad, rpad))

frame.rowconfigure(tuple(range(4)), weight = 1)
frame.columnconfigure(tuple(range(5)), weight = 1)

win_x = round((win.winfo_screenwidth() - WIN_WIDTH) / 2)
win_y = round((win.winfo_screenheight() - WIN_HEIGHT) / 2)

win.after(UPDATE_RATE, on_status_update)
win.geometry(f'{WIN_WIDTH}x{WIN_HEIGHT}+{win_x}+{win_y}')
win.resizable(False, False)
win.mainloop()
