# SST_XIAO_ESP32S3
A compact voice-to-text system using Seeed Studio XIAO ESP32S3 and ElevenLabs API.

## Features
- Record voice using XIAO ESP32S3's built-in microphone
- Save as WAV file to microSD card
- Send audio to ElevenLabs API and get text transcription

## Folders

- `xiao_s3_audio`: Records voice as `record.wav` to SD card.
- `xiao_s3_eleven_api`: Sends `record.wav` to ElevenLabs and prints transcribed text.

## Getting Started

1. **Connect SD card** to the XIAO ESP32S3
2. Upload the sketch from either folder.
3. Use Serial Monitor to view logs.
4. Add your ElevenLabs API key inside the sketch file.
