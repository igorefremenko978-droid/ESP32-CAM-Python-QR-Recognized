# core/camera_client.py
import requests
import logging

logger = logging.getLogger(__name__)

class CameraClient:
    def __init__(self, ip: str, port: int = 80):
        self.ip = ip                # <-- добавить
        self.port = port            # <-- добавить
        self.base_url = f"http://{ip}:{port}"
        self.timeout = 5.0          # Таймаут на случай, если ESP32 зависнет

    def check_connection(self) -> bool:
        """Проверка доступности камеры"""
        try:
            response = requests.get(f"{self.base_url}/", timeout=self.timeout)
            return response.status_code == 200
        except requests.RequestException:
            return False

    def capture_frame(self) -> bytes | None:
        """Запрос на получение кадра (JPEG)"""
        try:
            response = requests.get(f"{self.base_url}/capture", timeout=self.timeout)
            if response.status_code == 200:
                return response.content
            else:
                logger.error(f"Ошибка захвата: HTTP {response.status_code}")
                return None
        except requests.RequestException as e:
            logger.error(f"Ошибка сети при захвате кадра: {e}")
            return None

    def set_led(self, state: bool, brightness: int = 128) -> bool:
        """Управление светодиодом (включение/выключение и яркость)"""
        try:
            state_str = "on" if state else "off"
            url = f"{self.base_url}/led?state={state_str}&brightness={brightness}"
            response = requests.get(url, timeout=self.timeout)
            return response.status_code == 200
        except requests.RequestException as e:
            logger.error(f"Ошибка при управлении LED: {e}")
            return False
            