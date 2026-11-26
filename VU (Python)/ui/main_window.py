# ui/main_window.py
from PyQt6.QtWidgets import (QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
                             QPushButton, QLabel, QTextEdit, QCheckBox, 
                             QSpinBox, QGroupBox, QMessageBox)
from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtGui import QImage, QPixmap, QAction
import cv2
import logging

# Импорт наших модулей
from core.camera_client import CameraClient
from core.qr_processor import QRProcessor
from ui.settings_dialog import SettingsDialog

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("ESP32-CAM QR Scanner")
        self.resize(900, 700)

        # Состояние
        self.camera_client = CameraClient("192.168.88.206", 80) # Начальный IP
        self.is_auto_mode = False
        self.auto_timer = QTimer()
        self.auto_timer.timeout.connect(self.process_single_shot)

        self.init_ui()
        self.setup_menu()
        self.check_connection_startup()

    def init_ui(self):
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        main_layout = QHBoxLayout(central_widget)

        # --- Левая часть (Изображение) ---
        left_layout = QVBoxLayout()
        self.image_label = QLabel("Нет изображения\nНажмите «Снимок»")
        self.image_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.image_label.setStyleSheet("background-color: #222; color: white; border: 1px solid #444;")
        self.image_label.setMinimumSize(640, 480)
        left_layout.addWidget(self.image_label)
        
        main_layout.addLayout(left_layout, stretch=2)

        # --- Правая часть (Управление и Лог) ---
        right_layout = QVBoxLayout()

        # Группа: Ручное управление
        manual_group = QGroupBox("Ручное управление")
        manual_layout = QVBoxLayout()

        self.btn_snapshot = QPushButton("📷 Сделать снимок")
        self.btn_snapshot.setMinimumHeight(40)
        self.btn_snapshot.clicked.connect(self.process_single_shot)
        manual_layout.addWidget(self.btn_snapshot)

        # Чекбокс подсветки
        self.chk_flash = QCheckBox("Включить подсветку при съёмке")
        self.chk_flash.setChecked(True)
        manual_layout.addWidget(self.chk_flash)

        # Настройка яркости вспышки
        flash_layout = QHBoxLayout()
        flash_layout.addWidget(QLabel("Яркость:"))
        self.spin_flash = QSpinBox()
        self.spin_flash.setRange(0, 255)
        self.spin_flash.setValue(200)
        flash_layout.addWidget(self.spin_flash)
        manual_layout.addLayout(flash_layout)

        manual_group.setLayout(manual_layout)
        right_layout.addWidget(manual_group)

        # Группа: Автоматический режим
        auto_group = QGroupBox("Автоматический режим")
        auto_layout = QVBoxLayout()

        self.chk_auto = QCheckBox("Включить авто-сканирование")
        self.chk_auto.stateChanged.connect(self.toggle_auto_mode)
        auto_layout.addWidget(self.chk_auto)

        interval_layout = QHBoxLayout()
        interval_layout.addWidget(QLabel("Интервал (сек):"))
        self.spin_interval = QSpinBox()
        self.spin_interval.setRange(1, 60)
        self.spin_interval.setValue(3)
        self.spin_interval.valueChanged.connect(self.update_auto_interval)
        interval_layout.addWidget(self.spin_interval)
        auto_layout.addLayout(interval_layout)

        auto_group.setLayout(auto_layout)
        right_layout.addWidget(auto_group)

        # Лог
        right_layout.addWidget(QLabel("Результаты распознавания:"))
        self.log_text = QTextEdit()
        self.log_text.setReadOnly(True)
        self.log_text.setStyleSheet("background-color: #f4f4f4; font-family: Consolas;")
        right_layout.addWidget(self.log_text)

        main_layout.addLayout(right_layout, stretch=1)

    def setup_menu(self):
        menubar = self.menuBar()
        settings_menu = menubar.addMenu("Настройки")
        
        action_conn = QAction("Подключение...", self)
        action_conn.triggered.connect(self.open_settings)
        settings_menu.addAction(action_conn)

        action_exit = QAction("Выход", self)
        action_exit.triggered.connect(self.close)
        settings_menu.addAction(action_exit)

    def check_connection_startup(self):
        if self.camera_client.check_connection():
            self.log_text.append("✅ Камера доступна.")
        else:
            self.log_text.append("❌ Ошибка подключения. Проверьте IP в настройках.")

    def open_settings(self):
        dialog = SettingsDialog(self.camera_client.ip, self.camera_client.port, self)
        if dialog.exec():
            ip, port = dialog.get_settings()
            self.camera_client = CameraClient(ip, port)
            self.log_text.append(f"🔄 Настройки обновлены. Новый адрес: http://{ip}:{port}")
            self.check_connection_startup()

    def toggle_auto_mode(self, state):
        if state == 2: # Qt.CheckState.Checked
            self.is_auto_mode = True
            self.btn_snapshot.setEnabled(False)
            self.log_text.append(f"▶ Автоматический режим включен (каждые {self.spin_interval.value()} сек).")
            self.process_single_shot() # Запуск сразу
            self.auto_timer.start(self.spin_interval.value() * 1000)
        else:
            self.is_auto_mode = False
            self.btn_snapshot.setEnabled(True)
            self.auto_timer.stop()
            self.log_text.append("⏹ Автоматический режим остановлен.")

    def update_auto_interval(self, value):
        if self.is_auto_mode:
            self.auto_timer.start(value * 1000)
            self.log_text.append(f"⏱ Интервал изменен на {value} сек.")

    def process_single_shot(self):
        """Основная логика: Вспышка -> Снимок -> Выключение -> Распознавание"""
        use_flash = self.chk_flash.isChecked()
        flash_brightness = self.spin_flash.value()

        # 1. Включить вспышку
        if use_flash:
            self.camera_client.set_led(True, flash_brightness)
            # Даем время камере привыкнуть к свету (экспозиция)
            # В идеале нужно замерить задержку, но пока берем 0.3 сек.
            import time
            time.sleep(0.3)

        # 2. Получить кадр
        frame_bytes = self.camera_client.capture_frame()

        # 3. Выключить вспышку
        if use_flash:
            self.camera_client.set_led(False)

        if not frame_bytes:
            self.log_text.append("❌ Не удалось получить кадр с камеры.")
            return

        # 4. Обработать кадр
        img_bgr, qr_data, _ = QRProcessor.process_frame(frame_bytes)

        # 5. Отобразить
        if img_bgr is not None:
            self.display_image(img_bgr)
        else:
            self.log_text.append("❌ Ошибка декодирования изображения.")

        # 6. Вывести результат
        if qr_data:
            for text in qr_data:
                timestamp = __import__('datetime').datetime.now().strftime("%H:%M:%S")
                self.log_text.append(f"[{timestamp}] 🟢 QR: {text}")
        else:
            self.log_text.append("⚪ QR-код не найден.")

    def display_image(self, cv_img):
        """Конвертация OpenCV BGR -> QPixmap для отображения"""
        # Конвертируем BGR в RGB
        rgb_image = cv2.cvtColor(cv_img, cv2.COLOR_BGR2RGB)
        h, w, ch = rgb_image.shape
        bytes_per_line = ch * w
        qt_img = QImage(rgb_image.data, w, h, bytes_per_line, QImage.Format.Format_RGB888)
        
        # Масштабируем под размер Label (сохраняя пропорции)
        pixmap = QPixmap.fromImage(qt_img)
        pixmap = pixmap.scaled(
            self.image_label.size(), 
            Qt.AspectRatioMode.KeepAspectRatio, 
            Qt.TransformationMode.SmoothTransformation
        )
        self.image_label.setPixmap(pixmap)
        