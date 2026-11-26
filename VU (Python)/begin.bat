py -m venv venv
call venv\Scripts\activate
pip install -r requirements.txt
python -c "import PyQt6, cv2, requests; print('OK')"
pause