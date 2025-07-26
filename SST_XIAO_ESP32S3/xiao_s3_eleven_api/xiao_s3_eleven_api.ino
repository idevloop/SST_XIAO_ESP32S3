  /*
 * XIAO ESP32S3 Audio Recorder with ElevenLabs Speech-to-Text
 * Records audio from built-in microphone, saves to SD card, and sends to ElevenLabs STT
 */

#include <I2S.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <base64.h>

// WiFi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_PASSWORD";

// ElevenLabs API configuration
const char* elevenlabs_api_key = "******";
const char* elevenlabs_stt_url = "https://api.elevenlabs.io/v1/speech-to-text";

// Audio recording settings
#define RECORD_TIME   5  // seconds, adjust as needed
#define WAV_FILE_NAME "recording"
#define SAMPLE_RATE   16000U
#define SAMPLE_BITS   16
#define WAV_HEADER_SIZE 44
#define VOLUME_GAIN   2

// Button pin for recording control (optional)
// #define RECORD_BUTTON_PIN 1  // Commented out for auto-recording mode

// Global variables
bool recording_active = false;
bool new_recording_available = false;
String last_transcription = "";

void setup() {
  Serial.begin(115200);
  while (!Serial);
  
  Serial.println("XIAO ESP32S3 Audio Recorder with ElevenLabs STT");
  
  // Initialize button pin (commented out for auto-recording)
  // pinMode(RECORD_BUTTON_PIN, INPUT_PULLUP);
  
  // Initialize I2S for built-in microphone
  I2S.setAllPins(-1, 42, 41, -1, -1);
  if (!I2S.begin(PDM_MONO_MODE, SAMPLE_RATE, SAMPLE_BITS)) {
    Serial.println("Failed to initialize I2S!");
    while (1);
  }
  Serial.println("I2S initialized successfully");
  
  // Initialize SD card
  if (!SD.begin(21)) {
    Serial.println("Failed to mount SD Card!");
    while (1);
  }
  Serial.println("SD Card initialized successfully");
  
  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi connected successfully");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  Serial.println("System ready. Auto-recording will start in 5 seconds...");
  delay(5000); // Initial delay before first recording
}

void loop() {
  // Auto-record every 15 seconds (5 seconds recording + 10 seconds processing)
  static unsigned long last_record_time = 0;
  if (millis() - last_record_time > 15000) { // Record every 15 seconds
    last_record_time = millis();
    Serial.println("Starting auto-recording...");
    record_and_process();
  }
  
  delay(100);
}

void start_recording() {
  recording_active = true;
  // Recording logic will be handled in stop_recording()
}

void stop_recording() {
  if (!recording_active) return;
  
  recording_active = false;
  record_wav();
}

void record_and_process() {
  record_wav();
  process_recording();
}

void record_wav() {
  uint32_t sample_size = 0;
  uint32_t record_size = (SAMPLE_RATE * SAMPLE_BITS / 8) * RECORD_TIME;
  uint8_t *rec_buffer = NULL;
  
  Serial.printf("Recording for %d seconds...\n", RECORD_TIME);
  
  // Create new filename with timestamp
  String filename = "/" + String(WAV_FILE_NAME) + "_" + String(millis()) + ".wav";
  
  File file = SD.open(filename.c_str(), FILE_WRITE);
  if (!file) {
    Serial.println("Failed to create file on SD card!");
    return;
  }
  
  // Write the header to the WAV file
  uint8_t wav_header[WAV_HEADER_SIZE];
  generate_wav_header(wav_header, record_size, SAMPLE_RATE);
  file.write(wav_header, WAV_HEADER_SIZE);
  
  // PSRAM malloc for recording
  rec_buffer = (uint8_t *)ps_malloc(record_size);
  if (rec_buffer == NULL) {
    Serial.println("malloc failed!");
    file.close();
    return;
  }
  
  Serial.printf("Buffer allocated: %d bytes\n", record_size);
  
  // Start recording
  esp_i2s::i2s_read(esp_i2s::I2S_NUM_0, rec_buffer, record_size, &sample_size, portMAX_DELAY);
  if (sample_size == 0) {
    Serial.println("Record Failed!");
    free(rec_buffer);
    file.close();
    return;
  } else {
    Serial.printf("Recorded %d bytes\n", sample_size);
  }
  
  // Increase volume
  for (uint32_t i = 0; i < sample_size; i += SAMPLE_BITS/8) {
    (*(uint16_t *)(rec_buffer+i)) <<= VOLUME_GAIN;
  }
  
  // Write data to the WAV file
  Serial.println("Writing to SD card...");
  if (file.write(rec_buffer, record_size) != record_size) {
    Serial.println("Write file Failed!");
  }
  
  free(rec_buffer);
  file.close();
  Serial.printf("Recording saved as: %s\n", filename.c_str());
}

void process_recording() {
  // Find the most recent recording file
  String latest_file = find_latest_recording();
  if (latest_file.isEmpty()) {
    Serial.println("No recording file found!");
    return;
  }
  
  Serial.printf("Processing file: %s\n", latest_file.c_str());
  
  // Send to ElevenLabs STT
  String transcription = send_to_elevenlabs_stt(latest_file);
  
  if (!transcription.isEmpty()) {
    Serial.println("Transcription result:");
    Serial.println(transcription);
    last_transcription = transcription;
    
    // Optional: Delete the processed file to save space
    // SD.remove(latest_file);
  } else {
    Serial.println("Failed to get transcription");
  }
}

String find_latest_recording() {
  File root = SD.open("/");
  if (!root) {
    return "";
  }
  
  String latest_file = "";
  unsigned long latest_time = 0;
  
  File file = root.openNextFile();
  while (file) {
    String filename = file.name();
    if (filename.startsWith(WAV_FILE_NAME) && filename.endsWith(".wav")) {
      // Extract timestamp from filename
      int start = filename.indexOf('_') + 1;
      int end = filename.lastIndexOf('.');
      if (start > 0 && end > start) {
        unsigned long timestamp = filename.substring(start, end).toInt();
        if (timestamp > latest_time) {
          latest_time = timestamp;
          latest_file = "/" + filename;
        }
      }
    }
    file = root.openNextFile();
  }
  
  root.close();
  return latest_file;
}


String send_to_elevenlabs_stt(String filename) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected!");
    return "";
  }
  
  // Read the audio file
  File file = SD.open(filename.c_str());
  if (!file) {
    Serial.println("Failed to open audio file!");
    return "";
  }
  
  size_t file_size = file.size();
  Serial.printf("File size: %d bytes\n", file_size);
  
  // Check if file is too large
  if (file_size > 500000) { // 500KB limit for testing
    Serial.println("File too large, skipping...");
    file.close();
    return "";
  }
  
  uint8_t* audio_data = (uint8_t*)malloc(file_size);
  if (!audio_data) {
    Serial.println("Failed to allocate memory for audio data!");
    file.close();
    return "";
  }
  
  file.read(audio_data, file_size);
  file.close();
  
  // Try a simpler approach with HTTPClient
  HTTPClient http;
  
  // Set timeout values
  http.setTimeout(30000); // 30 seconds
  http.setConnectTimeout(10000); // 10 seconds
  
  // Begin connection
  if (!http.begin(elevenlabs_stt_url)) {
    Serial.println("Failed to begin HTTP connection!");
    free(audio_data);
    return "";
  }
  
  // Set headers
  http.addHeader("xi-api-key", elevenlabs_api_key);
  
  // Create simple multipart form data as a string
  String boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  
  // Build multipart body
  String body_start = "--" + boundary + "\r\n";
  body_start += "Content-Disposition: form-data; name=\"model_id\"\r\n\r\n";
  body_start += "scribe_v1\r\n";
  body_start += "--" + boundary + "\r\n";
  body_start += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
  body_start += "Content-Type: audio/wav\r\n\r\n";
  
  String body_end = "\r\n--" + boundary + "--\r\n";
  
  // Calculate total size
  size_t total_size = body_start.length() + file_size + body_end.length();
  
  // Create complete body
  uint8_t* complete_body = (uint8_t*)malloc(total_size);
  if (!complete_body) {
    Serial.println("Failed to allocate memory for complete body!");
    free(audio_data);
    http.end();
    return "";
  }
  
  // Copy parts
  memcpy(complete_body, body_start.c_str(), body_start.length());
  memcpy(complete_body + body_start.length(), audio_data, file_size);
  memcpy(complete_body + body_start.length() + file_size, body_end.c_str(), body_end.length());
  
  free(audio_data);
  
  Serial.println("Sending request to ElevenLabs...");
  
  // Send POST request
  int httpResponseCode = http.POST(complete_body, total_size);
  
  free(complete_body);
  
  String response = "";
  
  if (httpResponseCode > 0) {
    Serial.printf("HTTP Response code: %d\n", httpResponseCode);
    response = http.getString();
    
    if (httpResponseCode == 200) {
      Serial.println("Success! Response received:");
      Serial.println(response);
      
      // Parse JSON response
      DynamicJsonDocument doc(2048);
      DeserializationError error = deserializeJson(doc, response);
      
      if (error) {
        Serial.printf("JSON parsing failed: %s\n", error.c_str());
        http.end();
        return "";
      }
      
      if (doc.containsKey("text")) {
        String transcription = doc["text"].as<String>();
        http.end();
        return transcription;
      } else {
        Serial.println("No text field in response");
      }
    } else {
      Serial.println("HTTP Error. Response:");
      Serial.println(response);
    }
  } else {
    Serial.printf("HTTP request failed with error: %d\n", httpResponseCode);
    
    // Print specific error meanings
    switch(httpResponseCode) {
      case -1:
        Serial.println("Error: Connection refused");
        break;
      case -2:
        Serial.println("Error: Send header failed");
        break;
      case -3:
        Serial.println("Error: Send payload failed");
        break;
      case -4:
        Serial.println("Error: Not connected");
        break;
      case -5:
        Serial.println("Error: Connection lost");
        break;
      case -6:
        Serial.println("Error: No stream");
        break;
      case -7:
        Serial.println("Error: No HTTP server");
        break;
      case -8:
        Serial.println("Error: Too less RAM");
        break;
      case -9:
        Serial.println("Error: Encoding");
        break;
      case -10:
        Serial.println("Error: Stream write");
        break;
      case -11:
        Serial.println("Error: Read timeout");
        break;
      default:
        Serial.println("Error: Unknown");
        break;
    }
  }
  
  http.end();
  return "";
}

void generate_wav_header(uint8_t *wav_header, uint32_t wav_size, uint32_t sample_rate) {
  // See this for reference: http://soundfile.sapp.org/doc/WaveFormat/
  uint32_t file_size = wav_size + WAV_HEADER_SIZE - 8;
  uint32_t byte_rate = sample_rate * SAMPLE_BITS / 8;
  
  const uint8_t set_wav_header[] = {
    'R', 'I', 'F', 'F', // ChunkID
    file_size, file_size >> 8, file_size >> 16, file_size >> 24, // ChunkSize
    'W', 'A', 'V', 'E', // Format
    'f', 'm', 't', ' ', // Subchunk1ID
    0x10, 0x00, 0x00, 0x00, // Subchunk1Size (16 for PCM)
    0x01, 0x00, // AudioFormat (1 for PCM)
    0x01, 0x00, // NumChannels (1 channel)
    sample_rate, sample_rate >> 8, sample_rate >> 16, sample_rate >> 24, // SampleRate
    byte_rate, byte_rate >> 8, byte_rate >> 16, byte_rate >> 24, // ByteRate
    0x02, 0x00, // BlockAlign
    0x10, 0x00, // BitsPerSample (16 bits)
    'd', 'a', 't', 'a', // Subchunk2ID
    wav_size, wav_size >> 8, wav_size >> 16, wav_size >> 24, // Subchunk2Size
  };
  memcpy(wav_header, set_wav_header, sizeof(set_wav_header));
}

// Helper function to get the last transcription (for external use)
String get_last_transcription() {
  return last_transcription;
}

// Helper function to check if currently recording
bool is_recording() {
  return recording_active;
}