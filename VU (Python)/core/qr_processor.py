# core/qr_processor.py
import cv2
import numpy as np
from pyzbar.pyzbar import decode
import logging

logger = logging.getLogger(__name__)

class QRProcessor:
    @staticmethod
    def process_frame(image_bytes: bytes):
        """
        Принимает байты JPEG, возвращает:
        - Изображение в формате BGR (для отображения в UI)
        - Список найденных строк (текст QR)
        - Список координат рамок (для отрисовки)
        """
        if not image_bytes:
            return None, [], []

        # 1. Декодируем байты в картинку OpenCV
        np_arr = np.frombuffer(image_bytes, np.uint8)
        img = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)

        if img is None:
            return None, [], []

        # 2. Ищем QR-коды
        decoded_objects = decode(img)
        
        qr_data = []
        rects = []

        for obj in decoded_objects:
            # Декодируем текст (бывает, что кодировка не UTF-8)
            try:
                text = obj.data.decode('utf-8')
            except UnicodeDecodeError:
                text = obj.data.decode('latin-1', errors='replace')
            
            qr_data.append(text)
            
            # Получаем координаты рамки
            rect = obj.rect
            pts = np.array([
                [rect.left, rect.top],
                [rect.left + rect.width, rect.top],
                [rect.left + rect.width, rect.top + rect.height],
                [rect.left, rect.top + rect.height]
            ], np.int32)
            rects.append(pts)

        # 3. Рисуем рамки на изображении (для UI)
        for pts in rects:
            cv2.polylines(img, [pts], True, (0, 255, 0), 2)

        return img, qr_data, rects
        