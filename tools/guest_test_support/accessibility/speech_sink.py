# SPDX-FileCopyrightText: 2026 Alex Indigo
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Orca speech-server plugin for explicit synthetic app-side test capture."""
import json
import os
from pathlib import Path
import time
from orca import speechserver


class SpeechServer(speechserver.SpeechServer):
    _instance = None

    @staticmethod
    def getFactoryName():
        return 'Alloy synthetic app sink'

    @classmethod
    def getSpeechServer(cls, info=None):
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance

    @classmethod
    def getSpeechServers(cls):
        return [cls.getSpeechServer()]

    @classmethod
    def shutdownActiveServers(cls):
        cls._instance = None

    def getInfo(self):
        return ['Alloy synthetic app sink', 'alloy-synthetic']

    def getVoiceFamilies(self):
        return [speechserver.VoiceFamily({'name': 'synthetic', 'lang': 'en'})]

    def speak(self, text=None, acss=None, interrupt=True):
        if text:
            path = Path(os.environ['ALLOY_AX_SPEECH_SINK'])
            with path.open('a', encoding='utf-8') as sink:
                sink.write(json.dumps({'text': text, 'time_us': time.monotonic_ns() // 1000}) + '\n')

    def speakCharacter(self, character, acss=None):
        self.speak(character, acss)

    def speakKeyEvent(self, event, acss=None):
        self.speak(event.event_string, acss)

    def sayAll(self, utteranceIterator, progressCallback):
        for context, acss in utteranceIterator:
            self.speak(context.utterance, acss)
            progressCallback(context, speechserver.SayAllContext.COMPLETED)
