"""One process-wide lock around model loading.

Importing torch and transformers from two threads at once (the detector warm-up
and an embed job, for example) made both imports fail with ImportError and the
worker restart in a loop, so every backend load takes this lock. Inference runs
outside it, so the lanes stay independent once their models are loaded. The
lock is reentrant because loading a backend may warm it up, which loads the
index version's embedder on the same thread.
"""
from __future__ import annotations

import threading

MODEL_LOAD_LOCK = threading.RLock()
