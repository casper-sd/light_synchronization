import math

import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
import numpy as np
import sounddevice as sd
import soundfile as sf
from queue import Queue
from threading import Thread
from math import floor

data, SAMPLE_RATE = sf.read('monoman.wav', always_2d=True)
FRAME_SIZE = 1024
FR = np.zeros(FRAME_SIZE)
current_frame = 0
audio_queue = Queue(maxsize=100)

K = 9
COMP = np.linspace(0, K, K+1)
SIN = {
    0: [0, 1, 2, 3, 4, 5],
    1: [4, 5, 6, 7, 8, 9, 10],
    2: [9, 10, 11, 12, 13, 14, 15, 16],
    3: [15, 16, 18, 19, 20, 22, 23, 25, 26, 28],
    4: [26, 28, 30, 31, 33, 35, 37, 39, 42, 44, 46, 49],
    5: [46, 49, 51, 54, 57, 60, 63, 66, 69, 73, 76, 80, 84],
    6: [80, 84, 88, 92, 96, 101, 105, 110, 115, 121, 126, 132, 138, 144],
    7: [138, 144, 151, 157, 164, 172, 179, 187, 195, 204, 213, 222, 232, 242, 252, 263],
    8: [252, 263, 275, 286, 299, 311, 325, 339, 353, 368, 384, 400, 417, 435, 453, 472, 511]
}
L = []
for v in SIN.values():
    L += v
print(len(set(L)))

c_data = [[0, 255, 0] for _ in range(K)]
#
# CSN = [0 for _ in range(K)]


def incr_color(COL, ind, slope):
    if slope:
        COL[ind] += 1
    else:
        COL[ind] -= 1
    if COL[ind] == 255 or COL[ind] == 0:
        slope = not slope
        ind = (ind + 2) % 3

    return ind, slope


def calc_color():
    THRESHOLD = 0.7
    SMOOTHNESS = 0.1
    DROP_RATE = 0.005
    RISE_RATE = 0.003
    MAX = [0 for _ in range(K)]
    MIN = [math.inf for _ in range(K)]
    RATE = [0 for _ in range(K)]
    CD = [0 for _ in range(K)]
    CS = [0 for _ in range(K)]

    L_COLOR = [255, 0, 0]
    H_COLOR = [255, 128, 0]
    LCI = 1
    HCI = 1
    L_SLOPE = True
    H_SLOPE = True

    while True:
        d = audio_queue.get()
        spectrum = abs(np.fft.rfft(d))
        for i in range(K):
            CD[i] = 0
            B = SIN[i]
            for j in range(len(B) - 2):
                CC = 0
                for k in range(B[j], B[j+1]):
                    CC += spectrum[k] * (B[j+1] - k) / (B[j+1] - B[j])
                for k in range(B[j+1], B[j+2]):
                    CC += spectrum[k] * (B[j+2] - k) / (B[j+2] - B[j+1])
                if CC > CD[i]:
                    CD[i] = CC

        for i in range(K):
            R = (CD[i] - CS[i]) * SMOOTHNESS
            if abs(R) > RATE[i]:
                RATE[i] = R
            CS[i] += RATE[i]
            MAX[i] = CS[i] if CS[i] > MAX[i] else MAX[i] - DROP_RATE * (MAX[i] - CS[i])
            MIN[i] = CS[i] if CS[i] < MIN[i] else MIN[i] + RISE_RATE * (CS[i] - MIN[i])

        for k in range(K):
            C = 0 if MAX[k] == MIN[k] else (CS[k] - MIN[k]) / (MAX[k] - MIN[k])
            if C < THRESHOLD:
                c_data[k][0] = floor(L_COLOR[0] * C)
                c_data[k][1] = floor(L_COLOR[1] * C)
                c_data[k][2] = floor(L_COLOR[2] * C)
            else:
                c_data[k][0] = L_COLOR[0] + floor((H_COLOR[0] - L_COLOR[0]) * C)
                c_data[k][1] = L_COLOR[1] + floor((H_COLOR[1] - L_COLOR[1]) * C)
                c_data[k][2] = L_COLOR[2] + floor((H_COLOR[2] - L_COLOR[2]) * C)

        LCI, L_SLOPE = incr_color(L_COLOR, LCI, L_SLOPE)
        HCI, H_SLOPE = incr_color(H_COLOR, HCI, H_SLOPE)


def callback(outdata, frames, time, status):
    global current_frame, FR
    if status:
        print(status)
    chunksize = min(len(data) - current_frame, frames)
    d = data[current_frame:current_frame + chunksize]
    outdata[:chunksize] = d
    if d.shape[0] != 0:
        d = np.average(d, axis=1)
        d = np.concatenate((d, np.zeros(FRAME_SIZE - chunksize)), axis=0)
        audio_queue.put(d)
    if chunksize < frames:
        outdata[chunksize:] = 0
    current_frame += chunksize
    if current_frame == len(data):
        current_frame = 0


Thread(target=calc_color).start()

fig, ax = plt.subplots()
plt.ylim((0, 1.2))
# _, _, bar_container = ax.hist([], COMP, lw=1, alpha=0.5, ec='yellow', fc='blue')
im = plt.imshow([[[0, 255, 0] for _ in range(K)]], aspect='auto', interpolation='nearest',
                extent=(0, K, 0, 2))


# def update_plot_(frame):
#     for i, rect in enumerate(bar_container.patches):
#         rect.set_height(CSN[i])
#
#     return bar_container.patches


def update_plot(frame):
    im.set_data([c_data])
    return [im]


stream = sd.OutputStream(samplerate=SAMPLE_RATE, channels=data.shape[1], callback=callback, blocksize=FRAME_SIZE)
ani = FuncAnimation(fig, update_plot, interval=1, blit=True, repeat=False)
with stream:
    plt.show()
