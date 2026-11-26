# ui/settings_dialog.py
from PyQt6.QtWidgets import (QDialog, QVBoxLayout, QFormLayout, 
                             QLineEdit, QDialogButtonBox, QSpinBox, QLabel)

class SettingsDialog(QDialog):
    def __init__(self, current_ip, current_port, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Настройки подключения")
        self.setFixedSize(300, 200)

        layout = QVBoxLayout(self)
        form = QFormLayout()

        # Поле для IP
        self.ip_input = QLineEdit()
        self.ip_input.setText(current_ip)
        form.addRow("IP-адрес камеры:", self.ip_input)

        # Поле для порта
        self.port_input = QSpinBox()
        self.port_input.setRange(1, 65535)
        self.port_input.setValue(current_port)
        form.addRow("Порт:", self.port_input)

        layout.addLayout(form)

        # Кнопки OK/Cancel
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def get_settings(self):
        return self.ip_input.text().strip(), self.port_input.value()
        