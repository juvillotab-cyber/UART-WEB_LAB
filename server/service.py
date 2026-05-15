from flask import Flask, render_template
from flask_socketio import SocketIO
import serial
import threading

# UART
ser = serial.Serial(
    '/dev/ttyAMA3',
    baudrate=921600,
    timeout=1
)

# Flask
app = Flask(__name__)
socketio = SocketIO(
    app,
    cors_allowed_origins="*",
    async_mode="threading"
)

@app.route("/")
def index():
    return render_template("index.html")

def read_uart():
    while True:
        try:
            line = ser.readline().decode().strip()

            if line:
                print("UART:", line)

                socketio.emit("uart_data", {
                    "data": line
                })

        except Exception as e:
            print(e)

thread = threading.Thread(target=read_uart)
thread.daemon = True
thread.start()

if __name__ == "__main__":

    print("SERVIDOR INICIANDO...")

    socketio.run(
    app,
    host="0.0.0.0",
    port=5000,
    debug=False,
    use_reloader=False,
    allow_unsafe_werkzeug=True
)

