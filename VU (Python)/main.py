# main.py
import sys
import logging
from PyQt6.QtWidgets import QApplication
from ui.main_window import MainWindow

# Настройка логгирования в консоль (для отладки)
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)

def main():
    app = QApplication(sys.argv)
    
    # Применяем базовую темную тему оформления (опционально, но приятно)
    app.setStyle("Fusion") 
    
    window = MainWindow()
    window.show()
    sys.exit(app.exec())

if __name__ == "__main__":
    main()
    