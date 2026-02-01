/*******************************************************************************
 * SimpleMIDIPlayer.h
 * 
 * Standard SD library compatible MIDI file player
 * Supports: Format 0/1, SysEx, all standard MIDI messages
 ******************************************************************************/

#ifndef SIMPLE_MIDI_PLAYER_H
#define SIMPLE_MIDI_PLAYER_H

#include <Arduino.h>
#include <SD.h>

class SimpleMIDIPlayer {
public:
    // Callback types
    typedef void (*MidiCallback)(uint8_t* data, uint16_t len);
    typedef void (*SysExCallback)(uint8_t* data, uint32_t len);

    SimpleMIDIPlayer();
    ~SimpleMIDIPlayer();

    bool load(const char* filename);
    void close();
    void play();
    void stop();
    void pause();
    void resume();
    bool isPlaying();
    bool isEOF();
    
    void setMidiCallback(MidiCallback cb) { _midiCb = cb; }
    void setSysExCallback(SysExCallback cb) { _sysExCb = cb; }
    
    // Call this in loop() - returns true if still playing
    bool update();

private:
    File _file;
    bool _playing;
    bool _paused;
    bool _eof;
    
    // MIDI file info
    uint16_t _format;
    uint16_t _trackCount;
    uint16_t _ticksPerQuarter;
    uint32_t _tempo;  // microseconds per quarter note
    
    // Track state
    struct TrackState {
        uint32_t offset;      // Current position in file
        uint32_t endOffset;   // End of track
        uint32_t nextTick;    // Next event tick
        uint8_t runningStatus;
        bool active;
    };
    
    static const int MAX_TRACKS = 16;
    TrackState _tracks[MAX_TRACKS];
    int _activeTrackCount;
    
    // Timing
    uint32_t _currentTick;
    unsigned long _lastUpdateMicros;
    float _tickDurationMicros;
    
    // Callbacks
    MidiCallback _midiCb;
    SysExCallback _sysExCb;
    
    // Buffer for SysEx
    static const int SYSEX_BUF_SIZE = 512;
    uint8_t _sysExBuf[SYSEX_BUF_SIZE];
    
    // Internal methods
    bool parseHeader();
    bool parseTrackHeader(int trackIdx);
    uint32_t readVarLen(uint32_t& offset);
    uint8_t readByte(uint32_t& offset);
    uint16_t readWord(uint32_t offset);
    uint32_t readDWord(uint32_t offset);
    void processTrackEvent(int trackIdx);
    void updateTickDuration();
    int findNextTrack();
};

//==============================================================================
// Implementation
//==============================================================================

SimpleMIDIPlayer::SimpleMIDIPlayer() {
    _playing = false;
    _paused = false;
    _eof = true;
    _midiCb = nullptr;
    _sysExCb = nullptr;
    _format = 0;
    _trackCount = 0;
    _ticksPerQuarter = 480;
    _tempo = 500000;  // Default 120 BPM
    _activeTrackCount = 0;
}

SimpleMIDIPlayer::~SimpleMIDIPlayer() {
    close();
}

bool SimpleMIDIPlayer::load(const char* filename) {
    close();
    
    _file = SD.open(filename, FILE_READ);
    if (!_file) {
        Serial.printf("MIDI: Failed to open %s\n", filename);
        return false;
    }
    
    if (!parseHeader()) {
        Serial.println("MIDI: Invalid header");
        close();
        return false;
    }
    
    // Parse all track headers
    _activeTrackCount = 0;
    for (int i = 0; i < _trackCount && i < MAX_TRACKS; i++) {
        if (parseTrackHeader(i)) {
            _activeTrackCount++;
        }
    }
    
    if (_activeTrackCount == 0) {
        Serial.println("MIDI: No valid tracks");
        close();
        return false;
    }
    
    _eof = false;
    _currentTick = 0;
    updateTickDuration();
    
    Serial.printf("MIDI: Loaded - Format %d, %d tracks, %d ticks/quarter\n",
                  _format, _activeTrackCount, _ticksPerQuarter);
    
    return true;
}

void SimpleMIDIPlayer::close() {
    if (_file) {
        _file.close();
    }
    _playing = false;
    _paused = false;
    _eof = true;
    _activeTrackCount = 0;
}

void SimpleMIDIPlayer::play() {
    if (!_eof && _activeTrackCount > 0) {
        _playing = true;
        _paused = false;
        _lastUpdateMicros = micros();
    }
}

void SimpleMIDIPlayer::stop() {
    _playing = false;
    _paused = false;
}

void SimpleMIDIPlayer::pause() {
    _paused = true;
}

void SimpleMIDIPlayer::resume() {
    if (_paused) {
        _paused = false;
        _lastUpdateMicros = micros();
    }
}

bool SimpleMIDIPlayer::isPlaying() {
    return _playing && !_paused;
}

bool SimpleMIDIPlayer::isEOF() {
    return _eof;
}

bool SimpleMIDIPlayer::update() {
    if (!_playing || _paused || _eof) {
        return _playing;
    }
    
    unsigned long now = micros();
    unsigned long elapsed = now - _lastUpdateMicros;
    _lastUpdateMicros = now;
    
    // Calculate ticks elapsed
    float ticksElapsed = elapsed / _tickDurationMicros;
    _currentTick += (uint32_t)ticksElapsed;
    
    // Process events from all tracks
    bool anyActive = false;
    for (int safety = 0; safety < 100; safety++) {  // Limit events per update
        int nextTrack = findNextTrack();
        if (nextTrack < 0) {
            break;
        }
        
        if (_tracks[nextTrack].nextTick <= _currentTick) {
            processTrackEvent(nextTrack);
            anyActive = true;
        } else {
            anyActive = true;
            break;
        }
    }
    
    // Check if all tracks are done
    bool allDone = true;
    for (int i = 0; i < _activeTrackCount; i++) {
        if (_tracks[i].active) {
            allDone = false;
            break;
        }
    }
    
    if (allDone) {
        _eof = true;
        _playing = false;
        Serial.println("MIDI: Playback complete");
    }
    
    return _playing;
}

bool SimpleMIDIPlayer::parseHeader() {
    uint8_t buf[14];
    
    _file.seek(0);
    if (_file.read(buf, 14) != 14) {
        return false;
    }
    
    // Check "MThd"
    if (buf[0] != 'M' || buf[1] != 'T' || buf[2] != 'h' || buf[3] != 'd') {
        return false;
    }
    
    // Header length (should be 6)
    uint32_t headerLen = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                         ((uint32_t)buf[6] << 8) | buf[7];
    if (headerLen < 6) {
        return false;
    }
    
    // Format
    _format = ((uint16_t)buf[8] << 8) | buf[9];
    
    // Track count
    _trackCount = ((uint16_t)buf[10] << 8) | buf[11];
    if (_trackCount > MAX_TRACKS) {
        _trackCount = MAX_TRACKS;
    }
    
    // Division
    uint16_t division = ((uint16_t)buf[12] << 8) | buf[13];
    if (division & 0x8000) {
        // SMPTE format - convert to approximate ticks per quarter
        int fps = -(int8_t)(division >> 8);
        int ticksPerFrame = division & 0xFF;
        _ticksPerQuarter = fps * ticksPerFrame * 2;  // Approximate
    } else {
        _ticksPerQuarter = division;
    }
    
    return true;
}

bool SimpleMIDIPlayer::parseTrackHeader(int trackIdx) {
    // Find track start position
    uint32_t pos = 14;  // After header
    
    for (int i = 0; i < trackIdx; i++) {
        _file.seek(pos);
        uint8_t buf[8];
        if (_file.read(buf, 8) != 8) return false;
        
        // Skip "MTrk" + length
        if (buf[0] != 'M' || buf[1] != 'T' || buf[2] != 'r' || buf[3] != 'k') {
            return false;
        }
        
        uint32_t trackLen = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                            ((uint32_t)buf[6] << 8) | buf[7];
        pos += 8 + trackLen;
    }
    
    // Parse this track's header
    _file.seek(pos);
    uint8_t buf[8];
    if (_file.read(buf, 8) != 8) return false;
    
    if (buf[0] != 'M' || buf[1] != 'T' || buf[2] != 'r' || buf[3] != 'k') {
        return false;
    }
    
    uint32_t trackLen = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                        ((uint32_t)buf[6] << 8) | buf[7];
    
    _tracks[trackIdx].offset = pos + 8;
    _tracks[trackIdx].endOffset = pos + 8 + trackLen;
    _tracks[trackIdx].runningStatus = 0;
    _tracks[trackIdx].active = true;
    
    // Read first delta time
    _tracks[trackIdx].nextTick = readVarLen(_tracks[trackIdx].offset);
    
    return true;
}

uint32_t SimpleMIDIPlayer::readVarLen(uint32_t& offset) {
    uint32_t value = 0;
    uint8_t b;
    
    _file.seek(offset);
    do {
        if (_file.read(&b, 1) != 1) {
            return 0;
        }
        offset++;
        value = (value << 7) | (b & 0x7F);
    } while (b & 0x80);
    
    return value;
}

uint8_t SimpleMIDIPlayer::readByte(uint32_t& offset) {
    uint8_t b = 0;
    _file.seek(offset);
    _file.read(&b, 1);
    offset++;
    return b;
}

void SimpleMIDIPlayer::updateTickDuration() {
    // microseconds per tick = tempo / ticksPerQuarter
    _tickDurationMicros = (float)_tempo / (float)_ticksPerQuarter;
}

int SimpleMIDIPlayer::findNextTrack() {
    int next = -1;
    uint32_t minTick = 0xFFFFFFFF;
    
    for (int i = 0; i < _activeTrackCount; i++) {
        if (_tracks[i].active && _tracks[i].nextTick < minTick) {
            minTick = _tracks[i].nextTick;
            next = i;
        }
    }
    
    return next;
}

void SimpleMIDIPlayer::processTrackEvent(int trackIdx) {
    TrackState& track = _tracks[trackIdx];
    
    if (track.offset >= track.endOffset) {
        track.active = false;
        return;
    }
    
    uint8_t status = readByte(track.offset);
    
    // Running status
    if (status < 0x80) {
        track.offset--;  // Put back the data byte
        status = track.runningStatus;
    } else if (status < 0xF0) {
        track.runningStatus = status;
    }
    
    uint8_t msgBuf[3];
    int msgLen = 0;
    
    if (status >= 0x80 && status < 0xF0) {
        // Channel messages
        msgBuf[0] = status;
        msgLen = 1;
        
        uint8_t type = status & 0xF0;
        if (type == 0xC0 || type == 0xD0) {
            // Program Change, Channel Pressure: 1 data byte
            msgBuf[1] = readByte(track.offset);
            msgLen = 2;
        } else {
            // Note Off, Note On, Poly Pressure, CC, Pitch Bend: 2 data bytes
            msgBuf[1] = readByte(track.offset);
            msgBuf[2] = readByte(track.offset);
            msgLen = 3;
        }
        
        if (_midiCb && msgLen > 0) {
            _midiCb(msgBuf, msgLen);
        }
        
    } else if (status == 0xF0) {
        // SysEx
        uint32_t len = readVarLen(track.offset);
        if (len < SYSEX_BUF_SIZE - 1) {
            _sysExBuf[0] = 0xF0;
            _file.seek(track.offset);
            _file.read(_sysExBuf + 1, len);
            track.offset += len;
            
            if (_sysExCb) {
                _sysExCb(_sysExBuf, len + 1);
            } else if (_midiCb) {
                _midiCb(_sysExBuf, len + 1);
            }
        } else {
            track.offset += len;  // Skip large SysEx
        }
        
    } else if (status == 0xF7) {
        // SysEx continuation or escape
        uint32_t len = readVarLen(track.offset);
        track.offset += len;  // Skip for now
        
    } else if (status == 0xFF) {
        // Meta event
        uint8_t metaType = readByte(track.offset);
        uint32_t len = readVarLen(track.offset);
        
        if (metaType == 0x51 && len == 3) {
            // Tempo change
            uint8_t t[3];
            _file.seek(track.offset);
            _file.read(t, 3);
            _tempo = ((uint32_t)t[0] << 16) | ((uint32_t)t[1] << 8) | t[2];
            updateTickDuration();
            // Serial.printf("MIDI: Tempo change -> %d us/quarter\n", _tempo);
        } else if (metaType == 0x2F) {
            // End of track
            track.active = false;
        }
        
        track.offset += len;
        
    } else {
        // Unknown - skip
        Serial.printf("MIDI: Unknown status 0x%02X\n", status);
    }
    
    // Read next delta time if track still active
    if (track.active && track.offset < track.endOffset) {
        uint32_t delta = readVarLen(track.offset);
        track.nextTick += delta;
    } else {
        track.active = false;
    }
}

#endif // SIMPLE_MIDI_PLAYER_H