import io, os, numpy as np
from flask import Flask, request, jsonify
from PIL import Image
import tensorflow as tf
from datetime import datetime

CLASS_NAMES = ["LOW", "MEDIUM", "HIGH"]
N_VALUES    = [2.2, 3.0, 3.8]

model = tf.keras.models.load_model("rf_classifier.keras")
app   = Flask(__name__)

os.makedirs("captures", exist_ok=True)
latest_n = {"class": "MEDIUM", "n_value": 3.0}

@app.route("/classify", methods=["POST"])
def classify():
    global latest_n
    raw = request.data

    img = Image.open(io.BytesIO(raw)).rotate(180)  # fix upside-down mounting

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    img.save(f"captures/{timestamp}.jpg")

    img  = img.convert("L").resize((224, 224))
    x    = np.array(img)
    x    = np.repeat(x[..., None], 3, axis=-1)
    x    = (x / 127.5 - 1)[np.newaxis]
    idx  = int(np.argmax(model.predict(x, verbose=0)))
    latest_n = {"class": CLASS_NAMES[idx], "n_value": N_VALUES[idx]}
    print(f"Saved {timestamp}.jpg — {CLASS_NAMES[idx]}")
    return jsonify(latest_n)

@app.route("/n_value", methods=["GET"])
def n_value():
    return jsonify(latest_n)

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
