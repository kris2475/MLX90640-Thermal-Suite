import threading
import tkinter as tk
from tkinter import ttk, filedialog
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
import matplotlib.pyplot as plt
import numpy as np
import cv2
import serial
import serial.tools.list_ports
import time
from datetime import datetime
import csv

ROWS = 24
COLS = 32

class ThermalApp:
    def __init__(self, root):
        self.root = root
        self.root.title("Pro Thermal Suite - Teensy MLX90640")
        self.root.geometry("1300x850")

        self.ser = None
        self.is_connected = False
        self.running = True
        self.latest_frame = np.zeros((ROWS, COLS))
        self.frame_lock = threading.Lock()

        # Time-series history buffers (last 60 data points)
        self.time_history = []
        self.max_history = []
        self.min_history = []
        self.avg_history = []
        self.start_time = time.time()

        # Logging state
        self.is_recording = False
        self.csv_writer = None
        self.csv_file = None

        # --- TOP CONTROL PANEL ---
        control_frame = tk.Frame(root, padx=10, pady=10, bg="#1e272e")
        control_frame.pack(fill=tk.X)

        tk.Label(control_frame, text="Port:", fg="white", bg="#1e272e", font=("Arial", 9, "bold")).pack(side=tk.LEFT, padx=(0, 5))
        self.port_combobox = ttk.Combobox(control_frame, width=10, state="readonly")
        self.port_combobox.pack(side=tk.LEFT, padx=(0, 5))
        self.refresh_ports()

        refresh_btn = tk.Button(control_frame, text="🔄", command=self.refresh_ports, bg="#2f3640", fg="white")
        refresh_btn.pack(side=tk.LEFT, padx=(0, 10))

        tk.Label(control_frame, text="Baud:", fg="white", bg="#1e272e", font=("Arial", 9, "bold")).pack(side=tk.LEFT, padx=(0, 5))
        self.baud_combobox = ttk.Combobox(control_frame, width=8, state="readonly")
        self.baud_combobox['values'] = [115200, 230400, 460800, 921600]
        self.baud_combobox.set("460800")
        self.baud_combobox.pack(side=tk.LEFT, padx=(0, 10))

        tk.Label(control_frame, text="Colormap:", fg="white", bg="#1e272e", font=("Arial", 9, "bold")).pack(side=tk.LEFT, padx=(0, 5))
        self.cmap_combobox = ttk.Combobox(control_frame, width=9, state="readonly")
        self.cmap_combobox['values'] = ['inferno', 'plasma', 'magma', 'turbo', 'jet', 'coolwarm']
        self.cmap_combobox.set("inferno")
        self.cmap_combobox.pack(side=tk.LEFT, padx=(0, 10))

        tk.Label(control_frame, text="CV Mode:", fg="white", bg="#1e272e", font=("Arial", 9, "bold")).pack(side=tk.LEFT, padx=(0, 5))
        self.mode_combobox = ttk.Combobox(control_frame, width=12, state="readonly")
        self.mode_combobox['values'] = ['Standard', 'Edge Detection', 'Contours']
        self.mode_combobox.set("Standard")
        self.mode_combobox.pack(side=tk.LEFT, padx=(0, 15))

        self.connect_btn = tk.Button(control_frame, text="Connect", command=self.toggle_connection, bg="#2ecc71", fg="white", font=("Arial", 9, "bold"))
        self.connect_btn.pack(side=tk.LEFT, padx=(0, 10))

        self.record_btn = tk.Button(control_frame, text="🔴 Record CSV", command=self.toggle_recording, bg="#e74c3c", fg="white", font=("Arial", 9, "bold"))
        self.record_btn.pack(side=tk.LEFT, padx=(0, 10))

        self.snapshot_btn = tk.Button(control_frame, text="📸 Snapshot", command=self.take_snapshot, bg="#3498db", fg="white", font=("Arial", 9, "bold"))
        self.snapshot_btn.pack(side=tk.LEFT)

        # --- CONTENT AREA (Plots + Side Panel) ---
        content_frame = tk.Frame(root)
        content_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)

        # Matplotlib Subplots (Row 0: Heatmap, Row 1: Time Series Chart)
        self.fig, (self.ax_heat, self.ax_chart) = plt.subplots(2, 1, figsize=(7, 8), gridspec_kw={'height_ratios': [3, 1.5]})
        self.fig.tight_layout(pad=3.0)

        # Heatmap setup
        self.im = self.ax_heat.imshow(self.latest_frame, cmap='inferno', interpolation='bicubic', vmin=15, vmax=40)
        self.cbar = self.fig.colorbar(self.im, ax=self.ax_heat, fraction=0.046, pad=0.04)
        self.cbar.set_label('Temp (°C)', rotation=270, labelpad=12)
        self.ax_heat.set_title("Live Thermal Feed & Hotspot Tracking", fontsize=11, fontweight='bold')

        # Hotspot / Coldspot graphic annotations
        self.max_marker, = self.ax_heat.plot([], [], 'rX', markersize=10, label='Max (Hotspot)')
        self.min_marker, = self.ax_heat.plot([], [], 'bo', markersize=8, label='Min (Coldspot)')
        self.ax_heat.legend(loc='upper right', fontsize=8)

        # Strip chart setup
        self.line_max, = self.ax_chart.plot([], [], 'r-', label='Max Temp')
        self.line_avg, = self.ax_chart.plot([], [], 'g-', label='Avg Temp')
        self.line_min, = self.ax_chart.plot([], [], 'b-', label='Min Temp')
        self.ax_chart.set_title("Temperature History (°C)", fontsize=10, fontweight='bold')
        self.ax_chart.set_xlim(0, 60)
        self.ax_chart.set_ylim(15, 50)
        self.ax_chart.legend(loc='upper left', fontsize=8)
        self.ax_chart.grid(True, linestyle='--', alpha=0.6)

        self.canvas = FigureCanvasTkAgg(self.fig, master=content_frame)
        self.canvas.draw()
        self.canvas.get_tk_widget().pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        # --- SIDE TELEMETRY DASHBOARD ---
        self.side_panel = tk.Frame(content_frame, width=310, padx=15, pady=15, bg="#dcdde1", relief=tk.SUNKEN, borderwidth=1)
        self.side_panel.pack(side=tk.RIGHT, fill=tk.Y, padx=(10, 0))
        self.side_panel.pack_propagate(False)  # <-- LOCKS THE WIDTH PERMANENTLY

        tk.Label(self.side_panel, text="Analytics & Alarms", font=("Arial", 12, "bold"), bg="#dcdde1", fg="#2f3640").pack(anchor="w", pady=(0, 10))

        self.lbl_max = tk.Label(self.side_panel, text="Max Temp: -- °C", font=("Arial", 10, "bold"), bg="#dcdde1", fg="#c0392b", anchor="w")
        self.lbl_max.pack(fill=tk.X, pady=4)

        self.lbl_min = tk.Label(self.side_panel, text="Min Temp: -- °C", font=("Arial", 10, "bold"), bg="#dcdde1", fg="#2980b9", anchor="w")
        self.lbl_min.pack(fill=tk.X, pady=4)

        self.lbl_avg = tk.Label(self.side_panel, text="Avg Temp: -- °C", font=("Arial", 10, "bold"), bg="#dcdde1", fg="#27ae60", anchor="w")
        self.lbl_avg.pack(fill=tk.X, pady=4)

        tk.Frame(self.side_panel, height=2, bd=1, relief=tk.SUNKEN, bg="#bdc3c7").pack(fill=tk.X, pady=12)

        # Alarm Control Settings
        tk.Label(self.side_panel, text="Alarm Threshold (°C):", font=("Arial", 9, "bold"), bg="#dcdde1", fg="#2f3640").pack(anchor="w")
        self.alarm_slider = ttk.Scale(self.side_panel, from_=25, to=100, value=40, orient=tk.HORIZONTAL)
        self.alarm_slider.pack(fill=tk.X, pady=5)
        self.lbl_alarm_val = tk.Label(self.side_panel, text="Threshold: 40 °C", font=("Arial", 9), bg="#dcdde1", fg="#2f3640")
        self.lbl_alarm_val.pack(anchor="w", pady=(0, 5))
        self.alarm_slider.configure(command=self.update_alarm_label)

        self.lbl_alarm_status = tk.Label(self.side_panel, text="Status: NORMAL", font=("Arial", 10, "bold"), bg="#27ae60", fg="white", pady=5)
        self.lbl_alarm_status.pack(fill=tk.X, pady=10)

        tk.Frame(self.side_panel, height=2, bd=1, relief=tk.SUNKEN, bg="#bdc3c7").pack(fill=tk.X, pady=10)

        self.lbl_status = tk.Label(self.side_panel, text="Connection: Disconnected", font=("Arial", 9, "italic"), bg="#dcdde1", fg="#7f8c8d", wraplength=270, justify="left")
        self.lbl_status.pack(fill=tk.X)

        # Background worker loop & UI ticker
        self.read_thread = threading.Thread(target=self.serial_reader_loop, daemon=True)
        self.read_thread.start()
        self.update_ui()

    def refresh_ports(self):
        ports = [port.device for port in serial.tools.list_ports.comports()]
        self.port_combobox['values'] = ports
        if ports:
            self.port_combobox.current(0)
        else:
            self.port_combobox.set("")

    def toggle_connection(self):
        if not self.is_connected:
            port = self.port_combobox.get()
            baud = self.baud_combobox.get()
            if not port:
                self.lbl_status.config(text="Error: No port selected.")
                return
            try:
                self.ser = serial.Serial(port, int(baud), timeout=1)
                self.is_connected = True
                self.connect_btn.config(text="Disconnect", bg="#c0392b")
                self.port_combobox.config(state="disabled")
                self.baud_combobox.config(state="disabled")
                self.lbl_status.config(text=f"Connected to {port}")
            except Exception as e:
                self.lbl_status.config(text=f"Error: {e}")
        else:
            if self.is_recording:
                self.toggle_recording()
            if self.ser and self.ser.is_open:
                self.ser.close()
            self.is_connected = False
            self.connect_btn.config(text="Connect", bg="#2ecc71")
            self.port_combobox.config(state="readonly")
            self.baud_combobox.config(state="readonly")
            self.lbl_status.config(text="Connection: Disconnected")

    def update_alarm_label(self, val):
        self.lbl_alarm_val.config(text=f"Threshold: {float(val):.1f} °C")

    def toggle_recording(self):
        if not self.is_recording:
            filename = f"thermal_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
            filepath = filedialog.asksaveasfilename(defaultextension=".csv", initialfile=filename, filetypes=[("CSV Files", "*.csv")])
            if filepath:
                self.csv_file = open(filepath, mode='w', newline='')
                self.csv_writer = csv.writer(self.csv_file)
                header = ['Timestamp', 'MaxTemp', 'MinTemp', 'AvgTemp'] + [f'P_{i}' for i in range(768)]
                self.csv_writer.writerow(header)
                self.is_recording = True
                self.record_btn.config(text="⏹ Stop CSV", bg="#7f8c8d")
        else:
            self.is_recording = False
            if self.csv_file:
                self.csv_file.close()
            self.record_btn.config(text="🔴 Record CSV", bg="#e74c3c")

    def take_snapshot(self):
        filename = f"thermal_pro_{datetime.now().strftime('%Y%m%d_%H%M%S')}.png"
        filepath = filedialog.asksaveasfilename(defaultextension=".png", initialfile=filename, filetypes=[("PNG Image", "*.png")])
        if filepath:
            self.fig.savefig(filepath, dpi=300, bbox_inches='tight')
            self.lbl_status.config(text=f"Saved snapshot to {filepath.split('/')[-1]}")

    def serial_reader_loop(self):
        while self.running:
            if self.is_connected and self.ser and self.ser.is_open:
                try:
                    line = self.ser.readline().decode('ascii', errors='ignore').strip()
                    if line.startswith("FRAME:"):
                        vals = line[6:].split(',')
                        if len(vals) == ROWS * COLS:
                            data_points = [float(v) for v in vals]
                            frame = np.array(data_points).reshape((ROWS, COLS))
                            with self.frame_lock:
                                self.latest_frame = frame
                            
                            if self.is_recording and self.csv_writer:
                                timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]
                                row_data = [timestamp, np.max(frame), np.min(frame), np.mean(frame)] + data_points
                                self.csv_writer.writerow(row_data)
                except Exception:
                    pass
            else:
                time.sleep(0.1)

    def update_ui(self):
        if self.running:
            with self.frame_lock:
                data = self.latest_frame.copy()
            
            if np.any(data):
                mode = self.mode_combobox.get()
                selected_cmap = self.cmap_combobox.get()
                self.im.set_cmap(selected_cmap)

                min_val, max_val = np.min(data), np.max(data)
                min_idx = np.argmin(data)
                max_idx = np.argmax(data)
                min_y, min_x = divmod(min_idx, COLS)
                max_y, max_x = divmod(max_idx, COLS)

                if mode == 'Edge Detection':
                    norm = cv2.normalize(data, None, 0, 255, cv2.NORM_MINMAX).astype(np.uint8)
                    resized = cv2.resize(norm, (320, 240), interpolation=cv2.INTER_CUBIC)
                    edges = cv2.Canny(resized, 50, 150)
                    self.im.set_data(edges)
                    self.im.set_cmap('gray')
                    self.max_marker.set_data([], [])
                    self.min_marker.set_data([], [])
                elif mode == 'Contours':
                    norm = cv2.normalize(data, None, 0, 255, cv2.NORM_MINMAX).astype(np.uint8)
                    resized = cv2.resize(norm, (320, 240), interpolation=cv2.INTER_CUBIC)
                    filtered = cv2.bilateralFilter(resized, 9, 75, 75)
                    self.im.set_data(filtered)
                    self.im.set_cmap(selected_cmap)
                    self.max_marker.set_data([], [])
                    self.min_marker.set_data([], [])
                else:
                    self.im.set_data(data)
                    self.im.set_clim(vmin=min_val, vmax=max_val)
                    self.max_marker.set_data([max_x], [max_y])
                    self.min_marker.set_data([min_x], [min_y])

                current_time = time.time() - self.start_time
                self.time_history.append(current_time)
                self.max_history.append(max_val)
                self.avg_history.append(np.mean(data))
                self.min_history.append(min_val)

                if len(self.time_history) > 60:
                    self.time_history.pop(0)
                    self.max_history.pop(0)
                    self.avg_history.pop(0)
                    self.min_history.pop(0)

                self.line_max.set_data(self.time_history, self.max_history)
                self.line_avg.set_data(self.time_history, self.avg_history)
                self.line_min.set_data(self.time_history, self.min_history)
                self.ax_chart.set_xlim(max(0, current_time - 60), current_time + 2)
                
                all_vals = self.max_history + self.min_history
                if all_vals:
                    self.ax_chart.set_ylim(min(all_vals) - 2, max(all_vals) + 2)

                self.canvas.draw()

                self.lbl_max.config(text=f"Max Temp: {max_val:.2f} °C (X:{max_x}, Y:{max_y})")
                self.lbl_min.config(text=f"Min Temp: {min_val:.2f} °C (X:{min_x}, Y:{min_y})")
                self.lbl_avg.config(text=f"Avg Temp: {np.mean(data):.2f} °C")

                threshold = self.alarm_slider.get()
                if max_val >= threshold:
                    self.lbl_alarm_status.config(text=f"⚠️ OVERHEAT ALERT! (>{threshold}°C)", bg="#c0392b")
                    self.side_panel.config(bg="#ff7675")
                else:
                    self.lbl_alarm_status.config(text="Status: NORMAL", bg="#27ae60")
                    self.side_panel.config(bg="#dcdde1")

            self.root.after(40, self.update_ui)

    def on_close(self):
        self.running = False
        if self.is_recording and self.csv_file:
            self.csv_file.close()
        if self.ser and self.ser.is_open:
            self.ser.close()
        self.root.destroy()

if __name__ == "__main__":
    root = tk.Tk()
    app = ThermalApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()