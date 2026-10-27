from __future__ import annotations

import time

import numpy as np
from PySide6.QtCore import QByteArray, QObject, QSettings, QIODevice, Signal, Slot
from PySide6.QtMultimedia import QAudio, QAudioDevice, QAudioFormat, QAudioSource, QMediaDevices

from .core import apply_gain


def audio_device_id(device: QAudioDevice) -> str:
    return bytes(device.id()).decode("utf-8", errors="surrogateescape")


class AudioController(QObject):
    changed = Signal()
    levelChanged = Signal(float, bool)
    samplesReady = Signal(bytes, float)
    failed = Signal(str)

    def __init__(self, settings: QSettings):
        super().__init__()
        self.settings = settings
        self.devices: list[QAudioDevice] = []
        self.selected_id = "none"
        self.gain_db = min(12.0, max(-36.0, settings.value("audioGainDB", 0.0, type=float)))
        self.reasons: set[str] = set()
        self.source: QAudioSource | None = None
        self.io: QIODevice | None = None
        self.format = QAudioFormat()
        self.status = "Audio off"
        self._last_meter = 0.0
        self.media_devices = QMediaDevices()
        self.media_devices.audioInputsChanged.connect(self.refresh_devices)
        self.refresh_devices()

    @property
    def has_audio(self) -> bool:
        return self.selected_id != "none"

    @property
    def name(self) -> str:
        device = self._selected_device()
        return device.description() if device is not None else ("Microphone unavailable" if self.has_audio else "No Audio")

    def refresh_devices(self) -> None:
        self.devices = sorted(QMediaDevices.audioInputs(), key=lambda d: d.description().casefold())
        ids = [audio_device_id(device) for device in self.devices]
        default = QMediaDevices.defaultAudioInput()
        default_id = audio_device_id(default) if not default.isNull() else "none"
        if not self.settings.contains("audioID"):
            self.selected_id = default_id
        else:
            self.selected_id = self.settings.value("audioID", "none", type=str)
        if self.selected_id != "none" and self.selected_id not in ids:
            self.status = "Microphone disconnected"
            self._stop_source()
        elif not self.source:
            self.status = "Microphone idle" if self.has_audio else "Audio off"
        if self.reasons and self.has_audio and self.source is None:
            self._start_source()
        self.changed.emit()

    def select(self, identifier: str) -> None:
        self.selected_id = identifier
        self.settings.setValue("audioID", identifier)
        self._stop_source()
        self.status = "Microphone idle" if self.has_audio else "Audio off"
        if self.reasons and self.has_audio:
            self._start_source()
        self.changed.emit()

    def set_gain(self, decibels: float) -> None:
        self.gain_db = min(12.0, max(-36.0, float(decibels)))
        self.settings.setValue("audioGainDB", self.gain_db)
        self.changed.emit()

    def acquire(self, reason: str) -> None:
        self.reasons.add(reason)
        if self.has_audio:
            self._start_source()

    def release(self, reason: str) -> None:
        self.reasons.discard(reason)
        if not self.reasons:
            self._stop_source()
            self.status = "Microphone idle" if self.has_audio else "Audio off"
            self.levelChanged.emit(0.0, False)
            self.changed.emit()

    def monitor(self, reason: str, enabled: bool) -> None:
        if enabled:
            self.acquire(reason)
        else:
            self.release(reason)

    def shutdown(self) -> None:
        self.reasons.clear()
        self._stop_source()

    def _selected_device(self) -> QAudioDevice | None:
        return next((device for device in self.devices if audio_device_id(device) == self.selected_id), None)

    def _start_source(self) -> None:
        if self.source is not None:
            return
        device = self._selected_device()
        if device is None:
            message = "The selected microphone is disconnected. Choose another Audio Input."
            self.status = message
            self.failed.emit(message)
            self.changed.emit()
            return
        requested = QAudioFormat()
        requested.setSampleRate(48000)
        requested.setChannelCount(1)
        requested.setSampleFormat(QAudioFormat.SampleFormat.Float)
        self.format = requested if device.isFormatSupported(requested) else device.preferredFormat()
        self.source = QAudioSource(device, self.format)
        self.source.stateChanged.connect(self._state_changed)
        self.io = self.source.start()
        if self.io is None:
            message = "The microphone could not be opened."
            self.status = message
            self.failed.emit(message)
            self._stop_source()
        else:
            self.io.readyRead.connect(self._read)
            self.status = device.description()
        self.changed.emit()

    def _stop_source(self) -> None:
        if self.source is not None:
            self.source.stop()
            self.source.deleteLater()
        self.source = None
        self.io = None

    @Slot(QAudio.State)
    def _state_changed(self, state) -> None:
        if self.source is None or not self.reasons:
            return
        if state == QAudio.State.StoppedState and self.source.error() != QAudio.Error.NoError:
            message = "The microphone stopped unexpectedly."
            self.status = message
            self.failed.emit(message)
            self._stop_source()
            self.changed.emit()

    def _read(self) -> None:
        if self.io is None:
            return
        data: QByteArray = self.io.readAll()
        raw = bytes(data)
        if not raw:
            return
        timestamp = time.monotonic()
        samples = self._decode(raw)
        if not samples.size:
            return
        processed, peak, clipped = apply_gain(samples, self.gain_db)
        now = time.monotonic()
        if now - self._last_meter >= 0.05:
            self._last_meter = now
            self.levelChanged.emit(peak, clipped)
        self.samplesReady.emit(processed.tobytes(), timestamp)

    def _decode(self, raw: bytes) -> np.ndarray:
        sample_format = self.format.sampleFormat()
        formats = QAudioFormat.SampleFormat
        if sample_format == formats.Float:
            samples = np.frombuffer(raw, dtype="<f4").astype(np.float32)
        elif sample_format == formats.Int16:
            samples = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
        elif sample_format == formats.Int32:
            samples = np.frombuffer(raw, dtype="<i4").astype(np.float32) / 2147483648.0
        elif sample_format == formats.UInt8:
            samples = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
        else:
            return np.empty(0, dtype=np.float32)
        channels = max(1, self.format.channelCount())
        usable = samples.size // channels * channels
        samples = samples[:usable].reshape(-1, channels).mean(axis=1)
        rate = self.format.sampleRate()
        if rate != 48000 and samples.size > 1:
            count = max(1, round(samples.size * 48000 / rate))
            positions = np.arange(count) * rate / 48000
            samples = np.interp(positions, np.arange(samples.size), samples).astype(np.float32)
        return samples.astype(np.float32, copy=False)
