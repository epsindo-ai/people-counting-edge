import subprocess

import cv2
import numpy as np
import onnxruntime as ort

COCO_CLASSES = ["PLATE","paper","scissors"]
CLASS_COLORS = [(0, 255, 0), (255, 0, 0), (0, 0, 255)]  # one per class

RTSP_INPUT = "rtsp://192.168.1.218:8080/h264.sdp"

RTSP_OUTPUT = "rtsp://localhost:8554/stream"
STREAM_WIDTH = 640
STREAM_HEIGHT = 480
FPS = 25

session = ort.InferenceSession(
    "/app/models/yolox_n_lp.onnx",
    providers=["CUDAExecutionProvider","CPUExecutionProvider"]
)

input_name = session.get_inputs()[0].name

cap = cv2.VideoCapture(RTSP_INPUT)

INPUT_SIZE = 640
THRESHOLD = 0.7
# TODO: Fix error bounding box misaligned


ffmpeg = subprocess.Popen(
[
    "ffmpeg",
    "-loglevel","error",
    "-f","rawvideo",
    "-pix_fmt","bgr24",
    "-s",f"{STREAM_WIDTH}x{STREAM_HEIGHT}",
    "-r",str(FPS),
    "-i","-",
    "-c:v","libx264",
    "-preset","ultrafast",
    "-tune","zerolatency",
    "-g","10",
    "-f","rtsp",
    RTSP_OUTPUT
],
stdin=subprocess.PIPE
)


while True:

    ret, frame = cap.read()
    if not ret:
        break

    h, w = frame.shape[:2]
    ratio = min(INPUT_SIZE / h, INPUT_SIZE / w)
    # print(ratio)
    scale_x = w / INPUT_SIZE
    scale_y = h / INPUT_SIZE

# When drawing boxes:

    img = cv2.resize(frame, (INPUT_SIZE, INPUT_SIZE))
    img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)

    blob = img.transpose(2, 0, 1)
    blob = np.expand_dims(blob, axis=0).astype(np.uint8)

    outputs = session.run(None, {input_name: blob})

    num = int(outputs[0][0][0])
    boxes = outputs[1][0]
    scores = outputs[2][0]
    class_ids = outputs[3][0]

    for i in range(num):

        score = scores[i]
        if score < THRESHOLD:
            continue

        x1,y1,x2,y2 = boxes[i]
        x1 = int(boxes[i][0] * scale_x)
        y1 = int(boxes[i][1] * scale_y)
        x2 = int(boxes[i][2] * scale_x)
        y2 = int(boxes[i][3] * scale_y)

        label = COCO_CLASSES[int(class_ids[i])]
        color = CLASS_COLORS[int(class_ids[i])]

        cv2.rectangle(frame,(x1,y1),(x2,y2),color,2)
        cv2.putText(frame,label,(x1,y1-5),
                    cv2.FONT_HERSHEY_SIMPLEX,1,color,2)
    out_frame = cv2.resize(frame,(STREAM_WIDTH,STREAM_HEIGHT))
    ffmpeg.stdin.write(out_frame.tobytes())

cap.release()
ffmpeg.stdin.close()
ffmpeg.wait()