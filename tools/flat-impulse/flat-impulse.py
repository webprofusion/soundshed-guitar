#!/usr/bin/env python3

import numpy as np
from scipy.io import wavfile

def create_flat_ir(filename, sample_rate=48000, length_samples=2048):
    # Create an array of zeros
    ir = np.zeros(length_samples, dtype=np.float32)
    
    # Set the first sample to 1.0 (The Impulse)
    ir[0] = 1.0
    
    # Write to WAV file
    wavfile.write(filename, sample_rate, ir)

create_flat_ir("flat_response_48k.wav")